/* config.h -- global configuration
 *
 * NetherSX2_nx -- NetherSX2 (AetherSX2 / PCSX2-lineage PS2 emulator) on the
 * Nintendo Switch via a libnx so-loader.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Fixed load region for the .so image. libemucore's PT_LOAD spans ~199 MB (incl.
// a ~187 MB BSS), so this must exceed that; the rest of the Horizon heap becomes
// the newlib heap. Recompiler RX caches + PS2 RAM mirrors are mapped separately.
#define SO_REGION_MB 256

// RAM left unmapped by our newlib heap so the GPU can use it: NVK allocates
// outside the process heap and reports free-system-memory as its Vulkan budget,
// so claiming every byte starved the GPU and VMA then refused all allocations.
#define GPU_RESERVE_MB 1024

// libemucore.so is self-contained (statically linked NDK r25b C++ runtime).
#define SO_NAME "libemucore.so"


// On-SD layout. Paths are plain absolute SD paths (no "sdmc:" prefix): the core's
// PCSX2 FileSystem treats a colon as a scheme, and libnx resolves "/..." to the SD
// root, keeping the core off its Java FileHelper (SAF) bridge.
#define DATA_ROOT      "/switch/nethersx2"
#define RESOURCES_DIR  DATA_ROOT "/resources"
#define CHEATS_DIR     DATA_ROOT "/cheats"
#define BIOS_DIR       DATA_ROOT "/bios"
#define CACHE_DIR      DATA_ROOT "/cache"
#define PREFS_NAME     "nethersx2.ini"
#define PREFS_PATH     DATA_ROOT "/" PREFS_NAME

// Default boot disc if prefs (EmuCore/DiscPath) don't override it.
#define DEFAULT_DISC_PATH DATA_ROOT "/game.iso"

// Android identity handed to the core via the fake JNI env.
#define ANDROID_MANUFACTURER "Nintendo"
#define ANDROID_MODEL "Switch"
#define ANDROID_DEVICE_NAME "Nintendo Switch"
#define ANDROID_SDK_INT 30   // Android 11: gates AAudio + SAF behaviour

// GSRendererType (PCSX2): OGL=12, VK=14. GL and Vulkan are mutually exclusive at
// build time (both bundle mesa object code). The Makefile picks one: `make` =
// OpenGL, `make RENDERER=VK` = Vulkan (NVK), which also sets -DGS_RENDERER=14.
#define GS_RENDERER_OGL 12
#define GS_RENDERER_VK  14
#ifndef GS_RENDERER
#define GS_RENDERER GS_RENDERER_OGL
#endif

// actual render/surface size (picked at runtime from docked state; see main.c)
extern int screen_width;
extern int panel_width;
extern int panel_height;
extern int screen_height;

#endif
