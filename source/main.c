/* main.c
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <math.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "jni_fake.h"
#include "prefs.h"
#include "libc_shim.h"
#include "pthr.h"
#include "keycodes.h"
#include "syslang.h"
#include "SwitchStorageBridge.h"
#include "CheatManager.h"
#ifdef USE_VULKAN
#include "hooks/vk.h"
#endif

// Android MotionEvent axis codes (handleControllerAxisEvent expects these)
#define AAXIS_X         0
#define AAXIS_Y         1
#define AAXIS_Z         11
#define AAXIS_RZ        14
#define AAXIS_LTRIGGER  17
#define AAXIS_RTRIGGER  18
#define AAXIS_HAT_X     15
#define AAXIS_HAT_Y     16

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module emu_mod; // libemucore.so

// Handles passed to the NativeLibrary natives. These MUST be real tagged
// FakeObjects (not magic sentinels): the core dereferences them -- e.g. it
// calls GetObjectClass(context) during initialize, and obj->tag would fault on
// a bare sentinel address. Created from jni_obj_new() after jni_init().
static void *NATIVE_CLASS = NULL; // jclass for the NativeLibrary static natives
static void *FAKE_CONTEXT = NULL; // android.content.Context
static void *FAKE_SURFACE = NULL; // android.view.Surface

#if GS_RENDERER == GS_RENDERER_VK
// NVK opens the GPU (nvInitialize/nvMap) internally during vkCreateInstance and
// needs a larger NV transfer-memory arena than the GL path's libnx default.
// libnx declares this weak; a strong definition here overrides it. Tune on
// device if nvMap/instance creation fails.
u32 __nx_nv_transfermem_size = 16 * 1024 * 1024;
#endif

// separate the newlib heap from the .so load region (see config.h SO_REGION_MB)
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    // Leave GPU_RESERVE_MB unmapped: NVK/nvmap allocates outside this heap, and
    // its VK_EXT_memory_budget report is what VMA trusts. See config.h.
    {
      const size_t gpu_reserve = (size_t)GPU_RESERVE_MB * 1024 * 1024;
      if (size > gpu_reserve + (size_t)512 * 1024 * 1024)
        size = (size - gpu_reserve) & ~0x1FFFFF;
    }
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // newlib heap backs the core's malloc + mesa's host allocations (the nouveau
  // GLSL compiler is hungry) + the GameIndex.yaml parse. Reserve a fixed slice
  // for the loaded .so image and give all the rest to newlib. The PS2 RAM
  // mirrors and the recompiler RX caches are mapped SEPARATELY by the core via
  // mmap()/svcMapProcessCodeMemory (see libc_shim.c), not from this heap.
  const size_t so_reserve = (size_t)SO_REGION_MB * 1024 * 1024;
  fake_heap_size = (size > so_reserve + so_reserve / 2) ? (size - so_reserve)
                                                        : (size / 2);

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
  if (fastmem_get_mode() != FASTMEM_MODE_OFF &&
      (!envIsSyscallHinted(0x74) || !envIsSyscallHinted(0x75)))
    fatal_error("Fastmem requires svcMapProcessMemory and svcUnmapProcessMemory.");
}

static int path_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static int dir_nonempty(const char *p) {
  // a usable BIOS dir / resources dir must exist; a deeper content check is the
  // core's job (isBIOSAvailable). Here we only confirm the directory is present.
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void check_data(const char *core_so, const char *disc_path) {
  if (!path_exists(core_so))
    fatal_error("Could not find\n%s.\nPlace it in /switch/nethersx2/\n(extract from the APK lib/arm64-v8a/).", core_so);
  if (!dir_nonempty(RESOURCES_DIR))
    fatal_error("Missing %s.\nCopy the APK assets/ folder there\n(shaders, GameIndex.yaml, fonts).", RESOURCES_DIR);
#if GS_RENDERER == GS_RENDERER_VK
  // The Vulkan renderer loads GLSL from shaders/vulkan/ (compiled to SPIR-V at
  // runtime). Check the dir, not a specific file, so a filename difference
  // across core versions doesn't false-fail a valid setup.
  if (!dir_nonempty(RESOURCES_DIR "/shaders/vulkan"))
    fatal_error("Missing Vulkan shaders in\n%s/shaders/vulkan/.\nCopy the APK assets/shaders/.", RESOURCES_DIR);
#else
  if (!path_exists(RESOURCES_DIR "/shaders/opengl/tfx_fs.glsl"))
    fatal_error("Missing OpenGL shaders in\n%s/shaders/opengl/.\nCopy the APK assets/shaders/.", RESOURCES_DIR);
#endif
  if (!dir_nonempty(BIOS_DIR))
    fatal_error("Missing %s.\nPlace your PS2 BIOS dump there.", BIOS_DIR);
  if (disc_path && *disc_path && !path_exists(disc_path))
    fatal_error("Boot disc not found:\n%s\nSet EmuCore/DiscPath in %s\nor place game.iso in /switch/nethersx2/.",
                disc_path, PREFS_NAME);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      panel_width = 1920; panel_height = 1080;
    } else {
      panel_width = 1280; panel_height = 720;
    }
  } else {
    panel_width = w; panel_height = h;
  }

  // The surface IS the full panel; the core aspect-fits and centres its draw rect
  // within it (via the ComputeDrawRectangle centring patch in hooks/patches.c).
  screen_width = panel_width;
  screen_height = panel_height;

}

// ---------------------------------------------------------------------------
// NativeLibrary JNI entry points (resolved from libemucore.so)
// ---------------------------------------------------------------------------

typedef unsigned char jbool;

static struct {
  int  (*JNI_OnLoad)(void *vm, void *reserved);
  void (*JNI_OnUnload)(void *vm, void *reserved);
  jbool(*initialize)(void *env, void *cls, void *ctx, void *dataDir, void *devName, void *cacheDir);
  void (*applySettings)(void *env, void *cls);
  jbool(*isBIOSAvailable)(void *env, void *cls);
  void (*setDefaultPadSettings)(void *env, void *cls);
  void (*setInputDevices)(void *env, void *cls, void *deviceArray);
  void (*changeSurface)(void *env, void *cls, void *surface, int w, int h, float hz);
  void (*runVMThread)(void *env, void *cls, void *ctx, void *bootPath, void *saveStatePath);
  void (*stopVMThreadLoop)(void *env, void *cls, jbool wait);
  void (*waitForSaveStateFlush)(void *env, void *cls);
  void (*resetVM)(void *env, void *cls);
  jbool(*hasValidRenderSurface)(void *env, void *cls);
  void (*addKeyedOSDMessage)(void *env, void *cls, void *key, void *message, float duration);
  void (*saveStateSlot)(void *env, void *cls, int slot);
  void (*loadStateSlot)(void *env, void *cls, int slot);
  void (*toggleLimiterMode)(void *env, void *cls, int mode);
  void (*reloadPatches)(void *env, void *cls);
  void (*setPadValue)(void *env, void *cls, int controller, int bind, float value);
  void (*handleControllerButtonEvent)(void *env, void *cls, int dev, int code, jbool down);
  void (*handleControllerAxisEvent)(void *env, void *cls, int dev, int axis, float value);
  void (*handlePointerEvent)(void *env, void *cls, int id, float x, float y);
} nl;

#define RESOLVE(field, sym) \
  nl.field = (void *)so_find_addr_rx(&emu_mod, sym)
#define NLSYM(name) "Java_xyz_aethersx2_android_NativeLibrary_" name

static void resolve_entry_points(void) {
  nl.JNI_OnLoad = (void *)so_find_addr_rx(&emu_mod, "JNI_OnLoad");
  nl.JNI_OnUnload = (void *)so_try_find_addr_rx(&emu_mod, "JNI_OnUnload");
  RESOLVE(initialize,                  NLSYM("initialize"));
  RESOLVE(applySettings,               NLSYM("applySettings"));
  RESOLVE(isBIOSAvailable,             NLSYM("isBIOSAvailable"));
  RESOLVE(setInputDevices,             NLSYM("setInputDevices"));
  RESOLVE(changeSurface,               NLSYM("changeSurface"));
  RESOLVE(runVMThread,                 NLSYM("runVMThread"));
  RESOLVE(stopVMThreadLoop,            NLSYM("stopVMThreadLoop"));
  RESOLVE(waitForSaveStateFlush,       NLSYM("waitForSaveStateFlush"));
  RESOLVE(resetVM,                     NLSYM("resetVM"));
  RESOLVE(hasValidRenderSurface,       NLSYM("hasValidRenderSurface"));
  RESOLVE(addKeyedOSDMessage,          NLSYM("addKeyedOSDMessage"));
  RESOLVE(saveStateSlot,               NLSYM("saveStateSlot"));
  RESOLVE(loadStateSlot,               NLSYM("loadStateSlot"));
  RESOLVE(toggleLimiterMode,           NLSYM("toggleLimiterMode"));
  nl.reloadPatches = (void *)so_try_find_addr_rx(&emu_mod, NLSYM("reloadPatches"));
  // setPadValue drives the emulated DualShock2 DIRECTLY (this is what AetherSX2's
  // on-screen touch controls use), bypassing InputManager's binding layer -- which
  // is empty for us: setDefaultPadSettings only writes Pad1 knobs (Type/Deadzone/
  // AxisScale/...), never any button bindings, so handleControllerButtonEvent
  // events matched nothing and were silently dropped.
  nl.setPadValue = (void *)so_try_find_addr_rx(&emu_mod, NLSYM("setPadValue"));
  RESOLVE(handleControllerButtonEvent, NLSYM("handleControllerButtonEvent"));
  RESOLVE(handleControllerAxisEvent,   NLSYM("handleControllerAxisEvent"));
  RESOLVE(handlePointerEvent,          NLSYM("handlePointerEvent"));
  // setDefaultPadSettings is optional (default controller bindings)
  nl.setDefaultPadSettings = (void *)so_try_find_addr_rx(&emu_mod, NLSYM("setDefaultPadSettings"));
}

// ---------------------------------------------------------------------------
// emulation thread: runVMThread blocks here, booting the disc + running the VM
// ---------------------------------------------------------------------------

static char g_disc_path[1024];
static char g_core_so[256];   // core .so to load (Wrapper/CoreSo, default SO_NAME)
static volatile int g_vm_running = 0;
static uint32_t g_game_crc;
static int g_game_crc_changed;

// The Android frontend receives this same callback and uses the CRC to resolve
// cheats/<CRC>.pnach. Keep the callback data instead of discarding it so the
// native overlay can operate on the exact file selected by the core.
void wrapper_game_changed(const char *path, const char *serial,
                          const char *title, uint32_t crc) {
  (void)path;
  (void)serial;
  (void)title;
  __atomic_store_n(&g_game_crc, crc, __ATOMIC_RELEASE);
  __atomic_store_n(&g_game_crc_changed, 1, __ATOMIC_RELEASE);
}

static void *emu_thread_main(void *arg) {
  (void)arg;
  pthr_ensure_fake_tls();
  pthr_pin_ee_core();
  g_vm_running = 1;
  nl.runVMThread(fake_env, NATIVE_CLASS, FAKE_CONTEXT,
                 jni_make_string(g_disc_path), NULL);
  g_vm_running = 0;
  return NULL;
}

static pthread_t emu_thread;

// ---------------------------------------------------------------------------
// startup sequence
// ---------------------------------------------------------------------------

#define MAX_CONTROLLERS 2
// --- HD rumble: driven by the core's setVibratorIntensity JNI callback -------
static HidVibrationDeviceHandle g_vib_players[MAX_CONTROLLERS][2], g_vib_hh[2];
static int g_vib_ready[MAX_CONTROLLERS], g_vib_hh_ready;
static int g_vibration = 1;   // Wrapper/Vibration user toggle
static int g_controller_count = 2;

static void rumble_init(void) {
  static const HidNpadIdType ids[MAX_CONTROLLERS] = {
    HidNpadIdType_No1, HidNpadIdType_No2
  };
  for (int player = 0; player < g_controller_count; player++)
    g_vib_ready[player] = R_SUCCEEDED(hidInitializeVibrationDevices(
        g_vib_players[player], 2, ids[player], HidNpadStyleSet_NpadStandard));
  g_vib_hh_ready = R_SUCCEEDED(hidInitializeVibrationDevices(
      g_vib_hh, 2, HidNpadIdType_Handheld, HidNpadStyleSet_NpadStandard));
}

// large = low-frequency motor, small = high-frequency motor, both 0..1. Sent to
// both the attached-controller and handheld device sets (the inactive one is a
// no-op). Called from jni_fake.c's setVibratorIntensity handler.
void wrapper_rumble(int player, float large, float small) {
  if (!g_vibration || player < 0 || player >= g_controller_count) return;
  if (large < 0.f) large = 0.f; else if (large > 1.f) large = 1.f;
  if (small < 0.f) small = 0.f; else if (small > 1.f) small = 1.f;
  HidVibrationValue v[2];
  v[0].amp_low = large; v[0].freq_low = 160.0f;
  v[0].amp_high = small; v[0].freq_high = 320.0f;
  v[1] = v[0];
  if (g_vib_ready[player])
    hidSendVibrationValues(g_vib_players[player], v, 2);
  if (player == 0 && g_vib_hh_ready)
    hidSendVibrationValues(g_vib_hh, v, 2);
}

static void *make_input_devices(int count) {
  void *arr = jni_make_object_array(count);
  for (int player = 0; player < count; player++) {
    char descriptor[32];
    snprintf(descriptor, sizeof(descriptor), "Switch-Pad-%d", player);
    void *dev = jni_obj_new("xyz/aethersx2/android/NativeLibrary$InputDeviceInfo");
    jni_obj_set_string(dev, "descriptor", descriptor);
    void *vibs = jni_make_object_array(1);
    void *vibrator = jni_obj_new("android/os/Vibrator");
    jni_obj_set_int(vibrator, "player", player);
    jni_obj_array_set(vibs, 0, vibrator);
    jni_obj_set_object(dev, "vibratorManager", NULL);
    jni_obj_set_object(dev, "vibrators", vibs);
    jni_obj_array_set(arr, player, dev);
  }
  return arr;
}

extern volatile int g_net_ready;  // imports.c -- gates the DEV9 socket shims

// Initialize the DEV9 socket backend only when enabled.
static void apply_network_settings(void) {
  int on = prefs_get_bool("Wrapper/Network", false);
  if (on && !g_net_ready) {
    if (R_SUCCEEDED(socketInitializeDefault())) {
      g_net_ready = 1;
    } else {

      on = 0;
    }
  }
  prefs_set_string("DEV9/Eth/EthEnable",     on ? "true" : "false");
  prefs_set_string("DEV9/Eth/EthApi",        "Sockets");
  prefs_set_string("DEV9/Eth/EthDevice",     "");
  prefs_set_string("DEV9/Eth/InterceptDHCP", "true");
  prefs_set_string("DEV9/Eth/ModeDNS1",      "Auto");
  prefs_set_string("DEV9/Eth/ModeDNS2",      "Auto");
  prefs_set_string("DEV9/Hdd/HddEnable",     "false");
  const char *dns = prefs_get_string("Wrapper/NetDNS", "");
  if (on && dns && dns[0]) {
    prefs_set_string("DEV9/Eth/ModeDNS1", "Manual");
    prefs_set_string("DEV9/Eth/DNS1",     dns);
  }
}

static void run_startup_sequence(void) {
  nl.JNI_OnLoad(fake_vm, NULL);

  prefs_set_bool("EmuCore/CPU/Recompiler/EnableFastmem",
                 fastmem_get_mode() != FASTMEM_MODE_OFF);

  jbool ok = nl.initialize(fake_env, NATIVE_CLASS, FAKE_CONTEXT,
                           jni_make_string(DATA_ROOT),
                           jni_make_string(CACHE_DIR),
                           jni_make_string(ANDROID_DEVICE_NAME));
  if (!ok)
    fatal_error("NativeLibrary.initialize() failed.\nCheck %s and the BIOS/resources.", PREFS_NAME);

  // Force the selected renderer and boot disc, overriding any defaults the core
  // wrote during initialize.
  prefs_set_int("EmuCore/GS/Renderer", GS_RENDERER);
  prefs_set_string("EmuCore/DiscPath", g_disc_path);
  prefs_set_string("EmuCore/EnableFastBoot",
                   prefs_get_bool("Wrapper/FastBoot", true) ? "1" : "0");
  prefs_set_bool("EmuCore/Speedhacks/fastCDVD", false);
  // AspectRatio is a PCSX2 enum NAME; repair an invalid value, else respect the
  // user's. The core reads this pref directly to aspect-fit + centre the display.
  {
    const char *ar = prefs_get_string("EmuCore/GS/AspectRatio", "");
    if (strcmp(ar, "Stretch") && strcmp(ar, "Auto 4:3/3:2") && strcmp(ar, "4:3") &&
        strcmp(ar, "16:9"))
      prefs_set_string("EmuCore/GS/AspectRatio", "4:3");
  }
  // Neutral display geometry -- the renderer handles centring itself.
  prefs_set_bool("EmuCore/GS/pcrtc_offsets", false);
  prefs_set_bool("EmuCore/GS/pcrtc_overscan", false);
  prefs_set_bool("EmuCore/GS/IntegerScaling", false);
  prefs_set_int("EmuCore/GS/CropLeft", 0);
  prefs_set_int("EmuCore/GS/CropTop", 0);
  prefs_set_int("EmuCore/GS/CropRight", 0);
  prefs_set_int("EmuCore/GS/CropBottom", 0);
  prefs_set_int("EmuCore/GS/StretchY", 100);
  apply_network_settings();
  // Core logging off: the EE/IOP console formats a lot of strings per frame.
  prefs_set_string("Logging/EnableSystemConsole", "0");
  prefs_set_string("Logging/EnableFileLogging", "0");
  prefs_set_string("Logging/EnableVerbose", "0");
  prefs_set_string("Logging/EnableEEConsole", "0");
  prefs_set_string("Logging/EnableIOPConsole", "0");
  prefs_save();

  if (!nl.isBIOSAvailable(fake_env, NATIVE_CLASS))
    fatal_error("No usable PS2 BIOS found in\n%s.", BIOS_DIR);

  // Set the PS2 OSD language in the BIOS NVRAM from the Switch system language
  // (before the VM boots and reads it), so games that follow the console language
  // come up localised instead of always English.
  apply_system_language();

  // Default bindings must be generated after registering input devices.
  nl.setInputDevices(fake_env, NATIVE_CLASS, make_input_devices(g_controller_count));

  if (nl.setDefaultPadSettings)
    nl.setDefaultPadSettings(fake_env, NATIVE_CLASS);

  for (int player = 0; player < MAX_CONTROLLERS; player++) {
    char key[32];
    snprintf(key, sizeof(key), "Pad%d/Type", player + 1);
    prefs_set_string(key, player < g_controller_count ? "DualShock2" : "None");
  }

  for (int player = 0; player < g_controller_count; player++) {
    char key[40], value[40];
    snprintf(value, sizeof(value), "Switch-Pad-%d/Vibrator0", player);
    snprintf(key, sizeof(key), "Pad%d/LargeMotor", player + 1);
    prefs_set_string(key, value);
    snprintf(key, sizeof(key), "Pad%d/SmallMotor", player + 1);
    prefs_set_string(key, value);
    snprintf(key, sizeof(key), "Pad%d/LargeMotorScale", player + 1);
    prefs_set_string(key, "1.0");
    snprintf(key, sizeof(key), "Pad%d/SmallMotorScale", player + 1);
    prefs_set_string(key, "1.0");
  }

  nl.applySettings(fake_env, NATIVE_CLASS);

  pthread_create(&emu_thread, NULL, emu_thread_main, NULL);

  svcSleepThread(250000000ull);
  nl.changeSurface(fake_env, NATIVE_CLASS, FAKE_SURFACE,
                   screen_width, screen_height, 60.0f);
  nl.applySettings(fake_env, NATIVE_CLASS);

  egl_gl_ownership_release();
}

// ---------------------------------------------------------------------------
// input pump (main thread)
// ---------------------------------------------------------------------------

typedef struct { u64 hid; int keycode; } PadMap;

// positional mapping: Switch bottom face button (A in Switch layout) -> PS2
// cross. Switch A=right -> PS2 circle, etc. Android keycodes; the core's pad
// binding layer maps them to PS2 buttons (default bindings via setDefaultPadSettings).
static const PadMap pad_map[] = {
  { HidNpadButton_B,      AKEYCODE_BUTTON_A },  // Switch B (down)  -> A (cross)
  { HidNpadButton_A,      AKEYCODE_BUTTON_B },  // Switch A (right) -> B (circle)
  { HidNpadButton_Y,      AKEYCODE_BUTTON_X },  // Switch Y (left)  -> X (square)
  { HidNpadButton_X,      AKEYCODE_BUTTON_Y },  // Switch X (up)    -> Y (triangle)
  { HidNpadButton_L,      AKEYCODE_BUTTON_L1 },
  { HidNpadButton_R,      AKEYCODE_BUTTON_R1 },
  { HidNpadButton_ZL,     AKEYCODE_BUTTON_L2 },
  { HidNpadButton_ZR,     AKEYCODE_BUTTON_R2 },
  { HidNpadButton_StickL, AKEYCODE_BUTTON_THUMBL },
  { HidNpadButton_StickR, AKEYCODE_BUTTON_THUMBR },
  { HidNpadButton_Plus,   AKEYCODE_BUTTON_START },
  { HidNpadButton_Minus,  AKEYCODE_BUTTON_SELECT },
  { HidNpadButton_Up,     AKEYCODE_DPAD_UP },
  { HidNpadButton_Down,   AKEYCODE_DPAD_DOWN },
  { HidNpadButton_Left,   AKEYCODE_DPAD_LEFT },
  { HidNpadButton_Right,  AKEYCODE_DPAD_RIGHT },
};

static PadState g_pads[MAX_CONTROLLERS];
static u64 g_pad_previous[MAX_CONTROLLERS];

// PS2 DualShock2 bind indices, recovered from libemucore.so's InputBindingInfo
// array (0x75d88, stride 0x30). This is the `bind` argument of setPadValue().
enum {
  PB_UP = 0, PB_RIGHT, PB_DOWN, PB_LEFT,
  PB_TRIANGLE, PB_CIRCLE, PB_CROSS, PB_SQUARE,
  PB_SELECT, PB_START,
  PB_L1, PB_L2, PB_R1, PB_R2, PB_L3, PB_R3,
  PB_ANALOG, PB_PRESSURE,
  PB_LUP, PB_LRIGHT, PB_LDOWN, PB_LLEFT,
  PB_RUP, PB_RRIGHT, PB_RDOWN, PB_RLEFT,
};

// ---------------------------------------------------------------------------
// Controller mapping. The core has no Android-side InputManager binding table
// (the "Pad1"/"SDL-"/"MapController" machinery is absent -- confirmed by RE), so
// input is driven DIRECTLY through setPadValue. Full user rebinding is therefore
// done in the wrapper: each PS2 target reads a Switch-button/stick TOKEN from
// nethersx2.ini ("Wrapper/Pad1/<name>"), written by the SDL launcher. Defaults
// reproduce the previous hardcoded Nintendo-face layout (A=Circle, B=Cross...).
// ---------------------------------------------------------------------------

// Switch button name token -> HID mask (case-insensitive; "None"/"" = unbound).
static const struct { const char *tok; u64 hid; } hid_tokens[] = {
  { "A", HidNpadButton_A },     { "B", HidNpadButton_B },
  { "X", HidNpadButton_X },     { "Y", HidNpadButton_Y },
  { "L", HidNpadButton_L },     { "R", HidNpadButton_R },
  { "ZL", HidNpadButton_ZL },   { "ZR", HidNpadButton_ZR },
  { "Plus", HidNpadButton_Plus }, { "Minus", HidNpadButton_Minus },
  { "StickL", HidNpadButton_StickL }, { "StickR", HidNpadButton_StickR },
  { "Up", HidNpadButton_Up },   { "Down", HidNpadButton_Down },
  { "Left", HidNpadButton_Left }, { "Right", HidNpadButton_Right },
};

// 16 PS2 digital targets in ini order: { ini key, PB_* bind, default Switch token }.
static const struct { const char *key; int bind; const char *def; } ps2_buttons[] = {
  { "Up", PB_UP, "Up" },         { "Down", PB_DOWN, "Down" },
  { "Left", PB_LEFT, "Left" },   { "Right", PB_RIGHT, "Right" },
  { "Triangle", PB_TRIANGLE, "X" }, { "Circle", PB_CIRCLE, "A" },
  { "Cross", PB_CROSS, "B" },    { "Square", PB_SQUARE, "Y" },
  { "L1", PB_L1, "L" },          { "R1", PB_R1, "R" },
  { "L2", PB_L2, "ZL" },         { "R2", PB_R2, "ZR" },
  { "Select", PB_SELECT, "Minus" }, { "Start", PB_START, "Plus" },
  { "L3", PB_L3, "StickL" },     { "R3", PB_R3, "StickR" },
};
#define NUM_PS2_BUTTONS (sizeof(ps2_buttons) / sizeof(*ps2_buttons))

typedef struct { int src; int invX, invY; } PadStick; // src: 0=LStick 1=RStick -1=none
typedef struct {
  struct { u64 hid; int bind; } binds[NUM_PS2_BUTTONS];
  int bind_count;
  PadStick left_stick;
  PadStick right_stick;
  float deadzone;
} PadConfig;
static PadConfig g_pad_config[MAX_CONTROLLERS];
static u64 g_turbo_combo;
static int g_turbo_active;
static int g_turbo_blocked;

static u64 tok_to_hid(const char *tok) {
  if (!tok || !*tok || !strcasecmp(tok, "None")) return 0;
  for (unsigned i = 0; i < sizeof(hid_tokens) / sizeof(*hid_tokens); i++)
    if (!strcasecmp(tok, hid_tokens[i].tok)) return hid_tokens[i].hid;
  return 0; // unknown token -> unbound
}

static u64 combo_to_hid(const char *combo) {
  if (!combo || !*combo || !strcasecmp(combo, "None")) return 0;
  char buffer[192];
  if (strlen(combo) >= sizeof(buffer)) return 0;
  strcpy(buffer, combo);
  u64 mask = 0;
  char *save = NULL;
  for (char *part = strtok_r(buffer, "+", &save); part; part = strtok_r(NULL, "+", &save)) {
    while (*part && isspace((unsigned char)*part)) part++;
    char *end = part + strlen(part);
    while (end > part && isspace((unsigned char)end[-1])) *--end = '\0';
    const u64 button = tok_to_hid(part);
    if (!button) return 0;
    mask |= button;
  }
  return mask;
}

static int stick_src_from_tok(const char *tok) {
  if (tok && !strcasecmp(tok, "LStick")) return 0;
  if (tok && !strcasecmp(tok, "RStick")) return 1;
  return -1; // None / unknown
}

static const char *pad_pref_string(int player, const char *name, const char *def) {
  char key[64];
  snprintf(key, sizeof(key), "Wrapper/Pad%d/%s", player + 1, name);
  if (player > 0 && !prefs_contains(key)) {
    snprintf(key, sizeof(key), "Wrapper/Pad1/%s", name);
  }
  return prefs_get_string(key, def);
}

static bool pad_pref_bool(int player, const char *name, bool def) {
  char key[64];
  snprintf(key, sizeof(key), "Wrapper/Pad%d/%s", player + 1, name);
  if (player > 0 && !prefs_contains(key)) {
    snprintf(key, sizeof(key), "Wrapper/Pad1/%s", name);
  }
  return prefs_get_bool(key, def);
}

static int pad_pref_int(int player, const char *name, int def) {
  char key[64];
  snprintf(key, sizeof(key), "Wrapper/Pad%d/%s", player + 1, name);
  if (player > 0 && !prefs_contains(key)) {
    snprintf(key, sizeof(key), "Wrapper/Pad1/%s", name);
  }
  return prefs_get_int(key, def);
}

static void pad_load_bindings(void) {
  g_controller_count = prefs_get_int("Wrapper/ControllerCount", 2);
  if (g_controller_count < 1) g_controller_count = 1;
  if (g_controller_count > MAX_CONTROLLERS) g_controller_count = MAX_CONTROLLERS;
  memset(g_pad_config, 0, sizeof(g_pad_config));
  for (int player = 0; player < MAX_CONTROLLERS; player++) {
    PadConfig *config = &g_pad_config[player];
    for (unsigned i = 0; i < NUM_PS2_BUTTONS; i++) {
      const u64 hid = tok_to_hid(pad_pref_string(player, ps2_buttons[i].key,
                                                  ps2_buttons[i].def));
      if (hid) {
        config->binds[config->bind_count].hid = hid;
        config->binds[config->bind_count].bind = ps2_buttons[i].bind;
        config->bind_count++;
      }
    }
    config->left_stick.src = stick_src_from_tok(pad_pref_string(player, "LeftStick", "LStick"));
    config->left_stick.invX = pad_pref_bool(player, "LeftStickInvertX", false);
    config->left_stick.invY = pad_pref_bool(player, "LeftStickInvertY", false);
    config->right_stick.src = stick_src_from_tok(pad_pref_string(player, "RightStick", "RStick"));
    config->right_stick.invX = pad_pref_bool(player, "RightStickInvertX", false);
    config->right_stick.invY = pad_pref_bool(player, "RightStickInvertY", false);
    int deadzone = pad_pref_int(player, "Deadzone", 10);
    if (deadzone < 0) deadzone = 0;
    if (deadzone > 90) deadzone = 90;
    config->deadzone = (float)deadzone / 100.0f;
  }
  g_vibration = prefs_get_bool("Wrapper/Vibration", true);
  g_turbo_combo = combo_to_hid(prefs_get_string("Wrapper/TurboCombo", "None"));
}

static void pad_axis(int controller, int neg_bind, int pos_bind, float v) {
  nl.setPadValue(fake_env, NATIVE_CLASS, controller, pos_bind, v > 0.f ? v : 0.f);
  nl.setPadValue(fake_env, NATIVE_CLASS, controller, neg_bind, v < 0.f ? -v : 0.f);
}

static void apply_ps2_stick(int controller, const PadConfig *config, const PadStick *c,
                            HidAnalogStickState ls, HidAnalogStickState rs,
                            int negX, int posX, int negY, int posY) {
  if (c->src < 0) {
    pad_axis(controller, negX, posX, 0.f);
    pad_axis(controller, negY, posY, 0.f);
    return;
  }
  const HidAnalogStickState s = (c->src == 0) ? ls : rs;
  const float scale = 1.f / 32767.0f;
  float x = (float)s.x * scale, y = (float)s.y * scale;
  if (config->deadzone > 0.f) {
    float mag = sqrtf(x * x + y * y);
    if (mag <= config->deadzone) { x = 0.f; y = 0.f; }
    else { float k = (mag - config->deadzone) / ((1.f - config->deadzone) * mag); x *= k; y *= k; }
  }
  if (c->invX) x = -x;
  if (c->invY) y = -y;
  pad_axis(controller, negX, posX, x);
  pad_axis(controller, negY, posY, y);
}

typedef enum {
  QUICK_MENU_CLOSED,
  QUICK_MENU_MAIN,
  QUICK_MENU_FRAMERATE,
  QUICK_MENU_CHEATS,
  QUICK_MENU_MAPPING,
  QUICK_MENU_CAPTURE
} QuickMenuMode;

typedef enum {
  QUICK_ITEM_RESUME,
  QUICK_ITEM_STATE_SLOT,
  QUICK_ITEM_LOAD_STATE,
  QUICK_ITEM_SAVE_STATE,
  QUICK_ITEM_MAPPING,
  QUICK_ITEM_CHEATS,
#ifdef USE_VULKAN
  QUICK_ITEM_LSFG,
#endif
  QUICK_ITEM_FRAMERATE,
  QUICK_ITEM_RESET,
  QUICK_ITEM_EXIT,
  QUICK_ITEM_COUNT
} QuickMenuMainItem;

typedef enum {
  QUICK_FRAMERATE_LIMITER,
  QUICK_FRAMERATE_NTSC,
  QUICK_FRAMERATE_PAL,
  QUICK_FRAMERATE_EE_CYCLE_RATE,
  QUICK_FRAMERATE_EE_CYCLE_SKIP,
  QUICK_FRAMERATE_BACK,
  QUICK_FRAMERATE_COUNT
} QuickMenuFrameRateItem;

static QuickMenuMode g_quick_menu_mode;
static int g_quick_menu_selection;
static int g_quick_menu_framerate_selection;
static int g_quick_menu_slot;
static int g_quick_menu_cheat_selection;
static int g_quick_menu_map_player;
static int g_quick_menu_map_selection;
static int g_quick_menu_capture_armed;
static int g_quick_menu_exit_requested;
static int g_quick_menu_limiter_unlimited;
static float g_quick_menu_ntsc_rate;
static float g_quick_menu_pal_rate;
static int g_quick_menu_ee_cycle_rate;
static int g_quick_menu_ee_cycle_skip;
static int g_quick_menu_restore_messages;
static int g_quick_menu_ready;
#ifdef USE_VULKAN
static int g_quick_menu_lsfg_enabled;
#endif

static const char *const g_ee_cycle_rate_labels[] = {
  "50%", "60%", "75%", "100%", "130%", "180%", "300%"
};

static const char *const g_ee_cycle_skip_labels[] = {
  "Off", "Mild", "Moderate", "Maximum"
};

static NxCheatList g_quick_cheats;
static char g_quick_cheat_path[256];
static int g_quick_cheat_load_ok;

static void text_append(char *buffer, size_t size, const char *format, ...) {
  size_t used = strlen(buffer);
  if (used >= size - 1) return;
  va_list args;
  va_start(args, format);
  vsnprintf(buffer + used, size - used, format, args);
  va_end(args);
}

static void quick_menu_message(const char *key, const char *message, float duration) {
  nl.addKeyedOSDMessage(fake_env, NATIVE_CLASS, jni_make_string(key),
                        jni_make_string(message), duration);
}

static void quick_menu_status(const char *message) {
  quick_menu_message("nethersx2_quick_status", message, 3.0f);
}

static void quick_menu_release_inputs(void) {
  if (!nl.setPadValue) return;
  for (int player = 0; player < g_controller_count; player++) {
    PadConfig *config = &g_pad_config[player];
    for (int i = 0; i < config->bind_count; i++)
      nl.setPadValue(fake_env, NATIVE_CLASS, player, config->binds[i].bind, 0.0f);
    pad_axis(player, PB_LLEFT, PB_LRIGHT, 0.0f);
    pad_axis(player, PB_LDOWN, PB_LUP, 0.0f);
    pad_axis(player, PB_RLEFT, PB_RRIGHT, 0.0f);
    pad_axis(player, PB_RDOWN, PB_RUP, 0.0f);
  }
}

static bool quick_menu_item_available(int item) {
#ifdef USE_VULKAN
  if (item == QUICK_ITEM_LSFG) return vk_lsfg_is_available();
#else
  (void)item;
#endif
  return true;
}

static void quick_menu_move_selection(int direction) {
  for (int i = 0; i < QUICK_ITEM_COUNT; i++) {
    g_quick_menu_selection =
        (g_quick_menu_selection + direction + QUICK_ITEM_COUNT) % QUICK_ITEM_COUNT;
    if (quick_menu_item_available(g_quick_menu_selection)) return;
  }
}

static void quick_menu_draw_main(void) {
  static const char *labels[] = {
    "Resume game", "State slot", "Load state", "Save state",
    "Controller mapping", "Cheat codes",
#ifdef USE_VULKAN
    "Frame Generation",
#endif
    "Frame Rate Control", "Reset console", "Exit game"
  };
  if (!quick_menu_item_available(g_quick_menu_selection))
    quick_menu_move_selection(1);
  char text[1024] = "NETHERSX2 QUICK MENU\n\n";
  for (int i = 0; i < (int)(sizeof(labels) / sizeof(*labels)); i++) {
    const char *marker = i == g_quick_menu_selection &&
                         quick_menu_item_available(i) ? "> " : "  ";
    if (i == QUICK_ITEM_STATE_SLOT)
      text_append(text, sizeof(text), "%s%s: %d\n", marker, labels[i], g_quick_menu_slot);
#ifdef USE_VULKAN
    else if (i == QUICK_ITEM_LSFG) {
      if (!vk_lsfg_is_available())
        text_append(text, sizeof(text), "  %s: Disabled\n", labels[i]);
      else
        text_append(text, sizeof(text), "%s%s: %s\n", marker, labels[i],
                    g_quick_menu_lsfg_enabled ? "On" : "Off");
    }
#endif
    else if (i == QUICK_ITEM_FRAMERATE)
      text_append(text, sizeof(text), "%s%s >\n", marker, labels[i]);
    else
      text_append(text, sizeof(text), "%s%s\n", marker, labels[i]);
  }
  text_append(text, sizeof(text), "\nA  Select     B  Close     Left/Right  Change");
  quick_menu_message("nethersx2_quick_menu", text, 3600.0f);
}

static void quick_menu_draw_framerate(void) {
  static const char *labels[] = {
    "Frame limiter", "NTSC frame rate", "PAL frame rate",
    "EE cycle rate", "EE cycle skip", "Back"
  };
  char text[1024] = "FRAME RATE CONTROL\n\n";
  for (int i = 0; i < QUICK_FRAMERATE_COUNT; i++) {
    const char *marker = i == g_quick_menu_framerate_selection ? "> " : "  ";
    if (i == QUICK_FRAMERATE_LIMITER)
      text_append(text, sizeof(text), "%s%s: %s\n", marker, labels[i],
                  g_quick_menu_limiter_unlimited ? "Unlimited" : "Limited");
    else if (i == QUICK_FRAMERATE_NTSC)
      text_append(text, sizeof(text), "%s%s: %.2f Hz\n", marker, labels[i],
                  g_quick_menu_ntsc_rate);
    else if (i == QUICK_FRAMERATE_PAL)
      text_append(text, sizeof(text), "%s%s: %.2f Hz\n", marker, labels[i],
                  g_quick_menu_pal_rate);
    else if (i == QUICK_FRAMERATE_EE_CYCLE_RATE)
      text_append(text, sizeof(text), "%s%s: %s\n", marker, labels[i],
                  g_ee_cycle_rate_labels[g_quick_menu_ee_cycle_rate + 3]);
    else if (i == QUICK_FRAMERATE_EE_CYCLE_SKIP)
      text_append(text, sizeof(text), "%s%s: %s\n", marker, labels[i],
                  g_ee_cycle_skip_labels[g_quick_menu_ee_cycle_skip]);
    else
      text_append(text, sizeof(text), "%s%s\n", marker, labels[i]);
  }
  text_append(text, sizeof(text), "\nA  Select/Toggle     Left/Right  Change     B  Back");
  quick_menu_message("nethersx2_quick_menu", text, 3600.0f);
}

static void quick_menu_refresh_cheats(void) {
  memset(&g_quick_cheats, 0, sizeof(g_quick_cheats));
  g_quick_cheat_path[0] = '\0';
  const uint32_t crc = __atomic_load_n(&g_game_crc, __ATOMIC_ACQUIRE);
  if (!crc) {
    g_quick_cheat_load_ok = 1;
    return;
  }
  snprintf(g_quick_cheat_path, sizeof(g_quick_cheat_path),
           DATA_ROOT "/cheats/%08X.pnach", crc);
  g_quick_cheat_load_ok = nx_cheat_load(g_quick_cheat_path, &g_quick_cheats);
  if (g_quick_menu_cheat_selection > (int)g_quick_cheats.count)
    g_quick_menu_cheat_selection = (int)g_quick_cheats.count;
}

static void quick_menu_draw_cheats(void) {
  quick_menu_refresh_cheats();
  const uint32_t crc = __atomic_load_n(&g_game_crc, __ATOMIC_ACQUIRE);
  char text[1024];
  if (crc)
    snprintf(text, sizeof(text), "CHEAT CODES - CRC %08X\n\n", crc);
  else
    snprintf(text, sizeof(text), "CHEAT CODES\n\n");
  if (!crc) {
    text_append(text, sizeof(text), "Waiting for the running game's CRC.\n\n> Back\n");
  } else if (!g_quick_cheat_load_ok) {
    text_append(text, sizeof(text), "Could not read %08X.pnach.\n\n> Back\n", crc);
  } else if (!g_quick_cheats.file_exists) {
    text_append(text, sizeof(text), "No cheat file found.\n\nPlace %08X.pnach in:\n/switch/nethersx2/cheats/\n\n> Back\n", crc);
  } else if (!g_quick_cheats.count) {
    text_append(text, sizeof(text), "No named PNACH code blocks were found.\n\n> Back\n");
  } else {
    const int count = (int)g_quick_cheats.count;
    int first = g_quick_menu_cheat_selection > 3 ? g_quick_menu_cheat_selection - 3 : 0;
    if (first > count - 6) first = count > 6 ? count - 6 : 0;
    int last = first + 7;
    if (last > count + 1) last = count + 1;
    for (int item = first; item < last; item++) {
      const char *marker = item == g_quick_menu_cheat_selection ? "> " : "  ";
      if (item == count) {
        text_append(text, sizeof(text), "%sBack\n", marker);
      } else {
        const NxCheatEntry *entry = &g_quick_cheats.entries[item];
        const char *state = !entry->enabled_count ? "[ ]" :
                            entry->enabled_count == entry->patch_count ? "[x]" : "[-]";
        text_append(text, sizeof(text), "%s%s %.74s\n", marker, state, entry->name);
      }
    }
    if (g_quick_cheats.truncated)
      text_append(text, sizeof(text), "\nOnly the first %d codes are shown.", NX_CHEAT_MAX_ENTRIES);
  }
  text_append(text, sizeof(text), "\nA  Toggle     Left  Off     Right  On     B  Back");
  quick_menu_message("nethersx2_quick_menu", text, 3600.0f);
}

static void quick_menu_draw_mapping(void) {
  const int item_count = (int)NUM_PS2_BUTTONS + 2;
  const int first = g_quick_menu_map_selection > 3 ? g_quick_menu_map_selection - 3 : 0;
  int last = first + 7;
  if (last > item_count) last = item_count;
  char text[1024] = "CONTROLLER MAPPING\n\n";
  for (int item = first; item < last; item++) {
    const char *marker = item == g_quick_menu_map_selection ? "> " : "  ";
    if (item == 0) {
      text_append(text, sizeof(text), "%sPlayer: %d\n", marker, g_quick_menu_map_player + 1);
    } else if (item <= (int)NUM_PS2_BUTTONS) {
      const unsigned bind = (unsigned)(item - 1);
      text_append(text, sizeof(text), "%s%s: %s\n", marker, ps2_buttons[bind].key,
                  pad_pref_string(g_quick_menu_map_player, ps2_buttons[bind].key,
                                  ps2_buttons[bind].def));
    } else {
      text_append(text, sizeof(text), "%sBack\n", marker);
    }
  }
  text_append(text, sizeof(text), "\nA  Rebind     Y  Clear     B  Back");
  quick_menu_message("nethersx2_quick_menu", text, 3600.0f);
}

static void quick_menu_draw_capture(void) {
  const unsigned bind = (unsigned)(g_quick_menu_map_selection - 1);
  char text[512];
  snprintf(text, sizeof(text),
           "CONTROLLER MAPPING\n\nPlayer %d - %s\n\nRelease all buttons, then press the new button.\nL + R + Plus cancels.",
           g_quick_menu_map_player + 1, ps2_buttons[bind].key);
  quick_menu_message("nethersx2_quick_menu", text, 3600.0f);
}

static void quick_menu_close(void) {
  quick_menu_message("nethersx2_quick_menu", "", 0.01f);
  g_quick_menu_mode = QUICK_MENU_CLOSED;
  if (g_quick_menu_restore_messages) {
    prefs_set_bool("EmuCore/GS/OsdShowMessages", false);
    prefs_save();
    nl.applySettings(fake_env, NATIVE_CLASS);
    g_quick_menu_restore_messages = 0;
  }
}

static void quick_menu_open(void) {
  quick_menu_release_inputs();
  g_quick_menu_mode = QUICK_MENU_MAIN;
  g_quick_menu_selection = 0;
  g_quick_menu_limiter_unlimited = !prefs_get_bool("EmuCore/GS/FrameLimitEnable", true);
  g_quick_menu_ntsc_rate = prefs_get_float("EmuCore/GS/FramerateNTSC", 59.94f);
  if (!isfinite(g_quick_menu_ntsc_rate) || g_quick_menu_ntsc_rate < 15.0f)
    g_quick_menu_ntsc_rate = 15.0f;
  if (g_quick_menu_ntsc_rate > 120.0f) g_quick_menu_ntsc_rate = 120.0f;
  g_quick_menu_pal_rate = prefs_get_float("EmuCore/GS/FrameratePAL", 50.0f);
  if (!isfinite(g_quick_menu_pal_rate) || g_quick_menu_pal_rate < 12.0f)
    g_quick_menu_pal_rate = 12.0f;
  if (g_quick_menu_pal_rate > 100.0f) g_quick_menu_pal_rate = 100.0f;
  g_quick_menu_ee_cycle_rate = prefs_get_int("EmuCore/Speedhacks/EECycleRate", 0);
  if (g_quick_menu_ee_cycle_rate < -3) g_quick_menu_ee_cycle_rate = -3;
  if (g_quick_menu_ee_cycle_rate > 3) g_quick_menu_ee_cycle_rate = 3;
  g_quick_menu_ee_cycle_skip = prefs_get_int("EmuCore/Speedhacks/EECycleSkip", 0);
  if (g_quick_menu_ee_cycle_skip < 0) g_quick_menu_ee_cycle_skip = 0;
  if (g_quick_menu_ee_cycle_skip > 3) g_quick_menu_ee_cycle_skip = 3;
#ifdef USE_VULKAN
  g_quick_menu_lsfg_enabled = vk_lsfg_is_enabled();
#endif
  if (!prefs_get_bool("EmuCore/GS/OsdShowMessages", true)) {
    g_quick_menu_restore_messages = 1;
    prefs_set_bool("EmuCore/GS/OsdShowMessages", true);
    nl.applySettings(fake_env, NATIVE_CLASS);
  }
  quick_menu_draw_main();
}

static const char *quick_menu_pressed_token(u64 pressed) {
  for (unsigned i = 0; i < sizeof(hid_tokens) / sizeof(*hid_tokens); i++)
    if (pressed & hid_tokens[i].hid) return hid_tokens[i].tok;
  return NULL;
}

static bool quick_menu_persist_setting(const char *path, const char *key, const char *value) {
  char tmp[1024], old[1024];
  if (!path || !*path || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp) ||
      snprintf(old, sizeof(old), "%s.old", path) >= (int)sizeof(old))
    return false;
  remove(tmp);
  struct stat st;
  if (stat(path, &st) != 0 && stat(old, &st) == 0)
    rename(old, path);
  else
    remove(old);

  FILE *input = fopen(path, "r");
  FILE *output = fopen(tmp, "w");
  if (!output) {
    if (input) fclose(input);
    return false;
  }
  bool replaced = false;
  char line[2048];
  while (input && fgets(line, sizeof(line), input)) {
    char *start = line;
    while (*start && isspace((unsigned char)*start)) start++;
    char *equals = strchr(start, '=');
    char *end = equals;
    while (end && end > start && isspace((unsigned char)end[-1])) end--;
    if (equals && (size_t)(end - start) == strlen(key) &&
        !memcmp(start, key, strlen(key))) {
      fprintf(output, "%s = %s\n", key, value);
      replaced = true;
    } else {
      fputs(line, output);
    }
  }
  if (input) fclose(input);
  if (!replaced) fprintf(output, "%s = %s\n", key, value);
  bool ok = fflush(output) == 0 && fsync(fileno(output)) == 0;
  if (fclose(output) != 0) ok = false;
  if (!ok) {
    remove(tmp);
    return false;
  }
  if (rename(path, old) != 0 && errno != ENOENT) {
    remove(tmp);
    return false;
  }
  if (rename(tmp, path) != 0) {
    rename(old, path);
    return false;
  }
  fsdevCommitDevice("sdmc");
  remove(old);
  fsdevCommitDevice("sdmc");
  return true;
}

static bool quick_menu_persist_launcher_setting(const char *key, const char *value) {
  return quick_menu_persist_setting(DATA_ROOT "/launcher.ini", key, value);
}

static bool quick_menu_persist_game_setting(const char *key, const char *value) {
  const char *path = prefs_get_string("Wrapper/GameConfigPath", "");
  static const char prefix[] = DATA_ROOT "/gamecfg/";
  if (strncmp(path, prefix, sizeof(prefix) - 1) || strstr(path, "/../"))
    return false;
  return quick_menu_persist_setting(path, key, value);
}

static void quick_menu_persist_game_crc(void) {
  if (!__atomic_exchange_n(&g_game_crc_changed, 0, __ATOMIC_ACQ_REL)) return;
  const uint32_t crc = __atomic_load_n(&g_game_crc, __ATOMIC_ACQUIRE);
  const char *path = prefs_get_string("Wrapper/GameCRCPath", "");
  static const char prefix[] = DATA_ROOT "/gamecrc/";
  if (!crc || strncmp(path, prefix, sizeof(prefix) - 1) || strstr(path, "/../")) return;
  char value[16];
  snprintf(value, sizeof(value), "%08X", crc);
  // One attempt per core callback. Retrying a failed SD write at the 120 Hz
  // input rate would only make a storage fault worse.
  quick_menu_persist_setting(path, "CRC", value);
}

static void quick_menu_save_prefs_preserving_messages(void) {
  if (g_quick_menu_restore_messages)
    prefs_set_bool("EmuCore/GS/OsdShowMessages", false);
  prefs_save();
  if (g_quick_menu_restore_messages)
    prefs_set_bool("EmuCore/GS/OsdShowMessages", true);
}

#ifdef USE_VULKAN
static void quick_menu_set_lsfg_enabled(bool enabled) {
  g_quick_menu_lsfg_enabled = enabled;
  vk_lsfg_request_enabled(enabled);
}
#endif

typedef enum {
  QUICK_CHEAT_UPDATE_FAILED,
  QUICK_CHEAT_UPDATE_OK,
  QUICK_CHEAT_UPDATE_PROFILE_UNSAVED
} QuickCheatUpdateResult;

static QuickCheatUpdateResult quick_menu_set_cheat_enabled(unsigned index, bool enabled) {
  if (!g_quick_cheat_load_ok || index >= g_quick_cheats.count ||
      !nx_cheat_set_enabled(g_quick_cheat_path, index, enabled))
    return QUICK_CHEAT_UPDATE_FAILED;

  bool profile_saved = true;
  if (enabled) {
    // This is the core's master gate for cheats/<CRC>.pnach. Enabling an
    // individual code also enables that gate for this game; disabling a code
    // leaves the user's existing master setting untouched.
    prefs_set_bool("EmuCore/EnableCheats", true);
    quick_menu_save_prefs_preserving_messages();
    nl.applySettings(fake_env, NATIVE_CLASS);
    profile_saved = quick_menu_persist_game_setting("EmuCore/EnableCheats", "true");
  }
  if (nl.reloadPatches) nl.reloadPatches(fake_env, NATIVE_CLASS);
  return profile_saved ? QUICK_CHEAT_UPDATE_OK : QUICK_CHEAT_UPDATE_PROFILE_UNSAVED;
}

static bool quick_menu_store_binding(const char *token) {
  const unsigned bind = (unsigned)(g_quick_menu_map_selection - 1);
  char key[64];
  snprintf(key, sizeof(key), "Wrapper/Pad%d/%s", g_quick_menu_map_player + 1,
           ps2_buttons[bind].key);
  prefs_set_string(key, token);
  quick_menu_save_prefs_preserving_messages();
  pad_load_bindings();
  return quick_menu_persist_launcher_setting(key, token);
}

static bool quick_menu_update(u64 down, u64 pressed) {
  const bool chord = (pressed & HidNpadButton_Plus) &&
                     (down & HidNpadButton_L) && (down & HidNpadButton_R);
  if (g_quick_menu_mode == QUICK_MENU_CLOSED) {
    if (!g_quick_menu_ready || !chord) return false;
    quick_menu_open();
    return true;
  }
  if (chord) {
    quick_menu_close();
    return true;
  }

  if (g_quick_menu_mode == QUICK_MENU_CAPTURE) {
    if (!down) {
      g_quick_menu_capture_armed = 1;
    } else if (g_quick_menu_capture_armed && pressed) {
      const char *token = quick_menu_pressed_token(pressed);
      if (token) {
        const bool saved = quick_menu_store_binding(token);
        g_quick_menu_mode = QUICK_MENU_MAPPING;
        quick_menu_draw_mapping();
        quick_menu_status(saved ? "Controller binding updated" :
                                  "Binding updated, but launcher.ini could not be saved");
      }
    }
    return true;
  }
  if (!pressed) return true;

  if (g_quick_menu_mode == QUICK_MENU_MAIN) {
    if (pressed & HidNpadButton_Up)
      quick_menu_move_selection(-1);
    if (pressed & HidNpadButton_Down)
      quick_menu_move_selection(1);
    if (g_quick_menu_selection == QUICK_ITEM_STATE_SLOT && (pressed & HidNpadButton_Left))
      g_quick_menu_slot = (g_quick_menu_slot + 9) % 10;
    if (g_quick_menu_selection == QUICK_ITEM_STATE_SLOT && (pressed & HidNpadButton_Right))
      g_quick_menu_slot = (g_quick_menu_slot + 1) % 10;
    if (pressed & HidNpadButton_B) {
      quick_menu_close();
      return true;
    }
    if (pressed & HidNpadButton_A) {
      switch (g_quick_menu_selection) {
        case QUICK_ITEM_RESUME: quick_menu_close(); return true;
        case QUICK_ITEM_LOAD_STATE:
          nl.loadStateSlot(fake_env, NATIVE_CLASS, g_quick_menu_slot);
          quick_menu_close();
          quick_menu_status("State loaded");
          return true;
        case QUICK_ITEM_SAVE_STATE:
          nl.saveStateSlot(fake_env, NATIVE_CLASS, g_quick_menu_slot);
          quick_menu_close();
          quick_menu_status("State saved");
          return true;
        case QUICK_ITEM_MAPPING:
          g_quick_menu_mode = QUICK_MENU_MAPPING;
          g_quick_menu_map_selection = 0;
          quick_menu_draw_mapping();
          return true;
        case QUICK_ITEM_CHEATS:
          g_quick_menu_mode = QUICK_MENU_CHEATS;
          g_quick_menu_cheat_selection = 0;
          quick_menu_draw_cheats();
          return true;
#ifdef USE_VULKAN
        case QUICK_ITEM_LSFG:
          if (!g_quick_menu_lsfg_enabled && !vk_lsfg_is_available()) {
            quick_menu_status("Frame generation is disabled for this session");
            break;
          }
          {
            const bool enabled = !g_quick_menu_lsfg_enabled;
            quick_menu_set_lsfg_enabled(enabled);
            quick_menu_status(enabled ?
                "Frame generation enabled for this session" :
                "Frame generation disabled for this session");
          }
          break;
#endif
        case QUICK_ITEM_FRAMERATE:
          g_quick_menu_mode = QUICK_MENU_FRAMERATE;
          g_quick_menu_framerate_selection = 0;
          quick_menu_draw_framerate();
          return true;
        case QUICK_ITEM_RESET:
          nl.resetVM(fake_env, NATIVE_CLASS);
          quick_menu_close();
          quick_menu_status("Console reset");
          return true;
        case QUICK_ITEM_EXIT:
          g_quick_menu_exit_requested = 1;
          quick_menu_close();
          return true;
      }
    }
    quick_menu_draw_main();
    return true;
  }

  if (g_quick_menu_mode == QUICK_MENU_FRAMERATE) {
    if (pressed & HidNpadButton_Up)
      g_quick_menu_framerate_selection =
          (g_quick_menu_framerate_selection + QUICK_FRAMERATE_COUNT - 1) %
          QUICK_FRAMERATE_COUNT;
    if (pressed & HidNpadButton_Down)
      g_quick_menu_framerate_selection =
          (g_quick_menu_framerate_selection + 1) % QUICK_FRAMERATE_COUNT;
    if (pressed & HidNpadButton_B) {
      g_quick_menu_mode = QUICK_MENU_MAIN;
      quick_menu_draw_main();
      return true;
    }
    if ((pressed & HidNpadButton_A) &&
        g_quick_menu_framerate_selection == QUICK_FRAMERATE_BACK) {
      g_quick_menu_mode = QUICK_MENU_MAIN;
      quick_menu_draw_main();
      return true;
    }

    const int direction = (pressed & HidNpadButton_Right) ? 1 :
                          (pressed & HidNpadButton_Left) ? -1 : 0;
    bool apply_settings = false;
    if (g_quick_menu_framerate_selection == QUICK_FRAMERATE_LIMITER &&
        (direction || (pressed & HidNpadButton_A))) {
      const bool unlimited = direction ? direction > 0 :
          !g_quick_menu_limiter_unlimited;
      if (unlimited != (bool)g_quick_menu_limiter_unlimited) {
        nl.toggleLimiterMode(fake_env, NATIVE_CLASS, 3);
        g_quick_menu_limiter_unlimited = unlimited;
        prefs_set_bool("EmuCore/GS/FrameLimitEnable", !unlimited);
      }
    } else if (direction &&
               g_quick_menu_framerate_selection == QUICK_FRAMERATE_NTSC) {
      g_quick_menu_ntsc_rate += (float)direction;
      if (g_quick_menu_ntsc_rate < 15.0f) g_quick_menu_ntsc_rate = 15.0f;
      if (g_quick_menu_ntsc_rate > 120.0f) g_quick_menu_ntsc_rate = 120.0f;
      prefs_set_float("EmuCore/GS/FramerateNTSC", g_quick_menu_ntsc_rate);
      apply_settings = true;
    } else if (direction &&
               g_quick_menu_framerate_selection == QUICK_FRAMERATE_PAL) {
      g_quick_menu_pal_rate += (float)direction;
      if (g_quick_menu_pal_rate < 12.0f) g_quick_menu_pal_rate = 12.0f;
      if (g_quick_menu_pal_rate > 100.0f) g_quick_menu_pal_rate = 100.0f;
      prefs_set_float("EmuCore/GS/FrameratePAL", g_quick_menu_pal_rate);
      apply_settings = true;
    } else if (direction &&
               g_quick_menu_framerate_selection == QUICK_FRAMERATE_EE_CYCLE_RATE) {
      g_quick_menu_ee_cycle_rate += direction;
      if (g_quick_menu_ee_cycle_rate < -3) g_quick_menu_ee_cycle_rate = -3;
      if (g_quick_menu_ee_cycle_rate > 3) g_quick_menu_ee_cycle_rate = 3;
      prefs_set_int("EmuCore/Speedhacks/EECycleRate", g_quick_menu_ee_cycle_rate);
      apply_settings = true;
    } else if (direction &&
               g_quick_menu_framerate_selection == QUICK_FRAMERATE_EE_CYCLE_SKIP) {
      g_quick_menu_ee_cycle_skip += direction;
      if (g_quick_menu_ee_cycle_skip < 0) g_quick_menu_ee_cycle_skip = 0;
      if (g_quick_menu_ee_cycle_skip > 3) g_quick_menu_ee_cycle_skip = 3;
      prefs_set_int("EmuCore/Speedhacks/EECycleSkip", g_quick_menu_ee_cycle_skip);
      apply_settings = true;
    }
    if (apply_settings) nl.applySettings(fake_env, NATIVE_CLASS);
    quick_menu_draw_framerate();
    return true;
  }

  if (g_quick_menu_mode == QUICK_MENU_CHEATS) {
    const int code_count = (int)g_quick_cheats.count;
    const int item_count = code_count + 1;
    if (pressed & HidNpadButton_Up)
      g_quick_menu_cheat_selection = (g_quick_menu_cheat_selection + item_count - 1) % item_count;
    if (pressed & HidNpadButton_Down)
      g_quick_menu_cheat_selection = (g_quick_menu_cheat_selection + 1) % item_count;
    if (pressed & HidNpadButton_B) {
      g_quick_menu_mode = QUICK_MENU_MAIN;
      quick_menu_draw_main();
      return true;
    }
    if ((pressed & HidNpadButton_A) && g_quick_menu_cheat_selection == code_count) {
      g_quick_menu_mode = QUICK_MENU_MAIN;
      quick_menu_draw_main();
      return true;
    }
    if (g_quick_menu_cheat_selection < code_count &&
        (pressed & (HidNpadButton_A | HidNpadButton_Left | HidNpadButton_Right))) {
      const NxCheatEntry *entry = &g_quick_cheats.entries[g_quick_menu_cheat_selection];
      bool enable = entry->enabled_count != entry->patch_count;
      if (pressed & HidNpadButton_Left) enable = false;
      if (pressed & HidNpadButton_Right) enable = true;
      const QuickCheatUpdateResult result = quick_menu_set_cheat_enabled(
          (unsigned)g_quick_menu_cheat_selection, enable);
      if (result == QUICK_CHEAT_UPDATE_FAILED) {
        quick_menu_status("Could not update the PNACH cheat file");
      } else if (result == QUICK_CHEAT_UPDATE_PROFILE_UNSAVED) {
        quick_menu_status("Code changed; per-game cheat gate could not be saved");
      } else {
        quick_menu_status(enable ? "Cheat code enabled" :
                                   "Cheat code disabled (reset may be required)");
      }
    }
    quick_menu_draw_cheats();
    return true;
  }

  const int item_count = (int)NUM_PS2_BUTTONS + 2;
  if (pressed & HidNpadButton_Up)
    g_quick_menu_map_selection = (g_quick_menu_map_selection + item_count - 1) % item_count;
  if (pressed & HidNpadButton_Down)
    g_quick_menu_map_selection = (g_quick_menu_map_selection + 1) % item_count;
  if (g_quick_menu_map_selection == 0 &&
      (pressed & (HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_A)))
    g_quick_menu_map_player = (g_quick_menu_map_player + 1) % g_controller_count;
  if (pressed & HidNpadButton_B) {
    g_quick_menu_mode = QUICK_MENU_MAIN;
    quick_menu_draw_main();
    return true;
  }
  if ((pressed & HidNpadButton_Y) && g_quick_menu_map_selection > 0 &&
      g_quick_menu_map_selection <= (int)NUM_PS2_BUTTONS) {
    const bool saved = quick_menu_store_binding("None");
    quick_menu_status(saved ? "Controller binding cleared" :
                                "Binding cleared, but launcher.ini could not be saved");
  }
  if (pressed & HidNpadButton_A) {
    if (g_quick_menu_map_selection > 0 &&
        g_quick_menu_map_selection <= (int)NUM_PS2_BUTTONS) {
      g_quick_menu_mode = QUICK_MENU_CAPTURE;
      g_quick_menu_capture_armed = 0;
      quick_menu_draw_capture();
      return true;
    }
    if (g_quick_menu_map_selection == item_count - 1) {
      g_quick_menu_mode = QUICK_MENU_MAIN;
      quick_menu_draw_main();
      return true;
    }
  }
  quick_menu_draw_mapping();
  return true;
}

static void turbo_set_active(int active) {
  active = active != 0;
  if (active == g_turbo_active) return;
  // Limiter mode 1 is Turbo; requesting it again returns the core to Nominal.
  nl.toggleLimiterMode(fake_env, NATIVE_CLASS, 1);
  g_turbo_active = active;
}

static void update_gamepads(void) {
  quick_menu_persist_game_crc();
  for (int player = 0; player < g_controller_count; player++)
    padUpdate(&g_pads[player]);
  const u64 menu_down = padGetButtons(&g_pads[0]);
  const u64 menu_pressed = menu_down & ~g_pad_previous[0];
  if (quick_menu_update(menu_down, menu_pressed)) {
    turbo_set_active(0);
    g_turbo_blocked = 1;
    for (int player = 0; player < g_controller_count; player++)
      g_pad_previous[player] = padGetButtons(&g_pads[player]);
    return;
  }
  int turbo_down = g_turbo_combo && (menu_down & g_turbo_combo) == g_turbo_combo;
  if (g_turbo_blocked) {
    if (!turbo_down) g_turbo_blocked = 0;
    turbo_down = 0;
  }
  turbo_set_active(turbo_down);
  for (int player = 0; player < g_controller_count; player++) {
    PadState *pad = &g_pads[player];
    PadConfig *config = &g_pad_config[player];
    const u64 down = padGetButtons(pad);
    const u64 changed = down ^ g_pad_previous[player];
    if (nl.setPadValue) {
      for (int i = 0; i < config->bind_count; i++) {
        if (!(changed & config->binds[i].hid)) continue;
        nl.setPadValue(fake_env, NATIVE_CLASS, player, config->binds[i].bind,
                       (down & config->binds[i].hid) ? 1.0f : 0.0f);
      }
      const HidAnalogStickState ls = padGetStickPos(pad, 0);
      const HidAnalogStickState rs = padGetStickPos(pad, 1);
      apply_ps2_stick(player, config, &config->left_stick, ls, rs,
                      PB_LLEFT, PB_LRIGHT, PB_LDOWN, PB_LUP);
      apply_ps2_stick(player, config, &config->right_stick, ls, rs,
                      PB_RLEFT, PB_RRIGHT, PB_RDOWN, PB_RUP);
    } else {
      for (unsigned i = 0; i < sizeof(pad_map) / sizeof(*pad_map); i++) {
        if (changed & pad_map[i].hid)
          nl.handleControllerButtonEvent(fake_env, NATIVE_CLASS, player, pad_map[i].keycode,
                                         (down & pad_map[i].hid) ? 1 : 0);
      }
      const float scale = 1.f / 32767.0f;
      const HidAnalogStickState ls = padGetStickPos(pad, 0);
      const HidAnalogStickState rs = padGetStickPos(pad, 1);
      nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, player, AAXIS_X,  (float)ls.x *  scale);
      nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, player, AAXIS_Y,  (float)ls.y * -scale);
      nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, player, AAXIS_Z,  (float)rs.x *  scale);
      nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, player, AAXIS_RZ, (float)rs.y * -scale);
      nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, player, AAXIS_LTRIGGER, (down & HidNpadButton_ZL) ? 1.f : 0.f);
      nl.handleControllerAxisEvent(fake_env, NATIVE_CLASS, player, AAXIS_RTRIGGER, (down & HidNpadButton_ZR) ? 1.f : 0.f);
    }
    g_pad_previous[player] = down;
  }
}

#define MAX_TOUCHES 8
typedef struct { int active; u32 finger_id; float x, y; } TouchSlot;
static TouchSlot touch_prev[MAX_TOUCHES];

static int touch_slot_find(u32 id) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (touch_prev[i].active && touch_prev[i].finger_id == id) return i;
  return -1;
}
static int touch_slot_alloc(void) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (!touch_prev[i].active) return i;
  return -1;
}

static void update_touch(void) {
  HidTouchScreenState state = { 0 };
  if (!hidGetTouchScreenStates(&state, 1))
    return;
  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;
  int seen[MAX_TOUCHES] = { 0 };
  for (int i = 0; i < state.count; i++) {
    const HidTouchState *t = &state.touches[i];
    const float x = (float)t->x * sx;
    const float y = (float)t->y * sy;
    int slot = touch_slot_find(t->finger_id);
    if (slot < 0) {
      slot = touch_slot_alloc();
      if (slot < 0) continue;
      touch_prev[slot].active = 1;
      touch_prev[slot].finger_id = t->finger_id;
    }
    nl.handlePointerEvent(fake_env, NATIVE_CLASS, slot, x, y);
    touch_prev[slot].x = x; touch_prev[slot].y = y;
    seen[slot] = 1;
  }
  for (int slot = 0; slot < MAX_TOUCHES; slot++) {
    if (touch_prev[slot].active && !seen[slot]) {
      // pointer up: send an off-screen position (the core releases on no-move)
      nl.handlePointerEvent(fake_env, NATIVE_CLASS, slot, -1.0f, -1.0f);
      touch_prev[slot].active = 0;
    }
  }
}

int main(void) {
  extern int crash_in_progress(void);
  cpu_boost(1);

  // settings store: load nethersx2.ini + seed OpenGL/folder defaults
  prefs_init(PREFS_PATH);
  fastmem_set_mode(!strcmp(prefs_get_string("Wrapper/FastmemMode", "hybrid"), "hybrid") ?
                       FASTMEM_MODE_ON : FASTMEM_MODE_OFF);
  pad_load_bindings();
  snprintf(g_disc_path, sizeof(g_disc_path), "%s",
           prefs_get_string("EmuCore/DiscPath", DEFAULT_DISC_PATH));
  char storage_error[256];
  if (!switchStorageInitializeForPath(DATA_ROOT "/launcher.ini", g_disc_path, sizeof(g_disc_path),
                                      storage_error, sizeof(storage_error)))
    fatal_error("Could not mount game storage:\n%s\n\n%s", g_disc_path, storage_error);
  prefs_set_disc_path(g_disc_path);
  if (switchStorageSocketReady())
    g_net_ready = 1;

  snprintf(g_core_so, sizeof(g_core_so), "%s",
           prefs_get_string("Wrapper/CoreSo", SO_NAME));

  check_syscalls();
  check_data(g_core_so, g_disc_path);
  set_screen_size(0, 0);

  extern char *fake_heap_start;
  const unsigned heap_mb =
      (unsigned)(((char *)heap_so_base - fake_heap_start) / (1024 * 1024));
  // Applet mode does not provide enough memory for the emulator.
  if (heap_mb < 1500)
    fatal_error("Not enough memory (%u MB).\n\n"
                "Launch hbmenu over a game (hold R while\n"
                "starting any installed title), then start\n"
                "NetherSX2 from there.", heap_mb);

  if (so_load(&emu_mod, g_core_so, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", g_core_so);

  update_imports();
  so_relocate(&emu_mod);
  so_resolve(&emu_mod, dynlib_functions, dynlib_numfunctions, 1);

  patch_game();
  resolve_entry_points();

  so_finalize(&emu_mod);
  so_flush_caches(&emu_mod);

  // Core constructors read the stack guard through TLS.
  pthr_install_fake_tls();

  so_execute_init_array(&emu_mod);
  so_free_temp(&emu_mod);

  jni_init();
  NATIVE_CLASS = jni_obj_new("xyz/aethersx2/android/NativeLibrary");
  FAKE_CONTEXT = jni_obj_new("android/content/Context");
  FAKE_SURFACE = jni_obj_new("android/view/Surface");
  // Keep both Horizon player slots advertised even when only one PS2 port is
  // enabled. Advertising g_controller_count here made HOS power off player 2
  // as soon as the game runtime replaced the launcher's input configuration.
  padConfigureInput(MAX_CONTROLLERS, HidNpadStyleSet_NpadStandard);
  padInitialize(&g_pads[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
  padInitialize(&g_pads[1], HidNpadIdType_No2);
  hidInitializeTouchScreen();
  run_startup_sequence();
  rumble_init();

#if GS_RENDERER == GS_RENDERER_VK
  extern volatile int vk_present_count;
#define FRAME_COUNT vk_present_count
#else
  extern volatile int egl_swap_count;
#define FRAME_COUNT egl_swap_count
#endif
  u64 ticks = 0;
  int boosting = 1;
  int quick_menu_hint_shown = 0;
  const int cpu_boost_present_limit = 60;

  pthr_pin_bg_core();

  while (appletMainLoop() && !g_quick_menu_exit_requested) {
    if (crash_in_progress())
      for (;;) svcSleepThread(1000000000ULL);
    ++ticks;
    const int frame_count = FRAME_COUNT;
    if (frame_count > 0)
      g_quick_menu_ready = 1;
    update_gamepads();
    update_touch();
    svcSleepThread(1000000000ull / 120); // ~120 Hz input polling
    if (boosting && FRAME_COUNT >= cpu_boost_present_limit) {
      // One second at 60 FPS (two seconds for a duplicate-skipped 30 FPS game)
      // is enough to cover core and renderer startup without holding FastLoad.
      cpu_boost(0);
      boosting = 0;
    }
    if (!quick_menu_hint_shown && FRAME_COUNT >= 300) {
      quick_menu_status("Quick menu: L + R + Plus");
      quick_menu_hint_shown = 1;
    }

    if (!g_vm_running && ticks > 240)       // VM thread exited -> shut down
      break;
  }

  if (g_quick_menu_mode != QUICK_MENU_CLOSED)
    quick_menu_close();
  turbo_set_active(0);

  const int has_next_load = envHasNextLoad();
  const char *launcher_path = prefs_get_string("Wrapper/LauncherPath", "");
  if (g_quick_menu_exit_requested && has_next_load) {
    if (launcher_path[0])
      envSetNextLoad(launcher_path, launcher_path);
  }

  // The boot-time stop filter must be disabled during shutdown.
  { extern volatile int g_allow_stop; g_allow_stop = 1; }
  nl.stopVMThreadLoop(fake_env, NATIVE_CLASS, 1);
  pthread_join(emu_thread, NULL);
  nl.waitForSaveStateFlush(fake_env, NATIVE_CLASS);
  if (nl.JNI_OnUnload)
    nl.JNI_OnUnload(fake_vm, NULL);
  core_shutdown_mtgs();
  libc_finalize_core();
  pthr_shutdown();
  libc_memory_shutdown();
  so_unload(&emu_mod);
  prefs_save();
  const bool storage_socket = switchStorageSocketReady();
  switchStorageShutdown();
  if (g_net_ready && !storage_socket)
    socketExit();
  g_net_ready = 0;
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
