#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include <map>
#include <unordered_map>
#include <iterator>
#include <array>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <dirent.h>

#include "griddb.h"
#include "forwarder.h"
#include "SwitchStorage.h"
#include "CheatManager.h"
#include "ui_audio.h"
#include "retroachievements.h"
#include "launcher_update.h"

// SDL uses Xbox button names.
#define BTN_CONFIRM  SDL_CONTROLLER_BUTTON_B
#define BTN_CANCEL   SDL_CONTROLLER_BUTTON_A
#define BTN_SETTINGS SDL_CONTROLLER_BUTTON_Y

static const char *DATA_DIR    = "sdmc:/switch/nethersx2";
static const char *LAUNCHER_INI= "sdmc:/switch/nethersx2/launcher.ini";
static const char *EMU_INI     = "sdmc:/switch/nethersx2/nethersx2.ini";
static const char *COVERS_DIR = "sdmc:/switch/nethersx2/covers";
static const char *CORES_DIR  = "sdmc:/switch/nethersx2/cores";
static const char *GAMECFG_DIR= "sdmc:/switch/nethersx2/gamecfg";
static const char *GAMECRC_DIR= "sdmc:/switch/nethersx2/gamecrc";
static const char *CHEATS_DIR = "sdmc:/switch/nethersx2/cheats";
static const char *DEF_GAMEDIR= "sdmc:/switch/nethersx2/games";
static const char *BIOS_DIR   = "sdmc:/switch/nethersx2/bios";
static const char *RESOURCES_DIR = "sdmc:/switch/nethersx2/resources";
static const char *LSFG_DIR = "sdmc:/switch/nethersx2/lsfg";
static const char *EMU_HOST_DIR = "sdmc:/switch/nethersx2/.emu";
static const char *LSFG_DLL_FILE = "sdmc:/switch/nethersx2/lsfg/Lossless.dll";
static const char *LAUNCHER_NRO = "sdmc:/switch/NetherSX2.nro";
static std::string g_launcherNroPath = LAUNCHER_NRO;

struct KV { std::string k, v; };
struct Store { std::vector<KV> kv; };

static Store g_global;
static Store g_game;
static Store g_titles;
static Store *g_active = &g_global;
static const char *TITLES_INI = "sdmc:/switch/nethersx2/titles.ini";

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
static const char *storeGet(Store &s, const char *key, const char *def) {
  for (auto &e : s.kv) if (e.k == key) return e.v.c_str();
  return def;
}
static void storeSet(Store &s, const char *key, const char *val) {
  for (auto &e : s.kv) if (e.k == key) { e.v = val; return; }
  s.kv.push_back({ key, val });
}
static void storeRemove(Store &s, const char *key) {
  for (size_t i = 0; i < s.kv.size(); i++) if (s.kv[i].k == key) { s.kv.erase(s.kv.begin()+i); return; }
}
static bool storeHas(const Store &s, const char *key) {
  for (const auto &e : s.kv) if (e.k == key) return true;
  return false;
}
static void storeRemovePrefix(Store &s, const char *prefix) {
  const size_t length = strlen(prefix);
  s.kv.erase(std::remove_if(s.kv.begin(), s.kv.end(), [&](const KV &entry) {
    return entry.k.compare(0, length, prefix) == 0;
  }), s.kv.end());
}
static bool recoverAtomicFile(const std::string &path);
static void storeLoad(Store &s, const char *path) {
  s.kv.clear();
  if (!recoverAtomicFile(path)) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[2048];
  while (fgets(line, sizeof(line), f)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
    if (!k.empty()) s.kv.push_back({ k, v });
  }
  fclose(f);
}

static bool queryRegularFile(const std::string &path, bool &exists) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    exists = true;
    return S_ISREG(st.st_mode);
  }
  exists = false;
  return errno == ENOENT;
}

static bool regularFileExists(const std::string &path) {
  bool exists = false;
  return queryRegularFile(path, exists) && exists;
}

static bool lsfgDllInstalled() {
  return regularFileExists(LSFG_DLL_FILE);
}

static bool normalizeLsfgStore(Store &store) {
  bool changed = false;
  const bool installed = lsfgDllInstalled();
  if (storeHas(store, "Wrapper/LSFGDllPath")) {
    storeRemove(store, "Wrapper/LSFGDllPath");
    changed = true;
  }
  if (storeHas(store, "Wrapper/LSFGFlowScale")) {
    const double flow = std::strtod(
        storeGet(store, "Wrapper/LSFGFlowScale", "0.25"), nullptr);
    const char *normalized = flow > 0.375 ? "0.5" : "0.25";
    if (strcmp(storeGet(store, "Wrapper/LSFGFlowScale", "0.25"), normalized)) {
      storeSet(store, "Wrapper/LSFGFlowScale", normalized);
      changed = true;
    }
  }
  if (!installed &&
      storeHas(store, "Wrapper/LSFGEnabled") &&
      !strcmp(storeGet(store, "Wrapper/LSFGEnabled", "false"), "true")) {
    storeSet(store, "Wrapper/LSFGEnabled", "false");
    changed = true;
  }
  if (installed &&
      !strcmp(storeGet(store, "Wrapper/LSFGEnabled", "false"), "true") &&
      strcmp(storeGet(store, "EmuCore/GS/SkipDuplicateFrames", "false"), "true")) {
    // Frame generation must consume unique game frames. Feeding it the core's
    // repeated 30 -> 60 FPS presents makes LSFG submit 120 FIFO frames to a
    // 60 Hz display and stalls emulation at roughly half speed.
    storeSet(store, "EmuCore/GS/SkipDuplicateFrames", "true");
    changed = true;
  }
  return changed;
}

static bool removeLegacySmcSettings(Store &store) {
  bool changed = false;
  if (storeHas(store, "Wrapper/EESmcCheck")) {
    storeRemove(store, "Wrapper/EESmcCheck");
    changed = true;
  }
  if (storeHas(store, "Wrapper/EESmcMode")) {
    storeRemove(store, "Wrapper/EESmcMode");
    changed = true;
  }
  return changed;
}

static bool removeLegacyCheatGate(Store &store) {
  if (!storeHas(store, "EmuCore/EnableCheats")) return false;
  storeRemove(store, "EmuCore/EnableCheats");
  return true;
}

static bool setStoreDefault(Store &store,const char *key,const char *value) {
  if(storeHas(store,key)) return false;
  storeSet(store,key,value);
  return true;
}

static bool setStoreValue(Store &store,const char *key,const char *value) {
  if(!strcmp(storeGet(store,key,""),value)&&storeHas(store,key)) return false;
  storeSet(store,key,value);
  return true;
}

static bool normalizeRetroAchievementsStore(Store &store) {
  bool changed=false;
  changed|=setStoreDefault(store,"Achievements/Enabled","false");
  changed|=setStoreDefault(store,"Achievements/Username","");
  changed|=setStoreDefault(store,"Achievements/Token","");
  changed|=setStoreDefault(store,"Achievements/RichPresence","true");
  changed|=setStoreDefault(store,"Achievements/Notifications","true");
  changed|=setStoreDefault(store,"Achievements/SoundEffects","true");
  changed|=setStoreDefault(store,"Achievements/PrimedIndicators","true");
  changed|=setStoreDefault(store,"Achievements/NotificationsDuration","5");
  // NetherSX2-nx intentionally implements Casual achievements only.
  changed|=setStoreValue(store,"Achievements/ChallengeMode","false");
  changed|=setStoreValue(store,"Achievements/Leaderboards","false");
  changed|=setStoreValue(store,"Achievements/TestMode","false");
  changed|=setStoreValue(store,"Achievements/UnofficialTestMode","false");
  if((!storeGet(store,"Achievements/Username","")[0]||
      !storeGet(store,"Achievements/Token","")[0])&&
      !strcmp(storeGet(store,"Achievements/Enabled","false"),"true"))
    changed|=setStoreValue(store,"Achievements/Enabled","false");
  return changed;
}

static void applyGlobalRetroAchievementsSettings(Store &effective) {
  storeRemovePrefix(effective,"Achievements/");
  for(const auto &entry:g_global.kv)
    if(entry.k.compare(0,13,"Achievements/")==0)
      storeSet(effective,entry.k.c_str(),entry.v.c_str());
  normalizeRetroAchievementsStore(effective);
}

static bool recoverAtomicFile(const std::string &path) {
  const std::string tmp = path + ".tmp";
  const std::string old = path + ".old";
  bool currentExists = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, currentExists) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists)) return false;
  if (!currentExists && oldExists) {
    if (rename(old.c_str(), path.c_str()) != 0) return false;
    fsdevCommitDevice("sdmc");
    currentExists = true;
    oldExists = false;
  }
  if (tmpExists && remove(tmp.c_str()) != 0) return false;
  if (currentExists && oldExists && remove(old.c_str()) != 0) return false;
  if (tmpExists || oldExists) fsdevCommitDevice("sdmc");
  return true;
}

static bool replaceAtomic(const std::string &path, const std::string &tmp) {
  const std::string old = path + ".old";
  bool hadCurrent = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, hadCurrent) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists) || !tmpExists) return false;
  if (oldExists && remove(old.c_str()) != 0) return false;
  if (hadCurrent && rename(path.c_str(), old.c_str()) != 0) return false;
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    if (hadCurrent) {
      rename(old.c_str(), path.c_str());
      fsdevCommitDevice("sdmc");
    }
    return false;
  }
  fsdevCommitDevice("sdmc");
  if (hadCurrent && remove(old.c_str()) == 0) fsdevCommitDevice("sdmc");
  return true;
}

static bool writeAtomicText(const std::string &path, const std::string &text) {
  const std::string tmp = path + ".tmp";
  if (!recoverAtomicFile(path)) return false;
  FILE *file = fopen(tmp.c_str(), "wb");
  if (!file) return false;
  bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
  if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
  if (fclose(file) != 0) ok = false;
  if (!ok) { remove(tmp.c_str()); return false; }
  if (!replaceAtomic(path, tmp)) { remove(tmp.c_str()); return false; }
  return true;
}

static bool storeSave(Store &s, const char *path) {
  mkdir(DATA_DIR, 0777);
  std::string text = "# NetherSX2 launcher\n";
  for (auto &e : s.kv) text += e.k + " = " + e.v + "\n";
  return writeAtomicText(path, text);
}

static bool syncRetroAchievementsToEmulatorConfig() {
  // Keep an existing launch configuration in sync, including sign-out.
  if (!regularFileExists(EMU_INI)) return true;
  Store emulatorConfig;
  storeLoad(emulatorConfig,EMU_INI);
  applyGlobalRetroAchievementsSettings(emulatorConfig);
  return storeSave(emulatorConfig,EMU_INI);
}

static const char *iniGet(const char *key, const char *def) {
  if (g_active == &g_game) {
    for (auto &e : g_game.kv)   if (e.k == key) return e.v.c_str();
    for (auto &e : g_global.kv) if (e.k == key) return e.v.c_str();
    return def;
  }
  return storeGet(*g_active, key, def);
}
static void iniSet(const char *key, const char *val) { storeSet(*g_active, key, val); }

enum OType { OT_CHOICE, OT_RANGE, OT_SCALED_RANGE, OT_SUBMENU, OT_TEXT, OT_HOTKEY, OT_STATUS };
struct Choice { const char *label, *val; };
struct Opt {
  const char *label;
  const char *key;
  OType type;
  const Choice *ch; int nch;
  int lo, hi, step;
  const char *def;
  int sub;
  const char *gateKey;
  const char *gateOff;
  int multiplier;
  const char *suffix;
};
#define O_CHOICE(l,k,c,d)      { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, nullptr, nullptr, 1, nullptr }
#define O_RANGE(l,k,lo,hi,s,d) { l, k, OT_RANGE,  nullptr,0, lo,hi,s, d, 0, nullptr, nullptr, 1, nullptr }
#define O_SCALED_RANGE(l,k,lo,hi,s,d,m,u) { l, k, OT_SCALED_RANGE, nullptr,0, lo,hi,s, d, 0, nullptr, nullptr, m, u }
#define O_SCALED_RANGEG(l,k,lo,hi,s,d,m,u,gk,go) { l, k, OT_SCALED_RANGE, nullptr,0, lo,hi,s, d, 0, gk, go, m, u }
#define O_SUB(l,scr)           { l, nullptr, OT_SUBMENU, nullptr,0, 0,0,0, nullptr, scr, nullptr, nullptr, 1, nullptr }
#define O_CHOICEG(l,k,c,d,gk,go) { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, gk, go, 1, nullptr }
#define O_RANGEG(l,k,lo,hi,s,d,gk,go) { l, k, OT_RANGE, nullptr,0, lo,hi,s, d, 0, gk, go, 1, nullptr }
#define O_TEXT(l,k,d)          { l, k, OT_TEXT, nullptr,0, 0,0,0, d, 0, nullptr, nullptr, 1, nullptr }
#define O_TEXTG(l,k,d,gk,go)   { l, k, OT_TEXT, nullptr,0, 0,0,0, d, 0, gk, go, 1, nullptr }
#define O_HOTKEY(l,k,d)        { l, k, OT_HOTKEY, nullptr,0, 0,0,0, d, 0, nullptr, nullptr, 1, nullptr }
#define O_STATUS(l)            { l, nullptr, OT_STATUS, nullptr,0, 0,0,0, nullptr, 0, nullptr, nullptr, 1, nullptr }

static const Choice C_backend[]  = { {"Vulkan (NVK)","14"}, {"OpenGL","12"} };
static const Choice C_build[]    = { {"Patched (4248)","4248"}, {"Classic (3668)","3668"} };
static const Choice C_fastmem[]  = { {"Off","off"}, {"On","hybrid"} };
static const Choice C_upscale[]  = { {"0.25x","0.25"},{"0.5x","0.5"},{"0.75x","0.75"},
                                     {"1x (native ~480p)","1"},{"1.25x","1.25"},{"1.5x","1.5"},{"1.75x","1.75"},
                                     {"2x (~720p)","2"},{"2.25x","2.25"},{"2.5x","2.5"},{"2.75x","2.75"},{"3x (~1080p)","3"},
                                     {"4x (~1440p)","4"},{"5x (~1800p)","5"},{"6x (4K ~2160p)","6"} };
static const Choice C_bool[]     = { {"Off","false"}, {"On","true"} };
static const Choice C_lsfgFlow[] = { {"Quarter (recommended)","0.25"}, {"Half","0.5"} };
static const Choice C_aspect[]   = { {"4:3","4:3"}, {"16:9","16:9"}, {"Stretch","Stretch"}, {"Auto","Auto 4:3/3:2"} };
static const Choice C_fmvasp[]   = { {"Off","Off"}, {"4:3","4:3"}, {"16:9","16:9"} };
static const Choice C_vsync[]    = { {"Off","0"}, {"On","1"}, {"Adaptive","2"} };
static const Choice C_filter[]   = { {"Nearest","0"}, {"Bilinear (PS2)","2"}, {"Bilinear (Forced)","1"}, {"Bilinear no-sprite","3"} };
static const Choice C_aniso[]    = { {"Off","0"}, {"2x","2"}, {"4x","4"}, {"8x","8"}, {"16x","16"} };
static const Choice C_tvshader[] = { {"None","0"}, {"Scanline","1"}, {"Diagonal","2"}, {"Triangular","3"}, {"Wave","4"}, {"Lottes CRT","5"} };
static const Choice C_blend[]    = { {"Minimum","0"}, {"Basic","1"}, {"Medium","2"}, {"High","3"}, {"Full","4"}, {"Maximum","5"} };
static const Choice C_deint[]    = { {"Auto","0"}, {"None","1"}, {"Weave TFF","2"}, {"Weave BFF","3"}, {"Bob TFF","4"}, {"Bob BFF","5"}, {"Blend TFF","6"}, {"Blend BFF","7"}, {"Adaptive TFF","8"}, {"Adaptive BFF","9"} };
static const Choice C_dither[]   = { {"Off","0"}, {"Scaled","1"}, {"Unscaled","2"} };
static const Choice C_trifilter[]= { {"Automatic","-1"}, {"Off","0"}, {"Trilinear (PS2)","1"}, {"Trilinear (Forced)","2"} };
static const Choice C_crc[]      = { {"Auto","-1"}, {"None","0"}, {"Minimum","1"}, {"Partial","2"}, {"Full","3"}, {"Aggressive","4"} };
static const Choice C_preload[]  = { {"None","0"}, {"Partial","1"}, {"Full","2"} };
static const Choice C_cas[]      = { {"Off","0"}, {"Sharpen only","1"}, {"Sharpen + upscale","2"} };
static const Choice C_hwdl[]     = { {"Accurate","0"}, {"Disable readbacks","1"}, {"Unsynchronized","2"}, {"Disabled","3"} };
static const Choice C_interp[]   = { {"Nearest","0"}, {"Linear","1"}, {"Cubic","2"}, {"Hermite","3"}, {"Catmull-Rom","4"} };
static const Choice C_sync[]     = { {"TimeStretch","0"}, {"Async","1"}, {"None","2"} };
static const Choice C_eecr[]     = { {"50%","-3"}, {"60%","-2"}, {"75%","-1"}, {"100%","0"}, {"130%","1"}, {"180%","2"}, {"300%","3"} };
static const Choice C_eecs[]     = { {"Off","0"}, {"Mild","1"}, {"Moderate","2"}, {"Maximum","3"} };
static const Choice C_rounding[] = { {"Nearest","0"}, {"Negative","1"}, {"Positive","2"}, {"Chop / Zero","3"} };
static const Choice C_eeClamp[]  = { {"None","0"}, {"Normal","1"}, {"Extra + Preserve Sign","2"}, {"Full","3"} };
static const Choice C_vuClamp[]  = { {"None","0"}, {"Normal","1"}, {"Extra","2"}, {"Extra + Preserve Sign","3"} };
static const Choice C_syslang[]  = { {"Auto","auto"}, {"English","1"}, {"Japanese","0"},
                                     {"French","2"}, {"Spanish","3"}, {"German","4"}, {"Italian","5"},
                                     {"Dutch","6"}, {"Portuguese","7"}, {"Don't change","off"} };
static const Choice C_btn[]      = { {"A","A"},{"B","B"},{"X","X"},{"Y","Y"},{"L","L"},{"R","R"},{"ZL","ZL"},{"ZR","ZR"},
                                     {"Plus","Plus"},{"Minus","Minus"},{"L-Stick","StickL"},{"R-Stick","StickR"},
                                     {"D-Up","Up"},{"D-Down","Down"},{"D-Left","Left"},{"D-Right","Right"},{"None","None"} };
static const Choice C_stick[]    = { {"Left Stick","LStick"}, {"Right Stick","RStick"}, {"None","None"} };
static const Choice C_players[]  = { {"1","1"}, {"2","2"} };
static const Choice C_launcherTheme[] = { {"XMB (PS3)","xmb"}, {"Glow","animated"}, {"Bubbles","homebrew"},
                                          {"Classic","classic"}, {"OLED black","oled"} };
static const Choice C_gridColumns[] = { {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {"8","8"} };
static const Choice C_gridRows[] = { {"1","1"}, {"2","2"}, {"3","3"} };

enum { SCR_GRAPHICS, SCR_ENHANCE, SCR_FRAMEGEN, SCR_AUDIO, SCR_EMU, SCR_ADVANCED, SCR_GAMEFIXES, SCR_FRAMERATE, SCR_NETWORK, SCR_CONTROLLER, SCR_COUNT };

static const Opt S_graphics[] = {
  O_CHOICE("Renderer",         "EmuCore/GS/Renderer", C_backend, "14"),
  O_CHOICE("Resolution scale", "EmuCore/GS/upscale_multiplier", C_upscale, "1"),
  O_CHOICE("Aspect ratio",     "EmuCore/GS/AspectRatio", C_aspect, "4:3"),
  O_CHOICE("FMV aspect",       "EmuCore/GS/FMVAspectRatioSwitch", C_fmvasp, "Off"),
  O_CHOICE("VSync",            "EmuCore/GS/VsyncEnable", C_vsync, "0"),
  O_CHOICE("Skip duplicate frames", "EmuCore/GS/SkipDuplicateFrames", C_bool, "false"),
  O_CHOICE("Disable threaded presentation", "EmuCore/GS/DisableThreadedPresentation", C_bool, "false"),
  O_CHOICE("Texture filtering","EmuCore/GS/filter", C_filter, "2"),
  O_CHOICE("Anisotropic",      "EmuCore/GS/MaxAnisotropy", C_aniso, "0"),
  O_CHOICE("Show FPS",         "EmuCore/GS/OsdShowFPS", C_bool, "false"),
  O_CHOICE("On-screen messages","EmuCore/GS/OsdShowMessages", C_bool, "true"),
  O_CHOICE("Widescreen patch", "EmuCore/EnableWideScreenPatches", C_bool, "false"),
  O_CHOICE("No-interlace patch","EmuCore/EnableNoInterlacingPatches", C_bool, "false"),
  O_SUB   ("Enhancements...",  SCR_ENHANCE),
};
static const Opt S_enhance[] = {
  O_CHOICE("Blending accuracy","EmuCore/GS/accurate_blending_unit", C_blend, "1"),
  O_CHOICE("Deinterlacing",    "EmuCore/GS/deinterlace_mode", C_deint, "0"),
  O_CHOICE("Dithering",        "EmuCore/GS/dithering_ps2", C_dither, "1"),
  O_CHOICE("Trilinear",        "EmuCore/GS/UserHacks_TriFilter", C_trifilter, "-1"),
  O_CHOICE("Mipmapping",       "EmuCore/GS/mipmap_hw", C_bool, "true"),
  O_CHOICE("CRC hack level",   "EmuCore/GS/CRCHackLevel", C_crc, "-1"),
  O_CHOICE("Texture preload",  "EmuCore/GS/texture_preloading", C_preload, "2"),
  O_CHOICE("GPU palette conv", "EmuCore/GS/paltex", C_bool, "false"),
  O_CHOICE("Anti-blur",        "EmuCore/GS/pcrtc_antiblur", C_bool, "true"),
  O_CHOICE("CRT/TV shader",    "EmuCore/GS/TVShader", C_tvshader, "0"),
  O_CHOICE("CAS sharpening",   "EmuCore/GS/CASMode", C_cas, "0"),
  O_RANGEG("CAS strength",     "EmuCore/GS/CASSharpness", 0, 100, 5, "50", "EmuCore/GS/CASMode", "0"),
  O_CHOICE("Shade boost",      "EmuCore/GS/ShadeBoost", C_bool, "false"),
  O_RANGEG("  Brightness",     "EmuCore/GS/ShadeBoost_Brightness", 0, 100, 5, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("  Contrast",       "EmuCore/GS/ShadeBoost_Contrast", 0, 100, 5, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_RANGEG("  Saturation",     "EmuCore/GS/ShadeBoost_Saturation", 0, 100, 5, "50", "EmuCore/GS/ShadeBoost", "false"),
  O_CHOICE("Texture replace",  "EmuCore/GS/LoadTextureReplacements", C_bool, "false"),
  O_CHOICE("SW renderer FMV",  "EmuCore/GS/SoftwareRendererFMV", C_bool, "false"),
  O_CHOICE("HW download mode", "EmuCore/GS/HWDownloadMode", C_hwdl, "0"),
};
static const Opt S_framegen[] = {
  O_CHOICE("LSFG 2x (Vulkan only)", "Wrapper/LSFGEnabled", C_bool, "false"),
  O_CHOICEG("Flow resolution", "Wrapper/LSFGFlowScale", C_lsfgFlow, "0.25", "Wrapper/LSFGEnabled", "false"),
  O_CHOICEG("Performance mode", "Wrapper/LSFGPerformance", C_bool, "true", "Wrapper/LSFGEnabled", "false"),
  O_STATUS("Lossless.dll"),
};
static const Opt S_audio[] = {
  O_RANGE ("Volume",           "SPU2/Mixing/FinalVolume", 0, 100, 5, "100"),
  O_CHOICE("Interpolation",    "SPU2/Mixing/Interpolation", C_interp, "4"),
  O_CHOICE("Sync mode",        "SPU2/Output/SynchMode", C_sync, "0"),
  O_RANGE ("Buffer latency",   "SPU2/Output/Latency", 15, 200, 5, "60"),
  O_RANGE ("Output latency",   "SPU2/Output/OutputLatency", 20, 200, 10, "100"),
};
static const Opt S_emu[] = {
  O_CHOICE("Core version",     "Wrapper/CoreBuild", C_build, "4248"),
  O_CHOICE("Fastmem",          "Wrapper/FastmemMode", C_fastmem, "hybrid"),
  O_CHOICE("System language",  "Wrapper/SystemLanguage", C_syslang, "auto"),
  O_CHOICE("EE cycle rate",    "EmuCore/Speedhacks/EECycleRate", C_eecr, "0"),
  O_CHOICE("EE cycle skip",    "EmuCore/Speedhacks/EECycleSkip", C_eecs, "0"),
  O_CHOICE("Fast boot",        "Wrapper/FastBoot", C_bool, "true"),
  O_CHOICE("MTVU (multi-VU)",  "EmuCore/Speedhacks/vuThread", C_bool, "true"),
  O_CHOICE("Instant VU1",      "EmuCore/Speedhacks/vu1Instant", C_bool, "true"),
  O_CHOICE("VU flag hack",     "EmuCore/Speedhacks/vuFlagHack", C_bool, "true"),
  O_SUB   ("Frame rate control...", SCR_FRAMERATE),
  O_CHOICE("Sync to refresh",  "EmuCore/GS/SyncToHostRefreshRate", C_bool, "false"),
  O_CHOICE("Game patches",     "EmuCore/EnablePatches", C_bool, "true"),
};
static const Opt S_gamefixes[] = {
  O_CHOICE("FPU multiply hack",       "EmuCore/Gamefixes/FpuMulHack", C_bool, "false"),
  O_CHOICE("FPU negative divide hack","EmuCore/Gamefixes/FpuNegDivHack", C_bool, "false"),
  O_CHOICE("Software renderer for FMVs", "EmuCore/Gamefixes/SoftwareRendererFMVHack", C_bool, "false"),
  O_CHOICE("Skip MPEG hack",          "EmuCore/Gamefixes/SkipMPEGHack", C_bool, "false"),
  O_CHOICE("Preload TLB hack",        "EmuCore/Gamefixes/GoemonTlbHack", C_bool, "false"),
  O_CHOICE("EE timing hack",          "EmuCore/Gamefixes/EETimingHack", C_bool, "false"),
  O_CHOICEG("Instant DMA hack",       "EmuCore/Gamefixes/InstantDMAHack", C_bool, "false", "Wrapper/CoreBuild", "3668"),
  O_CHOICE("OPH flag hack",           "EmuCore/Gamefixes/OPHFlagHack", C_bool, "false"),
  O_CHOICE("Emulate GIF FIFO",        "EmuCore/Gamefixes/GIFFIFOHack", C_bool, "false"),
  O_CHOICE("DMA busy hack",           "EmuCore/Gamefixes/DMABusyHack", C_bool, "false"),
  O_CHOICE("Delay VIF1 stalls",       "EmuCore/Gamefixes/VIF1StallHack", C_bool, "false"),
  O_CHOICE("Emulate VIF FIFO",        "EmuCore/Gamefixes/VIFFIFOHack", C_bool, "false"),
  O_CHOICE("Full VU0 synchronization","EmuCore/Gamefixes/FullVU0SyncHack", C_bool, "false"),
  O_CHOICE("VU I-bit hack",           "EmuCore/Gamefixes/IbitHack", C_bool, "false"),
  O_CHOICE("VU add/sub hack",         "EmuCore/Gamefixes/VuAddSubHack", C_bool, "false"),
  O_CHOICE("VU overflow hack",        "EmuCore/Gamefixes/VUOverflowHack", C_bool, "false"),
  O_CHOICE("VU synchronization",      "EmuCore/Gamefixes/VUSyncHack", C_bool, "false"),
  O_CHOICE("VU XGKick sync",          "EmuCore/Gamefixes/XgKickHack", C_bool, "false"),
  O_CHOICE("Force blit FPS detection", "EmuCore/Gamefixes/BlitInternalFPSHack", C_bool, "false"),
};
static const Opt S_advanced[] = {
  O_CHOICE("EE rounding mode",       "EmuCore/CPU/FPU.Roundmode", C_rounding, "3"),
  O_CHOICE("EE clamping mode",       "EmuCore/CPU/Recompiler/FPUClampMode", C_eeClamp, "1"),
  O_CHOICE("EE recompiler",          "EmuCore/CPU/Recompiler/EnableEE", C_bool, "true"),
  O_CHOICE("EE cache (slow)",        "EmuCore/CPU/Recompiler/EnableEECache", C_bool, "false"),
  O_CHOICE("Wait loop detection",    "EmuCore/Speedhacks/WaitLoop", C_bool, "true"),
  O_CHOICE("INTC spin detection",    "EmuCore/Speedhacks/IntcStat", C_bool, "true"),
  O_CHOICE("Pause on TLB miss",      "EmuCore/CPU/Recompiler/PauseOnTLBMiss", C_bool, "false"),
  O_CHOICE("VU0 rounding mode",      "EmuCore/CPU/VU0.Roundmode", C_rounding, "3"),
  O_CHOICE("VU0 clamping mode",      "EmuCore/CPU/Recompiler/VU0ClampMode", C_vuClamp, "1"),
  O_CHOICE("VU0 recompiler",         "EmuCore/CPU/Recompiler/EnableVU0", C_bool, "true"),
  O_CHOICE("VU1 rounding mode",      "EmuCore/CPU/VU1.Roundmode", C_rounding, "3"),
  O_CHOICE("VU1 clamping mode",      "EmuCore/CPU/Recompiler/VU1ClampMode", C_vuClamp, "1"),
  O_CHOICE("VU1 recompiler",         "EmuCore/CPU/Recompiler/EnableVU1", C_bool, "true"),
  O_CHOICE("IOP recompiler",         "EmuCore/CPU/Recompiler/EnableIOP", C_bool, "true"),
  O_CHOICE("Automatic game fixes",   "EmuCore/EnableGameFixes", C_bool, "true"),
};
static const Opt S_network[] = {
  O_CHOICE("Network adapter",  "Wrapper/Network", C_bool, "false"),
  O_TEXTG ("Custom DNS server", "Wrapper/NetDNS", "", "Wrapper/Network", "false"),
};
static const Opt S_controller[] = {
  O_CHOICE("Controller ports", "Wrapper/ControllerCount", C_players, "2"),
  O_CHOICE("Vibration",   "Wrapper/Vibration",     C_bool, "true"),
  O_HOTKEY("Turbo hotkey", "Wrapper/TurboCombo", "None"),
  O_CHOICE("Cross  (X)",  "Wrapper/Pad1/Cross",    C_btn, "B"),
  O_CHOICE("Circle (O)",  "Wrapper/Pad1/Circle",   C_btn, "A"),
  O_CHOICE("Square",      "Wrapper/Pad1/Square",   C_btn, "Y"),
  O_CHOICE("Triangle",    "Wrapper/Pad1/Triangle", C_btn, "X"),
  O_CHOICE("L1",          "Wrapper/Pad1/L1",       C_btn, "L"),
  O_CHOICE("R1",          "Wrapper/Pad1/R1",       C_btn, "R"),
  O_CHOICE("L2",          "Wrapper/Pad1/L2",       C_btn, "ZL"),
  O_CHOICE("R2",          "Wrapper/Pad1/R2",       C_btn, "ZR"),
  O_CHOICE("L3",          "Wrapper/Pad1/L3",       C_btn, "StickL"),
  O_CHOICE("R3",          "Wrapper/Pad1/R3",       C_btn, "StickR"),
  O_CHOICE("Analog toggle","Wrapper/Pad1/Analog",   C_btn, "None"),
  O_CHOICE("Select",      "Wrapper/Pad1/Select",   C_btn, "Minus"),
  O_CHOICE("Start",       "Wrapper/Pad1/Start",    C_btn, "Plus"),
  O_CHOICE("D-Pad Up",    "Wrapper/Pad1/Up",       C_btn, "Up"),
  O_CHOICE("D-Pad Down",  "Wrapper/Pad1/Down",     C_btn, "Down"),
  O_CHOICE("D-Pad Left",  "Wrapper/Pad1/Left",     C_btn, "Left"),
  O_CHOICE("D-Pad Right", "Wrapper/Pad1/Right",    C_btn, "Right"),
  O_CHOICE("Left stick",  "Wrapper/Pad1/LeftStick",  C_stick, "LStick"),
  O_CHOICE("  invert X",  "Wrapper/Pad1/LeftStickInvertX",  C_bool, "false"),
  O_CHOICE("  invert Y",  "Wrapper/Pad1/LeftStickInvertY",  C_bool, "false"),
  O_CHOICE("Right stick", "Wrapper/Pad1/RightStick", C_stick, "RStick"),
  O_CHOICE("  invert X",  "Wrapper/Pad1/RightStickInvertX", C_bool, "false"),
  O_CHOICE("  invert Y",  "Wrapper/Pad1/RightStickInvertY", C_bool, "false"),
  O_RANGE ("Stick deadzone %", "Wrapper/Pad1/Deadzone", 0, 50, 5, "10"),
};
static const Opt S_framerate[] = {
  O_CHOICE("Frame limiter",    "EmuCore/GS/FrameLimitEnable", C_bool, "true"),
  O_SCALED_RANGEG("Normal speed", "Framerate/NominalScalar", 10, 300, 1, "1", 100, "%", "EmuCore/GS/FrameLimitEnable", "false"),
  O_SCALED_RANGEG("Turbo speed",  "Framerate/TurboScalar",   10, 300, 1, "2", 100, "%", "EmuCore/GS/FrameLimitEnable", "false"),
  O_SCALED_RANGEG("Slow motion",  "Framerate/SlomoScalar",   10, 300, 1, "0.5", 100, "%", "EmuCore/GS/FrameLimitEnable", "false"),
  O_SCALED_RANGE("NTSC frame rate", "EmuCore/GS/FramerateNTSC", 15, 120, 1, "59.94", 1, " Hz"),
  O_SCALED_RANGE("PAL frame rate",  "EmuCore/GS/FrameratePAL",  12, 100, 1, "50", 1, " Hz"),
};
static const Opt S_launcher[] = {
  O_CHOICE("Theme",             "Wrapper/Theme",          C_launcherTheme, "animated"),
  O_CHOICE("Games per row",     "Wrapper/GridColumns",    C_gridColumns,   "6"),
  O_CHOICE("Rows per page",     "Wrapper/GridRows",       C_gridRows,      "2"),
  O_CHOICE("Show game titles",  "Wrapper/ShowGameTitles", C_bool,          "true"),
  O_CHOICE("Show PS2 BIOS",     "Wrapper/ShowPS2BIOS",    C_bool,          "true"),
  O_CHOICE("UI animations",     "Wrapper/UiAnimations",   C_bool,          "true"),
  O_CHOICE("Sound effects",     "Wrapper/UiSounds",       C_bool,          "true"),
  O_CHOICE("Check updates at boot", "Wrapper/CheckUpdatesAtBoot", C_bool,   "true"),
};
struct Screen { const char *title; const Opt *opts; int n; bool binds; };
static const Screen g_screens[SCR_COUNT] = {
  { "Graphics",            S_graphics,   (int)(sizeof(S_graphics)/sizeof(Opt)),   false },
  { "Enhancements",        S_enhance,    (int)(sizeof(S_enhance)/sizeof(Opt)),    false },
  { "Frame Generation",      S_framegen, (int)(sizeof(S_framegen)/sizeof(Opt)),   false },
  { "Audio",               S_audio,      (int)(sizeof(S_audio)/sizeof(Opt)),      false },
  { "Emulation / System",  S_emu,        (int)(sizeof(S_emu)/sizeof(Opt)),        false },
  { "Advanced",            S_advanced,   (int)(sizeof(S_advanced)/sizeof(Opt)),   false },
  { "Game Fixes",          S_gamefixes,  (int)(sizeof(S_gamefixes)/sizeof(Opt)),  false },
  { "Frame Rate Control",  S_framerate,  (int)(sizeof(S_framerate)/sizeof(Opt)),  false },
  { "Network (experimental)", S_network, (int)(sizeof(S_network)/sizeof(Opt)),    false },
  { "Controller",          S_controller, (int)(sizeof(S_controller)/sizeof(Opt)), true  },
};

struct SettingHelpEntry {
  const char *key;
  const char *kind;
  const char *text;
};

/* Condensed Android/PCSX2 and Switch-specific setting help. */
static const SettingHelpEntry SETTING_HELP[] = {
  {"EmuCore/GS/Renderer", "Graphics backend",
   "Chooses the graphics backend used by the emulator. Vulkan (NVK) is recommended for Switch performance and is required for frame generation. OpenGL can help diagnose renderer-specific compatibility issues."},
  {"EmuCore/GS/upscale_multiplier", "Resolution / performance",
   "Sets the internal resolution used for 3D rendering. Higher scales improve clarity but increase GPU load; sub-native scales reduce GPU work at the cost of image quality. Pre-rendered FMV resolution does not change."},
  {"EmuCore/GS/AspectRatio", "Display geometry",
   "Chooses the shape of the final game image. 4:3 preserves the original PS2 display, 16:9 is intended for compatible games or widescreen patches, and Stretch fills the screen by distorting the image."},
  {"EmuCore/GS/FMVAspectRatioSwitch", "Video display",
   "Overrides the aspect ratio used for pre-rendered movie sequences independently of gameplay. Leave it Off unless a game's FMV is shown with the wrong shape."},
  {"EmuCore/GS/VsyncEnable", "Frame pacing / latency",
   "Synchronizes presentation with the Switch display to reduce tearing. It can add latency or reduce performance headroom; Adaptive synchronizes only when emulation can keep up."},
  {"EmuCore/GS/SkipDuplicateFrames", "Performance / latency trade-off",
   "Avoids presenting unchanged frames in many 25/30 FPS games. This is not emulation frame skipping: the frame is still rendered. It can free GPU time and helps frame generation, but may worsen pacing or input latency."},
  {"EmuCore/GS/DisableThreadedPresentation", "Frame pacing / performance",
   "Controls whether final presentation may run on its own thread. Leave this Off for normal threaded presentation and better overlap. Turn it On only when diagnosing a presentation or compatibility problem, as throughput can decrease."},
  {"EmuCore/GS/filter", "Texture filtering",
   "Selects where bilinear filtering is used on PS2 textures. PS2 follows each game's request; Forced smooths more surfaces, while Nearest keeps hard pixel edges. Forced filtering can blur 2D sprites."},
  {"EmuCore/GS/MaxAnisotropy", "Texture filtering",
   "Improves texture clarity at steep viewing angles. Higher values add some GPU work and are most useful when rendering above native resolution."},
  {"EmuCore/GS/OsdShowFPS", "Performance display",
   "Shows the emulator's on-screen frame-rate and speed information while a game is running."},
  {"EmuCore/GS/OsdShowMessages", "On-screen display",
   "Shows emulator status and warning messages over the game image."},
  {"EmuCore/EnableWideScreenPatches", "Game patches",
   "Loads compatible widescreen PNACH patches so supported games render a wider view instead of stretching 4:3 output. Not every game has a patch, and some patches can affect menus or effects."},
  {"EmuCore/EnableNoInterlacingPatches", "Game patches",
   "Loads compatible no-interlacing PNACH patches. These can reduce flicker and simplify deinterlacing, but game-specific patches may introduce visual issues."},

  {"EmuCore/GS/accurate_blending_unit", "Accuracy / performance",
   "Controls how accurately unsupported PS2 GS blend modes are reproduced in shaders. Higher levels fix more effects but can carry a large GPU cost. Basic is the recommended starting point."},
  {"EmuCore/GS/deinterlace_mode", "Video processing",
   "Chooses how interlaced PS2 output is converted for the progressive Switch display. Auto follows the game; manual modes are useful only when a title shows combing, shaking, or an incorrect field order."},
  {"EmuCore/GS/dithering_ps2", "Image accuracy",
   "Controls PS2 dithering, which reduces visible color banding. Scaled enlarges the dither pattern with internal resolution; Unscaled keeps the native-sized pattern; Off removes it."},
  {"EmuCore/GS/UserHacks_TriFilter", "Texture filtering",
   "Samples between nearby mip levels to reduce texture transitions and angled-surface blur. Automatic follows safe defaults, PS2 follows the game's request, and Forced applies it more broadly with extra GPU cost."},
  {"EmuCore/GS/mipmap_hw", "Rendering compatibility",
   "Emulates the GS texture mipmapping used by many games. Disabling it can break rendering or cause distant-texture artifacts, so keep it enabled unless testing a specific workaround."},
  {"EmuCore/GS/CRCHackLevel", "Game-specific fixes",
   "Controls automatic renderer fixes selected from the game's CRC. Auto is recommended. Lower levels may leave graphical problems; Aggressive can remove effects or geometry while fixing difficult titles."},
  {"EmuCore/GS/texture_preloading", "Performance / memory",
   "Uploads complete textures instead of only the regions currently used. Full usually avoids redundant transfers and improves performance, but consumes more memory and can slow a small number of games."},
  {"EmuCore/GS/paltex", "CPU / GPU trade-off",
   "Moves color-palette texture conversion from the CPU to the GPU. It can help CPU-limited games but adds GPU work; results depend on the title and renderer."},
  {"EmuCore/GS/pcrtc_antiblur", "Visual enhancement",
   "Applies internal anti-blur fixes to many games. The image is often clearer, although it is less faithful to the original PS2 output and can affect titles that rely on the blur."},
  {"EmuCore/GS/TVShader", "Post-processing",
   "Applies a display shader that imitates scanlines or CRT/television patterns. This changes only the final image and adds GPU work."},
  {"EmuCore/GS/CASMode", "Post-processing",
   "Applies Contrast Adaptive Sharpening to the final image. Sharpen Only adds clarity; Sharpen + Upscale also participates in scaling. Strong sharpening can emphasize aliasing or noise."},
  {"EmuCore/GS/CASSharpness", "Post-processing",
   "Sets the strength of Contrast Adaptive Sharpening. Higher values produce a crisper image but can create halos or emphasize pixel shimmer."},
  {"EmuCore/GS/ShadeBoost", "Color adjustment",
   "Enables manual brightness, contrast, and saturation adjustments on the final game image. It does not change the emulated game's own lighting."},
  {"EmuCore/GS/ShadeBoost_Brightness", "Color adjustment",
   "Adjusts final-image brightness while Shade Boost is enabled. The default value keeps the original balance."},
  {"EmuCore/GS/ShadeBoost_Contrast", "Color adjustment",
   "Adjusts final-image contrast while Shade Boost is enabled. Extreme values can hide shadow or highlight detail."},
  {"EmuCore/GS/ShadeBoost_Saturation", "Color adjustment",
   "Adjusts final-image color saturation while Shade Boost is enabled. The default value keeps the original color intensity."},
  {"EmuCore/GS/LoadTextureReplacements", "Custom textures",
   "Loads replacement textures prepared for the current game. This increases storage and memory use; matching depends on the game's texture hashes and replacement pack."},
  {"EmuCore/GS/SoftwareRendererFMV", "Game-specific workaround",
   "Temporarily uses the software renderer for FMV sequences. It can fix videos that hardware rendering displays incorrectly, but costs substantial CPU time during those scenes."},
  {"EmuCore/GS/HWDownloadMode", "Accuracy / performance",
   "Changes synchronization for data read back from the GPU. Accurate is safest. Less synchronized modes may improve demanding games but can break effects, videos, or other rendering that depends on GS downloads."},

  {"Wrapper/LSFGEnabled", "Frame generation",
   "Makes LSFG 2x available for this launch. Frame generation still starts Off in-game and must be enabled from the overlay. On Switch's 60 Hz output, 25/30 FPS sources are interpolated while native 50/60 FPS sources automatically stay on the normal presentation path so emulation is never halved. Requires Vulkan and Lossless.dll and may add artifacts or latency."},
  {"Wrapper/LSFGFlowScale", "Frame generation quality",
   "Sets the resolution used for optical-flow analysis. Half can retain more motion detail but costs considerably more GPU time and memory; Quarter is recommended on Switch."},
  {"Wrapper/LSFGPerformance", "Frame generation performance",
   "Uses LSFG's lighter performance-oriented path. Keep it enabled on Switch unless testing image quality with ample GPU headroom."},

  {"SPU2/Mixing/FinalVolume", "Audio output",
   "Sets the final emulator volume sent to the Switch audio output. It does not change the game's internal volume setting."},
  {"SPU2/Mixing/Interpolation", "Audio quality / performance",
   "Chooses how the SPU2 reconstructs audio samples. Higher-quality interpolation can sound smoother but uses more CPU; Catmull-Rom is the normal quality setting."},
  {"SPU2/Output/SynchMode", "Audio synchronization",
   "TimeStretch adjusts audio smoothly when emulation timing varies and is recommended. Async keeps audio independent of emulation speed; None performs no synchronization and can crackle or drift."},
  {"SPU2/Output/Latency", "Audio latency / stability",
   "Sets the emulated audio buffer length. Lower values reduce response delay but are more likely to crackle when frame times fluctuate; higher values are safer but add latency."},
  {"SPU2/Output/OutputLatency", "Audio latency / stability",
   "Sets additional buffering in the Switch output path. Reduce it for lower audio delay, or raise it if sound underruns and crackles persist."},

  {"Wrapper/CoreBuild", "Compatibility core",
   "Selects which bundled NetherSX2 Android core is loaded. Patched 4248 is the normal choice; Classic 3668 is retained for games or behavior that are more compatible with the older core."},
  {"Wrapper/FastmemMode", "CPU performance",
   "Uses fast mapped access for emulated PS2 memory while retaining NetherSX2_nx's self-modifying-code invalidation. It is a major CPU optimization. Disabling it keeps the safe slower memory path for troubleshooting."},
  {"Wrapper/SystemLanguage", "PS2 firmware",
   "Sets the language stored in the PS2 BIOS NVM before launch. Auto follows the Switch language when supported; Don't change preserves the existing BIOS setting."},
  {"EmuCore/Speedhacks/EECycleRate", "CPU speed control",
   "Underclocks or overclocks the emulated Emotion Engine. Lower values can reduce host CPU load but may lower a game's internal frame rate or break timing. Higher values greatly increase CPU requirements."},
  {"EmuCore/Speedhacks/EECycleSkip", "CPU timing hack",
   "Makes the emulated Emotion Engine skip work cycles. It helps a small set of demanding games, but usually harms performance, pacing, or compatibility. Keep it Off unless a game specifically benefits."},
  {"Wrapper/FastBoot", "Boot behavior",
   "Skips the PS2 BIOS boot animation and starts the selected game directly. Disable it when a title or BIOS function requires a full boot."},
  {"EmuCore/Speedhacks/vuThread", "CPU threading",
   "Runs VU1 work on another CPU thread. It is normally a useful Switch speedup, but a small number of games are incompatible or may hang."},
  {"EmuCore/Speedhacks/vu1Instant", "CPU performance",
   "Uses the optimized Instant VU1 synchronization path when compatible. It usually improves performance, but should be disabled if a game develops VU-related graphics or timing errors."},
  {"EmuCore/Speedhacks/vuFlagHack", "CPU performance",
   "Optimizes VU flag calculations for a useful speedup with high compatibility. Disable it only when diagnosing VU graphics or calculation errors in a specific game."},
  {"EmuCore/GS/SyncToHostRefreshRate", "Frame pacing",
   "Slightly adjusts emulation speed so the guest refresh aligns with the Switch display. This can make animation smoother, but it only applies when the rates are already close and may change speed by less than one percent."},
  {"EmuCore/EnablePatches", "Game compatibility",
   "Loads the normal game-specific PNACH patches supplied with the core. These fixes are separate from user-selectable cheat codes and are recommended for compatibility."},

  {"EmuCore/Gamefixes/FpuMulHack", "Manual game fix",
   "Changes FPU multiplication behavior for Tales of Destiny. Leave it Off for unrelated games."},
  {"EmuCore/Gamefixes/FpuNegDivHack", "Manual game fix",
   "Changes negative FPU division behavior for Gundam games. Leave it Off for unrelated games."},
  {"EmuCore/Gamefixes/SoftwareRendererFMVHack", "Manual game fix",
   "Uses the software renderer for complex FMV sequences which do not render correctly in hardware mode. FMV playback will require substantially more CPU performance."},
  {"EmuCore/Gamefixes/SkipMPEGHack", "Manual game fix",
   "Skips MPEG videos and FMVs in games which otherwise hang or freeze while playing them."},
  {"EmuCore/Gamefixes/GoemonTlbHack", "Manual game fix",
   "Preloads TLB entries to avoid translation misses in Goemon titles."},
  {"EmuCore/Gamefixes/EETimingHack", "Manual game fix",
   "Applies a general-purpose Emotion Engine timing workaround used by games such as Digital Devil Saga and SSX."},
  {"EmuCore/Gamefixes/InstantDMAHack", "Manual game fix (4248 only)",
   "Completes DMA operations immediately to work around cache-emulation problems such as those in Fire Pro Wrestling Z. The Classic 3668 core does not expose this setting."},
  {"EmuCore/Gamefixes/OPHFlagHack", "Manual game fix",
   "Changes GIF OPH flag behavior for titles including Bleach Blade Battlers, Growlanser II and III, and Wizardry."},
  {"EmuCore/Gamefixes/GIFFIFOHack", "Manual game fix",
   "Emulates the GIF FIFO more accurately. This can fix FIFA Street 2 but is slower than the normal path."},
  {"EmuCore/Gamefixes/DMABusyHack", "Manual game fix",
   "Adjusts DMA busy-state timing for games including Mana Khemia, Metal Saga, and Pilot Down: Behind Enemy Lines."},
  {"EmuCore/Gamefixes/VIF1StallHack", "Manual game fix",
   "Delays VIF1 stalls to fix the SOCOM II HUD and the Spy Hunter loading hang."},
  {"EmuCore/Gamefixes/VIFFIFOHack", "Manual game fix",
   "Simulates VIF1 FIFO read-ahead for games such as Test Drive Unlimited and Transformers."},
  {"EmuCore/Gamefixes/FullVU0SyncHack", "Manual game fix",
   "Forces tight VU0 synchronization on every COP2 instruction. This improves accuracy but can reduce performance."},
  {"EmuCore/Gamefixes/IbitHack", "Manual game fix",
   "Avoids constant VU recompilation in games such as Scarface: The World Is Yours and Crash Tag Team Racing."},
  {"EmuCore/Gamefixes/VuAddSubHack", "Manual game fix",
   "Changes VU add/subtract behavior for tri-Ace games including Star Ocean 3, Radiata Stories, and Valkyrie Profile 2."},
  {"EmuCore/Gamefixes/VUOverflowHack", "Manual game fix",
   "Checks for VU floating-point overflows required by games such as Superman Returns."},
  {"EmuCore/Gamefixes/VUSyncHack", "Manual game fix",
   "Lets the VUs run behind the EE to avoid synchronization problems when games read or write VU registers."},
  {"EmuCore/Gamefixes/XgKickHack", "Manual game fix",
   "Uses more accurate VU XGKick timing. It can fix geometry and synchronization problems but is slower."},
  {"EmuCore/Gamefixes/BlitInternalFPSHack", "Manual game fix",
   "Uses framebuffer blits to estimate a game's internal frame rate when normal detection produces a false result."},

  {"EmuCore/CPU/FPU.Roundmode", "Advanced EE accuracy",
   "Changes how the Emotion Engine FPU rounds results. Chop / Zero is the normal PS2-compatible choice; use another mode only for a game known to require it."},
  {"EmuCore/CPU/Recompiler/FPUClampMode", "Advanced EE accuracy",
   "Controls how aggressively EE floating-point values are clamped. Normal is recommended; stricter modes can fix specific games but may reduce accuracy elsewhere."},
  {"EmuCore/CPU/Recompiler/EnableEE", "CPU execution mode",
   "Uses the fast EE just-in-time recompiler. Disabling it is intended only for compatibility diagnosis and is far too slow for normal play."},
  {"EmuCore/CPU/Recompiler/EnableEECache", "CPU compatibility",
   "Emulates the EE cache for titles which depend on it. This is substantially slower and should remain Off unless a game specifically needs it."},
  {"EmuCore/Speedhacks/WaitLoop", "CPU optimization",
   "Detects EE wait loops and skips idle work. It provides a moderate speedup in some games with very few compatibility side effects."},
  {"EmuCore/Speedhacks/IntcStat", "CPU optimization",
   "Detects EE interrupt-controller spin loops. This can provide a large speedup in affected games and is normally safe."},
  {"EmuCore/CPU/Recompiler/PauseOnTLBMiss", "Developer diagnostic",
   "Pauses emulation after an EE TLB miss. This is a debugging aid and should remain Off for gameplay."},
  {"EmuCore/CPU/VU0.Roundmode", "Advanced VU accuracy",
   "Changes rounding for Vector Unit 0. Chop / Zero is the default; other modes are compatibility overrides for specific games."},
  {"EmuCore/CPU/VU1.Roundmode", "Advanced VU accuracy",
   "Changes rounding for Vector Unit 1. Chop / Zero is the default; other modes are compatibility overrides for specific games."},
  {"EmuCore/CPU/Recompiler/VU0ClampMode", "Advanced VU accuracy",
   "Controls VU0 floating-point clamping. Normal is recommended unless a known game fix requires another mode."},
  {"EmuCore/CPU/Recompiler/VU1ClampMode", "Advanced VU accuracy",
   "Controls VU1 floating-point clamping. Normal is recommended unless a known game fix requires another mode."},
  {"EmuCore/CPU/Recompiler/EnableVU0", "VU execution mode",
   "Uses the fast VU0 micro recompiler. Turning it Off is for compatibility diagnosis and sharply reduces performance."},
  {"EmuCore/CPU/Recompiler/EnableVU1", "VU execution mode",
   "Uses the fast VU1 recompiler. Turning it Off is for compatibility diagnosis and sharply reduces performance."},
  {"EmuCore/CPU/Recompiler/EnableIOP", "IOP execution mode",
   "Uses the fast IOP just-in-time recompiler. Disabling it is intended only for compatibility diagnosis."},
  {"EmuCore/EnableGameFixes", "Automatic compatibility",
   "Allows the core to apply known fixes from GameIndex.yaml. Keep this On unless diagnosing an incorrect automatic game fix."},

  {"Wrapper/Network", "Experimental networking",
   "Enables NetherSX2_nx's experimental PS2 network-adapter path for games that support online or LAN networking. Leave it disabled when unused."},
  {"Wrapper/NetDNS", "Experimental networking",
   "Overrides the DNS server used by the emulated network adapter. Leave it on Auto unless a replacement service or network setup requires a specific address."},

  {"Wrapper/ControllerCount", "Controller input",
   "Sets how many PS2 controller ports NetherSX2_nx accepts from connected Switch controllers. Use 2 for local multiplayer; each player should connect their controller before launching the game."},
  {"Wrapper/Vibration", "Controller feedback",
   "Forwards PS2 DualShock vibration to connected Switch controllers. Disable it to avoid rumble or reduce controller battery use."},
  {"Wrapper/TurboCombo", "Hotkey binding",
   "Assigns a unique Switch button or multi-button combination to PS2 turbo speed. Press A, hold every button in the combination, then release them. None leaves turbo unmapped."},
  {"Wrapper/Pad1/Analog", "Controller mode",
   "Maps the physical DualShock 2 Analog button, which toggles the emulated controller between digital and analog modes when a game has not locked it. Leave it on None to use the in-game Controller Mapping action without sacrificing a Switch button."},
  {"Wrapper/Pad1/LeftStick", "Analog mapping",
   "Chooses which Switch analog stick controls the PS2 left stick. Set it to None to leave that emulated stick unmapped."},
  {"Wrapper/Pad1/RightStick", "Analog mapping",
   "Chooses which Switch analog stick controls the PS2 right stick. Set it to None to leave that emulated stick unmapped."},
  {"Wrapper/Pad1/LeftStickInvertX", "Analog mapping",
   "Reverses horizontal input on the emulated PS2 left stick."},
  {"Wrapper/Pad1/LeftStickInvertY", "Analog mapping",
   "Reverses vertical input on the emulated PS2 left stick."},
  {"Wrapper/Pad1/RightStickInvertX", "Analog mapping",
   "Reverses horizontal input on the emulated PS2 right stick."},
  {"Wrapper/Pad1/RightStickInvertY", "Analog mapping",
   "Reverses vertical input on the emulated PS2 right stick."},
  {"Wrapper/Pad1/Deadzone", "Analog input",
   "Sets how far a Switch stick must move before NetherSX2 registers input. Raise it to hide stick drift; lower it for more immediate small movements."},

  {"EmuCore/GS/FrameLimitEnable", "Speed control",
   "Keeps emulation at the selected normal, turbo, or slow-motion target. Disabling it lets the game run as fast as the Switch can manage."},
  {"Framerate/NominalScalar", "Speed control",
   "Sets the target speed during normal play. Reaching the target is not guaranteed when a game exceeds available CPU or GPU performance."},
  {"Framerate/TurboScalar", "Speed control",
   "Sets the target used while the configured turbo hotkey is active. Higher values require proportionally more CPU and GPU performance."},
  {"Framerate/SlomoScalar", "Speed control",
   "Sets the target used by slow-motion mode."},
  {"EmuCore/GS/FramerateNTSC", "Emulated video timing",
   "Sets the base refresh rate used by NTSC games. The normal value is 59.94 Hz. Changing it alters game timing and should be treated as a compatibility or speed experiment."},
  {"EmuCore/GS/FrameratePAL", "Emulated video timing",
   "Sets the base refresh rate used by PAL games. The normal value is 50 Hz. Changing it alters game timing and should be treated as a compatibility or speed experiment."},

  {"Wrapper/Theme", "Launcher appearance",
   "Changes the launcher background and visual style. It does not affect gameplay rendering or performance inside the emulator."},
  {"Wrapper/GridColumns", "Library layout",
   "Sets how many game covers appear across each library row. More columns make each cover smaller."},
  {"Wrapper/GridRows", "Library layout",
   "Sets how many cover rows appear on one library page. More rows make each cover smaller."},
  {"Wrapper/ShowGameTitles", "Library layout",
   "Shows or hides game names below their covers in the launcher library."},
  {"Wrapper/ShowPS2BIOS", "Library contents",
   "Shows the PS2 BIOS system-menu tile in the launcher library. Turn it Off to hide only the BIOS tile; installed games are never hidden."},
  {"Wrapper/UiAnimations", "Launcher appearance",
   "Enables launcher transitions, moving selection highlights, and animated theme effects."},
  {"Wrapper/UiSounds", "Launcher audio",
   "Enables navigation, confirmation, and back sound effects in the SDL launcher."},
  {"Wrapper/CheckUpdatesAtBoot", "Launcher updates",
   "Checks the latest published NetherSX2_nx GitHub release in the background when the launcher opens. Turn it off to check only from the launcher settings screen."},
};

struct SettingHelpInfo {
  const char *kind;
  std::string text;
};

static SettingHelpInfo settingHelpFor(const Opt &option) {
  if(option.key){
    for(const SettingHelpEntry &entry:SETTING_HELP)
      if(!strcmp(entry.key,option.key)) return {entry.kind,entry.text};
    if(!strncmp(option.key,"Wrapper/Pad1/",13)&&option.ch==C_btn)
      return {"Controller mapping",
              std::string("Maps PS2 ")+option.label+" to a Switch controller button. Press A on this row, then press the desired button; choose None to leave it unmapped."};
    if(option.type==OT_HOTKEY)
      return {"Hotkey binding",
              std::string("Assigns a Switch button or button combination to ")+option.label+". Press A, hold every button in the combination, then release them."};
  }
  if(option.type==OT_SUBMENU){
    if(option.sub==SCR_ENHANCE)
      return {"Settings group","Contains advanced GS accuracy controls, visual enhancements, and game-specific rendering workarounds. Higher accuracy often requires more GPU performance."};
    if(option.sub==SCR_FRAMERATE)
      return {"Settings group","Contains the frame limiter, normal/turbo/slow-motion targets, and the base NTSC/PAL video rates. Changing base video timing can affect game logic."};
  }
  if(option.type==OT_STATUS)
    return {"Required component","Shows whether Lossless.dll is installed in NetherSX2_nx's fixed frame-generation folder. LSFG options cannot be enabled while it is missing."};
  return {"Setting","Changes this launcher or emulator option. Use the default value when troubleshooting an unexpected game-specific problem."};
}

static void commitAll() {
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (o.key && (o.type == OT_CHOICE || o.type == OT_RANGE || o.type == OT_SCALED_RANGE ||
                    o.type == OT_TEXT || o.type == OT_HOTKEY)) {
        std::string v = iniGet(o.key, o.def);
        iniSet(o.key, v.c_str());
      }
    }
}

static SDL_Window   *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;
static TTF_Font     *g_font = nullptr, *g_font_sm = nullptr, *g_font_big = nullptr;
static SDL_Texture  *g_logo = nullptr;
static int SW = 1280, SH = 720;
static bool g_romfsReady = false;
static bool g_sdlReady = false;
static bool g_ttfReady = false;
static bool g_imgReady = false;
static bool g_plReady = false;
static bool g_griddbReady = false;
static bool g_storageSocketReady = false;
static std::string g_updateNoticeTag;
static std::string g_updateNotifiedTag;
static Uint32 g_updateNoticeUntil = 0;

enum class LauncherTheme { Xmb, Glow, Bubbles, Classic, Oled };
static LauncherTheme g_launcherTheme = LauncherTheme::Glow;
static bool g_uiAnimations = true;
static bool g_showGameTitles = true;
static int g_gridColumns = 6;
static int g_gridRows = 2;
static SDL_Texture *g_glowTexture = nullptr;

static SDL_Color COL_BG    = { 8, 12, 24, 255 };
static SDL_Color COL_TXT   = { 235, 239, 247, 255 };
static SDL_Color COL_DIM   = { 151, 163, 184, 255 };
static SDL_Color COL_HI    = { 100, 211, 255, 255 };
static SDL_Color COL_VAL   = { 255, 215, 120, 255 };
static SDL_Color COL_SEL   = { 116, 200, 255, 255 };
static SDL_Color COL_PANEL = { 16, 23, 39, 184 };
static SDL_Color COL_CARD  = { 22, 30, 49, 214 };
static SDL_Color COL_FOCUS = { 28, 69, 92, 210 };

static void fillRect(int x,int y,int w,int h, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(g_ren,&r); }
static void border(int x,int y,int w,int h,int t, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); for(int i=0;i<t;i++){ SDL_Rect r={x-i,y-i,w+2*i,h+2*i}; SDL_RenderDrawRect(g_ren,&r); } }

struct TextKey {
  TTF_Font *font;
  Uint32 color;
  std::string text;
  bool operator==(const TextKey &other) const {
    return font == other.font && color == other.color && text == other.text;
  }
};

struct TextKeyHash {
  size_t operator()(const TextKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<Uint32>{}(key.color) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct TextEntry {
  SDL_Texture *texture;
  int width;
  int height;
  size_t bytes;
  Uint64 use;
};

struct MetricKey {
  TTF_Font *font;
  std::string text;
  bool operator==(const MetricKey &other) const { return font == other.font && text == other.text; }
};

struct MetricKeyHash {
  size_t operator()(const MetricKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    return hash ^ (std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2));
  }
};

struct MetricEntry { int width; Uint64 use; };

struct EllipsisKey {
  TTF_Font *font;
  int maxWidth;
  std::string text;
  bool operator==(const EllipsisKey &other) const {
    return font == other.font && maxWidth == other.maxWidth && text == other.text;
  }
};

struct EllipsisKeyHash {
  size_t operator()(const EllipsisKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<int>{}(key.maxWidth) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct EllipsisEntry { std::string text; Uint64 use; };

static std::unordered_map<TextKey, TextEntry, TextKeyHash> g_textCache;
static std::unordered_map<MetricKey, MetricEntry, MetricKeyHash> g_metricCache;
static std::unordered_map<EllipsisKey, EllipsisEntry, EllipsisKeyHash> g_ellipsisCache;
static size_t g_textCacheBytes = 0;
static Uint64 g_textUseSerial = 0;
static constexpr size_t TEXT_CACHE_LIMIT = 512;
static constexpr size_t TEXT_CACHE_BYTES = 12 * 1024 * 1024;
static constexpr size_t METRIC_CACHE_LIMIT = 2048;
static constexpr size_t ELLIPSIS_CACHE_LIMIT = 512;

static Uint32 packColor(SDL_Color color) {
  return (Uint32)color.r | ((Uint32)color.g << 8) | ((Uint32)color.b << 16) | ((Uint32)color.a << 24);
}

static void rememberTextMetric(TTF_Font *font, const std::string &text, int width) {
  MetricKey key{font, text};
  auto found = g_metricCache.find(key);
  if (found != g_metricCache.end()) {
    found->second.width = width;
    found->second.use = ++g_textUseSerial;
    return;
  }
  if (g_metricCache.size() >= METRIC_CACHE_LIMIT) {
    auto victim = g_metricCache.begin();
    for (auto it = std::next(g_metricCache.begin()); it != g_metricCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    g_metricCache.erase(victim);
  }
  g_metricCache.emplace(std::move(key), MetricEntry{width, ++g_textUseSerial});
}

static void evictTextEntries(size_t incomingBytes) {
  while (!g_textCache.empty() &&
         (g_textCache.size() >= TEXT_CACHE_LIMIT || g_textCacheBytes > TEXT_CACHE_BYTES - incomingBytes)) {
    auto victim = g_textCache.begin();
    for (auto it = std::next(g_textCache.begin()); it != g_textCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    SDL_DestroyTexture(victim->second.texture);
    g_textCacheBytes -= victim->second.bytes;
    g_textCache.erase(victim);
  }
}

static void clearTextCaches() {
  for (auto &entry : g_textCache) SDL_DestroyTexture(entry.second.texture);
  g_textCache.clear();
  g_metricCache.clear();
  g_ellipsisCache.clear();
  g_textCacheBytes = 0;
  g_textUseSerial = 0;
}

static void applyLauncherAppearance() {
  LauncherTheme previous = g_launcherTheme;
  const char *theme = storeGet(g_global, "Wrapper/Theme", "animated");
  g_launcherTheme = !strcmp(theme, "classic") ? LauncherTheme::Classic :
                    !strcmp(theme, "oled") ? LauncherTheme::Oled :
                    !strcmp(theme, "homebrew") ? LauncherTheme::Bubbles :
                    !strcmp(theme, "xmb") ? LauncherTheme::Xmb : LauncherTheme::Glow;
  g_uiAnimations = strcmp(storeGet(g_global, "Wrapper/UiAnimations", "true"), "false") != 0;
  g_showGameTitles = strcmp(storeGet(g_global, "Wrapper/ShowGameTitles", "true"), "false") != 0;
  g_gridColumns = std::max(3, std::min(8, atoi(storeGet(g_global, "Wrapper/GridColumns", "6"))));
  g_gridRows = std::max(1, std::min(3, atoi(storeGet(g_global, "Wrapper/GridRows", "2"))));

  if (g_launcherTheme == LauncherTheme::Xmb) {
    COL_BG={2,35,92,255}; COL_TXT={246,250,255,255}; COL_DIM={176,207,233,255};
    COL_HI={151,229,255,255}; COL_VAL={255,255,255,255}; COL_SEL={116,218,255,255};
    COL_PANEL={4,28,73,164}; COL_CARD={5,36,86,196}; COL_FOCUS={20,91,148,214};
  } else if (g_launcherTheme == LauncherTheme::Classic) {
    COL_BG={22,24,30,255}; COL_TXT={228,230,235,255}; COL_DIM={150,155,165,255};
    COL_HI={96,200,255,255}; COL_VAL={255,210,100,255}; COL_SEL={255,170,0,255};
    COL_PANEL={28,31,40,255}; COL_CARD={24,26,34,255}; COL_FOCUS={66,56,30,235};
  } else if (g_launcherTheme == LauncherTheme::Oled) {
    COL_BG={0,0,0,255}; COL_TXT={245,247,249,255}; COL_DIM={145,151,158,255};
    COL_HI={105,220,255,255}; COL_VAL={255,255,255,255}; COL_SEL={0,210,190,255};
    COL_PANEL={4,4,5,248}; COL_CARD={8,8,10,250}; COL_FOCUS={0,58,53,245};
  } else if (g_launcherTheme == LauncherTheme::Bubbles) {
    COL_BG={0,8,16,255}; COL_TXT={235,248,255,255}; COL_DIM={143,192,216,255};
    COL_HI={118,222,255,255}; COL_VAL={194,239,255,255}; COL_SEL={61,183,235,255};
    COL_PANEL={4,31,50,190}; COL_CARD={5,35,56,218}; COL_FOCUS={12,76,108,220};
  } else {
    COL_BG={8,12,24,255}; COL_TXT={235,239,247,255}; COL_DIM={151,163,184,255};
    COL_HI={100,211,255,255}; COL_VAL={255,215,120,255}; COL_SEL={116,200,255,255};
    COL_PANEL={16,23,39,184}; COL_CARD={22,30,49,214}; COL_FOCUS={28,69,92,208};
  }
  if (previous != g_launcherTheme && g_ren)
    clearTextCaches();
}

static void ensureGlowTexture() {
  if (g_glowTexture || !g_ren) return;
  constexpr int size=256;
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,size,size,32,SDL_PIXELFORMAT_RGBA32);
  if(!surface) return;
  if(SDL_LockSurface(surface)==0){
    for(int y=0;y<size;y++){
      auto *row=(Uint32*)((Uint8*)surface->pixels+y*surface->pitch);
      for(int x=0;x<size;x++){
        float dx=(x-(size-1)*0.5f)/(size*0.5f),dy=(y-(size-1)*0.5f)/(size*0.5f);
        float distance=sqrtf(dx*dx+dy*dy);
        float strength=distance>=1.f?0.f:1.f-distance;
        Uint8 alpha=(Uint8)(255.f*strength*strength);
        row[x]=SDL_MapRGBA(surface->format,255,255,255,alpha);
      }
    }
    SDL_UnlockSurface(surface);
    g_glowTexture=SDL_CreateTextureFromSurface(g_ren,surface);
    if(g_glowTexture) SDL_SetTextureBlendMode(g_glowTexture,SDL_BLENDMODE_BLEND);
  }
  SDL_FreeSurface(surface);
}

static bool hasAnimatedBackground() {
  return g_launcherTheme==LauncherTheme::Xmb||g_launcherTheme==LauncherTheme::Bubbles||g_launcherTheme==LauncherTheme::Glow;
}

static void drawGlow(float x,float y,float radius,Uint8 red,Uint8 green,Uint8 blue,Uint8 alpha) {
  int diameter=(int)(SH*radius);
  SDL_Rect destination={(int)(SW*x)-diameter/2,(int)(SH*y)-diameter/2,diameter,diameter};
  SDL_SetTextureColorMod(g_glowTexture,red,green,blue);
  SDL_SetTextureAlphaMod(g_glowTexture,alpha);
  SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&destination);
}

static void drawBackgroundParticles(float time,SDL_Color color,int count,float speed) {
  for(int i=0;i<count;i++){
    float travel=fmodf(i*0.371f+time*speed*(0.65f+(i%5)*0.11f),1.12f)-0.06f;
    float y=fmodf(i*0.217f+0.11f*sinf(time*0.29f+i*1.73f),1.f);
    float pulse=0.45f+0.55f*sinf(time*(0.9f+(i%4)*0.17f)+i);
    Uint8 alpha=(Uint8)(color.a*(0.55f+0.45f*pulse));
    int size=(i%9==0)?3:2;
    fillRect((int)(travel*SW),(int)(y*SH),size,size,(SDL_Color){color.r,color.g,color.b,alpha});
  }
}

static Uint8 blendChannel(Uint8 first,Uint8 second,float amount) {
  return (Uint8)(first+(second-first)*std::clamp(amount,0.f,1.f));
}

static float xmbWaveY(float x,float time,float center,float amplitude,float frequency,float slope,float phase) {
  const float primary=sinf(x*6.2831853f*frequency+phase+time*0.115f);
  const float detail=sinf(x*6.2831853f*(frequency*2.07f)+phase*0.61f-time*0.072f);
  return center+slope*(x-0.5f)+amplitude*(primary+detail*0.24f);
}

static void drawXmbRibbon(float time,float center,float amplitude,float frequency,float slope,float phase,
                          int halfWidth,SDL_Color color) {
  constexpr int pointCount=121;
  std::array<SDL_Point,pointCount> points{};
  for(int offset=-halfWidth;offset<=halfWidth;offset++){
    float distance=halfWidth?fabsf((float)offset/halfWidth):0.f;
    Uint8 alpha=(Uint8)(color.a*powf(std::max(0.f,1.f-distance),1.45f));
    if(alpha<2) continue;
    for(int point=0;point<pointCount;point++){
      float x=(float)point/(pointCount-1);
      points[point]={(int)(x*SW),(int)(xmbWaveY(x,time,center,amplitude,frequency,slope,phase)*SH)+offset};
    }
    SDL_SetRenderDrawColor(g_ren,color.r,color.g,color.b,alpha);
    SDL_RenderDrawLines(g_ren,points.data(),pointCount);
  }
}

static void drawXmbFilament(float time,float center,float amplitude,float frequency,float slope,float phase,
                            SDL_Color color) {
  constexpr int pointCount=161;
  std::array<SDL_Point,pointCount> points{};
  for(int point=0;point<pointCount;point++){
    float x=(float)point/(pointCount-1);
    points[point]={(int)(x*SW),(int)(xmbWaveY(x,time,center,amplitude,frequency,slope,phase)*SH)};
  }
  SDL_SetRenderDrawColor(g_ren,color.r,color.g,color.b,color.a);
  SDL_RenderDrawLines(g_ren,points.data(),pointCount);
}

static void drawXmbSparkles(float time) {
  for(int index=0;index<42;index++){
    float x=fmodf(index*0.618034f+time*(0.0022f+(index%5)*0.00045f),1.08f)-0.04f;
    float y=xmbWaveY(x,time,0.585f,0.095f,0.91f,0.075f,0.4f)+
            (fmodf(index*0.413f,1.f)-0.5f)*0.31f;
    float pulse=0.5f+0.5f*sinf(time*(0.55f+(index%7)*0.08f)+index*1.731f);
    Uint8 alpha=(Uint8)(28.f+pulse*(index%9==0?142.f:82.f));
    int px=(int)(x*SW),py=(int)(y*SH);
    fillRect(px,py,index%9==0?3:2,index%9==0?3:2,(SDL_Color){220,246,255,alpha});
    if(index%9==0&&pulse>0.55f){
      SDL_SetRenderDrawColor(g_ren,235,251,255,(Uint8)(alpha*0.62f));
      SDL_RenderDrawLine(g_ren,px-5,py+1,px+7,py+1);
      SDL_RenderDrawLine(g_ren,px+1,py-5,px+1,py+7);
    }
  }
}

static void drawXmbBackground(float time) {
  const SDL_Color top={3,37,102,255},middle={8,93,184,255},bottom={0,20,68,255};
  constexpr int bands=72;
  for(int band=0;band<bands;band++){
    float y=(band+0.5f)/bands;
    SDL_Color color{};
    if(y<0.52f){
      float amount=y/0.52f;
      color={blendChannel(top.r,middle.r,amount),blendChannel(top.g,middle.g,amount),blendChannel(top.b,middle.b,amount),255};
    } else {
      float amount=(y-0.52f)/0.48f;
      color={blendChannel(middle.r,bottom.r,amount),blendChannel(middle.g,bottom.g,amount),blendChannel(middle.b,bottom.b,amount),255};
    }
    int y0=band*SH/bands,y1=(band+1)*SH/bands;
    fillRect(0,y0,SW,y1-y0,color);
  }
  if(g_glowTexture){
    drawGlow(0.10f,0.43f,1.18f,55,157,255,54);
    drawGlow(0.84f,0.38f,0.92f,41,112,228,42);
  }
  drawXmbRibbon(time,0.655f,0.082f,0.78f,-0.105f,2.15f,std::max(12,SH/18),(SDL_Color){63,166,255,31});
  drawXmbRibbon(time,0.575f,0.074f,0.96f,0.080f,0.35f,std::max(10,SH/25),(SDL_Color){189,235,255,48});
  drawXmbRibbon(time,0.605f,0.049f,1.28f,-0.025f,3.82f,std::max(5,SH/54),(SDL_Color){230,250,255,72});
  for(int trace=0;trace<9;trace++){
    float offset=(trace-4)*0.009f;
    drawXmbFilament(time,0.588f+offset,0.083f+trace*0.0017f,0.91f,0.052f,
                    0.62f+trace*0.19f,(SDL_Color){202,241,255,(Uint8)(18+trace%3*8)});
  }
  drawXmbFilament(time,0.578f,0.073f,0.96f,0.080f,0.35f,(SDL_Color){243,253,255,136});
  drawXmbSparkles(time);
}

static void drawBubble(int centerX,int centerY,int radius,Uint8 alpha) {
  if(radius<3||alpha==0) return;
  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,90,205,255);
    SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(alpha/5));
    SDL_Rect glow={centerX-radius*2,centerY-radius*2,radius*4,radius*4};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&glow);
  }
  const int segments=24;
  SDL_SetRenderDrawColor(g_ren,124,220,255,alpha);
  std::array<SDL_Point,segments+1> outer{},inner{};
  for(int segment=0;segment<=segments;segment++){
    float angle=segment*6.2831853f/segments;
    float x=cosf(angle),y=sinf(angle);
    outer[segment]={centerX+(int)(x*radius),centerY+(int)(y*radius)};
    inner[segment]={centerX+(int)(x*(radius-1)),centerY+(int)(y*(radius-1))};
  }
  SDL_RenderDrawLines(g_ren,outer.data(),(int)outer.size());
  SDL_RenderDrawLines(g_ren,inner.data(),(int)inner.size());
  SDL_SetRenderDrawColor(g_ren,235,252,255,(Uint8)std::min(255,(int)alpha+55));
  std::array<SDL_Point,6> highlight{};
  for(int segment=0;segment<(int)highlight.size();segment++){
    float angle=3.55f+segment*0.13f;
    highlight[segment]={centerX+(int)(cosf(angle)*radius),centerY+(int)(sinf(angle)*radius)};
  }
  SDL_RenderDrawLines(g_ren,highlight.data(),(int)highlight.size());
}

static void drawBubblesBackground(float time) {
  const SDL_Color top={20,126,169,255},middle={4,54,82,255},bottom={0,5,11,255};
  constexpr int bands=56;
  for(int band=0;band<bands;band++){
    float y=(band+0.5f)/bands;
    SDL_Color color{};
    if(y<0.58f){
      float amount=y/0.58f;
      color={blendChannel(top.r,middle.r,amount),blendChannel(top.g,middle.g,amount),blendChannel(top.b,middle.b,amount),255};
    } else {
      float amount=(y-0.58f)/0.42f;
      color={blendChannel(middle.r,bottom.r,amount),blendChannel(middle.g,bottom.g,amount),blendChannel(middle.b,bottom.b,amount),255};
    }
    int y0=band*SH/bands,y1=(band+1)*SH/bands;
    fillRect(0,y0,SW,y1-y0,color);
  }

  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,118,225,255);
    SDL_SetTextureAlphaMod(g_glowTexture,105);
    SDL_Rect surface={-SW/6,-SH/3,SW*4/3,SH*2/3};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&surface);
    for(int ray=0;ray<7;ray++){
      float sway=sinf(time*(0.10f+ray*0.013f)+ray*1.31f);
      int width=SW*(11+(ray%3)*3)/100;
      int x=SW*(8+ray*14)/100+(int)(sway*SW*0.025f)-width/2;
      SDL_Rect shaft={x,-SH/3,width,SH*4/3};
      SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(23+(ray%3)*7));
      SDL_RenderCopyEx(g_ren,g_glowTexture,nullptr,&shaft,-9.0+ray*2.7+sway*2.0,nullptr,SDL_FLIP_NONE);
    }
  }

  for(int index=0;index<18;index++){
    float progress=fmodf(index*0.173f+time*(0.038f+(index%5)*0.007f),1.18f);
    float y=1.08f-progress;
    float x=0.05f+fmodf(index*0.283f,0.90f)+0.032f*sinf(time*(0.31f+(index%4)*0.04f)+index);
    float fade=std::min(std::clamp((1.10f-y)*5.f,0.f,1.f),std::clamp((y+0.12f)*6.f,0.f,1.f));
    int radius=(int)(SH*(0.009f+(index%6)*0.0042f));
    if(index%11==0) radius=radius*3/2;
    drawBubble((int)(x*SW),(int)(y*SH),radius,(Uint8)(fade*(85+(index%4)*24)));
  }
  drawBackgroundParticles(time,(SDL_Color){164,228,255,62},24,0.008f);
}

static void clearUiBackground() {
  SDL_RenderSetClipRect(g_ren,nullptr);
  SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255);
  SDL_RenderClear(g_ren);
  if(!hasAnimatedBackground()) return;
  ensureGlowTexture();
  float time=g_uiAnimations?SDL_GetTicks()/1000.f:0.f;
  if(g_launcherTheme==LauncherTheme::Xmb){
    drawXmbBackground(time);
    if(g_glowTexture){ SDL_SetTextureColorMod(g_glowTexture,255,255,255); SDL_SetTextureAlphaMod(g_glowTexture,255); }
    return;
  }
  if(g_launcherTheme==LauncherTheme::Bubbles){
    drawBubblesBackground(time);
    if(g_glowTexture){ SDL_SetTextureColorMod(g_glowTexture,255,255,255); SDL_SetTextureAlphaMod(g_glowTexture,255); }
    return;
  }
  if(!g_glowTexture) return;
  drawGlow(0.10f+0.13f*sinf(time*0.43f),0.20f+0.11f*cosf(time*0.37f),0.90f,45,140,255,128);
  drawGlow(0.84f+0.12f*cosf(time*0.34f),0.34f+0.10f*sinf(time*0.41f),0.78f,154,75,255,112);
  drawGlow(0.54f+0.10f*sinf(time*0.29f),0.91f+0.06f*cosf(time*0.33f),0.94f,0,210,190,94);
  drawGlow(0.42f+0.08f*cosf(time*0.25f),0.48f+0.09f*sinf(time*0.31f),0.58f,64,125,255,67);
  drawBackgroundParticles(time,(SDL_Color){182,224,255,88},28,0.011f);
  SDL_SetTextureColorMod(g_glowTexture,255,255,255);
  SDL_SetTextureAlphaMod(g_glowTexture,255);
}

static void glassPanel(int x,int y,int width,int height) {
  fillRect(x,y,width,height,COL_PANEL);
  border(x,y,width,height,1,(SDL_Color){255,255,255,(Uint8)(hasAnimatedBackground()?28:16)});
}

static void drawText(TTF_Font*f,int x,int y,const char*s,SDL_Color c){
  if(!f||!s||!*s) return;
  TextKey key{f,packColor(c),s};
  auto found=g_textCache.find(key);
  if(found!=g_textCache.end()){
    found->second.use=++g_textUseSerial;
    SDL_Rect d={x,y,found->second.width,found->second.height};
    SDL_RenderCopy(g_ren,found->second.texture,nullptr,&d);
    return;
  }
  SDL_Surface*sf=TTF_RenderUTF8_Blended(f,s,c); if(!sf) return;
  SDL_Texture*t=SDL_CreateTextureFromSurface(g_ren,sf);
  int w=sf->w,h=sf->h; SDL_FreeSurface(sf);
  if(!t) return;
  rememberTextMetric(f,s,w);
  const size_t bytes=(size_t)w*(size_t)h*4;
  if(bytes<=TEXT_CACHE_BYTES){
    evictTextEntries(bytes);
    TextEntry entry{t,w,h,bytes,++g_textUseSerial};
    auto inserted=g_textCache.emplace(std::move(key),entry);
    g_textCacheBytes+=bytes;
    SDL_Rect d={x,y,w,h}; SDL_RenderCopy(g_ren,inserted.first->second.texture,nullptr,&d);
  } else {
    SDL_Rect d={x,y,w,h}; SDL_RenderCopy(g_ren,t,nullptr,&d); SDL_DestroyTexture(t);
  }
}
static int textW(TTF_Font*f,const char*s){
  if(!f||!s||!*s) return 0;
  MetricKey key{f,s}; auto found=g_metricCache.find(key);
  if(found!=g_metricCache.end()){ found->second.use=++g_textUseSerial; return found->second.width; }
  int w=0,h=0; if(TTF_SizeUTF8(f,s,&w,&h)!=0) return 0;
  rememberTextMetric(f,s,w); return w;
}

static const std::string &ellipsizedText(TTF_Font *font, const std::string &text, int maxWidth) {
  EllipsisKey key{font,maxWidth,text};
  auto found=g_ellipsisCache.find(key);
  if(found!=g_ellipsisCache.end()){ found->second.use=++g_textUseSerial; return found->second.text; }

  std::vector<size_t> boundaries{0};
  for(size_t i=0;i<text.size();){
    const unsigned char lead=(unsigned char)text[i];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(i+length>text.size()) length=1;
    for(size_t j=1;j<length;j++) if(((unsigned char)text[i+j]&0xc0)!=0x80){ length=1; break; }
    i+=length; boundaries.push_back(i);
  }
  size_t low=0,high=boundaries.size()-1;
  while(low<high){
    size_t middle=(low+high+1)/2;
    std::string candidate=text.substr(0,boundaries[middle])+"...";
    if(textW(font,candidate.c_str())<=maxWidth) low=middle; else high=middle-1;
  }
  std::string shortened=text.substr(0,boundaries[low])+"...";
  if(g_ellipsisCache.size()>=ELLIPSIS_CACHE_LIMIT){
    auto victim=g_ellipsisCache.begin();
    for(auto it=std::next(g_ellipsisCache.begin());it!=g_ellipsisCache.end();++it)
      if(it->second.use<victim->second.use) victim=it;
    g_ellipsisCache.erase(victim);
  }
  auto inserted=g_ellipsisCache.emplace(std::move(key),EllipsisEntry{std::move(shortened),++g_textUseSerial});
  return inserted.first->second.text;
}
static void drawTextR(TTF_Font*f,int xr,int y,const char*s,SDL_Color c){ drawText(f,xr-textW(f,s),y,s,c); }
static void drawTextC(TTF_Font*f,int cx,int y,const char*s,SDL_Color c){ drawText(f,cx-textW(f,s)/2,y,s,c); }

static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col);
static void downloadAllCovers();
static void toast(const char *msg);
static void modalMessage(const char *title, const std::vector<std::string> &lines);
static bool confirmBox(const char *title, const std::vector<std::string> &lines);
static void runUpdateScreen();
static std::string installedReleaseTag();
static void pollUpdateNotification();
static void drawUpdateNotification();
static int dropdown(const char *title, const char *const *labels, int n, int cur);
static void beginScreenFx();
static void drawFadeIn();
static int topBarH();
static void drawHeader(const char *title,const char *ctx);
static void drawScrollTextR(TTF_Font *font,int xRight,int y,int maxWidth,const char *text,SDL_Color color);
static void drawScrollTextL(TTF_Font *font,int x,int y,int maxWidth,const char *text,SDL_Color color);
static void drawWrapped(TTF_Font *font,int x,int y,int maxWidth,int lineHeight,int maxLines,const char *text,SDL_Color color);
static SDL_Texture *loadScaledTexture(const std::string &path,int width,int height);
static bool g_rescanAfterSettings = false;

static SDL_Texture *g_flag[4] = { nullptr, nullptr, nullptr, nullptr };
static void fillCircle(int cx,int cy,int r,SDL_Color c){
  SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a);
  for(int dy=-r;dy<=r;dy++){ int dx=(int)(sqrt((double)(r*r-dy*dy))+0.5); SDL_RenderDrawLine(g_ren,cx-dx,cy+dy,cx+dx,cy+dy); }
}
static SDL_Texture *makeFlagTex(int region,int W,int H){
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  if(region==3){
    fillRect(0,0,W,H,(SDL_Color){245,245,245,255});
    fillCircle(W/2,H/2,H*30/100,(SDL_Color){188,0,45,255});
  } else if(region==1){
    for(int i=0;i<7;i++) fillRect(0,i*H/7,W,H/7+1,(i%2)?(SDL_Color){235,235,235,255}:(SDL_Color){178,34,52,255});
    fillRect(0,0,W*2/5,(H*4)/7,(SDL_Color){45,50,110,255});
    for(int ry=0;ry<2;ry++)for(int cc=0;cc<3;cc++) fillRect(5+cc*(W*2/5-8)/3,4+ry*8,2,2,(SDL_Color){255,255,255,255});
  } else if(region==2){
    fillRect(0,0,W,H,(SDL_Color){0,51,153,255});
    for(int i=0;i<12;i++){ double a=i*6.28318/12.0; int sx=W/2+(int)(cos(a)*W*0.30), sy=H/2+(int)(sin(a)*H*0.32);
      fillRect(sx-1,sy-1,2,2,(SDL_Color){255,204,0,255}); }
  }
  SDL_SetRenderTarget(g_ren,nullptr);
  return t;
}
static void makeFlags(){ g_flag[1]=makeFlagTex(1,36,24); g_flag[2]=makeFlagTex(2,36,24); g_flag[3]=makeFlagTex(3,36,24); }

static SDL_Texture *g_gA=nullptr,*g_gB=nullptr,*g_gX=nullptr,*g_gY=nullptr,
                   *g_gPlus=nullptr,*g_gL=nullptr,*g_gR=nullptr;
// Supersampling keeps the downscaled glyphs crisp.
static const int GLYPH_SS = 3;
static SDL_Texture *makeGlyph(const char *label, bool pill){
  if(!g_font_sm || !g_font_big) return nullptr;
  const int S=GLYPH_SS, base=TTF_FontHeight(g_font_sm)+6;
  int H=base*S, W=(pill? base*8/5 : base)*S;
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  SDL_Color edge={14,16,22,255}, hi={92,99,114,255}, face={52,57,68,255}, ink={246,248,252,255};
  if(pill){
    int r=H/2;
    fillCircle(r,r,r,edge);     fillCircle(W-r,r,r,edge);     fillRect(r,0,W-2*r,H,edge);
    fillCircle(r,r,r-S,hi);     fillCircle(W-r,r,r-S,hi);     fillRect(r,S,W-2*r,H-2*S,hi);
    fillCircle(r,r,r-S*2,face); fillCircle(W-r,r,r-S*2,face); fillRect(r,S*2,W-2*r,H-S*4,face);
  } else {
    int R=H/2;
    fillCircle(W/2,H/2,R,edge);
    fillCircle(W/2,H/2,R-S,hi);
    fillCircle(W/2,H/2,R-S*2,face);
  }
  SDL_Surface *sf=TTF_RenderUTF8_Blended(g_font_big,label,ink);
  if(sf){ SDL_Texture *lt=SDL_CreateTextureFromSurface(g_ren,sf);
    if(lt) SDL_SetTextureBlendMode(lt,SDL_BLENDMODE_BLEND);
    int inner=H*56/100, lw=sf->w, lh=sf->h;
    if(lh>0){ lw=lw*inner/lh; lh=inner; }
    SDL_Rect d={(W-lw)/2,(H-lh)/2,lw,lh}; SDL_FreeSurface(sf);
    if(lt){ SDL_RenderCopy(g_ren,lt,nullptr,&d); SDL_DestroyTexture(lt); } }
  SDL_SetRenderTarget(g_ren,nullptr);
  return t;
}
static void makeGlyphs(){
  g_gA=makeGlyph("A",false); g_gB=makeGlyph("B",false);
  g_gX=makeGlyph("X",false); g_gY=makeGlyph("Y",false);
  g_gPlus=makeGlyph("+",false); g_gL=makeGlyph("L",true); g_gR=makeGlyph("R",true);
}

enum FootAct { FA_NONE, FA_LAUNCH, FA_SORT, FA_OPTIONS, FA_SETTINGS, FA_PAGEL, FA_PAGER, FA_QUIT };
struct FootItem { const char *button; const char *label; int act; };
static SDL_Rect g_footHit[10]; static int g_footAct[10]; static int g_footN=0;

static SDL_Texture *buttonGlyph(const char *button){
  if(!button) return nullptr;
  if(!strcmp(button,"A")) return g_gA;
  if(!strcmp(button,"B")) return g_gB;
  if(!strcmp(button,"X")) return g_gX;
  if(!strcmp(button,"Y")) return g_gY;
  if(!strcmp(button,"+")) return g_gPlus;
  if(!strcmp(button,"L")) return g_gL;
  if(!strcmp(button,"R")) return g_gR;
  return nullptr;
}

static void buttonHintSize(const char *button,int &width,int &height){
  SDL_Texture *glyph=buttonGlyph(button);
  width=height=0;
  if(glyph){
    SDL_QueryTexture(glyph,nullptr,nullptr,&width,&height);
    width/=GLYPH_SS; height/=GLYPH_SS;
  } else if(button&&button[0]) {
    width=textW(g_font_sm,button)+14;
    height=TTF_FontHeight(g_font_sm)+6;
  }
}

static void drawButtonHint(int x,int cy,const char *button,const char *label,SDL_Color color=COL_DIM){
  SDL_Texture *glyph=buttonGlyph(button);
  int width=0,height=0; buttonHintSize(button,width,height);
  if(glyph){
    SDL_Rect destination={x,cy-height/2,width,height};
    SDL_RenderCopy(g_ren,glyph,nullptr,&destination);
  } else if(button&&button[0]) {
    border(x,cy-height/2,width,height,1,COL_DIM);
    drawTextC(g_font_sm,x+width/2,cy-TTF_FontHeight(g_font_sm)/2,button,COL_TXT);
  }
  if(label&&label[0]) drawText(g_font_sm,x+width+8,cy-TTF_FontHeight(g_font_sm)/2,label,color);
}

static void drawButtonHintCentered(int cx,int cy,const char *button,const char *label,SDL_Color color=COL_DIM){
  int width=0,height=0; buttonHintSize(button,width,height);
  const int labelWidth=label&&label[0]?8+textW(g_font_sm,label):0;
  drawButtonHint(cx-(width+labelWidth)/2,cy,button,label,color);
}

static void drawFooterHints(const FootItem *it,int n,int cy){
  const int gap=8, pairGap=26, glyphGap=16, fh=TTF_FontHeight(g_font_sm);
  int total=0;
  for(int i=0;i<n;i++){ int gw=0,gh=0; buttonHintSize(it[i].button,gw,gh);
    total+=gw; bool L=it[i].label&&it[i].label[0];
    if(L) total+=gap+textW(g_font_sm,it[i].label);
    if(i<n-1) total+=(L?pairGap:glyphGap); }
  int x=(SW-total)/2; g_footN=0;
  for(int i=0;i<n;i++){ int gw=0,gh=0; buttonHintSize(it[i].button,gw,gh);
    int x0=x;
    drawButtonHint(x,cy,it[i].button,nullptr);
    x+=gw; bool L=it[i].label&&it[i].label[0];
    if(L){ x+=gap; drawText(g_font_sm,x,cy-fh/2,it[i].label,COL_DIM); x+=textW(g_font_sm,it[i].label); }
    if(g_footN<10){ g_footHit[g_footN]={x0-6,cy-gh/2-8,(x-x0)+12,gh+16}; g_footAct[g_footN]=it[i].act; g_footN++; }
    if(i<n-1) x+=(L?pairGap:glyphGap);
  }
}

static void drawSettingsFooter(const char *text,int cy=-1){
  if(!text||!text[0]) return;
  std::vector<std::string> tokens;
  const std::string source=text;
  size_t cursor=0;
  while(cursor<source.size()){
    while(cursor<source.size()&&source[cursor]==' ') cursor++;
    if(cursor>=source.size()) break;
    size_t end=cursor;
    while(end<source.size()&&!(source[end]==' '&&end+1<source.size()&&source[end+1]==' ')) end++;
    std::string token=trim(source.substr(cursor,end-cursor));
    if(!token.empty()) tokens.push_back(std::move(token));
    cursor=end;
    while(cursor<source.size()&&source[cursor]==' ') cursor++;
  }
  std::vector<FootItem> hints;
  hints.reserve(tokens.size()/2);
  for(size_t index=0;index+1<tokens.size();index+=2)
    hints.push_back({tokens[index].c_str(),tokens[index+1].c_str(),FA_NONE});
  if(hints.empty()) drawTextC(g_font_sm,SW/2,cy>=0?cy-TTF_FontHeight(g_font_sm)/2:SH-38,text,COL_DIM);
  else drawFooterHints(hints.data(),(int)hints.size(),cy>=0?cy:SH-26);
}
static int footTapAct(int px,int py){
  for(int i=0;i<g_footN;i++){ SDL_Rect &r=g_footHit[i];
    if(px>=r.x && px<r.x+r.w && py>=r.y && py<r.y+r.h) return g_footAct[i]; }
  return FA_NONE;
}

enum TouchKind { TOUCH_NONE, TOUCH_TAP, TOUCH_SWIPE_L, TOUCH_SWIPE_R, TOUCH_SCROLL_UP, TOUCH_SCROLL_DOWN };
struct TouchG {
  bool active=false, vertical=false;
  SDL_FingerID fid=0;
  float x0=0,y0=0,lastY=0;
  Uint32 t0=0;
};
static TouchG g_touch;
static int g_touchScrollSteps=1;
static TouchKind touchFeed(const SDL_Event &e,int *ox,int *oy){
  const int TAP_MOVE=26, SWIPE_DX=90, SCROLL_STEP=30; const Uint32 TAP_MS=400;
  if(e.type==SDL_FINGERDOWN){
    if(g_touch.active && SDL_GetTicks()-g_touch.t0 < 2000) return TOUCH_NONE;
    g_touch.active=true; g_touch.vertical=false; g_touch.fid=e.tfinger.fingerId;
    g_touch.x0=e.tfinger.x*SW; g_touch.y0=e.tfinger.y*SH; g_touch.lastY=g_touch.y0; g_touch.t0=SDL_GetTicks();
  } else if(e.type==SDL_FINGERMOTION && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    float ux=e.tfinger.x*SW, uy=e.tfinger.y*SH, dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    if(!g_touch.vertical && fabsf(dy)>TAP_MOVE && fabsf(dy)>fabsf(dx)*1.15f) g_touch.vertical=true;
    if(g_touch.vertical){
      float step=uy-g_touch.lastY;
      if(fabsf(step)>=SCROLL_STEP){
        g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(step)/SCROLL_STEP)));
        g_touch.lastY=uy;
        if(ox) *ox=(int)ux;
        if(oy) *oy=(int)uy;
        return step<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
      }
    }
  } else if(e.type==SDL_FINGERUP && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    g_touch.active=false;
    float ux=e.tfinger.x*SW, uy=e.tfinger.y*SH, dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    Uint32 dt=SDL_GetTicks()-g_touch.t0;
    if(ox) *ox=(int)ux;
    if(oy) *oy=(int)uy;
    if(g_touch.vertical || (fabsf(dy)>=55 && fabsf(dy)>fabsf(dx)*1.15f)){
      float remaining=uy-g_touch.lastY;
      if(fabsf(remaining)<18 && g_touch.vertical) return TOUCH_NONE;
      g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(g_touch.vertical?remaining:dy)/SCROLL_STEP)));
      return (g_touch.vertical?remaining:dy)<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
    }
    if(fabsf(dx)>=SWIPE_DX && fabsf(dx)>fabsf(dy)*1.5f) return dx<0?TOUCH_SWIPE_L:TOUCH_SWIPE_R;
    if(fabsf(dx)<=TAP_MOVE && fabsf(dy)<=TAP_MOVE && dt<=TAP_MS) return TOUCH_TAP;
  }
  return TOUCH_NONE;
}

static bool touchScrollList(TouchKind kind,int &sel,int &top,int count,int visible){
  if((kind!=TOUCH_SCROLL_UP && kind!=TOUCH_SCROLL_DOWN) || count<=0) return false;
  const int previous=sel;
  int delta=(kind==TOUCH_SCROLL_UP?1:-1)*g_touchScrollSteps;
  sel=std::max(0,std::min(count-1,sel+delta));
  if(sel<top) top=sel;
  if(sel>=top+visible) top=sel-visible+1;
  if(top<0) top=0;
  if(sel!=previous) uiAudioPlay(UiSound::Navigate);
  return true;
}

static bool g_stickXLatched=false, g_stickYLatched=false;
static char stickNav(const SDL_Event &e){
  const int TH=18000, DZ=8000;
  if(e.type!=SDL_CONTROLLERAXISMOTION) return 0;
  if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX){
    if(!g_stickXLatched && e.caxis.value<-TH){ g_stickXLatched=true; return 'L'; }
    if(!g_stickXLatched && e.caxis.value> TH){ g_stickXLatched=true; return 'R'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickXLatched=false;
  } else if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY){
    if(!g_stickYLatched && e.caxis.value<-TH){ g_stickYLatched=true; return 'U'; }
    if(!g_stickYLatched && e.caxis.value> TH){ g_stickYLatched=true; return 'D'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickYLatched=false;
  }
  return 0;
}
static void pumpStick(const SDL_Event &e){
  char n=stickNav(e); if(!n) return;
  SDL_Event s; memset(&s,0,sizeof(s));
  s.type=SDL_CONTROLLERBUTTONDOWN;
  s.cbutton.button = n=='U'?SDL_CONTROLLER_BUTTON_DPAD_UP : n=='D'?SDL_CONTROLLER_BUTTON_DPAD_DOWN
                   : n=='L'?SDL_CONTROLLER_BUTTON_DPAD_LEFT : SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  SDL_PushEvent(&s);
}

static SDL_GameController *g_pad=nullptr;
static bool g_exitRequested=false;
static int g_navHeld=0;
static Uint32 g_navSince=0,g_navLast=0;

static void openController(int index) {
  if (!g_pad && index >= 0 && SDL_IsGameController(index))
    g_pad = SDL_GameControllerOpen(index);
}

static void closeController() {
  if (!g_pad) return;
  SDL_GameControllerClose(g_pad);
  g_pad = nullptr;
  g_stickXLatched = g_stickYLatched = false;
  g_navHeld = 0;
  g_navSince = g_navLast = 0;
}

static bool beginUiFrame() {
  if (g_exitRequested) return false;
  if (!appletMainLoop()) {
    g_exitRequested = true;
    return false;
  }
  if(g_pad&&!SDL_GameControllerGetAttached(g_pad)) closeController();
  return true;
}

static bool pollUiEvent(SDL_Event &event) {
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      g_exitRequested = true;
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
      openController(event.cdevice.which);
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
      if (g_pad) {
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_pad);
        if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which)
          closeController();
      }
      continue;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
      switch (event.cbutton.button) {
        case BTN_CONFIRM: uiAudioPlay(UiSound::Confirm); break;
        case BTN_CANCEL: uiAudioPlay(UiSound::Back); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
          uiAudioPlay(UiSound::Navigate); break;
        default: break;
      }
    }
    return true;
  }
  return false;
}

static void navRepeat(){
  if(!g_pad||!SDL_GameControllerGetAttached(g_pad)) return;
  const int TH=18000;
  int dir=0;
  if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_UP)   || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_UP;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  Uint32 now=SDL_GetTicks();
  if(dir!=g_navHeld){ g_navHeld=dir; g_navSince=now; g_navLast=now; return; }
  if(!dir) return;
  const Uint32 DELAY=360, RATE=85;
  if(now-g_navSince<DELAY || now-g_navLast<RATE) return;
  g_navLast=now;
  SDL_Event s; memset(&s,0,sizeof(s)); s.type=SDL_CONTROLLERBUTTONDOWN; s.cbutton.button=(Uint8)dir;
  SDL_PushEvent(&s);
}

struct Game {
  std::string path;
  std::string file;
  std::string title;
  std::string key;
  std::string legacyKey;
  SDL_Texture *cover = nullptr;
  Uint32 coverAt = 0;
  Uint64 coverUse = 0;
  bool triedCover = false;
  bool hasCfg = false;
  bool legacyUnique = false;
  bool biosBoot = false;
  int region = 0;
  long long added = 0;
  long long played = 0;
};

static std::string gameCRCFile(const Game &game) {
  return std::string(GAMECRC_DIR) + "/" + game.key + ".ini";
}

static uint32_t loadGameCRC(const Game &game) {
  std::string path = gameCRCFile(game);
  if (!regularFileExists(path) && game.legacyUnique && !game.legacyKey.empty()) {
    const std::string legacy = std::string(GAMECRC_DIR) + "/" + game.legacyKey + ".ini";
    if (regularFileExists(legacy)) path = legacy;
  }
  Store values;
  storeLoad(values, path.c_str());
  const char *text = storeGet(values, "CRC", "");
  if (!text || !*text || strlen(text) > 8) return 0;
  char *end = nullptr;
  errno = 0;
  const unsigned long value = strtoul(text, &end, 16);
  return !errno && end && !*end && value && value <= UINT32_MAX ?
             static_cast<uint32_t>(value) : 0;
}

static std::string cheatFileForCRC(uint32_t crc) {
  char name[32];
  snprintf(name, sizeof(name), "%08X.pnach", crc);
  return std::string(CHEATS_DIR) + "/" + name;
}

static std::vector<Game> g_games;
static Uint64 g_coverUseSerial = 0;
static constexpr size_t COVER_CACHE_LIMIT = 28;

enum { SORT_ALPHA, SORT_RECENT, SORT_ADDED, SORT_COUNT };
static const char *SORT_NAME[SORT_COUNT] = { "A-Z", "Recently played", "Recently added" };
static int g_sort = SORT_ALPHA;
static Store g_recent;
static const char *RECENT_INI = "sdmc:/switch/nethersx2/recent.ini";

static int detectRegion(const std::string &file) {
  std::string tags; int depth = 0;
  for (char c : file) {
    if (c=='('||c=='[') depth++;
    else if (c==')'||c==']') { if (depth) depth--; if (depth==0) tags += '|'; }
    else if (depth) tags += (char)tolower((unsigned char)c);
  }
  auto has = [&](const char *s){ return tags.find(s) != std::string::npos; };
  if (has("japan")||has("ntsc-j")||has("jpn")||has("(j)")) return 3;
  if (has("usa")||has("ntsc-u")||has("america")||has("(u)")) return 1;
  if (has("europe")||has("pal")||has("australia")||has("(uk")||has("france")||
      has("germany")||has("spain")||has("ital")||has("(e)")) return 2;
  std::string l; for (char c : file) l += (char)tolower((unsigned char)c);
  if (l.find("ntsc-j")!=std::string::npos) return 3;
  if (l.find("ntsc-u")!=std::string::npos) return 1;
  return 0;
}
static void applySort() {
  auto cmpTitle = [](const Game &a, const Game &b){ return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; };
  std::sort(g_games.begin(), g_games.end(), [&](const Game &a, const Game &b){
    if (a.biosBoot != b.biosBoot) return a.biosBoot;
    if (g_sort == SORT_RECENT && a.played != b.played) return a.played > b.played;
    if (g_sort == SORT_ADDED  && a.added  != b.added)  return a.added  > b.added;
    return cmpTitle(a, b);
  });
}
static void recordPlayed(const Game &game){
  if (game.biosBoot) return;
  long long seq = atoll(storeGet(g_global,"Wrapper/PlaySeq","0")) + 1;
  char b[24]; snprintf(b,sizeof(b),"%lld",seq);
  storeSet(g_global,"Wrapper/PlaySeq",b);
  storeSet(g_recent,game.key.c_str(),b);
  if (game.legacyUnique && !game.legacyKey.empty())
    storeRemove(g_recent, game.legacyKey.c_str());
}

static bool hasDiscExt(const char *n) {
  const char *e = strrchr(n, '.');
  if (!e) return false;
  static const char *x[] = { ".iso",".chd",".cso",".zso",".bin",".mdf",".img",".gz",".nrg" };
  for (auto s : x) if (!strcasecmp(e, s)) return true;
  return false;
}
static std::string toEmu(const std::string &path) {
  return path.rfind("sdmc:", 0) == 0 ? path.substr(5) : path;
}
static std::string join(const std::string &b, const std::string &n) { std::string r=b; if(!r.empty()&&r.back()=='/') r.pop_back(); return r+"/"+n; }
static std::string foldedKey(std::string key);

static std::string normalizeLocationPath(const std::string &input) {
  std::string path=trim(input);
  if(path.empty()) return {};
  std::string output;
  output.reserve(path.size()+1);
  bool slash=false;
  for(char c:path){
    if(c=='\\') c='/';
    if(c=='/'){
      if(slash) continue;
      slash=true;
    } else slash=false;
    output+=c;
  }
  size_t colon=output.find(':');
  if(colon!=std::string::npos && colon+1==output.size()) output+='/';
  size_t minimum=colon==std::string::npos?1:colon+2;
  while(output.size()>minimum && output.back()=='/') output.pop_back();
  return output;
}

static std::string pathIdentity(const std::string &input) {
  return foldedKey(normalizeLocationPath(input));
}

static std::vector<std::string> loadGameSources() {
  std::vector<std::string> paths;
  if(storeHas(g_global,"Wrapper/GamePathCount")){
    int count=std::max(0,std::min(16,atoi(storeGet(g_global,"Wrapper/GamePathCount","0"))));
    for(int i=0;i<count;i++){
      std::string key="Wrapper/GamePath"+std::to_string(i);
      std::string path=normalizeLocationPath(storeGet(g_global,key.c_str(),""));
      if(!path.empty()) paths.push_back(std::move(path));
    }
  } else {
    std::string legacy=normalizeLocationPath(storeGet(g_global,"Wrapper/GameDir",DEF_GAMEDIR));
    if(!legacy.empty()) paths.push_back(std::move(legacy));
  }
  std::unordered_set<std::string> seen;
  paths.erase(std::remove_if(paths.begin(),paths.end(),[&](const std::string &path){
    return !seen.insert(pathIdentity(path)).second;
  }),paths.end());
  return paths;
}

static void saveGameSources(const std::vector<std::string> &input) {
  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty() && seen.insert(pathIdentity(path)).second && paths.size()<16) paths.push_back(std::move(path));
  }
  storeRemovePrefix(g_global,"Wrapper/GamePath");
  storeSet(g_global,"Wrapper/GamePathCount",std::to_string(paths.size()).c_str());
  for(size_t i=0;i<paths.size();i++){
    std::string key="Wrapper/GamePath"+std::to_string(i);
    storeSet(g_global,key.c_str(),paths[i].c_str());
  }
  storeRemove(g_global,"Wrapper/GameDir");
}

static std::vector<std::string> loadFavoriteFolders() {
  std::vector<std::string> paths;
  int count=std::max(0,std::min(24,atoi(storeGet(g_global,"Browser/FavoriteCount","0"))));
  std::unordered_set<std::string> seen;
  for(int i=0;i<count;i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    std::string path=normalizeLocationPath(storeGet(g_global,key.c_str(),""));
    if(!path.empty() && seen.insert(pathIdentity(path)).second) paths.push_back(std::move(path));
  }
  return paths;
}

static void saveFavoriteFolders(const std::vector<std::string> &input) {
  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty()&&seen.insert(pathIdentity(path)).second&&paths.size()<24) paths.push_back(std::move(path));
  }
  storeRemovePrefix(g_global,"Browser/Favorite");
  storeSet(g_global,"Browser/FavoriteCount",std::to_string(paths.size()).c_str());
  for(size_t i=0;i<paths.size();i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    storeSet(g_global,key.c_str(),paths[i].c_str());
  }
  storeSave(g_global,LAUNCHER_INI);
}

static std::vector<SwitchStorage::SmbShare> loadSmbSharesFromStore() {
  std::vector<SwitchStorage::SmbShare> shares;
  std::unordered_set<std::string> ids;
  int count=std::max(0,std::min(8,atoi(storeGet(g_global,"Storage/SmbCount","0"))));
  for(int i=0;i<count;i++){
    std::string prefix="Storage/Smb"+std::to_string(i);
    SwitchStorage::SmbShare share;
    share.id=storeGet(g_global,(prefix+"Id").c_str(),"");
    share.name=storeGet(g_global,(prefix+"Name").c_str(),"");
    share.server=storeGet(g_global,(prefix+"Server").c_str(),"");
    share.share=storeGet(g_global,(prefix+"Share").c_str(),"");
    share.path=storeGet(g_global,(prefix+"Path").c_str(),"");
    share.user=storeGet(g_global,(prefix+"User").c_str(),"");
    share.password=storeGet(g_global,(prefix+"Password").c_str(),"");
    share.domain=storeGet(g_global,(prefix+"Domain").c_str(),"");
    const char *automatic=storeGet(g_global,(prefix+"AutoMount").c_str(),"true");
    share.autoMount=!strcmp(automatic,"true")||!strcmp(automatic,"1");
    if(!SwitchStorage::SmbRootPath(share.id).empty()&&!share.server.empty()&&!share.share.empty()&&ids.insert(share.id).second)
      shares.push_back(std::move(share));
  }
  return shares;
}

static void saveSmbShares(const std::vector<SwitchStorage::SmbShare> &shares) {
  storeRemovePrefix(g_global,"Storage/Smb");
  storeSet(g_global,"Storage/SmbCount",std::to_string(shares.size()).c_str());
  for(size_t i=0;i<shares.size();i++){
    const auto &share=shares[i]; std::string prefix="Storage/Smb"+std::to_string(i);
    storeSet(g_global,(prefix+"Id").c_str(),share.id.c_str());
    storeSet(g_global,(prefix+"Name").c_str(),share.name.c_str());
    storeSet(g_global,(prefix+"Server").c_str(),share.server.c_str());
    storeSet(g_global,(prefix+"Share").c_str(),share.share.c_str());
    storeSet(g_global,(prefix+"Path").c_str(),share.path.c_str());
    storeSet(g_global,(prefix+"User").c_str(),share.user.c_str());
    storeSet(g_global,(prefix+"Password").c_str(),share.password.c_str());
    storeSet(g_global,(prefix+"Domain").c_str(),share.domain.c_str());
    storeSet(g_global,(prefix+"AutoMount").c_str(),share.autoMount?"true":"false");
  }
  storeSave(g_global,LAUNCHER_INI);
}

static bool isJunkToken(const std::string &tok) {
  std::string l;
  for (char c : tok) l += (char)tolower((unsigned char)c);
  static const char *junk[] = {
    "pal","ntsc","ntsc-u","ntsc-j","ntscu","ntscj","usa","us","europe","eu","japan","jp","jpn",
    "world","korea","asia","multi","multi3","multi5","nkit","redump","proper","unl","disc","cd","dvd",
    "iso","chd","cso","zso","enfrespt",
  };
  for (auto j : junk) if (l == j) return true;
  if (l.size() >= 2 && l[0] == 'v' && isdigit((unsigned char)l[1])) return true;
  return false;
}
static std::string cleanTitle(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o; int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
    else if (!depth) o += (c == '_') ? ' ' : c;
  }
  std::string w; bool sp = true;
  for (char c : o) { if (isspace((unsigned char)c)) { if (!sp) w += ' '; sp = true; } else { w += c; sp = false; } }
  o = trim(w);
  std::string filtered;
  for(size_t start=0;start<o.size();){
    size_t end=o.find(' ',start);
    std::string token=o.substr(start,end==std::string::npos?std::string::npos:end-start);
    if(foldedKey(token)!="enfrespt"){
      if(!filtered.empty()) filtered+=' ';
      filtered+=token;
    }
    if(end==std::string::npos) break;
    start=end+1;
  }
  o=std::move(filtered);
  for (;;) {
    size_t p = o.find_last_of(" -");
    std::string last = (p == std::string::npos) ? o : o.substr(p + 1);
    if (!last.empty() && isJunkToken(last) && p != std::string::npos) {
      o = trim(o.substr(0, p));
      while (!o.empty() && (o.back() == '-' || o.back() == ' ' || o.back() == '.')) o.pop_back();
    } else break;
  }
  return trim(o);
}
static std::string sanitize(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o;
  for (char c : s) o += (isalnum((unsigned char)c) || c=='-'||c=='_') ? c : '_';
  return o;
}

static std::string foldedKey(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return key;
}

static std::string makeGameKey(const std::string &file, const std::string &path) {
  std::string base = sanitize(file);
  if (base.empty()) base = "game";
  if (base.size() > 80) base.resize(80);

  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : pathIdentity(path)) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long)hash);
  return base + suffix;
}

static const char *gameStoreGet(Store &store, const Game &game, const char *def) {
  const char *value = storeGet(store, game.key.c_str(), "");
  if (*value || !game.legacyUnique || game.legacyKey.empty())
    return *value ? value : def;
  return storeGet(store, game.legacyKey.c_str(), def);
}

static bool gameFileExists(const char *dir, const Game &game, const char *extension) {
  if (regularFileExists(std::string(dir) + "/" + game.key + extension))
    return true;
  return game.legacyUnique && !game.legacyKey.empty() &&
         regularFileExists(std::string(dir) + "/" + game.legacyKey + extension);
}

static void scanGames(const std::vector<std::string> &sourcePaths) {
  for (auto &g : g_games) if (g.cover) SDL_DestroyTexture(g.cover);
  g_games.clear();
  g_coverUseSerial = 0;
  std::unordered_set<std::string> seenPaths;
  for (const auto &source : sourcePaths) {
    DIR *d = opendir(source.c_str());
    if(!d) continue;
    struct dirent *e;
    while ((e = readdir(d))) {
      if(e->d_name[0]=='.') continue;
      std::string full = join(source, e->d_name);
      struct stat sst{};
      if (stat(full.c_str(), &sst) != 0 || !S_ISREG(sst.st_mode) || !hasDiscExt(e->d_name)) continue;
      if(!seenPaths.insert(pathIdentity(full)).second) continue;
      Game g;
      g.file = e->d_name;
      g.path = full;
      g.legacyKey = sanitize(g.file);
      g.key = makeGameKey(g.file, full);
      g.added = (long long)sst.st_mtime;
      g_games.push_back(std::move(g));
    }
    closedir(d);
  }

  std::map<std::string, size_t> legacyCounts;
  for (const auto &game : g_games)
    legacyCounts[foldedKey(game.legacyKey)]++;
  for (auto &game : g_games) {
    game.legacyUnique = legacyCounts[foldedKey(game.legacyKey)] == 1;
    const char *customTitle = gameStoreGet(g_titles, game, "");
    game.title = *customTitle ? customTitle : cleanTitle(game.file);
    game.region = detectRegion(game.file);
    game.played = atoll(gameStoreGet(g_recent, game, "0"));
    game.hasCfg = gameFileExists(GAMECFG_DIR, game, ".ini");
  }
  if(strcmp(storeGet(g_global,"Wrapper/ShowPS2BIOS","true"),"false")!=0){
    Game bios;
    bios.title = "PS2 BIOS";
    bios.file = "System Menu";
    bios.key = "__ps2_bios__";
    bios.biosBoot = true;
    g_games.push_back(std::move(bios));
  }
  applySort();
}
static std::string coverPath(const Game &g) {
  return g.biosBoot ? "romfs:/bios-cover.png" : std::string(COVERS_DIR) + "/" + g.key + ".png";
}
static std::string existingCoverPath(const Game &g) {
  const std::string current = coverPath(g);
  if (regularFileExists(current)) return current;
  if (g.legacyUnique && !g.legacyKey.empty()) {
    const std::string legacy = std::string(COVERS_DIR) + "/" + g.legacyKey + ".png";
    if (regularFileExists(legacy)) return legacy;
  }
  return current;
}

static Game *findGameByKey(const std::string &key) {
  for (auto &game : g_games)
    if (game.key == key) return &game;
  Game *match = nullptr;
  for (auto &game : g_games) {
    if (!game.legacyUnique || game.legacyKey != key) continue;
    if (match) return nullptr;
    match = &game;
  }
  return match;
}

static constexpr int COVER_DECODE_BUDGET = 3;
static int g_cover_budget = 1 << 30;

static SDL_Texture *loadCoverTexture(const std::string &path) {
  SDL_Surface *s = IMG_Load(path.c_str());
  if (!s) return nullptr;
  const int MAXW = 360, MAXH = 540;
  int width=s->w,height=s->h;
  if(width>MAXW){ height=(int)((long long)height*MAXW/width); width=MAXW; }
  if(height>MAXH){ width=(int)((long long)width*MAXH/height); height=MAXH; }
  if(width<1) width=1;
  if(height<1) height=1;
  if (width != s->w || height != s->h) {
    SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!d) { SDL_FreeSurface(s); return nullptr; }
    SDL_BlendMode blend=SDL_BLENDMODE_NONE;
    SDL_GetSurfaceBlendMode(s,&blend);
    SDL_SetSurfaceBlendMode(s,SDL_BLENDMODE_NONE);
    const bool scaled=SDL_BlitScaled(s,nullptr,d,nullptr)==0;
    SDL_SetSurfaceBlendMode(s,blend);
    SDL_FreeSurface(s);
    if(!scaled){ SDL_FreeSurface(d); return nullptr; }
    s=d;
  }
  SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, s);
  SDL_FreeSurface(s);
  if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  return t;
}
static SDL_Texture *loadArt(const Game &g) {
  return loadCoverTexture(existingCoverPath(g));
}

static void touchCover(Game &g) {
  if (g.cover) g.coverUse = ++g_coverUseSerial;
}

static void evictLeastRecentlyUsedCover() {
  Game *victim = nullptr;
  for (auto &candidate : g_games)
    if (candidate.cover && (!victim || candidate.coverUse < victim->coverUse)) victim = &candidate;
  if (!victim) return;
  SDL_DestroyTexture(victim->cover);
  victim->cover = nullptr;
  victim->coverUse = 0;
  victim->triedCover = false;
}

static void installCover(Game &g, SDL_Texture *cover) {
  if (!cover) return;
  size_t resident = 0;
  for (const auto &candidate : g_games) if (candidate.cover) resident++;
  if (resident >= COVER_CACHE_LIMIT) evictLeastRecentlyUsedCover();
  g.cover = cover;
  g.coverAt = SDL_GetTicks();
  touchCover(g);
}

static void ensureCover(Game &g) {
  if (g.cover) { touchCover(g); return; }
  if (g.triedCover) return;
  if (g_cover_budget <= 0) return;
  g_cover_budget--;
  g.triedCover = true;
  installCover(g, loadArt(g));
}
static void reloadCover(Game &g) {
  if (g.cover) { SDL_DestroyTexture(g.cover); g.cover = nullptr; }
  g.coverUse = 0;
  g.triedCover = true;
  installCover(g, loadArt(g));
}

static bool promptTextMode(const char *header, const char *initial, char *out, size_t outSize,
                           bool password, bool allowEmpty,
                           const char *subText=nullptr, const char *guideText=nullptr) {
  SwkbdConfig kbd;
  out[0] = 0;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
  if(password) swkbdConfigMakePresetPassword(&kbd); else swkbdConfigMakePresetDefault(&kbd);
  if (header) swkbdConfigSetHeaderText(&kbd, header);
  if (subText) swkbdConfigSetSubText(&kbd, subText);
  if (guideText) swkbdConfigSetGuideText(&kbd, guideText);
  if (initial && *initial) swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, (u32)(outSize - 1));
  Result rc = swkbdShow(&kbd, out, outSize);
  swkbdClose(&kbd);
  return R_SUCCEEDED(rc) && (allowEmpty || out[0]);
}
static bool promptText(const char *header, const char *initial, char *out, size_t outSize) {
  return promptTextMode(header,initial,out,outSize,false,false);
}

struct FileClipboard {
  std::string path;
  bool move=false;
};
static FileClipboard g_fileClipboard;

static bool filesystemRoot(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t colon=normalized.find(':');
  if(colon==std::string::npos) return normalized=="/";
  for(size_t i=colon+1;i<normalized.size();i++) if(normalized[i]!='/') return false;
  return true;
}

static std::string parentFolder(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  if(filesystemRoot(normalized)) return {};
  size_t slash=normalized.find_last_of('/');
  if(slash==std::string::npos) return {};
  size_t colon=normalized.find(':');
  if(colon!=std::string::npos && slash<=colon+1) return normalized.substr(0,colon+2);
  return normalized.substr(0,slash);
}

static std::string fileNameOf(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t slash=normalized.find_last_of('/');
  return slash==std::string::npos?normalized:normalized.substr(slash+1);
}

static std::string humanBytes(uint64_t value) {
  static const char *units[]={"B","KiB","MiB","GiB","TiB"};
  double amount=(double)value;
  size_t unit=0;
  while(amount>=1024.0&&unit+1<sizeof(units)/sizeof(*units)){ amount/=1024.0; unit++; }
  char text[64];
  snprintf(text,sizeof(text),unit==0?"%.0f %s":"%.1f %s",amount,units[unit]);
  return text;
}

static std::string deviceOf(const std::string &path) {
  size_t colon=path.find(':');
  return foldedKey(colon==std::string::npos?std::string{}:path.substr(0,colon));
}

static bool pathAtOrBelow(const std::string &path,const std::string &root) {
  std::string candidate=pathIdentity(path), base=pathIdentity(root);
  if(base.empty()||candidate.size()<base.size()||candidate.compare(0,base.size(),base)!=0) return false;
  if(candidate.size()==base.size()) return true;
  return base.back()=='/'||candidate[base.size()]=='/';
}

static std::string gameLocationLabel(const Game &game) {
  if(game.biosBoot) return "PS2 system menu  \xc2\xb7  BIOS";
  const std::string path=normalizeLocationPath(game.path);
  if(path.empty()) return "Unknown location";
  for(const auto &share:loadSmbSharesFromStore()){
    const std::string root=normalizeLocationPath(SwitchStorage::SmbRootPath(share.id));
    if(!pathAtOrBelow(path,root)) continue;
    std::string relative=path.substr(std::min(path.size(),root.size()));
    while(!relative.empty()&&relative.front()=='/') relative.erase(relative.begin());
    std::string address="SMB: smb://"+share.server+"/"+share.share;
    if(!relative.empty()) address+="/"+relative;
    return address;
  }
  if(path.rfind("sdmc:",0)==0) return "SD: "+path;
  if(path.rfind("ums",0)==0) return "USB: "+path;
  return path;
}

static void replaceSavedPathPrefix(const std::string &oldPath,const std::string &newPath) {
  const std::string normalizedOld=normalizeLocationPath(oldPath);
  const std::string normalizedNew=normalizeLocationPath(newPath);
  const std::string oldIdentity=pathIdentity(normalizedOld);
  auto replace=[&](std::vector<std::string> &paths){
    for(auto &path:paths){
      const std::string normalizedPath=normalizeLocationPath(path);
      const std::string identity=pathIdentity(normalizedPath);
      if(identity==oldIdentity) path=normalizedNew;
      else if(identity.size()>oldIdentity.size() && identity.compare(0,oldIdentity.size(),oldIdentity)==0 && identity[oldIdentity.size()]=='/')
        path=normalizeLocationPath(normalizedNew+normalizedPath.substr(normalizedOld.size()));
    }
  };
  auto sources=loadGameSources(); replace(sources); saveGameSources(sources);
  auto favorites=loadFavoriteFolders(); replace(favorites); saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty() && pathAtOrBelow(g_fileClipboard.path,normalizedOld)){
    const std::string clipboardPath=normalizeLocationPath(g_fileClipboard.path);
    g_fileClipboard.path=normalizeLocationPath(normalizedNew+clipboardPath.substr(normalizedOld.size()));
  }
  g_rescanAfterSettings=true;
}

static void removeSavedPathsBelow(const std::string &root) {
  auto sources=loadGameSources();
  sources.erase(std::remove_if(sources.begin(),sources.end(),[&](const std::string &path){ return pathAtOrBelow(path,root); }),sources.end());
  saveGameSources(sources);
  auto favorites=loadFavoriteFolders();
  favorites.erase(std::remove_if(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathAtOrBelow(path,root); }),favorites.end());
  saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty()&&pathAtOrBelow(g_fileClipboard.path,root)) g_fileClipboard={};
  g_rescanAfterSettings=true;
}

static bool validEntryName(const std::string &name) {
  if(name.empty()||name=="."||name==".."||name.size()>255) return false;
  for(unsigned char c:name) if(c<' '||c=='/'||c=='\\'||c==':') return false;
  return true;
}

static bool removeTreeInternal(const std::string &path) {
  if(filesystemRoot(path)) return false;
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0) return errno==ENOENT;
  if(S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)) return remove(path.c_str())==0;
  if(!S_ISDIR(st.st_mode)) return false;
  DIR *dir=opendir(path.c_str()); if(!dir) return false;
  bool ok=true; struct dirent *entry;
  while(ok&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=removeTreeInternal(join(path,entry->d_name));
  }
  if(closedir(dir)!=0) ok=false;
  return ok&&rmdir(path.c_str())==0;
}

struct TransferState {
  std::atomic<uint64_t> total{0};
  std::atomic<uint64_t> done{0};
  std::string current,error;
  std::vector<unsigned char> buffer=std::vector<unsigned char>(1<<18);
  std::mutex detailMutex;
  std::atomic<bool> cancelled{false};
};

static void setTransferDetail(TransferState &state,const std::string &current,const std::string &error={}) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  if(!current.empty()) state.current=current;
  if(!error.empty()) state.error=error;
}

static std::string transferError(TransferState &state) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  return state.error;
}

static bool transferFrame(TransferState &state) {
  if(!beginUiFrame()){ state.cancelled.store(true); return false; }
  SDL_Event event;
  while(pollUiEvent(event)){
    pumpStick(event);
    int tx=0,ty=0;
    if(touchFeed(event,&tx,&ty)==TOUCH_TAP&&ty>=SH-100) state.cancelled.store(true);
    if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL) state.cancelled.store(true);
  }
  std::string current;
  { std::lock_guard<std::mutex> lock(state.detailMutex); current=state.current; }
  clearUiBackground();
  drawTextC(g_font_big,SW/2,80,"File transfer",COL_HI);
  drawTextC(g_font_sm,SW/2,150,ellipsizedText(g_font_sm,current,SW-180).c_str(),COL_DIM);
  int bw=SW*2/3,bx=(SW-bw)/2,by=SH/2-24,bh=42;
  border(bx,by,bw,bh,2,COL_SEL);
  uint64_t done=state.done.load(std::memory_order_relaxed);
  uint64_t total=state.total.load(std::memory_order_relaxed);
  uint64_t progress=total?std::min(done,total):0;
  int fill=total?(int)((bw-6)*progress/total):0;
  fillRect(bx+3,by+3,fill,bh-6,COL_HI);
  char text[96];
  int percent=total?(int)(progress*100/total):0;
  snprintf(text,sizeof(text),"%d%%  -  %.1f / %.1f MiB",percent,done/1048576.0,total/1048576.0);
  drawTextC(g_font,SW/2,by+66,text,COL_TXT);
  if(state.cancelled.load()) drawTextC(g_font_sm,SW/2,SH-72,"Cancelling...",COL_VAL);
  else drawSettingsFooter("B  Cancel",SH-62);
  SDL_RenderPresent(g_ren);
  return !state.cancelled.load();
}

static bool measureTree(const std::string &path,TransferState &state) {
  if(state.cancelled.load(std::memory_order_relaxed)) return false;
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)){ state.total.fetch_add((uint64_t)st.st_size,std::memory_order_relaxed); return true; }
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  DIR *dir=opendir(path.c_str()); if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&!state.cancelled.load(std::memory_order_relaxed)&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=measureTree(join(path,entry->d_name),state);
  }
  if(closedir(dir)!=0) ok=false;
  return ok;
}

static bool copyFileAtomic(const std::string &source,const std::string &destination,TransferState &state) {
  setTransferDetail(state,fileNameOf(source));
  const std::string partial=destination+".nx-part", backup=destination+".nx-old";
  remove(partial.c_str());
  FILE *input=fopen(source.c_str(),"rb");
  if(!input){ setTransferDetail(state,{},"Could not open the source file"); return false; }
  FILE *output=fopen(partial.c_str(),"wb");
  if(!output){ fclose(input); setTransferDetail(state,{},"Could not create the destination file"); return false; }
  bool ok=true;
  while(ok&&!state.cancelled.load(std::memory_order_relaxed)){
    size_t count=fread(state.buffer.data(),1,state.buffer.size(),input);
    if(count){
      if(fwrite(state.buffer.data(),1,count,output)!=count){ setTransferDetail(state,{},"Write failed; check free space and permissions"); ok=false; break; }
      state.done.fetch_add(count,std::memory_order_relaxed);
    }
    if(count<state.buffer.size()){
      if(ferror(input)){ setTransferDetail(state,{},"Read failed"); ok=false; }
      break;
    }
  }
  if(state.cancelled.load()) ok=false;
  if(ok&&fflush(output)!=0){ setTransferDetail(state,{},"Could not flush the destination file"); ok=false; }
  if(ok&&fsync(fileno(output))!=0){ setTransferDetail(state,{},"Could not commit the destination file"); ok=false; }
  if(fclose(input)!=0&&ok){ setTransferDetail(state,{},"Could not close the source file"); ok=false; }
  if(fclose(output)!=0&&ok){ setTransferDetail(state,{},"Could not close the destination file"); ok=false; }
  if(!ok||state.cancelled.load()){ remove(partial.c_str()); return false; }
  struct stat destinationStat{}; bool existed=stat(destination.c_str(),&destinationStat)==0;
  if(existed){
    struct stat backupStat{};
    if(lstat(backup.c_str(),&backupStat)==0){ setTransferDetail(state,{},"A previous backup file blocks this operation"); remove(partial.c_str()); return false; }
    if(rename(destination.c_str(),backup.c_str())!=0){ setTransferDetail(state,{},"Could not preserve the existing destination"); remove(partial.c_str()); return false; }
  }
  if(rename(partial.c_str(),destination.c_str())!=0){
    if(existed) rename(backup.c_str(),destination.c_str());
    setTransferDetail(state,{},"Could not finalize the copied file"); remove(partial.c_str()); return false;
  }
  if(existed) remove(backup.c_str());
  return true;
}

static bool copyTree(const std::string &source,const std::string &destination,TransferState &state) {
  struct stat st{};
  if(lstat(source.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)) return copyFileAtomic(source,destination,state);
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  if(mkdir(destination.c_str(),0777)!=0){ setTransferDetail(state,{},"Could not create a destination folder"); return false; }
  DIR *dir=opendir(source.c_str());
  if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&!state.cancelled.load()&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=copyTree(join(source,entry->d_name),join(destination,entry->d_name),state);
  }
  if(closedir(dir)!=0&&ok){ setTransferDetail(state,{},"Could not close a source folder"); ok=false; }
  return ok&&!state.cancelled.load();
}

static bool enoughFreeSpace(const std::string &folder,uint64_t bytes) {
  struct statvfs info{};
  if(statvfs(folder.c_str(),&info)!=0||!info.f_frsize) return true;
  return bytes<=static_cast<uint64_t>(info.f_bavail)*info.f_frsize;
}

static bool executePaste(const std::string &folder) {
  if(g_fileClipboard.path.empty()) return false;
  struct stat sourceStat{};
  if(lstat(g_fileClipboard.path.c_str(),&sourceStat)!=0){ modalMessage("Paste failed",{"The copied item is no longer available."}); g_fileClipboard={}; return false; }
  const std::string destination=join(folder,fileNameOf(g_fileClipboard.path));
  if(pathIdentity(destination)==pathIdentity(g_fileClipboard.path) ||
     (S_ISDIR(sourceStat.st_mode)&&pathAtOrBelow(destination,g_fileClipboard.path))){
    modalMessage("Paste failed",{"The destination cannot be inside the source."}); return false;
  }
  struct stat destinationStat{}; bool destinationExists=lstat(destination.c_str(),&destinationStat)==0;
  if(destinationExists&&S_ISDIR(sourceStat.st_mode)){
    modalMessage("Folder already exists",{"Choose another destination or rename the folder first.",destination}); return false;
  }
  if(destinationExists&&!S_ISREG(destinationStat.st_mode)){
    modalMessage("Paste failed",{"The destination is not a regular file."}); return false;
  }
  if(destinationExists&&!confirmBox("Replace existing file?",{fileNameOf(destination),"","The existing file will be replaced."})) return false;

  bool sameDevice=deviceOf(g_fileClipboard.path)==deviceOf(destination);
  if(g_fileClipboard.move&&sameDevice){
    const std::string backup=destination+".nx-old";
    bool preserved=false;
    if(destinationExists){
      struct stat backupStat{};
      if(lstat(backup.c_str(),&backupStat)==0||rename(destination.c_str(),backup.c_str())!=0){ modalMessage("Move failed",{"Could not preserve the existing destination."}); return false; }
      preserved=true;
    }
    if(rename(g_fileClipboard.path.c_str(),destination.c_str())==0){
      if(preserved) remove(backup.c_str());
      replaceSavedPathPrefix(g_fileClipboard.path,destination);
      g_fileClipboard={}; toast("Move complete"); SDL_Delay(700); return true;
    }
    if(preserved) rename(backup.c_str(),destination.c_str());
  }

  TransferState state;
  setTransferDetail(state,"Preparing transfer...");
  bool ok=false;
  std::atomic<bool> complete{false};
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  std::thread worker([&](){
    ok=measureTree(g_fileClipboard.path,state);
    if(ok&&!state.cancelled.load()&&!enoughFreeSpace(folder,state.total.load(std::memory_order_relaxed))){
      setTransferDetail(state,{},"The destination does not have enough available space");
      ok=false;
    }
    if(ok&&!state.cancelled.load()){
      setTransferDetail(state,fileNameOf(g_fileClipboard.path));
      ok=copyTree(g_fileClipboard.path,destination,state);
    }
    complete.store(true,std::memory_order_release);
  });
  while(!complete.load(std::memory_order_acquire)){
    transferFrame(state);
    SDL_Delay(8);
  }
  worker.join();
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  if(!ok&&S_ISDIR(sourceStat.st_mode)) removeTreeInternal(destination);
  if(ok&&g_fileClipboard.move){
    if(removeTreeInternal(g_fileClipboard.path)) replaceSavedPathPrefix(g_fileClipboard.path,destination);
    else { modalMessage("Move incomplete",{"The copy completed, but the original could not be removed completely.","Review both locations before trying again."}); ok=false; }
  }
  if(ok){ g_rescanAfterSettings=true; if(g_fileClipboard.move) g_fileClipboard={}; toast("Transfer complete"); SDL_Delay(700); }
  else if(state.cancelled.load()){ toast("Transfer cancelled"); SDL_Delay(700); }
  else { std::string error=transferError(state); modalMessage("Transfer failed",{error.empty()?"The file transfer could not be completed.":error}); }
  return ok;
}

static bool editSmbShare(SwitchStorage::SmbShare &share,bool creating) {
  SwitchStorage::SmbShare edited=share;
  constexpr int fieldCount=7,saveRow=7,totalRows=8;
  int sel=0;
  bool done=false,saved=false;
  beginScreenFx();

  auto cleanServer=[&](){
    edited.server=trim(edited.server);
    if(edited.server.rfind("smb://",0)==0) edited.server.erase(0,6);
    while(!edited.server.empty()&&edited.server.back()=='/') edited.server.pop_back();
  };
  auto cleanShare=[&](){
    std::string combined=trim(edited.share);
    if(!edited.path.empty()) combined+="/"+edited.path;
    std::replace(combined.begin(),combined.end(),'\\','/');
    while(!combined.empty()&&combined.front()=='/') combined.erase(combined.begin());
    while(!combined.empty()&&combined.back()=='/') combined.pop_back();
    std::string normalized; bool slash=false;
    for(char value:combined){
      if(value=='/'){ if(slash) continue; slash=true; }
      else slash=false;
      normalized+=value;
    }
    size_t separator=normalized.find('/');
    edited.share=trim(normalized.substr(0,separator));
    edited.path=separator==std::string::npos?std::string{}:trim(normalized.substr(separator+1));
  };
  auto sharedFolder=[&](){ return edited.path.empty()?edited.share:edited.share+"/"+edited.path; };
  auto validate=[&](){
    edited.name=trim(edited.name); cleanServer(); cleanShare();
    if(edited.name.empty()){ modalMessage("Display name required",{"Enter a name used to identify this share in NetherSX2."}); return false; }
    if(edited.server.empty()||edited.server.find('/')!=std::string::npos||edited.server.find('\\')!=std::string::npos){
      modalMessage("Invalid SMB server",{"Enter only a host name or IP address.","Example: 192.168.1.20"}); return false;
    }
    bool invalidPath=edited.share.empty()||edited.share.find(':')!=std::string::npos;
    size_t start=0;
    while(!invalidPath&&start<=edited.path.size()){
      size_t slash=edited.path.find('/',start);
      std::string component=trim(edited.path.substr(start,slash==std::string::npos?std::string::npos:slash-start));
      if((component.empty()&&!edited.path.empty())||component=="."||component==".."||component.find(':')!=std::string::npos) invalidPath=true;
      if(slash==std::string::npos) break;
      start=slash+1;
    }
    if(invalidPath){
      modalMessage("Invalid SMB share",{"Enter a share name, optionally followed by folders.","Do not include a drive letter or smb:// prefix."}); return false;
    }
    return true;
  };
  auto editField=[&](int index){
    char value[256]; bool accepted=false;
    if(index==0) accepted=promptTextMode("SMB display name",edited.name.c_str(),value,sizeof(value),false,false,
      "Friendly name shown in the NetherSX2 file browser.","Example: Living room NAS");
    else if(index==1) accepted=promptTextMode("Server or IP address",edited.server.c_str(),value,sizeof(value),false,false,
      "Enter the network host only. Do not include smb:// or a folder.","Example: 192.168.1.20 or NAS.local");
    else if(index==2){ std::string folder=sharedFolder(); accepted=promptTextMode("Shared folder",folder.c_str(),value,sizeof(value),false,false,
      "Enter the share and an optional folder path inside it.","Nested folders are supported."); }
    else if(index==3) accepted=promptTextMode("Username",edited.user.c_str(),value,sizeof(value),false,true,
      "Account used by the SMB server. Leave blank for guest access.","Leave blank for guest");
    else if(index==4) accepted=promptTextMode("Password",edited.password.c_str(),value,sizeof(value),true,true,
      "Password for the SMB account. It is stored in launcher.ini.","Leave blank when no password is required");
    else if(index==5) accepted=promptTextMode("Workgroup",edited.domain.c_str(),value,sizeof(value),false,true,
      "Usually optional on a home network.","Example: WORKGROUP, or leave blank");
    if(!accepted) return;
    if(index==0) edited.name=value;
    else if(index==1){ edited.server=value; cleanServer(); }
    else if(index==2){ edited.share=value; edited.path.clear(); cleanShare(); }
    else if(index==3) edited.user=value;
    else if(index==4) edited.password=value;
    else if(index==5) edited.domain=value;
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel<6) editField(sel);
    else if(sel==6) edited.autoMount=!edited.autoMount;
    else if(validate()){
      if(creating){
        std::unordered_set<std::string> ids;
        for(const auto &existing:loadSmbSharesFromStore()) ids.insert(existing.id);
        uint64_t seed=armGetSystemTick();
        do { char id[17]; snprintf(id,sizeof(id),"%08llx",(unsigned long long)(seed&0xffffffffULL)); edited.id=id; seed=seed*6364136223846793005ULL+1; } while(ids.count(edited.id));
      }
      share=std::move(edited); saved=true; done=true;
    }
  };

  while(!done){
    if(!beginUiFrame()) break;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      int scale=SW>=1600?3:2,rowHeight=27*scale,y0=topBarH()+26;
      int margin=SW>=1600?90:56,helpWidth=SW>=1600?570:420,gap=SW>=1600?44:28;
      int formWidth=SW-margin*2-helpWidth-gap;
      if(touch==TOUCH_TAP){
        if(ty>=SH-42){ done=true; continue; }
        for(int index=0;index<fieldCount;index++) if(tx>=margin&&tx<margin+formWidth&&ty>=y0+index*rowHeight&&ty<y0+(index+1)*rowHeight){ sel=index; activate(); break; }
        int buttonY=y0+fieldCount*rowHeight+10;
        if(tx>=margin&&tx<margin+formWidth&&ty>=buttonY&&ty<buttonY+rowHeight){ sel=saveRow; activate(); }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+totalRows-1)%totalRows;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%totalRows;
      else if((event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)&&sel==6) edited.autoMount=!edited.autoMount;
      else if(event.cbutton.button==BTN_CONFIRM) activate();
      else if(event.cbutton.button==BTN_CANCEL) done=true;
    }

    clearUiBackground();
    drawHeader(creating?"Add SMB network share":"Edit SMB network share",edited.name.empty()?nullptr:edited.name.c_str());
    int scale=SW>=1600?3:2,rowHeight=27*scale,y0=topBarH()+26;
    int margin=SW>=1600?90:56,helpWidth=SW>=1600?570:420,gap=SW>=1600?44:28;
    int formWidth=SW-margin*2-helpWidth-gap,helpX=margin+formWidth+gap;
    int panelHeight=fieldCount*rowHeight+rowHeight+30;
    glassPanel(margin,y0-10,formWidth,panelHeight);
    glassPanel(helpX,y0-10,helpWidth,panelHeight);
    const char *labels[fieldCount]={"Display name","Server / IP address","Shared folder","Username","Password","Workgroup","Connect at startup"};
    std::string password=edited.password.empty()?"Not set":std::string(std::min<size_t>(16,edited.password.size()),'*');
    const std::string values[fieldCount]={
      edited.name.empty()?"Not set":edited.name,
      edited.server.empty()?"Not set":edited.server,
      edited.share.empty()?"Not set":sharedFolder(),
      edited.user.empty()?"Guest":edited.user,
      password,
      edited.domain.empty()?"Optional":edited.domain,
      edited.autoMount?"On":"Off"
    };
    for(int index=0;index<fieldCount;index++){
      int y=y0+index*rowHeight; bool current=sel==index;
      if(current){ fillRect(margin+8,y,formWidth-16,rowHeight-2,COL_FOCUS); fillRect(margin+8,y,5,rowHeight-2,COL_SEL); }
      drawText(g_font_sm,margin+30,y+(rowHeight-TTF_FontHeight(g_font_sm))/2,labels[index],current?COL_VAL:COL_DIM);
      drawScrollTextR(g_font,margin+formWidth-24,y+(rowHeight-TTF_FontHeight(g_font))/2,formWidth/2-30,values[index].c_str(),current?COL_VAL:COL_TXT);
    }
    int buttonY=y0+fieldCount*rowHeight+10; bool buttonSelected=sel==saveRow;
    fillRect(margin+14,buttonY,formWidth-28,rowHeight-4,buttonSelected?COL_FOCUS:COL_CARD);
    if(buttonSelected) border(margin+14,buttonY,formWidth-28,rowHeight-4,2,COL_SEL);
    drawTextC(g_font,margin+formWidth/2,buttonY+(rowHeight-TTF_FontHeight(g_font))/2-2,
              creating?"Connect and save":"Save changes",buttonSelected?COL_VAL:COL_HI);

    static const char *helpTitle[totalRows]={"Display name","Server / IP address","Shared folder","Username","Password","Workgroup","Connect at startup","Save share"};
    static const char *helpLine1[totalRows]={
      "A friendly name shown only in NetherSX2.","The host name or IP of your SMB server.","The share name and optional folder path.","Leave blank when the share allows guests.",
      "The password for the selected account.","Usually optional on home networks.","Reconnect this share when the launcher opens.","Validate the fields and connect to the share."
    };
    static const char *helpLine2[totalRows]={
      "Example: Living room NAS","Example: 192.168.1.20 or NAS.local","Nested folders are supported.","Use the account configured on your NAS or PC.",
      "The value is masked on this screen.","Example: WORKGROUP","Turn this off for manually connected shares.","Connection errors will be shown after saving."
    };
    drawText(g_font_big,helpX+28,y0+22,helpTitle[sel],COL_HI);
    int helpLineHeight=TTF_FontHeight(g_font_sm)+4;
    drawWrapped(g_font_sm,helpX+28,y0+92,helpWidth-56,helpLineHeight,2,helpLine1[sel],COL_TXT);
    drawWrapped(g_font_sm,helpX+28,y0+156,helpWidth-56,helpLineHeight,2,helpLine2[sel],COL_DIM);
    std::string address="smb://"+(edited.server.empty()?std::string("server"):edited.server)+"/"+(edited.share.empty()?std::string("share"):sharedFolder());
    drawText(g_font_sm,helpX+28,y0+210,"Connection preview",COL_DIM);
    drawScrollTextL(g_font,helpX+28,y0+244,helpWidth-56,address.c_str(),COL_VAL);
    drawButtonHint(helpX+28,y0+panelHeight-66,"A","Edit / toggle");
    drawButtonHint(helpX+28,y0+panelHeight-32,"B","Cancel");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
  return saved;
}

static void networkSharesScreen() {
  int sel=0,top=0;
  for(;;){
    auto shares=loadSmbSharesFromStore(); int n=1+(int)shares.size();
    const int listY=112,rowHeight=60; int vis=std::max(1,(SH-listY-58)/rowHeight);
    sel=std::max(0,std::min(sel,n-1)); if(sel<top)top=sel; if(sel>=top+vis)top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48) return;
          for(int row=0;row<vis&&top+row<n;row++){ int y=listY+row*rowHeight; if(ty>=y&&ty<y+rowHeight-4){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(shares.size()>=8){ toast("Maximum of 8 SMB shares"); SDL_Delay(900); continue; }
            SwitchStorage::SmbShare share;
            if(editSmbShare(share,true)){
              shares.push_back(share); saveSmbShares(shares);
              std::string error; if(!SwitchStorage::MountSmb(share,&error)) modalMessage("SMB connection failed",{error});
              sel=(int)shares.size(); rebuild=true;
            }
          } else {
            auto &share=shares[sel-1]; bool mounted=SwitchStorage::IsSmbMounted(share.id);
            const char *actions[]={mounted?"Disconnect":"Connect","Edit","Toggle connect at startup","Remove"};
            int action=dropdown(share.name.c_str(),actions,4,0);
            if(action==0){
              if(mounted) SwitchStorage::UnmountSmb(share.id);
              else { std::string error; if(!SwitchStorage::MountSmb(share,&error)) modalMessage("SMB connection failed",{error}); }
              rebuild=true;
            } else if(action==1){
              SwitchStorage::SmbShare edited=share;
              if(editSmbShare(edited,false)){
                bool reconnect=mounted||edited.autoMount;
                SwitchStorage::UnmountSmb(share.id); share=std::move(edited); saveSmbShares(shares);
                if(reconnect){ std::string error; if(!SwitchStorage::MountSmb(share,&error)) modalMessage("SMB connection failed",{error}); }
                rebuild=true;
              }
            } else if(action==2){ share.autoMount=!share.autoMount; saveSmbShares(shares); rebuild=true; }
            else if(action==3&&confirmBox("Remove SMB share?",{share.name,"","Saved folders on this share will also be removed."})){
              std::string root=SwitchStorage::SmbRootPath(share.id); SwitchStorage::UnmountSmb(share.id);
              shares.erase(shares.begin()+sel-1); saveSmbShares(shares); removeSavedPathsBelow(root);
              sel=std::max(0,sel-1); rebuild=true;
            }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      std::string summary=std::to_string(shares.size())+(shares.size()==1?" saved share":" saved shares");
      drawHeader("SMB network shares",summary.c_str());
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=listY+row*rowHeight; bool current=index==sel;
        if(current){ fillRect(56,y-3,SW-112,rowHeight-4,COL_FOCUS); fillRect(56,y-3,5,rowHeight-4,COL_SEL); }
        if(index==0) drawText(g_font,82,y+(rowHeight-TTF_FontHeight(g_font))/2-2,"[ Add SMB share ]",current?COL_VAL:COL_HI);
        else { const auto &share=shares[index-1]; bool mounted=SwitchStorage::IsSmbMounted(share.id);
          drawText(g_font,82,y,share.name.c_str(),current?COL_VAL:COL_TXT);
          std::string status=mounted?"Connected":(share.autoMount?"Disconnected - auto":"Disconnected");
          drawTextR(g_font_sm,SW-82,y+4,status.c_str(),mounted?(SDL_Color){120,220,120,255}:COL_DIM);
          std::string address="smb://"+share.server+"/"+share.share+(share.path.empty()?std::string{}:"/"+share.path);
          drawText(g_font_sm,82,y+31,ellipsizedText(g_font_sm,address,SW-340).c_str(),COL_DIM); }
      }
      drawSettingsFooter("A  Select       B  Back");
      SDL_RenderPresent(g_ren); SDL_Delay(8);
    }
  }
}

enum class BrowserMode { SelectFolder, Manage };
enum class BrowserItemKind { Use, Up, Paste, Favorite, Directory, File, Location, Smb, ManageSmb };
struct BrowserItem {
  std::string label,path;
  BrowserItemKind kind=BrowserItemKind::File;
  bool directory=false;
  std::string value;
};

static bool ensurePathMounted(const std::string &path) {
  for(const auto &share:loadSmbSharesFromStore()){
    std::string root=SwitchStorage::SmbRootPath(share.id);
    if(pathAtOrBelow(path,root)){
      if(SwitchStorage::IsSmbMounted(share.id)) return true;
      std::string error;
      if(SwitchStorage::MountSmb(share,&error)) return true;
      modalMessage("SMB connection failed",{share.name,error}); return false;
    }
  }
  return true;
}

static bool isUsbStoragePath(const std::string &path) {
  size_t colon=path.find(':');
  if(colon<4) return false;
  if(tolower((unsigned char)path[0])!='u'||tolower((unsigned char)path[1])!='m'||tolower((unsigned char)path[2])!='s') return false;
  for(size_t index=3;index<colon;index++) if(!isdigit((unsigned char)path[index])) return false;
  return true;
}

static bool hasConfiguredUsbSource(const std::vector<std::string> &paths) {
  return std::any_of(paths.begin(),paths.end(),[](const std::string &path){ return isUsbStoragePath(path); });
}

static bool refreshConfiguredUsbSources(std::vector<std::string> &paths) {
  if(!hasConfiguredUsbSource(paths)) return false;
  const auto locations=SwitchStorage::ListUsbLocations();
  bool changed=false;
  for(auto &path:paths){
    if(!isUsbStoragePath(path)) continue;
    struct stat source{};
    if(stat(path.c_str(),&source)==0&&S_ISDIR(source.st_mode)) continue;
    size_t colon=path.find(':');
    std::string relative=colon==std::string::npos?std::string{}:path.substr(colon+1);
    while(!relative.empty()&&relative.front()=='/') relative.erase(relative.begin());
    std::vector<std::string> matches;
    for(const auto &location:locations){
      std::string candidate=normalizeLocationPath(location.path+relative);
      struct stat candidateStat{};
      if(stat(candidate.c_str(),&candidateStat)==0&&S_ISDIR(candidateStat.st_mode)) matches.push_back(std::move(candidate));
    }
    if(matches.size()==1&&pathIdentity(path)!=pathIdentity(matches.front())){
      path=std::move(matches.front());
      changed=true;
    }
  }
  if(changed){ saveGameSources(paths); storeSave(g_global,LAUNCHER_INI); }
  return changed;
}

static void renderUsbForwarderWait() {
  clearUiBackground();
  const int panelWidth=720,panelHeight=220;
  const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
  glassPanel(panelX,panelY,panelWidth,panelHeight);
  border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
  drawTextC(g_font_big,SW/2,panelY+42,"Connecting USB storage",COL_SEL);
  drawTextC(g_font,SW/2,panelY+108,"Waiting for the game drive...",COL_TXT);
  drawTextC(g_font_sm,SW/2,panelY+142,"The game will start automatically",COL_DIM);
  drawSettingsFooter("B  Cancel",panelY+184);
  SDL_RenderPresent(g_ren);
}

static void ensureSavedPathMountedAtStartup(const std::string &path) {
  auto shares=loadSmbSharesFromStore();
  bool changed=false;
  for(auto &share:shares){
    if(pathAtOrBelow(path,SwitchStorage::SmbRootPath(share.id))&&!share.autoMount){
      share.autoMount=true;
      changed=true;
    }
  }
  if(changed) saveSmbShares(shares);
}

static std::vector<BrowserItem> browserItems(const std::string &current,BrowserMode mode,bool &opened) {
  std::vector<BrowserItem> items; opened=true;
  if(current.empty()){
    SwitchStorage::InitializeUsb();
    items.push_back({"SD card","sdmc:/",BrowserItemKind::Location,true,"Internal SD storage"});
    for(const auto &usb:SwitchStorage::ListUsbLocations())
      items.push_back({usb.label,usb.path,BrowserItemKind::Location,true,"USB mass storage"});
    for(const auto &share:loadSmbSharesFromStore()){
      bool mounted=SwitchStorage::IsSmbMounted(share.id);
      std::string label="SMB - "+(share.name.empty()?share.share:share.name)+(mounted?"":" (disconnected)");
      items.push_back({label,SwitchStorage::SmbBrowsePath(share),BrowserItemKind::Smb,true,
                       mounted?"SMB - Connected":"SMB - Connect"});
    }
    for(const auto &favorite:loadFavoriteFolders())
      items.push_back({"Pinned - "+favorite,favorite,BrowserItemKind::Location,true,"Pinned folder"});
    items.push_back({"Manage SMB shares","",BrowserItemKind::ManageSmb,true,"Add / edit / connect"});
    return items;
  }
  if(mode==BrowserMode::SelectFolder) items.push_back({"[ Use this folder ]",current,BrowserItemKind::Use,true,current});
  if(mode==BrowserMode::Manage&&!g_fileClipboard.path.empty())
    items.push_back({std::string("[ Paste ")+(g_fileClipboard.move?"moved":"copied")+" item here ]",current,
                     BrowserItemKind::Paste,true,(g_fileClipboard.move?"Move ":"Copy ")+fileNameOf(g_fileClipboard.path)});
  if(mode==BrowserMode::Manage){
    auto favorites=loadFavoriteFolders();
    bool pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(current); });
    items.push_back({pinned?"[ Unpin this folder ]":"[ Pin this folder ]",current,BrowserItemKind::Favorite,true,
                     pinned?"Remove shortcut":"Keep in Locations"});
  }
  items.push_back({"[ .. locations / parent ]",parentFolder(current),BrowserItemKind::Up,true,"Parent folder"});
  DIR *dir=opendir(current.c_str());
  if(!dir){ opened=false; return items; }
  std::vector<BrowserItem> entries; struct dirent *entry;
  while((entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    std::string path=join(current,entry->d_name); struct stat info{};
    if(stat(path.c_str(),&info)!=0) continue;
    bool directory=S_ISDIR(info.st_mode);
    if(!directory&&mode==BrowserMode::SelectFolder) continue;
    entries.push_back({std::string(entry->d_name)+(directory?"/":""),path,
                       directory?BrowserItemKind::Directory:BrowserItemKind::File,directory,
                       directory?"Folder":humanBytes((uint64_t)info.st_size)});
  }
  closedir(dir);
  std::sort(entries.begin(),entries.end(),[](const BrowserItem &left,const BrowserItem &right){
    if(left.directory!=right.directory) return left.directory>right.directory;
    return strcasecmp(left.label.c_str(),right.label.c_str())<0;
  });
  items.insert(items.end(),std::make_move_iterator(entries.begin()),std::make_move_iterator(entries.end()));
  return items;
}

static bool toggleFavorite(const std::string &path) {
  auto favorites=loadFavoriteFolders(); std::string identity=pathIdentity(path);
  auto iterator=std::find_if(favorites.begin(),favorites.end(),[&](const std::string &entry){ return pathIdentity(entry)==identity; });
  bool pinned=iterator==favorites.end();
  if(pinned){
    if(favorites.size()>=24){ toast("Maximum of 24 pinned folders"); SDL_Delay(900); return false; }
    ensureSavedPathMountedAtStartup(path);
    favorites.push_back(normalizeLocationPath(path));
  }
  else favorites.erase(iterator);
  saveFavoriteFolders(favorites); toast(pinned?"Folder pinned":"Folder unpinned"); SDL_Delay(650); return true;
}

static bool browserActions(const BrowserItem &item,BrowserMode mode) {
  if(item.kind!=BrowserItemKind::Directory&&item.kind!=BrowserItemKind::File&&item.kind!=BrowserItemKind::Use) return false;
  std::vector<std::string> labels;
  if(mode==BrowserMode::Manage){ labels={"Copy","Move","Rename"}; }
  bool canPin=item.directory;
  bool pinned=false;
  if(canPin){
    auto favorites=loadFavoriteFolders();
    pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(item.path); });
    labels.push_back(pinned?"Unpin folder":"Pin folder");
  }
  if(labels.empty()) return false;
  std::vector<const char*> choices; for(const auto &label:labels) choices.push_back(label.c_str());
  int action=dropdown("File options",choices.data(),(int)choices.size(),0);
  if(action<0) return false;
  if(mode==BrowserMode::Manage&&action==0){ g_fileClipboard={item.path,false}; toast("Copied to clipboard"); SDL_Delay(600); return false; }
  if(mode==BrowserMode::Manage&&action==1){ g_fileClipboard={item.path,true}; toast("Move queued"); SDL_Delay(600); return false; }
  if(mode==BrowserMode::Manage&&action==2){
    char name[256]; std::string oldName=fileNameOf(item.path);
    if(!promptText("Rename",oldName.c_str(),name,sizeof(name))) return false;
    std::string newName=trim(name);
    if(!validEntryName(newName)){ modalMessage("Invalid name",{"Names cannot contain /, \\, :, or control characters."}); return false; }
    std::string destination=join(parentFolder(item.path),newName); struct stat st{};
    if(lstat(destination.c_str(),&st)==0){ modalMessage("Rename failed",{"An item with that name already exists."}); return false; }
    if(rename(item.path.c_str(),destination.c_str())!=0){ modalMessage("Rename failed",{strerror(errno)}); return false; }
    replaceSavedPathPrefix(item.path,destination); toast("Renamed"); SDL_Delay(600); return true;
  }
  if(canPin) return toggleFavorite(item.path);
  return false;
}

static std::string runFileBrowser(const std::string &start,BrowserMode mode) {
  std::string current=normalizeLocationPath(start);
  if(!current.empty()&&!ensurePathMounted(current)) current.clear();
  int sel=0,top=0;
  uint64_t locationsGeneration=SwitchStorage::UsbStatusGeneration();
  for(;;){
    bool opened=false; auto items=browserItems(current,mode,opened);
    locationsGeneration=SwitchStorage::UsbStatusGeneration();
    if(!opened){ modalMessage("Folder unavailable",{current,"","The device may be disconnected."}); current.clear(); sel=top=0; continue; }
    int n=(int)items.size(),vis=std::max(1,(SH-178)/46); if(n==0){ current.clear(); continue; }
    sel=std::max(0,std::min(sel,n-1)); if(sel<top)top=sel; if(sel>=top+vis)top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return {};
      const uint64_t currentGeneration=SwitchStorage::UsbStatusGeneration();
      if(currentGeneration!=locationsGeneration){ locationsGeneration=currentGeneration; rebuild=true; break; }
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48){ uiAudioPlay(UiSound::Back); return {}; }
          for(int row=0;row<vis&&top+row<n;row++){ int y=112+row*46; if(ty>=y&&ty<y+42){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL){ if(current.empty()) return {}; current=parentFolder(current); sel=top=0; rebuild=true; }
        else if(event.cbutton.button==BTN_SETTINGS){ if(browserActions(items[sel],mode)) rebuild=true; }
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&mode==BrowserMode::Manage&&!current.empty()&&!g_fileClipboard.path.empty()){ executePaste(current); rebuild=true; }
        else if(event.cbutton.button==BTN_CONFIRM){
          const BrowserItem item=items[sel];
          if(item.kind==BrowserItemKind::Use) return item.path;
          if(item.kind==BrowserItemKind::Paste){ executePaste(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Favorite){ toggleFavorite(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Up){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::ManageSmb){ networkSharesScreen(); sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::Directory){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::Location||item.kind==BrowserItemKind::Smb){
            if(ensurePathMounted(item.path)){ DIR *test=opendir(item.path.c_str()); if(test){ closedir(test); current=item.path; sel=top=0; rebuild=true; } else modalMessage("Location unavailable",{item.path}); }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      const char *title=mode==BrowserMode::Manage?"File manager":"Select game folder";
      drawText(g_font_big,64,30,title,COL_HI);
      drawTextR(g_font_sm,SW-64,48,current.empty()?"Locations":ellipsizedText(g_font_sm,current,SW/2).c_str(),COL_DIM);
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=112+row*46; bool selected=index==sel; const auto &item=items[index];
        if(selected){ fillRect(54,y-3,SW-108,42,COL_FOCUS); fillRect(54,y-3,5,42,COL_SEL); }
        SDL_Color color=item.kind==BrowserItemKind::Use||item.kind==BrowserItemKind::Paste||item.kind==BrowserItemKind::Favorite?COL_HI:(item.directory?COL_TXT:(SDL_Color){120,220,120,255});
        const std::string value=ellipsizedText(g_font_sm,item.value,SW/3);
        const int valueWidth=value.empty()?0:textW(g_font_sm,value.c_str());
        const int labelWidth=std::max(80,SW-180-(valueWidth?valueWidth+32:0));
        drawText(g_font,80,y,ellipsizedText(g_font,item.label,labelWidth).c_str(),selected?COL_VAL:color);
        if(!value.empty()) drawTextR(g_font_sm,SW-80,y+3,value.c_str(),selected?COL_VAL:COL_DIM);
      }
      const char *footer=mode==BrowserMode::Manage?"A  Open       X  Actions       Y  Paste       B  Back":"A  Open / Select       X  Pin       B  Back";
      drawSettingsFooter(footer);
      SDL_RenderPresent(g_ren); SDL_Delay(8);
    }
  }
}

static std::string browseFolder(const std::string &start) {
  return runFileBrowser(start,BrowserMode::SelectFolder);
}

static void runFileManager() {
  runFileBrowser({},BrowserMode::Manage);
}

static const char *optDefault(const Opt &o) {
  if(o.key && !strcmp(o.key,"EmuCore/GS/DisableThreadedPresentation"))
    return !strcmp(iniGet("EmuCore/GS/Renderer","14"),"12") ? "false" : "true";
  return o.def;
}
static int choiceIdx(const Opt &o) {
  const char *cur = iniGet(o.key,optDefault(o));
  for (int i=0;i<o.nch;i++) if (!strcmp(o.ch[i].val, cur)) return i;
  return -1;
}
static bool optEnabled(const Opt &o) {
  if(o.type==OT_STATUS) return true;
  if(o.key && !strncmp(o.key,"Wrapper/LSFG",12) && !lsfgDllInstalled())
    return false;
  if(o.key && !strcmp(o.key,"EmuCore/GS/SkipDuplicateFrames") &&
     !strcmp(iniGet("Wrapper/LSFGEnabled","false"),"true"))
    return false;
  return !o.gateKey || strcmp(iniGet(o.gateKey, ""), o.gateOff) != 0;
}
static void optValue(const Opt &o, char *out, int n) {
  out[0]=0;
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); snprintf(out,n,"%s", i>=0?o.ch[i].label:iniGet(o.key,optDefault(o))); }
  else if (o.type==OT_RANGE) snprintf(out,n,"%s", iniGet(o.key,o.def));
  else if (o.type==OT_SCALED_RANGE) {
    int value=(int)std::lround(std::strtod(iniGet(o.key,o.def),nullptr)*o.multiplier);
    snprintf(out,n,"%d%s",value,o.suffix?o.suffix:"");
  }
  else if (o.type==OT_TEXT){ const char *v=iniGet(o.key,o.def); snprintf(out,n,"%s", (v&&*v)?v:"(auto)"); }
  else if (o.type==OT_HOTKEY){ const char *v=iniGet(o.key,o.def); snprintf(out,n,"%s", (v&&*v)?v:"None"); }
  else if (o.type==OT_STATUS) snprintf(out,n,"%s",lsfgDllInstalled()?"Installed":"Missing");
  else if (o.type==OT_SUBMENU) snprintf(out,n,">");
}
static void optSetChoice(const Opt &o,const char *value) {
  iniSet(o.key,value);
  if(o.key && !strcmp(o.key,"Wrapper/LSFGEnabled") && !strcmp(value,"true"))
    iniSet("EmuCore/GS/SkipDuplicateFrames","true");
}
static void optAdjust(const Opt &o, int dir) {
  if (!optEnabled(o)) return;
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); if(i<0)i=0; i=(i+dir+o.nch)%o.nch; optSetChoice(o,o.ch[i].val); }
  else if (o.type==OT_RANGE){ int v=atoi(iniGet(o.key,o.def))+dir*o.step; if(v<o.lo)v=o.lo; if(v>o.hi)v=o.hi; char b[24]; snprintf(b,sizeof(b),"%d",v); iniSet(o.key,b); }
  else if (o.type==OT_SCALED_RANGE){
    int v=(int)std::lround(std::strtod(iniGet(o.key,o.def),nullptr)*o.multiplier)+dir*o.step;
    if(v<o.lo)v=o.lo;
    if(v>o.hi)v=o.hi;
    char b[24]; snprintf(b,sizeof(b),"%g",(double)v/o.multiplier); iniSet(o.key,b);
  }
  else if (o.type==OT_HOTKEY) iniSet(o.key,"None");
}

static const char *captureButton(SDL_GameController *pad) {
  struct M { SDL_GameControllerButton b; const char *tok; };
  static const M map[] = {
    {SDL_CONTROLLER_BUTTON_B,"A"},{SDL_CONTROLLER_BUTTON_A,"B"},{SDL_CONTROLLER_BUTTON_Y,"X"},{SDL_CONTROLLER_BUTTON_X,"Y"}, // Nintendo labels
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,"L"},{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,"R"},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK,"StickL"},{SDL_CONTROLLER_BUTTON_RIGHTSTICK,"StickR"},
    {SDL_CONTROLLER_BUTTON_START,"Plus"},{SDL_CONTROLLER_BUTTON_BACK,"Minus"},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,"Up"},{SDL_CONTROLLER_BUTTON_DPAD_DOWN,"Down"},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,"Left"},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,"Right"},
  };
  (void)pad;
  Uint32 start = SDL_GetTicks();
  // Ignore the button press that opened this screen.
  SDL_Delay(120);
  SDL_Event e;
  while (SDL_PollEvent(&e)) { /* flush */ }
  for (;;) {
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        for (auto &m : map) if (e.cbutton.button == m.b) return m.tok;
      } else if (e.type == SDL_CONTROLLERAXISMOTION) { // ZL/ZR are analog triggers on Switch
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT  && e.caxis.value > 16000) return "ZL";
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && e.caxis.value > 16000) return "ZR";
      } else if (e.type == SDL_QUIT) return "";
    }
    int remain = 6 - (int)((SDL_GetTicks() - start) / 1000);
    if (remain <= 0) return ""; // timed out -> cancel, keep current binding
    clearUiBackground();
    int pw=780,ph=210,px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big,SW/2,py+50,"Press a button to bind", COL_HI);
    char sub[64]; snprintf(sub,sizeof(sub),"wait %ds to cancel", remain);
    drawTextC(g_font,SW/2,py+126,sub, COL_DIM);
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

static std::string captureButtonCombo(SDL_GameController *pad) {
  struct M { SDL_GameControllerButton button; const char *token; };
  static const M buttons[] = {
    {SDL_CONTROLLER_BUTTON_B,"A"},{SDL_CONTROLLER_BUTTON_A,"B"},
    {SDL_CONTROLLER_BUTTON_Y,"X"},{SDL_CONTROLLER_BUTTON_X,"Y"},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,"L"},{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,"R"},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK,"StickL"},{SDL_CONTROLLER_BUTTON_RIGHTSTICK,"StickR"},
    {SDL_CONTROLLER_BUTTON_START,"Plus"},{SDL_CONTROLLER_BUTTON_BACK,"Minus"},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,"Up"},{SDL_CONTROLLER_BUTTON_DPAD_DOWN,"Down"},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,"Left"},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,"Right"},
  };
  if(!pad) return {};
  constexpr unsigned triggerLeftBit=(unsigned)(sizeof(buttons)/sizeof(*buttons));
  constexpr unsigned triggerRightBit=triggerLeftBit+1;
  auto heldMask=[&](){
    Uint32 mask=0;
    SDL_GameControllerUpdate();
    for(unsigned i=0;i<sizeof(buttons)/sizeof(*buttons);i++)
      if(SDL_GameControllerGetButton(pad,buttons[i].button)) mask|=(Uint32)1u<<i;
    if(SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT)>16000) mask|=(Uint32)1u<<triggerLeftBit;
    if(SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>16000) mask|=(Uint32)1u<<triggerRightBit;
    return mask;
  };
  auto maskText=[&](Uint32 mask){
    std::string value;
    auto append=[&](const char *token){ if(!value.empty()) value+='+'; value+=token; };
    for(unsigned i=0;i<sizeof(buttons)/sizeof(*buttons);i++) if(mask&((Uint32)1u<<i)) append(buttons[i].token);
    if(mask&((Uint32)1u<<triggerLeftBit)) append("ZL");
    if(mask&((Uint32)1u<<triggerRightBit)) append("ZR");
    return value;
  };

  const Uint32 start=SDL_GetTicks();
  bool armed=false,started=false;
  Uint32 captured=0;
  SDL_Event event;
  for(;;){
    while(SDL_PollEvent(&event)) if(event.type==SDL_QUIT) return {};
    const Uint32 held=heldMask();
    if(!armed){
      if(!held) armed=true;
    } else if(held){
      started=true;
      captured|=held;
    } else if(started){
      return maskText(captured);
    }

    int remain=10-(int)((SDL_GetTicks()-start)/1000);
    if(remain<=0) return {};
    clearUiBackground();
    int pw=840,ph=250,px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big,SW/2,py+42,"Hold the Turbo button combo",COL_HI);
    drawTextC(g_font,SW/2,py+104,armed?"Hold every button, then release them":"Release the button used to open this screen",COL_TXT);
    std::string current=maskText(captured|held);
    drawTextC(g_font,SW/2,py+148,current.empty()?"Waiting...":current.c_str(),current.empty()?COL_DIM:COL_VAL);
    char sub[64]; snprintf(sub,sizeof(sub),"%ds to cancel",remain);
    drawTextC(g_font_sm,SW/2,py+204,sub,COL_DIM);
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

static float g_hy = -1;
static Uint32 g_fxT = 0;
static void beginScreenFx(){ g_fxT = SDL_GetTicks(); g_hy = -1; }
static void drawFadeIn(){
  if(!g_uiAnimations) return;
  const int D = 160; int el = (int)(SDL_GetTicks() - g_fxT);
  if (el < D) fillRect(0,0,SW,SH,(SDL_Color){0,0,0,(Uint8)(200*(D-el)/D)});
}
static int topBarH(){ return SW >= 1600 ? 112 : 80; }
static void drawHeader(const char *title, const char *ctx){
  int bandH = topBarH() - 4;
  fillRect(0,0,SW,bandH,COL_PANEL);
  if(!hasAnimatedBackground()) fillRect(0,bandH,SW,2,COL_SEL);
  int lh = bandH - 12;
  if(g_logo){ SDL_Rect ld={26,(bandH-lh)/2,lh,lh}; SDL_RenderCopy(g_ren,g_logo,nullptr,&ld); }
  drawTextC(g_font_big,SW/2,(bandH-TTF_FontHeight(g_font_big))/2,title,COL_VAL);
  if (ctx&&*ctx) {
    int titleRight=SW/2+textW(g_font_big,title)/2;
    int maxWidth=(SW-28)-titleRight-30;
    if(maxWidth>40) drawScrollTextR(g_font_sm,SW-28,(bandH-TTF_FontHeight(g_font_sm))/2,maxWidth,ctx,COL_VAL);
  }
}
static const int ROW_H = 46, LIST_Y0 = 118;
static void listCol(int *colX,int *colW,int *labelX,int *valX){
  int w = SW-180; if (w>980) w=980;
  *colW=w; *colX=(SW-w)/2; *labelX=*colX+40; *valX=*colX+w-40;
}
static int listVis(){ int v=(SH-LIST_Y0-72)/ROW_H; return v<1?1:v; }

static void showHelpCard(const char *section,const char *title,const char *kind,
                         const std::string &description,const char *current,
                         const char *scope) {
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event event;
    while(pollUiEvent(event)){
      pumpStick(event);
      int touchX=0,touchY=0;
      if(touchFeed(event,&touchX,&touchY)==TOUCH_TAP) return;
      if(event.type==SDL_CONTROLLERBUTTONDOWN&&
         (event.cbutton.button==BTN_CONFIRM||
          event.cbutton.button==BTN_CANCEL||
          event.cbutton.button==BTN_SETTINGS)) return;
    }

    clearUiBackground();
    const int panelWidth=std::min(SW-120,1000);
    const int panelHeight=std::min(SH-96,500);
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
    drawText(g_font_sm,panelX+40,panelY+24,
             section&&*section?section:"Settings",COL_DIM);
    drawText(g_font_big,panelX+40,panelY+58,
             title&&*title?title:"Setting help",COL_VAL);

    std::string metadata=kind&&*kind?kind:"Setting";
    if(scope&&*scope){ metadata+="  |  "; metadata+=scope; }
    drawText(g_font_sm,panelX+40,panelY+114,metadata.c_str(),COL_SEL);
    int bodyY=panelY+164;
    if(current&&*current){
      const char *prefix="Current: ";
      drawText(g_font_sm,panelX+40,panelY+146,prefix,COL_DIM);
      drawScrollTextL(g_font_sm,panelX+40+textW(g_font_sm,prefix),panelY+146,
                      panelWidth-80-textW(g_font_sm,prefix),current,COL_TXT);
      bodyY=panelY+198;
    }
    fillRect(panelX+40,bodyY-18,panelWidth-80,2,(SDL_Color){70,78,92,210});
    drawWrapped(g_font,panelX+40,bodyY,panelWidth-80,32,7,
                description.c_str(),COL_TXT);
    FootItem footer[]={{"A","",FA_NONE},{"B","",FA_NONE},{"X","Close",FA_NONE},
                       {"Touch","anywhere to close",FA_NONE}};
    drawFooterHints(footer,4,panelY+panelHeight-30);
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

static void showOptionHelp(const char *section,const Opt &option,
                           const char *scope) {
  SettingHelpInfo help=settingHelpFor(option);
  char value[256]={};
  const char *current=nullptr;
  if(option.type!=OT_SUBMENU){
    optValue(option,value,sizeof(value));
    current=value;
  }
  showHelpCard(section,option.label,help.kind,help.text,current,scope);
}

static const char *settingsScreenDescription(int screen) {
  switch(screen){
    case SCR_GRAPHICS:
      return "Selects the graphics backend, internal resolution, display shape, presentation behavior, texture filtering, on-screen display, and common visual patches.";
    case SCR_ENHANCE:
      return "Contains advanced GS accuracy controls, visual enhancements, custom-texture support, and game-specific rendering workarounds. Higher accuracy often requires more GPU time.";
    case SCR_FRAMEGEN:
      return "Configures Vulkan-only LSFG 2x support for a launch. It interpolates 25/30 FPS sources to the 60 Hz display; native 50/60 FPS sources automatically use protected passthrough to avoid slowing emulation.";
    case SCR_AUDIO:
      return "Controls final volume, SPU2 interpolation, synchronization, and buffering. Smaller buffers reduce audio delay but are less tolerant of uneven performance.";
    case SCR_EMU:
      return "Selects the Android core and controls fastmem, PS2 firmware language, EE/VU performance settings, boot behavior, frame-rate controls, and compatibility patches.";
    case SCR_ADVANCED:
      return "Exposes the Android core's per-game EE, VU and IOP execution, rounding, clamping, cache, and diagnostic controls. Defaults are strongly recommended unless a game needs a documented override.";
    case SCR_GAMEFIXES:
      return "Provides manual per-game workarounds from NetherSX2 Android. Automatic fixes from GameIndex.yaml remain active; leave these extra switches Off unless a game specifically needs one.";
    case SCR_FRAMERATE:
      return "Controls the frame limiter, normal/turbo/slow-motion target speeds, and base NTSC/PAL video rates. Base-rate changes can alter game timing.";
    case SCR_NETWORK:
      return "Configures the experimental PS2 network-adapter path and an optional custom DNS server for compatible games or replacement services.";
    case SCR_CONTROLLER:
      return "Configures controller ports, vibration, PS2-to-Switch button and stick mappings, analog deadzone, and the multi-button turbo-speed hotkey.";
    default:
      return "Opens this group of emulator settings.";
  }
}

static void renderSettings(int scr,int sel,int top,const char *ctx){
  clearUiBackground();
  const Screen &S=g_screens[scr];
  drawHeader(S.title, ctx);
  int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
  int vis=listVis();
  glassPanel(colX-12,LIST_Y0-10,colW+24,vis*ROW_H+18);
  int fh0=TTF_FontHeight(g_font);
  float ty = (float)(LIST_Y0 + (sel-top)*ROW_H + 1);
  g_hy = (!g_uiAnimations||g_hy<0) ? ty : g_hy + (ty-g_hy)*0.30f;
  fillRect(colX,(int)g_hy,colW,ROW_H-2,COL_FOCUS);
  fillRect(colX,(int)g_hy,5,ROW_H-2,COL_SEL);
  for(int r=0;r<vis && top+r<S.n;r++){
    int i=top+r,y=LIST_Y0+r*ROW_H+(ROW_H-fh0)/2; bool cur=(i==sel); bool en=optEnabled(S.opts[i]);
    SDL_Color lc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_TXT);
    SDL_Color vc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_DIM);
    drawText(g_font,labelX,y,S.opts[i].label,lc);
    char v[96]; optValue(S.opts[i],v,sizeof(v));
    drawTextR(g_font,valX,y,v,vc);
  }
  if(S.n>vis){
    int trH=vis*ROW_H, trX=colX+colW+16, trY=LIST_Y0-2;
    fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
    int thH=trH*vis/S.n, denom=(S.n-vis>0?S.n-vis:1);
    fillRect(trX,trY+(trH-thH)*top/denom,4,thH,COL_SEL);
  }
  drawSettingsFooter("Left / Right  Change       A  Choose       X  Help       B  Back");
  drawFadeIn();
  SDL_RenderPresent(g_ren);
}

static int dropdown(const char *title, const char *const *labels, int n, int cur) {
  int sel = (cur < 0 || cur >= n) ? 0 : cur, top = 0;
  const int rowH = 52;
  int vis = (SH - 200) / rowH; if (vis < 1) vis = 1; if (vis > n) vis = n;
  beginScreenFx();
  for (;;) {
    if(!beginUiFrame()) return cur;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,top,n,vis)) continue;
        if(tk==TOUCH_TAP){ int pw=SW>760?760:SW-160,px=(SW-pw)/2,ly=(SH-(90+vis*rowH))/2+70;
          for(int r=0;r<vis&&top+r<n;r++){ int y=ly+r*rowH; if(ty>=y&&ty<y+rowH&&tx>=px&&tx<px+pw){ return top+r; } }
        } }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n;   break;
        case BTN_CONFIRM: return sel;
        case BTN_CANCEL:  return cur;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
      if(top<0) top=0;
    }
    clearUiBackground();
    int pw = SW>760?760:SW-160, ph = 90 + vis*rowH, px=(SW-pw)/2, py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big, SW/2, py+18, title, COL_VAL);
    int ly = py+70;
    for(int r=0;r<vis && top+r<n;r++){
      int i=top+r, y=ly+r*rowH; bool curr=(i==sel);
      if(curr){ fillRect(px+8,y,pw-16,rowH-4,COL_FOCUS); fillRect(px+8,y,5,rowH-4,COL_SEL); }
      drawText(g_font, px+34, y+(rowH-TTF_FontHeight(g_font))/2, labels[i], curr?COL_VAL:COL_TXT);
    }
    if(n>vis){ int trH=vis*rowH,trX=px+pw-12,trY=ly; fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
      int thH=trH*vis/n,dn=(n-vis>0?n-vis:1); fillRect(trX,trY+(trH-thH)*top/dn,4,thH,COL_SEL); }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}
static void optChoosePopup(const Opt &o) {
  if(o.type!=OT_CHOICE || o.nch<=0) return;
  const char* labels[32]; int n = o.nch>32?32:o.nch;
  for(int i=0;i<n;i++) labels[i]=o.ch[i].label;
  int idx = dropdown(o.label, labels, n, choiceIdx(o));
  if(idx>=0 && idx<o.nch) optSetChoice(o,o.ch[idx].val);
}

static int s_setSel[SCR_COUNT]={0}, s_setTop[SCR_COUNT]={0};
static void runSettings(int scr, SDL_GameController *pad, const char *ctx) {
  if(scr==SCR_FRAMEGEN){
    normalizeLsfgStore(g_global);
    if(g_active!=&g_global) normalizeLsfgStore(*g_active);
  }
  if(scr==SCR_EMU){
    removeLegacySmcSettings(g_global);
    if(g_active!=&g_global) removeLegacySmcSettings(*g_active);
  }
  const Screen &S=g_screens[scr];
  int sel=s_setSel[scr],top=s_setTop[scr];
  if(sel<0||sel>=S.n) sel=0;
  if(top<0||top>=S.n) top=0;
  while(sel<S.n-1 && !optEnabled(S.opts[sel])) sel++;
  auto nav=[&](int dir){ for(int k=0;k<S.n;k++){ sel=(sel+dir+S.n)%S.n; if(optEnabled(S.opts[sel])) break; } };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        int visible=listVis();
        if(touchScrollList(tk,sel,top,S.n,visible)){ s_setSel[scr]=sel; s_setTop[scr]=top; continue; }
        if(tk==TOUCH_SWIPE_L){ optAdjust(S.opts[sel],-1); continue; }
        if(tk==TOUCH_SWIPE_R){ optAdjust(S.opts[sel],+1); continue; }
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }
          int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX); int vis=listVis();
          for(int r=0;r<vis && top+r<S.n;r++){ int y=LIST_Y0+r*ROW_H;
            if(ty>=y && ty<y+ROW_H){ int ni=top+r; if(optEnabled(S.opts[ni])){ sel=ni;
              if(tx>=colX+colW/2){ SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); } }
              break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   nav(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  optAdjust(S.opts[sel],-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: optAdjust(S.opts[sel], 1); break;
        case BTN_CONFIRM: {
          const Opt &o=S.opts[sel];
          if(o.type==OT_SUBMENU){ runSettings(o.sub,pad,ctx); beginScreenFx(); }
          else if(o.type==OT_TEXT){
            if(optEnabled(o)){
              char buf[128];
              if(promptText(o.label, iniGet(o.key,o.def), buf, sizeof(buf))) iniSet(o.key,buf);
            }
            beginScreenFx();
          }
          else if(o.type==OT_HOTKEY){
            if(optEnabled(o)){
              std::string combo=captureButtonCombo(pad);
              if(!combo.empty()) iniSet(o.key,combo.c_str());
            }
            beginScreenFx();
          }
          else if(S.binds && o.type==OT_CHOICE && o.ch==C_btn){
            const char *tok=captureButton(pad);
            if(tok&&*tok) iniSet(o.key,tok);
            beginScreenFx();
          }
          else if(o.type==OT_CHOICE && o.nch>2 && optEnabled(o)){ optChoosePopup(o); beginScreenFx(); }
          else optAdjust(o,1);
          break;
        }
        case BTN_SETTINGS:
          showOptionHelp(S.title,S.opts[sel],ctx&&*ctx?"Per-game setting":"Global setting");
          beginScreenFx();
          break;
        case BTN_CANCEL: return;
      }
      int vis=listVis(); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1; if(top<0)top=0;
      s_setSel[scr]=sel; s_setTop[scr]=top;
    }
    renderSettings(scr,sel,top,ctx);
    SDL_Delay(8);
  }
}
static std::string launcherUpdateStatusText() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  switch(snapshot.state){
    case LauncherUpdateState::Checking: return "Checking...";
    case LauncherUpdateState::UpdateAvailable: return snapshot.release.tag+" available";
    case LauncherUpdateState::UpToDate: return "Up to date";
    case LauncherUpdateState::Downloading: {
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const uint64_t percent=total?std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      return "Downloading "+std::to_string(percent)+"%";
    }
    case LauncherUpdateState::ReadyToInstall: return "Ready to install";
    case LauncherUpdateState::Installing: return "Installing...";
    case LauncherUpdateState::Installed: return "Ready to exit";
    case LauncherUpdateState::Cancelled: return "Cancelled";
    case LauncherUpdateState::Error: return "Check failed";
    case LauncherUpdateState::Idle: break;
  }
  return std::string("Installed ")+installedReleaseTag();
}

static void launcherSettingsScreen() {
  static int savedSelection=0;
  const int optionCount=(int)(sizeof(S_launcher)/sizeof(Opt));
  const int coversRow=optionCount,listCount=optionCount+1;
  const int updateRow=listCount,selectionCount=listCount+1;
  int sel=std::max(0,std::min(savedSelection,selectionCount-1)),top=0;
  const bool originalShowBios=strcmp(storeGet(g_global,"Wrapper/ShowPS2BIOS","true"),"false")!=0;
  auto applyChange=[&](){
    applyLauncherAppearance();
    uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  };
  auto finish=[&](){
    savedSelection=sel;
    const bool showBios=strcmp(storeGet(g_global,"Wrapper/ShowPS2BIOS","true"),"false")!=0;
    if(showBios!=originalShowBios) g_rescanAfterSettings=true;
    storeSave(g_global,LAUNCHER_INI);
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ finish(); return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      const int visible=std::min(std::max(1,(SH-LIST_Y0-190)/ROW_H),listCount);
      const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
      const int buttonX=(SW-buttonWidth)/2;
      const int buttonY=std::min(SH-buttonHeight-104,LIST_Y0+visible*ROW_H+24);
      if(touchScrollList(touch,sel,top,listCount,visible)) continue;
      if(touch==TOUCH_SWIPE_L&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); continue; }
      if(touch==TOUCH_SWIPE_R&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); continue; }
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ finish(); return; }
        if(tx>=buttonX&&tx<buttonX+buttonWidth&&ty>=buttonY&&ty<buttonY+buttonHeight){
          sel=updateRow;
          SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
          continue;
        }
        for(int row=0;row<visible&&top+row<listCount;row++){
          int y=LIST_Y0+row*ROW_H;
          if(ty>=y&&ty<y+ROW_H){
            sel=top+row;
            SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
            break;
          }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+selectionCount-1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); }
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); }
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==updateRow){ runUpdateScreen(); beginScreenFx(); }
        else if(sel==coversRow){ downloadAllCovers(); beginScreenFx(); }
        else {
          const Opt &option=S_launcher[sel];
          if(option.type==OT_CHOICE&&option.nch>2){ optChoosePopup(option); beginScreenFx(); }
          else optAdjust(option,1);
          applyChange();
        }
      } else if(event.cbutton.button==BTN_SETTINGS){
        if(sel<optionCount)
          showOptionHelp("Launcher",S_launcher[sel],"Launcher setting");
        else if(sel==coversRow)
          showHelpCard("Launcher","Download all covers","Library artwork",
                       "Downloads missing cover artwork for the whole library from SteamGridDB. Existing local covers are kept.",
                       nullptr,"Launcher action");
        else
          showHelpCard("Launcher","Check for Updates","Verified launcher update",
                       "Checks the latest published NetherSX2_nx GitHub release, verifies its SHA-256 digest and NRO structure, then safely replaces this launcher with rollback recovery.",
                       nullptr,"Launcher action");
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL){ finish(); return; }
      if(sel<listCount){
        if(sel<top) top=sel;
        if(sel>=top+visible) top=sel-visible+1;
      }
    }

    clearUiBackground();
    drawHeader("Launcher",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    const int visible=std::min(std::max(1,(SH-LIST_Y0-190)/ROW_H),listCount);
    const int fontHeight=TTF_FontHeight(g_font);
    glassPanel(colX-12,LIST_Y0-10,colW+24,visible*ROW_H+18);
    if(sel<listCount){
      float target=(float)(LIST_Y0+(sel-top)*ROW_H+1);
      g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
      fillRect(colX,(int)g_hy,colW,ROW_H-2,COL_FOCUS);
      fillRect(colX,(int)g_hy,5,ROW_H-2,COL_SEL);
    }
    for(int row=0;row<visible&&top+row<listCount;row++){
      int index=top+row,y=LIST_Y0+row*ROW_H+(ROW_H-fontHeight)/2; bool current=index==sel;
      if(index==coversRow){
        drawText(g_font,labelX,y,"Download all covers",current?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,y+(fontHeight-TTF_FontHeight(g_font_sm))/2,"SteamGridDB",current?COL_VAL:COL_DIM);
      } else {
        drawText(g_font,labelX,y,S_launcher[index].label,current?COL_VAL:COL_TXT);
        char value[96]; optValue(S_launcher[index],value,sizeof(value));
        drawTextR(g_font,valX,y,value,current?COL_VAL:COL_DIM);
      }
    }
    const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
    const int buttonX=(SW-buttonWidth)/2;
    const int buttonY=std::min(SH-buttonHeight-104,LIST_Y0+visible*ROW_H+24);
    const bool updateSelected=sel==updateRow;
    fillRect(buttonX,buttonY,buttonWidth,buttonHeight,updateSelected?COL_FOCUS:(SDL_Color){35,40,50,225});
    border(buttonX,buttonY,buttonWidth,buttonHeight,2,updateSelected?COL_SEL:COL_DIM);
    drawTextC(g_font,SW/2,buttonY+(buttonHeight-fontHeight)/2,"Check for Updates",updateSelected?COL_VAL:COL_TXT);
    const std::string updateStatus=launcherUpdateStatusText();
    drawTextC(g_font_sm,SW/2,buttonY+buttonHeight+8,updateStatus.c_str(),updateSelected?COL_VAL:COL_DIM);
    drawSettingsFooter("Left / Right  Change       A  Choose       X  Help       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static void gameSourcesScreen() {
  int sel=0,top=0;
  for(;;){
    auto sources=loadGameSources(); int n=1+(int)sources.size(); int vis=std::max(1,(SH-176)/50);
    sel=std::max(0,std::min(sel,n-1)); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48) return;
          for(int row=0;row<vis&&top+row<n;row++){ int y=112+row*50; if(ty>=y&&ty<y+46){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(sources.size()>=16){ toast("Maximum of 16 game folders"); SDL_Delay(900); continue; }
            std::string selected=browseFolder({});
            if(!selected.empty()){
              std::string identity=pathIdentity(selected);
              if(std::any_of(sources.begin(),sources.end(),[&](const std::string &path){ return pathIdentity(path)==identity; })){
                toast("Folder already added"); SDL_Delay(800);
              } else {
                ensureSavedPathMountedAtStartup(selected); sources.push_back(selected); saveGameSources(sources); g_rescanAfterSettings=true; sel=(int)sources.size();
              }
              rebuild=true;
            }
          } else {
            const char *actions[]={"Change folder","Move up","Move down","Remove"};
            int action=dropdown("Game folder",actions,4,0); size_t index=(size_t)(sel-1);
            if(action==0){
              std::string selected=browseFolder(sources[index]);
              if(!selected.empty()){
                std::string identity=pathIdentity(selected); bool duplicate=false;
                for(size_t i=0;i<sources.size();i++) if(i!=index&&pathIdentity(sources[i])==identity) duplicate=true;
                if(duplicate){ toast("Folder already added"); SDL_Delay(800); }
                else { ensureSavedPathMountedAtStartup(selected); sources[index]=selected; saveGameSources(sources); g_rescanAfterSettings=true; }
                rebuild=true;
              }
            } else if(action==1&&index>0){ std::swap(sources[index],sources[index-1]); saveGameSources(sources); sel--; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==2&&index+1<sources.size()){ std::swap(sources[index],sources[index+1]); saveGameSources(sources); sel++; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==3&&confirmBox("Remove game folder?",{sources[index],"","No files will be deleted."})){
              sources.erase(sources.begin()+index); saveGameSources(sources); sel=std::max(0,sel-1); g_rescanAfterSettings=true; rebuild=true;
            }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      drawText(g_font_big,64,34,"Game folders",COL_HI);
      drawTextR(g_font_sm,SW-64,52,"All folders are scanned by NetherSX2",COL_DIM);
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=112+row*50; bool current=index==sel;
        if(current){ fillRect(56,y-3,SW-112,46,COL_FOCUS); fillRect(56,y-3,5,46,COL_SEL); }
        std::string label=index==0?"[ Add game folder ]":sources[index-1];
        drawText(g_font,82,y,ellipsizedText(g_font,label,SW-170).c_str(),current?COL_VAL:(index==0?COL_HI:COL_TXT));
      }
      drawSettingsFooter("A  Select       B  Back");
      SDL_RenderPresent(g_ren); SDL_Delay(8);
    }
  }
}

static void libraryStorageScreen() {
  static int savedSelection=0;
  constexpr int rowCount=3,rowHeight=64,startY=126;
  int sel=std::max(0,std::min(savedSelection,rowCount-1));
  auto openRow=[&](){
    if(sel==0) gameSourcesScreen();
    else if(sel==1) runFileManager();
    else networkSharesScreen();
    beginScreenFx();
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ savedSelection=sel; return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ savedSelection=sel; return; }
        for(int row=0;row<rowCount;row++){ int y=startY+row*rowHeight; if(ty>=y&&ty<y+rowHeight){ sel=row; openRow(); break; } }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+rowCount-1)%rowCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%rowCount;
      else if(event.cbutton.button==BTN_CONFIRM) openRow();
      else if(event.cbutton.button==BTN_CANCEL){ savedSelection=sel; return; }
    }

    clearUiBackground();
    drawHeader("Library & storage",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    glassPanel(colX-12,startY-10,colW+24,rowCount*rowHeight+18);
    float target=(float)(startY+sel*rowHeight+2);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,rowHeight-4,COL_FOCUS);
    fillRect(colX,(int)g_hy,5,rowHeight-4,COL_SEL);
    auto shares=loadSmbSharesFromStore(); size_t mounted=0;
    for(const auto &share:shares) if(SwitchStorage::IsSmbMounted(share.id)) mounted++;
    size_t folderCount=loadGameSources().size();
    std::string folderValue=std::to_string(folderCount)+(folderCount==1?" folder":" folders");
    std::string smbValue=std::to_string(mounted)+" / "+std::to_string(shares.size())+" connected";
    const char *labels[rowCount]={"Game folders","File manager","SMB network shares"};
    const char *values[rowCount]={folderValue.c_str(),"SD / USB / SMB",smbValue.c_str()};
    int fontHeight=TTF_FontHeight(g_font),smallHeight=TTF_FontHeight(g_font_sm);
    for(int row=0;row<rowCount;row++){
      int slot=startY+row*rowHeight,y=slot+(rowHeight-fontHeight)/2; bool current=row==sel;
      drawText(g_font,labelX,y,labels[row],current?COL_VAL:COL_TXT);
      drawTextR(g_font_sm,valX,slot+(rowHeight-smallHeight)/2,values[row],current?COL_VAL:COL_DIM);
    }
    drawSettingsFooter("A  Open       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static void runCheatSettings(Game &game,uint32_t crc) {
  const std::string path=crc?cheatFileForCRC(crc):std::string();
  NxCheatList cheats{};
  bool loaded=crc&&nx_cheat_load(path.c_str(),&cheats);
  int sel=0,top=0;
  const int rowH=58,y0=118,vis=std::max(1,(SH-y0-66)/rowH);
  std::string notice;
  beginScreenFx();
  for(;;){
    const int codeCount=loaded?(int)cheats.count:0;
    const int n=codeCount+1;
    if(sel>=n) sel=n-1;
    if(top>sel) top=sel;
    if(sel>=top+vis) top=sel-vis+1;
    if(!beginUiFrame()) return;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touchScrollList(touch,sel,top,n,vis)) continue;
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40) return;
        for(int row=0;row<vis&&top+row<n;row++){
          int index=top+row,y=y0+row*rowH;
          if(ty>=y&&ty<y+rowH){ sel=index; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
      else if(event.cbutton.button==BTN_CANCEL) return;
      else if(event.cbutton.button==BTN_CONFIRM&&sel==codeCount) return;
      else if(sel<codeCount&&(event.cbutton.button==BTN_CONFIRM||
              event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT||
              event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)){
        const NxCheatEntry &entry=cheats.entries[sel];
        bool enable=entry.enabled_count!=entry.patch_count;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT) enable=false;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT) enable=true;
        if(nx_cheat_set_enabled(path.c_str(),(size_t)sel,enable)){
          loaded=nx_cheat_load(path.c_str(),&cheats);
          notice=enable?"Cheat code enabled":"Cheat code disabled";
        } else notice="Could not update the PNACH file";
      }
    }

    clearUiBackground();
    char subtitle[256];
    if(crc) snprintf(subtitle,sizeof(subtitle),"%s  -  CRC %08X",game.title.c_str(),crc);
    else snprintf(subtitle,sizeof(subtitle),"%s",game.title.c_str());
    drawHeader("Cheat codes",subtitle);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    const int shown=std::min(vis,n);
    glassPanel(colX-12,y0-10,colW+24,shown*rowH+18);
    float target=(float)(y0+(sel-top)*rowH+2);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,rowH-4,COL_FOCUS);
    fillRect(colX,(int)g_hy,5,rowH-4,COL_SEL);
    const int fontHeight=TTF_FontHeight(g_font);
    for(int row=0;row<vis&&top+row<n;row++){
      const int index=top+row,slot=y0+row*rowH,y=slot+(rowH-fontHeight)/2;
      const bool current=index==sel;
      if(index==codeCount){
        drawText(g_font,labelX,y,"Back",current?COL_VAL:COL_TXT);
        drawTextR(g_font,valX,y,"<",current?COL_VAL:COL_DIM);
      } else {
        const NxCheatEntry &entry=cheats.entries[index];
        const char *state=!entry.enabled_count?"Off":(entry.enabled_count==entry.patch_count?"On":"Mixed");
        drawScrollTextL(g_font,labelX,y,valX-labelX-130,entry.name,current?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,state,current?COL_VAL:COL_DIM);
      }
    }
    if(!crc){
      drawTextC(g_font,SW/2,SH-150,"Launch this game once so NetherSX2 can identify its CRC.",COL_HI);
    } else if(!loaded){
      drawTextC(g_font,SW/2,SH-176,"The PNACH file could not be read.",COL_HI);
    } else if(!cheats.file_exists){
      drawTextC(g_font,SW/2,SH-184,"No cheat file found. Add this file to the cheats folder:",COL_HI);
      char filename[32]; snprintf(filename,sizeof(filename),"%08X.pnach",crc);
      drawTextC(g_font,SW/2,SH-144,filename,COL_VAL);
    } else if(!cheats.count){
      drawTextC(g_font,SW/2,SH-150,"No PNACH sections or named code blocks were found.",COL_HI);
    } else if(!notice.empty()){
      drawTextC(g_font_sm,SW/2,SH-86,notice.c_str(),notice.find("Could not")==0?(SDL_Color){235,120,120,255}:COL_HI);
    }
    drawSettingsFooter("A  Toggle     Left  Off     Right  On     B  Back",SH-30);
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static bool retroAchievementsLoggedIn() {
  return storeGet(g_global,"Achievements/Username","")[0]&&
         storeGet(g_global,"Achievements/Token","")[0];
}

static bool retroAchievementsSignIn() {
  if(!g_griddbReady){
    modalMessage("RetroAchievements",{"The network service is unavailable.","Check the Switch internet connection and try again."});
    return false;
  }

  const std::string initialUsername=storeGet(g_global,"Achievements/Username","");
  char usernameBuffer[96]{};
  snprintf(usernameBuffer,sizeof(usernameBuffer),"%s",
           initialUsername.c_str());
  if(!promptTextMode("RetroAchievements username",initialUsername.c_str(),
                     usernameBuffer,sizeof(usernameBuffer),false,false,
                     "Sign in to enable Casual achievements.","Username"))
    return false;
  const std::string username=trim(usernameBuffer);
  if(username.empty()||username.find_first_of("\r\n")!=std::string::npos){
    modalMessage("RetroAchievements",{"Enter a valid username."});
    return false;
  }

  char password[192]{};
  if(!promptTextMode("RetroAchievements password","",password,sizeof(password),
                     true,false,"","Password")){
    retroAchievementsClearSecret(password,sizeof(password));
    return false;
  }

  toast("Signing in to RetroAchievements...");
  SDL_RenderPresent(g_ren);
  RetroAchievementsLoginResponse response;
  const RetroAchievementsLoginResult result=
      retroAchievementsLoginWithPassword(username,password,response);
  retroAchievementsClearSecret(password,sizeof(password));
  if(result!=RetroAchievementsLoginResult::Success){
    modalMessage("RetroAchievements sign-in failed",{
      response.error.empty()?"The account could not be signed in.":response.error
    });
    return false;
  }
  if(response.username.find_first_of("\r\n")!=std::string::npos||
     response.token.find_first_of("\r\n")!=std::string::npos||
     response.token.size()>=256){
    modalMessage("RetroAchievements",{"The server returned an invalid account token."});
    return false;
  }

  storeSet(g_global,"Achievements/Username",response.username.c_str());
  storeSet(g_global,"Achievements/Token",response.token.c_str());
  storeSet(g_global,"Achievements/Enabled","true");
  normalizeRetroAchievementsStore(g_global);
  if(!storeSave(g_global,LAUNCHER_INI)){
    modalMessage("RetroAchievements",{"Signed in, but launcher.ini could not be saved."});
    return false;
  }
  syncRetroAchievementsToEmulatorConfig();
  modalMessage("RetroAchievements",{"Signed in as "+response.username+".",
               "Achievements will activate the next time a game starts."});
  return true;
}

static void retroAchievementsScreen() {
  enum { RA_ACCOUNT,RA_ENABLED,RA_PRESENCE,RA_NOTIFICATIONS,
         RA_SIGN_IN,RA_SIGN_OUT,RA_BACK,RA_ROW_COUNT };
  static int savedSelection=0;
  int sel=std::max(0,std::min(savedSelection,RA_ROW_COUNT-1));
  constexpr int rowH=58,y0=110;
  auto setToggle=[&](const char *key,bool value){
    storeSet(g_global,key,value?"true":"false");
    normalizeRetroAchievementsStore(g_global);
    storeSave(g_global,LAUNCHER_INI);
    syncRetroAchievementsToEmulatorConfig();
  };
  auto activate=[&](int direction){
    const bool loggedIn=retroAchievementsLoggedIn();
    if(sel==RA_ACCOUNT||sel==RA_SIGN_IN){
      retroAchievementsSignIn();
    } else if(sel==RA_ENABLED){
      if(!loggedIn){ retroAchievementsSignIn(); return; }
      const bool current=!strcmp(storeGet(g_global,"Achievements/Enabled","false"),"true");
      setToggle("Achievements/Enabled",direction?direction>0:!current);
    } else if(sel==RA_PRESENCE){
      const bool current=!strcmp(storeGet(g_global,"Achievements/RichPresence","true"),"true");
      setToggle("Achievements/RichPresence",direction?direction>0:!current);
    } else if(sel==RA_NOTIFICATIONS){
      const bool current=!strcmp(storeGet(g_global,"Achievements/Notifications","true"),"true");
      setToggle("Achievements/Notifications",direction?direction>0:!current);
    } else if(sel==RA_SIGN_OUT&&loggedIn&&
              confirmBox("Sign out of RetroAchievements?",{
                "The saved account token will be removed.","Achievements will be disabled."})){
      storeSet(g_global,"Achievements/Enabled","false");
      storeSet(g_global,"Achievements/Username","");
      storeSet(g_global,"Achievements/Token","");
      normalizeRetroAchievementsStore(g_global);
      storeSave(g_global,LAUNCHER_INI);
      syncRetroAchievementsToEmulatorConfig();
    }
  };

  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ savedSelection=sel; return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SWIPE_L&&sel>=RA_ENABLED&&sel<=RA_NOTIFICATIONS){ activate(-1); continue; }
      if(touch==TOUCH_SWIPE_R&&sel>=RA_ENABLED&&sel<=RA_NOTIFICATIONS){ activate(1); continue; }
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ savedSelection=sel; return; }
        for(int row=0;row<RA_ROW_COUNT;row++){
          int y=y0+row*rowH;
          if(ty>=y&&ty<y+rowH){ sel=row; if(sel==RA_BACK){ savedSelection=sel; return; } activate(0); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+RA_ROW_COUNT-1)%RA_ROW_COUNT;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%RA_ROW_COUNT;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT&&sel>=RA_ENABLED&&sel<=RA_NOTIFICATIONS) activate(-1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT&&sel>=RA_ENABLED&&sel<=RA_NOTIFICATIONS) activate(1);
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==RA_BACK){ savedSelection=sel; return; }
        activate(0);
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL){ savedSelection=sel; return; }
    }

    const bool loggedIn=retroAchievementsLoggedIn();
    const bool enabled=loggedIn&&!strcmp(storeGet(g_global,"Achievements/Enabled","false"),"true");
    clearUiBackground();
    drawHeader("RetroAchievements",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    glassPanel(colX-12,y0-10,colW+24,RA_ROW_COUNT*rowH+18);
    float target=(float)(y0+sel*rowH+2);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,rowH-4,COL_FOCUS);
    fillRect(colX,(int)g_hy,5,rowH-4,COL_SEL);
    const char *labels[RA_ROW_COUNT]={"Account","Achievements","Rich presence","Notifications",
                                      loggedIn?"Change account":"Sign in","Sign out","Back"};
    const char *values[RA_ROW_COUNT]={
      loggedIn?storeGet(g_global,"Achievements/Username",""):"Not signed in",
      enabled?"On":"Off",
      !strcmp(storeGet(g_global,"Achievements/RichPresence","true"),"true")?"On":"Off",
      !strcmp(storeGet(g_global,"Achievements/Notifications","true"),"true")?"On":"Off",
      ">",loggedIn?">":"Unavailable","<"
    };
    const int fontHeight=TTF_FontHeight(g_font),smallHeight=TTF_FontHeight(g_font_sm);
    for(int row=0;row<RA_ROW_COUNT;row++){
      const int slot=y0+row*rowH,y=slot+(rowH-fontHeight)/2;
      const bool current=row==sel;
      const bool available=row!=RA_SIGN_OUT||loggedIn;
      drawText(g_font,labelX,y,labels[row],current&&available?COL_VAL:(available?COL_TXT:COL_DIM));
      drawTextR(g_font_sm,valX,slot+(rowH-smallHeight)/2,values[row],
                current&&available?COL_VAL:COL_DIM);
    }
    drawSettingsFooter("A  Select       Left / Right  Change       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static void runSettingsRoot(SDL_GameController *pad, const char *ctx, Game *game) {
  const bool global=!(ctx&&*ctx);
  static const int globalOrder[] = { SCR_FRAMEGEN, SCR_EMU, SCR_GRAPHICS, SCR_AUDIO, SCR_NETWORK, SCR_CONTROLLER };
  static const int gameOrder[] = { SCR_FRAMEGEN, SCR_EMU, SCR_ADVANCED, SCR_GAMEFIXES, SCR_GRAPHICS, SCR_AUDIO, SCR_NETWORK, SCR_CONTROLLER };
  const int *order=global?globalOrder:gameOrder;
  const int nscr=global?(int)(sizeof(globalOrder)/sizeof(*globalOrder)):
                         (int)(sizeof(gameOrder)/sizeof(*gameOrder));
  const bool hasCheatRow=!global&&game;
  const int launcherRow=0,libraryRow=1,retroAchievementsRow=2,cheatRow=0;
  const int screenStart=global?3:(hasCheatRow?1:0);
  const int globalTopGroupRows=4;
  const int n=nscr+(global?3:(hasCheatRow?1:0));
  const uint32_t gameCRC=hasCheatRow?loadGameCRC(*game):0;
  int sel=0,top=0;
  const int rowH=58,y0=92,sectionGap=34,vis=std::max(1,(SH-y0-42-sectionGap)/rowH);
  auto rowY=[&](int index){ return y0+(index-top)*rowH+(global&&index>=globalTopGroupRows?sectionGap:0); };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touchScrollList(touch,sel,top,n,vis)) continue;
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40) return;
        for(int row=0;row<vis&&top+row<n;row++){ int index=top+row,y=rowY(index); if(ty>=y&&ty<y+rowH){ sel=index; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
      else if(event.cbutton.button==BTN_CONFIRM){
        if(global&&sel==launcherRow) launcherSettingsScreen();
        else if(global&&sel==libraryRow) libraryStorageScreen();
        else if(global&&sel==retroAchievementsRow) retroAchievementsScreen();
        else if(hasCheatRow&&sel==cheatRow) runCheatSettings(*game,gameCRC);
        else runSettings(order[sel-screenStart],pad,ctx);
        beginScreenFx();
      } else if(event.cbutton.button==BTN_SETTINGS){
        if(global&&sel==launcherRow)
          showHelpCard("Settings","Launcher","Launcher appearance",
                       "Changes the SDL launcher's theme, library grid, labels, animations, sounds, and cover downloads.",
                       nullptr,"Settings category");
        else if(global&&sel==libraryRow)
          showHelpCard("Settings","Library & storage","Game and file management",
                       "Manages scanned game folders, hidden library entries, local and removable files, and SMB network shares used by the launcher.",
                       nullptr,"Settings category");
        else if(global&&sel==retroAchievementsRow)
          showHelpCard("Settings","RetroAchievements","Online achievements",
                       "Signs in to RetroAchievements and controls Casual achievements, rich presence, and unlock notifications. Account credentials are stored as a reusable token after sign-in.",
                       nullptr,"Settings category");
        else if(hasCheatRow&&sel==cheatRow)
          showHelpCard("Game settings","Cheat codes","Per-game PNACH codes",
                       "Reads named cheat sections from the game's CRC-based PNACH file and lets each code be enabled or disabled separately. Launch the game once if its CRC is not known yet.",
                       nullptr,"Per-game setting");
        else {
          const int screen=order[sel-screenStart];
          showHelpCard(global?"Settings":"Game settings",g_screens[screen].title,
                       "Settings category",settingsScreenDescription(screen),nullptr,
                       global?"Global settings":"Per-game overrides");
        }
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL) return;
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
    }

    clearUiBackground();
    drawHeader(global?"Settings":"Game settings",global?nullptr:ctx);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    int shown=std::min(vis,n);
    if(global){
      glassPanel(colX-12,y0-10,colW+24,globalTopGroupRows*rowH+18);
      glassPanel(colX-12,y0+globalTopGroupRows*rowH+sectionGap-10,colW+24,
                 (shown-globalTopGroupRows)*rowH+18);
    } else glassPanel(colX-12,y0-10,colW+24,shown*rowH+18);
    int fontHeight=TTF_FontHeight(g_font);
    float target=(float)(rowY(sel)+2);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,rowH-4,COL_FOCUS);
    fillRect(colX,(int)g_hy,5,rowH-4,COL_SEL);
    for(int row=0;row<vis&&top+row<n;row++){
      int index=top+row,slot=rowY(index),y=slot+(rowH-fontHeight)/2; bool current=index==sel;
      if(global&&index==launcherRow){
        const char *theme=storeGet(g_global,"Wrapper/Theme","animated");
        const char *value=!strcmp(theme,"xmb")?"XMB (PS3)":(!strcmp(theme,"animated")?"Glow":(!strcmp(theme,"classic")?"Classic":(!strcmp(theme,"oled")?"OLED black":"Bubbles")));
        drawText(g_font,labelX,y,"Launcher",current?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,value,current?COL_VAL:COL_DIM);
      } else if(global&&index==libraryRow){
        drawText(g_font,labelX,y,"Library & storage",current?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,"games / files / network",current?COL_VAL:COL_DIM);
      } else if(global&&index==retroAchievementsRow){
        const bool loggedIn=retroAchievementsLoggedIn();
        const bool enabled=loggedIn&&!strcmp(storeGet(g_global,"Achievements/Enabled","false"),"true");
        drawText(g_font,labelX,y,"RetroAchievements",current?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,
                  enabled?storeGet(g_global,"Achievements/Username",""):(loggedIn?"Off":"Not signed in"),
                  current?COL_VAL:COL_DIM);
      } else if(hasCheatRow&&index==cheatRow){
        drawText(g_font,labelX,y,"Cheat codes",current?COL_VAL:COL_TXT);
        if(gameCRC){ char value[24]; snprintf(value,sizeof(value),"CRC %08X",gameCRC); drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,value,current?COL_VAL:COL_DIM); }
        else drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,"launch once",current?COL_VAL:COL_DIM);
      } else {
        drawText(g_font,labelX,y,g_screens[order[index-screenStart]].title,current?COL_VAL:COL_TXT);
        drawTextR(g_font,valX,y,">",current?COL_VAL:COL_DIM);
      }
    }
    if(n>vis){ int trackH=vis*rowH,trackX=colX+colW+16; fillRect(trackX,y0,4,trackH,(SDL_Color){40,44,54,255}); int thumbH=std::max(16,trackH*vis/n),denom=std::max(1,n-vis); fillRect(trackX,y0+(trackH-thumbH)*top/denom,4,thumbH,COL_SEL); }
    drawSettingsFooter("A  Open       X  Help       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static void toast(const char *msg) {
  for (int f = 0; f < 2; f++) {
    clearUiBackground();
    int pw=820,ph=120,px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph); border(px,py,pw,ph,2,COL_HI);
    drawTextC(g_font,SW/2,py+46,msg,COL_TXT);
    SDL_RenderPresent(g_ren); SDL_Delay(10);
  }
}

static void modalMessage(const char *title, const std::vector<std::string> &lines) {
  for (;;) {
    if(!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP) return; }
      if (e.type == SDL_CONTROLLERBUTTONDOWN &&
          (e.cbutton.button == BTN_CONFIRM || e.cbutton.button == BTN_CANCEL)) return;
    }
    clearUiBackground();
    int pw = SW*3/4, ph = 150 + (int)lines.size()*40, px = (SW-pw)/2, py = (SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big, SW/2, py+34, title, COL_SEL);
    int y = py+108;
    for (auto &l : lines) { drawTextC(g_font, SW/2, y, l.c_str(), COL_TXT); y += 40; }
    drawButtonHintCentered(SW/2,py+ph-30,"A","Continue");
    SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static bool confirmBox(const char *title, const std::vector<std::string> &lines) {
  int pw=SW*3/4, ph=200+(int)lines.size()*40, px=(SW-pw)/2, py=(SH-ph)/2;
  int bw=210, bh=56, bby=py+ph-bh-22, yesx=SW/2-bw-18, nox=SW/2+18;
  for(;;){
    if(!beginUiFrame()) return false;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP && ty>=bby && ty<bby+bh){
          if(tx>=yesx && tx<yesx+bw) return true;
          if(tx>=nox  && tx<nox+bw)  return false;
      } }
      if(e.type==SDL_CONTROLLERBUTTONDOWN){
        if(e.cbutton.button==BTN_CONFIRM) return true;
        if(e.cbutton.button==BTN_CANCEL) return false;
      }
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,(SDL_Color){210,70,70,255});
    drawTextC(g_font_big,SW/2,py+34,title,(SDL_Color){235,120,120,255});
    int y=py+112; for(auto&l:lines){ drawTextC(g_font,SW/2,y,l.c_str(),COL_TXT); y+=40; }
    fillRect(yesx,bby,bw,bh,(SDL_Color){150,50,50,255}); border(yesx,bby,bw,bh,2,(SDL_Color){215,95,95,255});
    drawButtonHintCentered(yesx+bw/2,bby+bh/2,"A","Yes",COL_TXT);
    fillRect(nox,bby,bw,bh,(SDL_Color){48,54,64,255}); border(nox,bby,bw,bh,2,COL_DIM);
    drawButtonHintCentered(nox+bw/2,bby+bh/2,"B","No",COL_TXT);
    SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}


static std::string installedReleaseTag() {
  const std::string built=LauncherUpdate_BuiltReleaseTag();
  const std::string stored=storeGet(g_global,"Wrapper/InstalledReleaseTag","");
  if(stored.empty()) return built;
  return LauncherUpdate_IsNewer(stored,built)?stored:built;
}

static std::vector<size_t> utf8Boundaries(const std::string &text) {
  std::vector<size_t> boundaries{0};
  for(size_t index=0;index<text.size();){
    const unsigned char lead=(unsigned char)text[index];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(index+length>text.size()) length=1;
    for(size_t part=1;part<length;part++) if(((unsigned char)text[index+part]&0xc0)!=0x80){ length=1; break; }
    index+=length;
    boundaries.push_back(index);
  }
  return boundaries;
}

static std::vector<std::string> wrapReleaseNotes(const std::string &notes,int maxWidth) {
  std::vector<std::string> lines;
  size_t paragraphStart=0;
  while(paragraphStart<=notes.size()){
    size_t paragraphEnd=notes.find('\n',paragraphStart);
    if(paragraphEnd==std::string::npos) paragraphEnd=notes.size();
    std::string paragraph=notes.substr(paragraphStart,paragraphEnd-paragraphStart);
    if(!paragraph.empty()&&paragraph.back()=='\r') paragraph.pop_back();
    for(char &value:paragraph) if(value=='\t'||(unsigned char)value<0x20) value=' ';
    while(!paragraph.empty()&&paragraph.back()==' ') paragraph.pop_back();
    if(paragraph.empty()) lines.emplace_back();
    else {
      bool continuation=false;
      while(!paragraph.empty()){
        while(!paragraph.empty()&&paragraph.front()==' ') paragraph.erase(paragraph.begin());
        if(paragraph.empty()) break;
        const std::string prefix=continuation&&paragraph.rfind("- ",0)!=0?"  ":"";
        if(textW(g_font_sm,(prefix+paragraph).c_str())<=maxWidth){ lines.push_back(prefix+paragraph); break; }
        const auto boundaries=utf8Boundaries(paragraph);
        size_t low=1,high=boundaries.size()-1;
        while(low<high){
          size_t middle=(low+high+1)/2;
          if(textW(g_font_sm,(prefix+paragraph.substr(0,boundaries[middle])).c_str())<=maxWidth) low=middle;
          else high=middle-1;
        }
        size_t split=boundaries[low];
        size_t space=paragraph.rfind(' ',split);
        if(space!=std::string::npos&&space>0&&space>=split/3) split=space;
        lines.push_back(prefix+paragraph.substr(0,split));
        paragraph.erase(0,split);
        continuation=true;
      }
    }
    if(paragraphEnd==notes.size()) break;
    paragraphStart=paragraphEnd+1;
  }
  if(lines.empty()) lines.emplace_back("No release notes were provided.");
  return lines;
}

static void requestLauncherExitAfterUpdate() {
  g_exitRequested=true;
}

static void runUpdateScreen() {
  if(!g_griddbReady){
    modalMessage("Update check unavailable",{
      "The launcher could not initialize its network connection.",
      "Check the connection and try again."
    });
    return;
  }
  LauncherUpdateSnapshot initial=LauncherUpdate_GetSnapshot();
  if(initial.state==LauncherUpdateState::Idle)
    LauncherUpdate_StartCheck(installedReleaseTag());

  int scroll=0;
  bool cancelRequested=false,installedSaved=false;
  std::string wrappedTag,wrappedBody;
  std::vector<std::string> wrappedLines;
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){
      LauncherUpdate_Cancel();
      return;
    }
    LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
    if(snapshot.state==LauncherUpdateState::ReadyToInstall){
      if(g_romfsReady){ romfsExit(); g_romfsReady=false; }
      LauncherUpdate_InstallDownloaded(g_launcherNroPath);
      snapshot=LauncherUpdate_GetSnapshot();
      if(snapshot.state==LauncherUpdateState::Error&&!g_romfsReady&&R_SUCCEEDED(romfsInit()))
        g_romfsReady=true;
    }
    if(snapshot.state==LauncherUpdateState::Installed&&!installedSaved){
      storeSet(g_global,"Wrapper/InstalledReleaseTag",snapshot.release.tag.c_str());
      storeSave(g_global,LAUNCHER_INI);
      g_updateNoticeTag.clear();
      installedSaved=true;
    }

    const int panelWidth=SW*7/8,panelHeight=SH*4/5;
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    const int bodyX=panelX+42,bodyY=panelY+126,bodyWidth=panelWidth-84;
    const int footerHeight=108,bodyBottom=panelY+panelHeight-footerHeight;
    const int lineHeight=TTF_FontHeight(g_font_sm)+8;
    const int visibleLines=std::max(1,(bodyBottom-bodyY)/lineHeight);
    if(snapshot.release.tag!=wrappedTag||snapshot.release.notes!=wrappedBody){
      wrappedTag=snapshot.release.tag;
      wrappedBody=snapshot.release.notes;
      wrappedLines=wrapReleaseNotes(wrappedBody.empty()?"Release notes will appear here.":wrappedBody,bodyWidth-20);
      scroll=0;
    }
    const int maxScroll=std::max(0,(int)wrappedLines.size()-visibleLines);
    scroll=std::max(0,std::min(scroll,maxScroll));

    SDL_Event event;
    navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SCROLL_UP) scroll=std::min(maxScroll,scroll+std::max(1,g_touchScrollSteps));
      else if(touch==TOUCH_SCROLL_DOWN) scroll=std::max(0,scroll-std::max(1,g_touchScrollSteps));
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) scroll=std::max(0,scroll-1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) scroll=std::min(maxScroll,scroll+1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER) scroll=std::max(0,scroll-visibleLines);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) scroll=std::min(maxScroll,scroll+visibleLines);
      else if(event.cbutton.button==BTN_CANCEL){
        if(snapshot.state==LauncherUpdateState::Downloading){ LauncherUpdate_Cancel(); cancelRequested=true; }
        else if(snapshot.state!=LauncherUpdateState::Installed) return;
      } else if(event.cbutton.button==BTN_CONFIRM){
        if(snapshot.state==LauncherUpdateState::UpdateAvailable){
          if(LauncherUpdate_StartDownload(g_launcherNroPath)) cancelRequested=false;
        } else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled){
          cancelRequested=false;
          LauncherUpdate_StartCheck(installedReleaseTag());
        } else if(snapshot.state==LauncherUpdateState::Installed){
          requestLauncherExitAfterUpdate();
          return;
        }
      }
    }

    snapshot=LauncherUpdate_GetSnapshot();
    clearUiBackground();
    fillRect(0,0,SW,SH,(SDL_Color){0,0,0,105});
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
    drawTextC(g_font_big,SW/2,panelY+24,"NetherSX2 Update",COL_SEL);

    std::string status;
    switch(snapshot.state){
      case LauncherUpdateState::Idle: status="Ready to check for updates"; break;
      case LauncherUpdateState::Checking: status="Checking GitHub for the latest release..."; break;
      case LauncherUpdateState::UpdateAvailable:
        status="Version "+snapshot.release.tag+" is available    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::UpToDate:
        status="You are up to date    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::Downloading:
        status=cancelRequested?"Cancelling download...":"Downloading "+snapshot.release.assetName; break;
      case LauncherUpdateState::ReadyToInstall: status="Preparing installation..."; break;
      case LauncherUpdateState::Installing: status="Installing update..."; break;
      case LauncherUpdateState::Installed: status="Update installed successfully - relaunch NetherSX2 manually"; break;
      case LauncherUpdateState::Cancelled: status="Update cancelled"; break;
      case LauncherUpdateState::Error: status=snapshot.error.empty()?"Update failed":snapshot.error; break;
    }
    drawScrollTextL(g_font_sm,bodyX,panelY+92,bodyWidth,status.c_str(),
      snapshot.state==LauncherUpdateState::Error?(SDL_Color){235,125,115,255}:COL_VAL);

    SDL_Rect clip={bodyX,bodyY,bodyWidth,bodyBottom-bodyY};
    SDL_RenderSetClipRect(g_ren,&clip);
    for(int row=0;row<visibleLines&&scroll+row<(int)wrappedLines.size();row++)
      drawText(g_font_sm,bodyX,bodyY+row*lineHeight,wrappedLines[scroll+row].c_str(),COL_TXT);
    SDL_RenderSetClipRect(g_ren,nullptr);
    if((int)wrappedLines.size()>visibleLines){
      const int trackX=panelX+panelWidth-25,trackHeight=bodyBottom-bodyY;
      fillRect(trackX,bodyY,4,trackHeight,(SDL_Color){40,44,54,255});
      const int thumbHeight=std::max(18,trackHeight*visibleLines/(int)wrappedLines.size());
      fillRect(trackX,bodyY+(trackHeight-thumbHeight)*scroll/std::max(1,maxScroll),4,thumbHeight,COL_SEL);
    }

    if(snapshot.state==LauncherUpdateState::Downloading){
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const int percent=total?(int)std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      const int barX=bodyX,barY=panelY+panelHeight-82,barWidth=bodyWidth,barHeight=24;
      border(barX,barY,barWidth,barHeight,2,COL_SEL);
      fillRect(barX+3,barY+3,(barWidth-6)*percent/100,barHeight-6,COL_HI);
      char progress[96];
      snprintf(progress,sizeof(progress),"%d%%    %.1f / %.1f MiB",percent,
        snapshot.downloaded/(1024.0*1024.0),total/(1024.0*1024.0));
      drawTextC(g_font_sm,SW/2,barY+30,progress,COL_DIM);
    } else {
      const char *controls="B  Back       Up / Down  Scroll       L / R  Page";
      if(snapshot.state==LauncherUpdateState::UpdateAvailable) controls="A  Download       B  Back       Up / Down  Scroll";
      else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled) controls="A  Retry       B  Back";
      else if(snapshot.state==LauncherUpdateState::Installed) controls="A  Exit NetherSX2";
      drawSettingsFooter(controls,panelY+panelHeight-38);
    }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

static void pollUpdateNotification() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  if(snapshot.state==LauncherUpdateState::UpdateAvailable&&!snapshot.release.tag.empty()&&
     snapshot.release.tag!=g_updateNotifiedTag){
    g_updateNotifiedTag=snapshot.release.tag;
    g_updateNoticeTag=snapshot.release.tag;
    g_updateNoticeUntil=SDL_GetTicks()+9000;
  }
}

static void drawUpdateNotification() {
  if(g_updateNoticeTag.empty()||SDL_TICKS_PASSED(SDL_GetTicks(),g_updateNoticeUntil)){
    g_updateNoticeTag.clear();
    return;
  }
  const int width=std::min(540,SW-40),height=92,x=SW-width-24,y=SH-height-58;
  glassPanel(x,y,width,height);
  border(x,y,width,height,2,COL_SEL);
  const std::string title="NetherSX2 "+g_updateNoticeTag+" is available";
  drawText(g_font,x+22,y+16,ellipsizedText(g_font,title,width-44).c_str(),COL_VAL);
  drawText(g_font_sm,x+22,y+54,"Open Settings > Launcher > Check for Updates",COL_TXT);
}

static bool biosPresent() {
  DIR *d = opendir(BIOS_DIR);
  if (!d) return false;
  bool found = false;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    struct stat st;
    if (stat((std::string(BIOS_DIR) + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      found = true; break;
    }
  }
  closedir(d);
  return found;
}

static void drawWrapped(TTF_Font *font,int x,int y,int maxWidth,int lineHeight,int maxLines,const char *text,SDL_Color color) {
  if(!text||!*text) return;
  std::string input=text,line; int drawn=0;
  auto emit=[&](const std::string &value){ if(drawn<maxLines){ drawText(font,x,y+drawn*lineHeight,value.c_str(),color); drawn++; } };
  size_t index=0;
  while(index<input.size()&&drawn<maxLines){
    size_t end=index; while(end<input.size()&&input[end]!=' '&&input[end]!='\n') end++;
    std::string word=input.substr(index,end-index);
    std::string candidate=line.empty()?word:line+" "+word;
    if(textW(font,candidate.c_str())>maxWidth&&!line.empty()){ emit(line); line=word; }
    else line=std::move(candidate);
    if(end<input.size()&&input[end]=='\n'){ emit(line); line.clear(); }
    index=end+1;
  }
  if(!line.empty()&&drawn<maxLines) emit(line);
}

static SDL_Texture *loadScaledTexture(const std::string &path,int width,int height) {
  if(width<1||height<1) return nullptr;
  SDL_Surface *source=IMG_Load(path.c_str());
  if(!source) return nullptr;
  SDL_Surface *scaled=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32);
  if(!scaled){ SDL_FreeSurface(source); return nullptr; }
  SDL_BlendMode blend=SDL_BLENDMODE_NONE;
  SDL_GetSurfaceBlendMode(source,&blend);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  bool ok=SDL_BlitScaled(source,nullptr,scaled,nullptr)==0;
  SDL_SetSurfaceBlendMode(source,blend);
  SDL_FreeSurface(source);
  if(!ok){ SDL_FreeSurface(scaled); return nullptr; }
  SDL_Texture *texture=SDL_CreateTextureFromSurface(g_ren,scaled);
  SDL_FreeSurface(scaled);
  if(texture) SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
  return texture;
}

static const char *gridDbErrorText(int result) {
  if(result==GRIDDB_NO_KEY) return "The SteamGridDB API key was rejected.";
  if(result==GRIDDB_NO_NET) return "Could not connect to SteamGridDB.";
  if(result==GRIDDB_NOT_FOUND) return "No matching artwork was found.";
  return "SteamGridDB returned an unexpected error.";
}

static int chooseCoverArtwork(const std::vector<GridDbArtwork> &artworks,const char *gameName) {
  if(artworks.empty()) return -1;
  const int listX=56,listWidth=SW/2-78,rowHeight=52,startY=116;
  const int previewX=SW/2+28,previewAreaWidth=SW-previewX-56;
  const int previewHeight=std::min(SH-210,SW>=1600?720:510);
  const int previewWidth=previewHeight*2/3;
  const int visible=std::max(1,(SH-startY-72)/rowHeight);
  const std::string temporary=std::string(COVERS_DIR)+"/.sgdb-preview.img";
  int sel=0,top=0,loaded=-1;
  SDL_Texture *preview=nullptr;
  bool previewFailed=false;
  auto releasePreview=[&](){ if(preview) SDL_DestroyTexture(preview); preview=nullptr; remove(temporary.c_str()); };
  auto loadPreview=[&](int index){
    releasePreview(); loaded=index; previewFailed=false;
    clearUiBackground(); drawHeader("Choose cover artwork",gameName);
    drawTextC(g_font,previewX+previewAreaWidth/2,SH/2-18,"Loading preview...",COL_DIM);
    SDL_RenderPresent(g_ren);
    const std::string &url=artworks[index].thumbnailUrl.empty()?artworks[index].url:artworks[index].thumbnailUrl;
    if(griddb_download_image(url,temporary)==GRIDDB_OK) preview=loadScaledTexture(temporary,previewWidth,previewHeight);
    previewFailed=preview==nullptr; remove(temporary.c_str()); beginScreenFx();
  };
  mkdir(COVERS_DIR,0777);
  loadPreview(0);
  for(;;){
    if(!beginUiFrame()){ releasePreview(); return -1; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty); int oldSelection=sel;
      if(touchScrollList(touch,sel,top,(int)artworks.size(),visible)){ if(sel!=oldSelection) loadPreview(sel); continue; }
      if(touch==TOUCH_TAP){
        if(ty>=SH-48){ releasePreview(); return -1; }
        if(tx>=listX&&tx<listX+listWidth) for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
          int itemY=startY+row*rowHeight;
          if(ty>=itemY&&ty<itemY+rowHeight){ sel=top+row; if(loaded!=sel) loadPreview(sel); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int previous=sel;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+(int)artworks.size()-1)%(int)artworks.size();
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%(int)artworks.size();
      else if(event.cbutton.button==BTN_CONFIRM){ releasePreview(); return sel; }
      else if(event.cbutton.button==BTN_CANCEL){ releasePreview(); return -1; }
      if(sel<top) top=sel;
      if(sel>=top+visible) top=sel-visible+1;
      if(sel!=previous) loadPreview(sel);
    }
    clearUiBackground(); drawHeader("Choose cover artwork",gameName);
    glassPanel(listX-10,startY-10,listWidth+20,std::min(visible,(int)artworks.size())*rowHeight+18);
    for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
      int index=top+row,itemY=startY+row*rowHeight,textY=itemY+(rowHeight-TTF_FontHeight(g_font))/2; bool current=index==sel;
      if(current){ fillRect(listX,itemY,listWidth,rowHeight-3,COL_FOCUS); fillRect(listX,itemY,5,rowHeight-3,COL_SEL); }
      std::string label="Artwork "+std::to_string(index+1);
      drawText(g_font,listX+26,textY,label.c_str(),current?COL_VAL:COL_TXT);
      if(artworks[index].width>0&&artworks[index].height>0){
        std::string dimensions=std::to_string(artworks[index].width)+"x"+std::to_string(artworks[index].height);
        drawTextR(g_font_sm,listX+listWidth-20,textY+(TTF_FontHeight(g_font)-TTF_FontHeight(g_font_sm))/2,dimensions.c_str(),current?COL_VAL:COL_DIM);
      }
    }
    int imageX=previewX+(previewAreaWidth-previewWidth)/2,imageY=startY;
    fillRect(imageX,imageY,previewWidth,previewHeight,COL_CARD);
    if(loaded==sel&&preview){ SDL_Rect destination={imageX,imageY,previewWidth,previewHeight}; SDL_RenderCopy(g_ren,preview,nullptr,&destination); }
    else if(loaded==sel&&previewFailed) drawTextC(g_font_sm,imageX+previewWidth/2,imageY+previewHeight/2,"Preview unavailable",COL_DIM);
    border(imageX,imageY,previewWidth,previewHeight,2,loaded==sel?COL_SEL:COL_DIM);
    drawSettingsFooter("A  Use artwork       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static void downloadCover(Game &g) {
  std::string key=storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(key.empty()){
    char buffer[128];
    if(promptText("Enter your free SteamGridDB API key","",buffer,sizeof(buffer))){ key=buffer; storeSet(g_global,"Wrapper/SteamGridDBKey",buffer); storeSave(g_global,LAUNCHER_INI); }
    else { toast("A SteamGridDB API key is required"); SDL_Delay(1200); return; }
  }
  mkdir(COVERS_DIR,0777);
  std::string query=g.title;
  GridDbGameResult selectedGame;
  for(;;){
    toast("Searching SteamGridDB...");
    std::vector<GridDbGameResult> matches;
    int result=griddb_search_games(key,query,matches);
    if(result!=GRIDDB_OK&&result!=GRIDDB_NOT_FOUND){ modalMessage("Cover search failed",{gridDbErrorText(result)}); return; }
    std::vector<std::string> labels={"Custom search..."};
    for(const auto &match:matches) labels.push_back(match.name);
    std::vector<const char*> names; names.reserve(labels.size());
    for(const auto &label:labels) names.push_back(label.c_str());
    int gameIndex=dropdown("Choose matching title",names.data(),(int)names.size(),-1);
    if(gameIndex<0) return;
    if(gameIndex==0){
      char custom[256];
      if(!promptText("Custom SteamGridDB search",query.c_str(),custom,sizeof(custom))) continue;
      std::string nextQuery=trim(custom); if(!nextQuery.empty()) query=std::move(nextQuery);
      continue;
    }
    selectedGame=matches[gameIndex-1]; break;
  }
  toast("Loading available artwork...");
  std::vector<GridDbArtwork> artworks;
  int result=griddb_fetch_artworks(key,selectedGame.id,artworks);
  if(result!=GRIDDB_OK){ modalMessage("Artwork search failed",{gridDbErrorText(result)}); return; }
  int artworkIndex=chooseCoverArtwork(artworks,selectedGame.name.c_str());
  if(artworkIndex<0) return;
  toast("Downloading selected cover...");
  result=griddb_download_image(artworks[artworkIndex].url,coverPath(g));
  if(result==GRIDDB_OK){ reloadCover(g); toast("Cover downloaded"); }
  else toast("Cover download failed");
  SDL_Delay(1200);
}

static void downloadAllCovers() {
  std::string key=storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(key.empty()){
    char buffer[128];
    if(promptText("Enter your free SteamGridDB API key","",buffer,sizeof(buffer))){ key=buffer; storeSet(g_global,"Wrapper/SteamGridDBKey",buffer); storeSave(g_global,LAUNCHER_INI); }
    else { toast("A SteamGridDB API key is required"); SDL_Delay(1200); return; }
  }
  mkdir(COVERS_DIR,0777);
  std::vector<int> pending;
  for(int index=0;index<(int)g_games.size();index++)
    if(!g_games[index].biosBoot&&!regularFileExists(existingCoverPath(g_games[index]))) pending.push_back(index);
  if(pending.empty()){ toast("All covers already downloaded"); SDL_Delay(1200); return; }
  int total=(int)pending.size(),done=0,ok=0,fail=0; bool cancel=false;
  for(int item=0;item<total&&!cancel;item++){
    if(!beginUiFrame()) return;
    Game &game=g_games[pending[item]];
    SDL_Event event; while(pollUiEvent(event)){ pumpStick(event); if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL) cancel=true; int tx=0,ty=0; if(touchFeed(event,&tx,&ty)==TOUCH_TAP&&ty>=SH-90) cancel=true; }
    if(cancel) break;
    clearUiBackground(); drawHeader("Download all covers",nullptr);
    drawTextC(g_font,SW/2,SH/2-96,("Downloading  "+std::to_string(done+1)+" / "+std::to_string(total)).c_str(),COL_VAL);
    drawTitleCell(SW/2,SW-260,SH/2-44,game.title,true,COL_TXT);
    int width=SW-360,x=180,y=SH/2+16,height=26;
    fillRect(x,y,width,height,(SDL_Color){40,44,54,255}); border(x,y,width,height,2,COL_DIM);
    fillRect(x,y,total?width*done/total:0,height,COL_SEL);
    char status[64]; snprintf(status,sizeof(status),"%d downloaded    %d failed",ok,fail);
    drawTextC(g_font_sm,SW/2,y+46,status,COL_DIM); SDL_RenderPresent(g_ren);
    int result=griddb_fetch_cover(key,game.title,coverPath(game));
    if(result==GRIDDB_OK){ ok++; reloadCover(game); } else fail++;
    done++;
  }
  char message[96]; snprintf(message,sizeof(message),"Covers: %d downloaded, %d failed%s",ok,fail,cancel?" (cancelled)":"");
  toast(message); SDL_Delay(1600);
}

static bool pickIcon(Game &g, char *outPath, size_t outSize) {
  std::string base = std::string(DATA_DIR) + "/forwarders", tmp = base + "/iconpick";
  mkdir(base.c_str(),0777); mkdir(tmp.c_str(),0777);
  if(DIR*d=opendir(tmp.c_str())){ struct dirent*e; while((e=readdir(d))) if(e->d_name[0]!='.') remove((tmp+"/"+std::string(e->d_name)).c_str()); closedir(d); }
  std::vector<std::string> paths; struct stat st;
  { std::string cp=existingCoverPath(g); if(stat(cp.c_str(),&st)==0) paths.push_back(cp); }
  std::string key = storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(!key.empty()){
    clearUiBackground();
    drawHeader("Choose an icon", g.title.c_str());
    drawTextC(g_font, SW/2, SH/2, "Fetching icons from SteamGridDB...", COL_TXT);
    SDL_RenderPresent(g_ren);
    int nf=griddb_fetch_icons(key,g.title,tmp,14);
    for(int i=0;i<nf;i++){ char p[300]; snprintf(p,sizeof(p),"%s/gicon_%d.png",tmp.c_str(),i); paths.push_back(p); }
  }
  if(paths.empty()){ toast("No icon found - add a SteamGridDB key or download a cover first"); SDL_Delay(1800); return false; }
  int n=(int)paths.size();
  int cols=n<5?n:5; if(cols<1)cols=1;
  int rows=(n+cols-1)/cols, gap=18, top=150, bot=40;
  int cw=(SW-80-(cols-1)*gap)/cols, ch=(SH-top-bot-(rows-1)*gap)/rows;
  int cell=cw<ch?cw:ch; if(cell>200)cell=200; if(cell<90)cell=90;
  int x0=(SW-(cols*cell+(cols-1)*gap))/2, y0=top;
  std::vector<SDL_Texture*> tex(n,nullptr);
  for(int i=0;i<n;i++) tex[i]=loadScaledTexture(paths[i],cell,cell);
  int sel=0, chosen=-1; bool done=false; beginScreenFx();
  while(!done){
    if(!beginUiFrame()){ done=true; break; }
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){ pumpStick(e);
      { int tx=0,ty=0; TouchKind touch=touchFeed(e,&tx,&ty);
        if(touch==TOUCH_SCROLL_UP){ sel=std::min(n-1,sel+cols); continue; }
        if(touch==TOUCH_SCROLL_DOWN){ sel=std::max(0,sel-cols); continue; }
        if(touch==TOUCH_TAP){
          for(int i=0;i<n;i++){ int row=i/cols,column=i%cols,x=x0+column*(cell+gap),y=y0+row*(cell+gap);
            if(tx>=x&&tx<x+cell&&ty>=y&&ty<y+cell){ sel=i; chosen=i; done=true; break; } }
          if(done) continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=(sel+1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel+cols)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel-cols+n)%n; break;
        case BTN_CONFIRM: chosen=sel; done=true; break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawHeader("Choose an icon", g.title.c_str());
    for(int i=0;i<n;i++){ int r=i/cols,c=i%cols, x=x0+c*(cell+gap), y=y0+r*(cell+gap);
      if(i==sel) fillRect(x-6,y-6,cell+12,cell+12,COL_SEL);
      fillRect(x,y,cell,cell,COL_CARD);
      if(tex[i]){ SDL_Rect d{x,y,cell,cell}; SDL_RenderCopy(g_ren,tex[i],nullptr,&d); }
      else drawTextC(g_font_sm,x+cell/2,y+cell/2,"?",COL_DIM);
    }
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
  for(auto t:tex) if(t) SDL_DestroyTexture(t);
  if(chosen>=0 && chosen<n){ snprintf(outPath,outSize,"%s",paths[chosen].c_str()); return true; }
  return false;
}

static void forwarderWizard(Game &g) {
  char name[256]; snprintf(name,sizeof(name),"%s",g.title.c_str());
  char author[128]; snprintf(author,sizeof(author),"%s","NetherSX2");
  char icon[300]={0};
  { struct stat st; std::string cp=existingCoverPath(g);
    if(stat(cp.c_str(),&st)==0) snprintf(icon,sizeof(icon),"%s",cp.c_str()); }
  SDL_Texture *iconTex = icon[0] ? loadScaledTexture(icon,280,280) : nullptr;

  const int ix=110, iy=176, isz=280;
  const int rx=ix+isz+70; int rw=SW-rx-90;
  const int nameY=196, authY=290, createY=406, fieldH=64, createH=58;
  int sel=0; bool done=false; beginScreenFx();

  auto edit=[&](const char *header,char *buffer,size_t size){
    char value[256];
    if(promptText(header,buffer,value,sizeof(value))&&value[0]&&size){ size_t length=std::min(strlen(value),size-1); memcpy(buffer,value,length); buffer[length]=0; }
  };
  auto build=[&](){
    if(!icon[0]){ toast("Pick an icon first"); SDL_Delay(1200); return; }
    clearUiBackground();
    drawHeader("Creating HOME shortcut", g.title.c_str());
    drawTextC(g_font, SW/2, SH/2, "Building + installing forwarder...", COL_TXT);
    SDL_RenderPresent(g_ren);
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    const std::string &shortcutKey=g.legacyUnique&&!g.legacyKey.empty()?g.legacyKey:g.key;
    char err[256]={0}; bool ok=forwarder_create(shortcutKey,name,author,icon,err,sizeof(err));
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(ok){ toast("HOME shortcut installed"); SDL_Delay(1800); done=true; }
    else modalMessage("Shortcut failed", { err[0]?err:"Unknown error" });
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel==0){ char p[300]; if(pickIcon(g,p,sizeof(p))){ snprintf(icon,sizeof(icon),"%s",p); if(iconTex)SDL_DestroyTexture(iconTex); iconTex=loadScaledTexture(icon,isz,isz); } beginScreenFx(); }
    else if(sel==1) edit("Shortcut name", name, sizeof(name));
    else if(sel==2) edit("Author", author, sizeof(author));
    else build();
  };

  while(!done){
    if(!beginUiFrame()){ done=true; break; }
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          if(tx>=ix&&tx<ix+isz&&ty>=iy&&ty<iy+isz){ sel=0; activate(); }
          else if(ty>=nameY-6&&ty<nameY+fieldH){ sel=1; activate(); }
          else if(ty>=authY-6&&ty<authY+fieldH){ sel=2; activate(); }
          else if(ty>=createY-6&&ty<createY+createH){ sel=3; activate(); }
          else if(ty>=SH-40) done=true;
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=0; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if(sel==0) sel=1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel==0)?3:(sel==1?3:sel-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel==0)?1:(sel==3?1:sel+1); break;
        case BTN_CONFIRM: activate(); break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawHeader("Create HOME shortcut", g.title.c_str());
    if(sel==0) fillRect(ix-6,iy-6,isz+12,isz+12,COL_SEL);
    fillRect(ix,iy,isz,isz,COL_CARD);
    if(iconTex){ SDL_Rect d{ix,iy,isz,isz}; SDL_RenderCopy(g_ren,iconTex,nullptr,&d); }
    else drawTextC(g_font_sm,ix+isz/2,iy+isz/2,"(no icon)",COL_DIM);
    drawTextC(g_font_sm, ix+isz/2, iy+isz+20, "Icon", sel==0?COL_VAL:COL_DIM);
    auto field=[&](int idx,int y,const char*label,const char*val){ bool cur=sel==idx;
      if(cur){ fillRect(rx-10,y-6,rw+20,fieldH,COL_FOCUS); fillRect(rx-10,y-6,5,fieldH,COL_SEL); }
      drawText(g_font_sm, rx, y, label, cur?COL_VAL:COL_DIM);
      drawScrollTextL(g_font,rx,y+26,rw-8,val,cur?COL_VAL:COL_TXT); };
    field(1,nameY,"Name",name);
    field(2,authY,"Author",author);
    { bool cur=sel==3;
      fillRect(rx-10,createY-6,rw+20,createH, cur?(SDL_Color){44,86,44,240}:(SDL_Color){30,46,32,200});
      if(cur) fillRect(rx-10,createY-6,5,createH,COL_SEL);
      drawTextC(g_font, rx+rw/2, createY+12, "Create shortcut", cur?COL_VAL:(SDL_Color){150,225,150,255}); }
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
  if(iconTex) SDL_DestroyTexture(iconTex);
}

static int biosGameMenu(Game &g, SDL_GameController *pad) {
  (void)pad;
  const char *items[] = { "Boot PS2 BIOS", "Back" };
  const int n=2;
  int sel=0;
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return 0;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty>=SH-40) return 0;
        for(int i=0;i<n;i++){ int y=250+i*62; if(ty>=y-8&&ty<y+50){ sel=i; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
      else if(event.cbutton.button==BTN_CANCEL) return 0;
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==0) return 1;
        return 0;
      }
    }
    clearUiBackground();
    g_cover_budget=1; ensureCover(g);
    const int cw=300,ch=450,cx=90,cy=(SH-ch)/2;
    fillRect(cx+5,cy+7,cw,ch,(SDL_Color){0,0,0,60});
    if(g.cover){ SDL_SetTextureAlphaMod(g.cover,255); SDL_SetTextureColorMod(g.cover,255,255,255); SDL_Rect dst={cx,cy,cw,ch}; SDL_RenderCopy(g_ren,g.cover,nullptr,&dst); border(cx,cy,cw,ch,2,COL_DIM); }
    else { fillRect(cx,cy,cw,ch,(SDL_Color){40,44,54,255}); border(cx,cy,cw,ch,2,COL_DIM); }
    drawText(g_font_big,cx+cw+70,130,"PS2 BIOS",COL_TXT);
    drawText(g_font_sm,cx+cw+72,178,"Boot the console firmware without a disc",COL_DIM);
    const int mx=cx+cw+64,mw=SW-mx-70;
    float target=(float)(250+sel*62-8);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(mx,(int)g_hy,mw,50,COL_FOCUS); fillRect(mx,(int)g_hy,5,50,COL_SEL);
    for(int i=0;i<n;i++) drawText(g_font,cx+cw+94,250+i*62,items[i],i==sel?COL_VAL:COL_TXT);
    drawSettingsFooter("A  Select       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static int perGameMenu(Game &g, SDL_GameController *pad) {
  if(g.biosBoot) return biosGameMenu(g,pad);
  const char *items[] = { "Launch", "Game settings", "Rename game", "Download cover (SteamGridDB)", "Create HOME shortcut", "Clear game settings", "Delete game" };
  int n=7, sel=0;
  std::string gp = std::string(GAMECFG_DIR) + "/" + g.key + ".ini";
  std::string legacyGp = std::string(GAMECFG_DIR) + "/" + g.legacyKey + ".ini";
  if(regularFileExists(gp)) storeLoad(g_game,gp.c_str());
  else if(g.legacyUnique&&!g.legacyKey.empty()&&regularFileExists(legacyGp)) storeLoad(g_game,legacyGp.c_str());
  else g_game.kv.clear();
  removeLegacyCheatGate(g_game);
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return 0;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          if(ty>=SH-40){ return 0; }
          for(int i=0;i<n;i++){ int y=210+i*56; if(ty>=y-6 && ty<y+50){ sel=i;
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CANCEL: return 0;
        case BTN_CONFIRM:
          if(sel==0) return 1;
          else if(sel==1){
            g_active=&g_game;
            runSettingsRoot(pad,g.title.c_str(),&g);
            g_active=&g_global;
            mkdir(GAMECFG_DIR,0777);
            bool saved=true;
            if(g_game.kv.empty()){
              if(remove(gp.c_str())!=0&&errno!=ENOENT) saved=false;
            } else saved=storeSave(g_game,gp.c_str());
            if(saved&&g.legacyUnique&&legacyGp!=gp) remove(legacyGp.c_str());
            g.hasCfg=saved&&!g_game.kv.empty();
            if(!saved) modalMessage("Game settings",{"Could not save the per-game settings."});
            beginScreenFx();
          }
          else if(sel==2){
            char buf[128];
            if(promptText("Rename game", g.title.c_str(), buf, sizeof(buf))){
              g.title = buf;
              storeSet(g_titles, g.key.c_str(), buf);
              if(g.legacyUnique&&!g.legacyKey.empty()) storeRemove(g_titles,g.legacyKey.c_str());
              storeSave(g_titles, TITLES_INI);
            }
          }
          else if(sel==3){ downloadCover(g); beginScreenFx(); }
          else if(sel==4){ forwarderWizard(g); beginScreenFx(); }
          else if(sel==5){
            g_game.kv.clear(); remove(gp.c_str());
            if(g.legacyUnique&&!g.legacyKey.empty()) remove(legacyGp.c_str());
            g.hasCfg=false; toast("Game settings cleared"); SDL_Delay(700); beginScreenFx();
          }
          else if(sel==6){
            if(confirmBox("Delete game?", { g.title, "", "This permanently deletes the game file from",
                                            "its storage device. This cannot be undone." })){
              if(remove(g.path.c_str())!=0){
                modalMessage("Delete failed",{strerror(errno)});
                beginScreenFx();
                break;
              }
              remove(coverPath(g).c_str());
              remove(gp.c_str());
              remove(gameCRCFile(g).c_str());
              if(g.legacyUnique&&!g.legacyKey.empty()){
                remove((std::string(COVERS_DIR)+"/"+g.legacyKey+".png").c_str());
                remove(legacyGp.c_str());
                remove((std::string(GAMECRC_DIR)+"/"+g.legacyKey+".ini").c_str());
                storeRemove(g_titles,g.legacyKey.c_str());
                storeRemove(g_recent,g.legacyKey.c_str());
              }
              storeRemove(g_titles,g.key.c_str()); storeSave(g_titles,TITLES_INI);
              storeRemove(g_recent,g.key.c_str()); storeSave(g_recent,RECENT_INI);
              toast("Game deleted"); SDL_Delay(800);
              return 2;
            }
          }
          break;
      }
    }
    clearUiBackground();
    g_cover_budget = 1;
    ensureCover(g);
    int cw=300,chh=450,cx=90,cy=(SH-chh)/2;
    fillRect(cx+5,cy+7,cw,chh,(SDL_Color){0,0,0,60}); fillRect(cx+2,cy+3,cw,chh,(SDL_Color){0,0,0,75});
    if(g.cover){ SDL_SetTextureAlphaMod(g.cover,255); SDL_SetTextureColorMod(g.cover,255,255,255);
      SDL_Rect d={cx,cy,cw,chh}; SDL_RenderCopy(g_ren,g.cover,nullptr,&d); border(cx,cy,cw,chh,2,COL_DIM); }
    else { fillRect(cx,cy,cw,chh,(SDL_Color){40,44,54,255}); border(cx,cy,cw,chh,2,COL_DIM); drawTextC(g_font,cx+cw/2,cy+chh/2,"NO COVER",COL_DIM); }
    drawText(g_font_big,cx+cw+70,120,g.title.c_str(),COL_TXT);
    int mx=cx+cw+64, mw=SW-mx-70;
    float ty=(float)(210+sel*56-6);
    g_hy=(!g_uiAnimations||g_hy<0)?ty:g_hy+(ty-g_hy)*0.30f;
    fillRect(mx,(int)g_hy,mw,48,COL_FOCUS);
    fillRect(mx,(int)g_hy,5,48,COL_SEL);
    for(int i=0;i<n;i++){ int y=210+i*56; bool cur=i==sel;
      SDL_Color rc = (i==n-1) ? (SDL_Color){228,120,120,255} : COL_TXT;
      drawText(g_font,cx+cw+94,y,items[i],cur?COL_VAL:rc);
    }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

static bool extractFromRomfs(const char *src, const char *dst, bool force=false) {
  struct stat ss{},ds{};
  if(stat(src,&ss)!=0||!S_ISREG(ss.st_mode)||!recoverAtomicFile(dst)) return false;
  if (!force && stat(dst,&ds)==0 && ds.st_size==ss.st_size) return true;
  std::string tmp = std::string(dst) + ".tmp";
  FILE *in=fopen(src,"rb"), *out=fopen(tmp.c_str(),"wb");
  if(!in||!out){ if(in)fclose(in); if(out)fclose(out); return false; }
  static char buf[1<<16]; size_t n; bool ok=true;
  while((n=fread(buf,1,sizeof(buf),in))>0){ if(fwrite(buf,1,n,out)!=n){ ok=false; break; } }
  if(ferror(in)) ok=false;
  if(fflush(out)!=0||fsync(fileno(out))!=0) ok=false;
  if(fclose(in)!=0) ok=false;
  if(fclose(out)!=0) ok=false;
  if(!ok){ remove(tmp.c_str()); return false; }
  struct stat temporary{};
  if(stat(tmp.c_str(),&temporary)!=0||temporary.st_size!=ss.st_size||!replaceAtomic(dst,tmp)){
    remove(tmp.c_str());
    return false;
  }
  return stat(dst,&ds)==0 && ds.st_size==ss.st_size;
}

static bool extractTree(const std::string &src, const std::string &dst, bool force) {
  mkdir(dst.c_str(), 0777);
  DIR *d = opendir(src.c_str());
  if (!d) return false;
  bool ok = true;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    std::string s = src + "/" + e->d_name, t = dst + "/" + e->d_name;
    struct stat st;
    if (stat(s.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      ok = extractTree(s, t, force) && ok;
    else
      ok = extractFromRomfs(s.c_str(), t.c_str(), force) && ok;
  }
  closedir(d);
  return ok;
}

static const char *BUILD_STAMP = __DATE__ " " __TIME__;
static const char *RES_MARKER = "sdmc:/switch/nethersx2/resources/.nethersx2_build";
static bool ensureResources(const std::string &build) {
  char cur[64] = {0};
  FILE *f = fopen(RES_MARKER, "r");
  if (f) { if (!fgets(cur, sizeof(cur), f)) cur[0] = 0; fclose(f); }
  struct stat st;
  bool present = stat((std::string(RESOURCES_DIR) + "/GameIndex.yaml").c_str(), &st) == 0;
  const std::string marker=build+" "+BUILD_STAMP;
  if(trim(cur)==marker&&present) return true;
  toast("Extracting NetherSX2 resources (one-time)...");
  mkdir(RESOURCES_DIR, 0777);
  bool ok = extractTree(std::string("romfs:/res/") + build, RESOURCES_DIR, true);
  if(ok) writeAtomicText(RES_MARKER,marker+"\n");
  return ok;
}

static bool ensureBundledFile(const char *src,const char *dst,const std::string &marker) {
  char cur[48] = {0};
  FILE *f = fopen(marker.c_str(), "r");
  if (f) { if (!fgets(cur, sizeof(cur), f)) cur[0] = 0; fclose(f); }
  struct stat st;
  if(trim(cur)==BUILD_STAMP&&stat(dst,&st)==0&&S_ISREG(st.st_mode)) return true;
  if(!extractFromRomfs(src,dst,true)) return false;
  writeAtomicText(marker,std::string(BUILD_STAMP)+"\n");
  return true;
}

static bool ensureCore(const char *src,const char *dst,const std::string &build) {
  return ensureBundledFile(src,dst,std::string(CORES_DIR)+"/.core_build_"+build);
}

static bool sameNroBuild(const char *first,const char *second) {
  struct stat firstStat{},secondStat{};
  if(stat(first,&firstStat)!=0||stat(second,&secondStat)!=0||
     !S_ISREG(firstStat.st_mode)||!S_ISREG(secondStat.st_mode)||
     firstStat.st_size!=secondStat.st_size) return false;
  auto readIdentity=[](const char *path,u8 identity[32]){
    u8 header[0x50];
    FILE *file=fopen(path,"rb");
    if(!file) return false;
    bool ok=fseek(file,0x10,SEEK_SET)==0&&fread(header,1,sizeof(header),file)==sizeof(header);
    if(fclose(file)!=0) ok=false;
    if(!ok||memcmp(header,"NRO0",4)!=0) return false;
    memcpy(identity,header+0x30,32);
    return std::any_of(identity,identity+32,[](u8 byte){ return byte!=0; });
  };
  u8 firstId[32],secondId[32];
  return readIdentity(first,firstId)&&readIdentity(second,secondId)&&
         memcmp(firstId,secondId,sizeof(firstId))==0;
}

static bool ensureEmu(const char *src,const char *dst) {
  if(sameNroBuild(src,dst)) return true;
  return extractFromRomfs(src,dst,true)&&sameNroBuild(src,dst);
}

static void migrateLegacyEmuHosts() {
  static const char *renderers[]={"vk","gl"};
  for(const char *renderer:renderers){
    const std::string filename=std::string("NetherSX2_nx_")+renderer+".nro";
    const std::string legacy=std::string(DATA_DIR)+"/"+filename;
    struct stat st{};
    if(stat(legacy.c_str(),&st)!=0||!S_ISREG(st.st_mode)) continue;

    const std::string source=std::string("romfs:/emu/")+filename;
    const std::string hidden=std::string(EMU_HOST_DIR)+"/"+filename;
    if(!ensureEmu(source.c_str(),hidden.c_str())) continue;

    if(remove(legacy.c_str())==0){
      remove((legacy+".tmp").c_str());
      remove((legacy+".old").c_str());
      fsdevCommitDevice("sdmc");
    }
  }
}

struct GLay { int cols, rows, cw, chh, gapx, gapy, x0, y0, titleH; };
static GLay gridLayout(){
  GLay g;
  bool big = SW >= 1600;
  g.gapx=big?24:18; g.gapy=big?18:14; g.titleH=g_showGameTitles?(big?30:24):0;
  int topBar=big?112:80,footer=big?54:38;
  g.rows=g_gridRows;
  int availH = SH - topBar - footer;
  int caption=g.titleH?g.titleH+8:0;
  int maxCoverH=(availH-(g.rows-1)*g.gapy-g.rows*caption)/g.rows;
  if(maxCoverH<72) maxCoverH=72;
  int margin = big?60:40;
  int autoWidth=maxCoverH*2/3;
  g.cols=g_gridColumns;
  int maxCoverW=(SW-2*margin-(g.cols-1)*g.gapx)/g.cols;
  g.cw=std::max(48,std::min(autoWidth,maxCoverW));
  g.chh=std::min(maxCoverH,g.cw*3/2);
  g.cw=g.chh*2/3;
  int gridW = g.cols*g.cw + (g.cols-1)*g.gapx;
  g.x0 = (SW - gridW)/2;
  int gridH=g.rows*(g.chh+caption)+(g.rows-1)*g.gapy;
  g.y0=topBar+std::max(0,(availH-gridH)/2);
  return g;
}
static int gridHitTest(int px,int py,int top){
  GLay L=gridLayout(); int n=(int)g_games.size();
  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c; if(idx>=n) continue;
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    if(px>=x-4 && px<x+L.cw+4 && py>=y-4 && py<y+L.chh+(L.titleH?L.titleH+8:0)) return idx;
  }
  return -1;
}
static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col){
  TTF_Font*f=g_font_sm;
  int tw=textW(f,title.c_str());
  if(tw<=cellW){ drawTextC(f,cx,y,title.c_str(),col); return; }
  int x0=cx-cellW/2;
  if(!sel){
    const std::string &shortened=ellipsizedText(f,title,cellW);
    drawTextC(f,cx,y,shortened.c_str(),col);
    return;
  }
  SDL_Rect clip={x0,y-2,cellW,(f?TTF_FontHeight(f):26)+8};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-cellW;
  float t=(SDL_GetTicks()%5000)/5000.0f;
  float pp = t<0.5f ? t*2.f : (1.f-t)*2.f;
  drawText(f,x0-(int)(pp*span),y,title.c_str(),col);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawTextR(f,xRight,y,s,c); return; }
  int x0=xRight-maxW;
  SDL_Rect clip={x0,y-2,maxW,(f?TTF_FontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-maxW;
  float t=(SDL_GetTicks()%6000)/6000.0f;
  float pp=t<0.5f? t*2.f : (1.f-t)*2.f;
  drawText(f,x0-(int)(pp*span),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void drawScrollTextL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawText(f,x,y,s,c); return; }
  SDL_Rect clip={x,y-2,maxW,(f?TTF_FontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-maxW;
  float t=(SDL_GetTicks()%6000)/6000.0f;
  float pp=t<0.5f? t*2.f : (1.f-t)*2.f;
  drawText(f,x-(int)(pp*span),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void renderGrid(int sel,int top,const char*gamedirLabel){
  clearUiBackground();
  g_cover_budget = COVER_DECODE_BUDGET;
  if(sel>=0 && sel<(int)g_games.size()) ensureCover(g_games[sel]);
  GLay L=gridLayout();
  int n=(int)g_games.size(), per=L.cols*L.rows;
  int pages=n?(n+per-1)/per:1, page=n?(sel/per)+1:1;
  int bandH = L.y0 - 4;
  fillRect(0,0,SW,bandH,COL_PANEL);
  if(!hasAnimatedBackground()) fillRect(0,bandH,SW,2,COL_SEL);
  int lh = bandH - 12;
  if(g_logo){ SDL_Rect ld={26,(bandH-lh)/2,lh,lh}; SDL_RenderCopy(g_ren,g_logo,nullptr,&ld); }
  char pinfo[160]; snprintf(pinfo,sizeof(pinfo),"%d / %d    \xc2\xb7    Page %d / %d    \xc2\xb7    Sort: %s",n?sel+1:0,n,page,pages,SORT_NAME[g_sort]);
  drawTextC(g_font,SW/2,(bandH-TTF_FontHeight(g_font))/2,pinfo,COL_VAL);
  int pinfoRight=SW/2+textW(g_font,pinfo)/2;
  int folderMaxW=(SW-34)-(pinfoRight+24);
  drawScrollTextR(g_font_sm,SW-34,(bandH-TTF_FontHeight(g_font_sm))/2,folderMaxW,gamedirLabel,COL_DIM);

  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c;
    if(idx>=n) continue;
    Game&g=g_games[idx];
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    bool cur=(idx==sel);
    ensureCover(g);
    fillRect(x+4,y+6,L.cw,L.chh,(SDL_Color){0,0,0,55});
    fillRect(x+2,y+3,L.cw,L.chh,(SDL_Color){0,0,0,70});
    if(g.cover){
      Uint32 el=SDL_GetTicks()-g.coverAt; Uint8 fa=!g_uiAnimations?255:(el<180?(Uint8)(255*el/180):255);
      SDL_SetTextureAlphaMod(g.cover,fa);
      SDL_SetTextureColorMod(g.cover,cur?255:150,cur?255:150,cur?255:150);
      SDL_Rect d={x,y,L.cw,L.chh}; SDL_RenderCopy(g_ren,g.cover,nullptr,&d);
      if(g.biosBoot){
        const int badgeH=std::max(24,L.chh/7);
        fillRect(x,y+L.chh-badgeH,L.cw,badgeH,(SDL_Color){2,10,32,220});
        const std::string badge=ellipsizedText(g_font_sm,"PS2 BIOS",L.cw-8);
        drawTextC(g_font_sm,x+L.cw/2,y+L.chh-badgeH+(badgeH-TTF_FontHeight(g_font_sm))/2,
                  badge.c_str(),cur?COL_VAL:COL_TXT);
      }
    }
    else { fillRect(x,y,L.cw,L.chh,COL_CARD); drawTextC(g_font_sm,x+L.cw/2,y+L.chh/2-8,"NO COVER",COL_DIM); }
    border(x,y,L.cw,L.chh,1,(SDL_Color){12,13,18,255});
    fillRect(x,y,L.cw,1,(SDL_Color){255,255,255,26});
    if(cur){ const int G=6;
      for(int i=G;i>=1;i--){ Uint8 a=(Uint8)(150*(G-i+1)/G); border(x-2-i,y-2-i,L.cw+4+2*i,L.chh+4+2*i,1,(SDL_Color){255,170,0,a}); }
      border(x-2,y-2,L.cw+4,L.chh+4,2,COL_SEL);
    }
    if(g.region>0 && g_flag[g.region]){
      int fw=L.cw*26/100; if(fw>30)fw=30; if(fw<16)fw=16; int fh=fw*2/3;
      SDL_Rect fd={x+6,y+6,fw,fh}; SDL_RenderCopy(g_ren,g_flag[g.region],nullptr,&fd);
      border(x+6,y+6,fw,fh,1,(SDL_Color){10,12,18,255});
    }
    if(g.hasCfg){ int ds=L.cw/11<12?12:L.cw/11; fillRect(x+L.cw-ds-8,y+8,ds,ds,COL_SEL); border(x+L.cw-ds-8,y+8,ds,ds,2,(SDL_Color){10,12,18,255}); }
    if(g_showGameTitles) drawTitleCell(x+L.cw/2,L.cw,y+L.chh+6,g.title,cur,cur?COL_VAL:COL_DIM);
  }
  if(n==0) drawTextC(g_font,SW/2,SH/2,"No games found -- open Settings > Library & storage",COL_DIM);
  drawUpdateNotification();
  FootItem foot[] = {
    { "A", "Launch", FA_LAUNCH }, { "Y", "Sort", FA_SORT },
    { "X", "Settings", FA_SETTINGS }, { "+", "Game Menu", FA_OPTIONS },
    { "L", "", FA_PAGEL }, { "R", "Page", FA_PAGER }, { "B", "Quit", FA_QUIT },
  };
  drawFooterHints(foot, 7, SH-26);
  SDL_RenderPresent(g_ren);
}

static int gridNav(int sel,int dx,int dy,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, page=sel/per, pos=sel%per, cr=pos/cols, cc=pos%cols;
  auto clamp=[&](int i){ return i>=n? n-1 : (i<0?0:i); };
  if(dx>0){
    if(cc<cols-1 && page*per+cr*cols+cc+1 < n) return page*per+cr*cols+cc+1;
    if((page+1)*per < n) return clamp((page+1)*per + cr*cols);
    return sel;
  }
  if(dx<0){
    if(cc>0) return sel-1;
    if(page>0) return clamp((page-1)*per + cr*cols + (cols-1));
    return sel;
  }
  if(dy>0){
    if(cr<rows-1 && page*per+(cr+1)*cols+cc < n) return page*per+(cr+1)*cols+cc;
    return sel;
  }
  if(dy<0){
    if(cr>0) return sel-cols;
    return sel;
  }
  return sel;
}

static int gridPage(int sel,int dir,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, pos=sel%per, maxpage=(n-1)/per;
  int np=sel/per + dir; if(np<0) np=0; if(np>maxpage) np=maxpage;
  int i=np*per+pos; return i>=n? n-1 : i;
}

static bool ensureDirectory(const char *path) {
  if(mkdir(path,0777)==0) return true;
  if(errno!=EEXIST) return false;
  struct stat st{};
  return stat(path,&st)==0&&S_ISDIR(st.st_mode);
}

static void cleanupLauncher() {
  LauncherUpdate_Shutdown();
  for(auto &game:g_games){ if(game.cover) SDL_DestroyTexture(game.cover); game.cover=nullptr; }
  clearTextCaches();
  for(int index=1;index<4;index++){ if(g_flag[index]) SDL_DestroyTexture(g_flag[index]); g_flag[index]=nullptr; }
  SDL_Texture **glyphs[]={&g_gA,&g_gB,&g_gX,&g_gY,&g_gPlus,&g_gL,&g_gR};
  for(SDL_Texture **glyph:glyphs){ if(*glyph) SDL_DestroyTexture(*glyph); *glyph=nullptr; }
  if(g_logo) SDL_DestroyTexture(g_logo);
  g_logo=nullptr;
  if(g_glowTexture) SDL_DestroyTexture(g_glowTexture);
  g_glowTexture=nullptr;
  if(g_font) TTF_CloseFont(g_font);
  if(g_font_sm) TTF_CloseFont(g_font_sm);
  if(g_font_big) TTF_CloseFont(g_font_big);
  g_font=g_font_sm=g_font_big=nullptr;
  if(g_plReady) plExit();
  g_plReady=false;
  uiAudioShutdown();
  SwitchStorage::Shutdown();
  closeController();
  if(g_ren) SDL_DestroyRenderer(g_ren);
  if(g_win) SDL_DestroyWindow(g_win);
  g_ren=nullptr; g_win=nullptr;
  if(g_imgReady) IMG_Quit();
  if(g_ttfReady) TTF_Quit();
  if(g_sdlReady) SDL_Quit();
  g_imgReady=g_ttfReady=g_sdlReady=false;
  if(g_griddbReady) griddb_global_exit();
  g_griddbReady=false;
  if(g_storageSocketReady) socketExit();
  g_storageSocketReady=false;
  if(g_romfsReady) romfsExit();
  g_romfsReady=false;
}

static int startupFailure(const char *message) {
  if(g_sdlReady) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"NetherSX2 Launcher",message,g_win);
  cleanupLauncher();
  return 1;
}

int main(int argc, char **argv){
  extern std::string g_forwarderSelfPath;
  if(argc>=1&&argv[0]&&argv[0][0]){
    g_forwarderSelfPath=argv[0];
    const std::string resolvedLauncherPath=launcherNroPath();
    if(!resolvedLauncherPath.empty()) g_launcherNroPath=resolvedLauncherPath;
  }
  std::string updateRecoveryError;
  const bool updateRecoveryOk=LauncherUpdate_RecoverInstallation(g_launcherNroPath,updateRecoveryError);
  if(R_FAILED(romfsInit())) return 1;
  g_romfsReady=true;
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO)!=0) return startupFailure("SDL initialization failed.");
  g_sdlReady=true;
  uiAudioInit();
  if(TTF_Init()!=0) return startupFailure("Font initialization failed.");
  g_ttfReady=true;
  const int imageFlags=IMG_INIT_PNG|IMG_INIT_JPG;
  if((IMG_Init(imageFlags)&imageFlags)!=imageFlags) return startupFailure("Image initialization failed.");
  g_imgReady=true;
  if(appletGetOperationMode()==AppletOperationMode_Console){ SW=1920; SH=1080; }
  g_win=SDL_CreateWindow("NetherSX2",0,0,SW,SH,SDL_WINDOW_FULLSCREEN);
  if(!g_win) return startupFailure("Could not create the launcher window.");
  g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!g_ren) return startupFailure("Could not create the launcher renderer.");
  SDL_SetRenderDrawBlendMode(g_ren,SDL_BLENDMODE_BLEND);
  if(SDL_GetRendererOutputSize(g_ren,&SW,&SH)!=0) return startupFailure("Could not query the display size.");
  if(SDL_Surface *logo=IMG_Load("romfs:/logo.png")){ g_logo=SDL_CreateTextureFromSurface(g_ren,logo); SDL_FreeSurface(logo); }
  makeFlags();
  for(int index=0;index<SDL_NumJoysticks();index++) if(SDL_IsGameController(index)){ openController(index); break; }

  if(R_FAILED(plInitialize(PlServiceType_User))) return startupFailure("System font service initialization failed.");
  g_plReady=true;
  PlFontData fontData{};
  if(R_FAILED(plGetSharedFontByType(&fontData,PlSharedFontType_Standard))||!fontData.address||!fontData.size||fontData.size>INT_MAX)
    return startupFailure("Could not load the system font.");
  int scale=SH>=1080?1:0;
  auto openFont=[&](int size)->TTF_Font*{ SDL_RWops *rw=SDL_RWFromConstMem(fontData.address,(int)fontData.size); return rw?TTF_OpenFontRW(rw,1,size):nullptr; };
  g_font_sm=openFont(scale?26:20);
  g_font=openFont(scale?32:26);
  g_font_big=openFont(scale?52:40);
  if(!g_font_sm||!g_font||!g_font_big) return startupFailure("Could not open the system font.");
  makeGlyphs();

  g_griddbReady=griddb_global_init();
  if(!g_griddbReady&&R_SUCCEEDED(socketInitializeDefault())) g_storageSocketReady=true;
  const char *directories[]={"sdmc:/switch",DATA_DIR,EMU_HOST_DIR,COVERS_DIR,CORES_DIR,GAMECFG_DIR,GAMECRC_DIR,CHEATS_DIR,DEF_GAMEDIR,BIOS_DIR,RESOURCES_DIR,LSFG_DIR};
  for(const char *directory:directories) if(!ensureDirectory(directory)) return startupFailure("Could not create the NetherSX2 data directories.");
  migrateLegacyEmuHosts();
  if(!updateRecoveryOk)
    modalMessage("Update recovery failed",{updateRecoveryError,"The installed launcher was left unchanged."});

  struct stat configStat{};
  bool firstRun=stat(LAUNCHER_INI,&configStat)!=0;
  storeLoad(g_global,LAUNCHER_INI);
  const bool settingsNormalized=normalizeLsfgStore(g_global) |
                                removeLegacySmcSettings(g_global) |
                                removeLegacyCheatGate(g_global) |
                                normalizeRetroAchievementsStore(g_global);
  storeLoad(g_titles,TITLES_INI);
  storeLoad(g_recent,RECENT_INI);
  int sortMode=atoi(storeGet(g_global,"Wrapper/SortMode","0"));
  if(sortMode>=0&&sortMode<SORT_COUNT) g_sort=sortMode;
  if(firstRun){
    g_active=&g_global;
    saveGameSources({DEF_GAMEDIR});
    storeSet(g_global,"Wrapper/SteamGridDBKey","");
    storeSet(g_global,"Wrapper/UiSounds","true");
    storeSet(g_global,"Wrapper/Theme","animated");
    storeSet(g_global,"Wrapper/GridColumns","6");
    storeSet(g_global,"Wrapper/GridRows","2");
    storeSet(g_global,"Wrapper/ControllerCount","2");
    storeSet(g_global,"Wrapper/Pad1/Deadzone","10");
    storeSet(g_global,"Wrapper/ShowGameTitles","true");
    storeSet(g_global,"Wrapper/ShowPS2BIOS","true");
    storeSet(g_global,"Wrapper/UiAnimations","true");
    storeSet(g_global,"Wrapper/CheckUpdatesAtBoot","true");
    storeSet(g_global,"Wrapper/InstalledReleaseTag",LauncherUpdate_BuiltReleaseTag());
    commitAll();
    if(!storeSave(g_global,LAUNCHER_INI)) return startupFailure("Could not create launcher.ini.");
  } else {
    bool changed=settingsNormalized;
    if(!storeHas(g_global,"Wrapper/GamePathCount")){ saveGameSources(loadGameSources()); changed=true; }
    int columns=atoi(storeGet(g_global,"Wrapper/GridColumns","6"));
    int rows=atoi(storeGet(g_global,"Wrapper/GridRows","2"));
    if(columns<3||columns>8){ storeSet(g_global,"Wrapper/GridColumns","6"); changed=true; }
    if(rows<1||rows>3){ storeSet(g_global,"Wrapper/GridRows","2"); changed=true; }
    if(changed&&!storeSave(g_global,LAUNCHER_INI)) return startupFailure("Could not update launcher.ini.");
  }
  applyLauncherAppearance();
  uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  std::vector<std::string> gamePaths=loadGameSources();
  bool hasUsbSource=hasConfiguredUsbSource(gamePaths);
  SwitchStorage::InitializeFromConfig(LAUNCHER_INI,hasUsbSource);
  uint64_t usbGeneration=SwitchStorage::UsbStatusGeneration();
  refreshConfiguredUsbSources(gamePaths);
  scanGames(gamePaths);
  Uint32 usbRefreshAt=0;
  const uint64_t startupUsbGeneration=SwitchStorage::UsbStatusGeneration();
  if(startupUsbGeneration!=usbGeneration){ usbGeneration=startupUsbGeneration; usbRefreshAt=SDL_GetTicks()+300; }

  if(!biosPresent()) modalMessage("No PS2 BIOS found",{"Copy a PS2 BIOS dump into:",toEmu(BIOS_DIR),"","Games will not boot until you add one."});

  int sel=0,top=0,rows=1;
  bool running=true,launch=false;
  bool launchBios=false;
  std::string launchKey,launchLegacyKey,launchPath;
  bool launchLegacyUnique=false;
  auto selectGame=[&](Game &game){
    launchBios=game.biosBoot;
    if(launchBios){
      launchKey.clear();
      launchLegacyKey.clear();
      launchLegacyUnique=false;
      launchPath.clear();
    } else {
      recordPlayed(game);
      storeSet(g_global,"EmuCore/DiscPath",toEmu(game.path).c_str());
      launchKey=game.key;
      launchLegacyKey=game.legacyKey;
      launchLegacyUnique=game.legacyUnique;
      launchPath=game.path;
    }
    launch=true;
    running=false;
  };

  bool forwarderRequested=false,forwarderMatched=false;
  std::string forwarderKey;
  for(int argument=1;argument+1<argc;argument++) if(!strcmp(argv[argument],"-g")){
    forwarderRequested=true;
    forwarderKey=argv[argument+1];
    if(Game *game=findGameByKey(forwarderKey)){ selectGame(*game); forwarderMatched=true; }
    break;
  }
  if(!forwarderRequested&&g_griddbReady&&
     strcmp(storeGet(g_global,"Wrapper/CheckUpdatesAtBoot","true"),"false")!=0)
    LauncherUpdate_StartCheck(installedReleaseTag());
  bool forwarderPending=forwarderRequested&&!forwarderMatched&&hasUsbSource;
  const Uint32 forwarderDeadline=forwarderPending?SDL_GetTicks()+6000:0;
  if(forwarderPending&&!usbRefreshAt) usbRefreshAt=SDL_GetTicks()+300;
  if(forwarderRequested&&!forwarderMatched&&!forwarderPending){
    modalMessage("Game not found",{"The shortcut's game is not in the current library.","","Reconnect its storage or update the game folders."});
    running=false;
  }

  while(running&&beginUiFrame()){
    if(hasUsbSource){
      const Uint32 now=SDL_GetTicks();
      const uint64_t generation=SwitchStorage::UsbStatusGeneration();
      if(generation!=usbGeneration){ usbGeneration=generation; usbRefreshAt=now+300; }
      if(usbRefreshAt&&SDL_TICKS_PASSED(now,usbRefreshAt)){
        usbRefreshAt=0;
        const std::string selected=!g_games.empty()?g_games[sel].key:std::string{};
        refreshConfiguredUsbSources(gamePaths);
        scanGames(gamePaths);
        sel=0;
        if(!selected.empty()) for(size_t index=0;index<g_games.size();index++) if(g_games[index].key==selected){ sel=(int)index; break; }
        top=0;
        if(forwarderPending) if(Game *game=findGameByKey(forwarderKey)){
          selectGame(*game);
          forwarderPending=false;
        }
      }
      if(forwarderPending&&SDL_TICKS_PASSED(now,forwarderDeadline)){
        forwarderPending=false;
        modalMessage("Game not found",{"The shortcut's game is not in the current library.","","Reconnect its storage or update the game folders."});
        running=false;
      }
      if(!running) break;
    }
    if(forwarderPending){
      SDL_Event event;
      while(pollUiEvent(event)){
        pumpStick(event);
        if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL){
          running=false;
          break;
        }
      }
      if(!running) break;
      renderUsbForwarderWait();
      SDL_Delay(8);
      continue;
    }
    GLay layout=gridLayout(); int cols=layout.cols; rows=layout.rows;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0,n=(int)g_games.size(); TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SWIPE_L||touch==TOUCH_SWIPE_R){ sel=gridPage(sel,touch==TOUCH_SWIPE_L?1:-1,cols,rows,n); top=n?(sel/(cols*rows))*rows:0; continue; }
      if(touch==TOUCH_TAP){
        int action=footTapAct(tx,ty);
        if(action==FA_NONE){
          int hit=gridHitTest(tx,ty,top);
          if(hit>=0){ if(hit==sel&&n) selectGame(g_games[sel]); else sel=hit; }
        } else {
          SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN;
          switch(action){
            case FA_LAUNCH: press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break;
            case FA_SORT: press.cbutton.button=SDL_CONTROLLER_BUTTON_X; SDL_PushEvent(&press); break;
            case FA_OPTIONS: press.cbutton.button=SDL_CONTROLLER_BUTTON_START; SDL_PushEvent(&press); break;
            case FA_SETTINGS: press.cbutton.button=BTN_SETTINGS; SDL_PushEvent(&press); break;
            case FA_PAGEL: sel=gridPage(sel,-1,cols,rows,n); break;
            case FA_PAGER: sel=gridPage(sel,1,cols,rows,n); break;
            case FA_QUIT: running=false; break;
          }
        }
        top=n?(sel/(cols*rows))*rows:0;
        if(!running) break;
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(event.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: sel=gridNav(sel,-1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=gridNav(sel,1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=gridNav(sel,0,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=gridNav(sel,0,1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: sel=gridPage(sel,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: sel=gridPage(sel,1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_X:
          if(n){
            std::string keep=g_games[sel].key; g_sort=(g_sort+1)%SORT_COUNT;
            storeSet(g_global,"Wrapper/SortMode",std::to_string(g_sort).c_str()); storeSave(g_global,LAUNCHER_INI);
            applySort(); sel=0; for(int index=0;index<n;index++) if(g_games[index].key==keep){ sel=index; break; }
          }
          break;
        case BTN_CONFIRM: if(n) selectGame(g_games[sel]); break;
        case SDL_CONTROLLER_BUTTON_START:
          if(n){ int result=perGameMenu(g_games[sel],g_pad); if(result==1) selectGame(g_games[sel]); else if(result==2){ scanGames(gamePaths); sel=top=0; } }
          break;
        case BTN_SETTINGS: {
          std::vector<std::string> oldPaths=gamePaths;
          g_active=&g_global; runSettingsRoot(g_pad,nullptr,nullptr); storeSave(g_global,LAUNCHER_INI);
          layout=gridLayout(); cols=layout.cols; rows=layout.rows;
          gamePaths=loadGameSources();
          if(gamePaths!=oldPaths||g_rescanAfterSettings){
            hasUsbSource=hasConfiguredUsbSource(gamePaths);
            if(hasUsbSource) SwitchStorage::InitializeUsb();
            usbGeneration=SwitchStorage::UsbStatusGeneration();
            usbRefreshAt=0;
            refreshConfiguredUsbSources(gamePaths);
            scanGames(gamePaths);
            sel=top=0;
            g_rescanAfterSettings=false;
          }
          break;
        }
        case BTN_CANCEL: running=false; break;
      }
      top=n?(sel/(cols*rows))*rows:0;
    }
    pollUpdateNotification();
    const std::string location=!g_games.empty()?gameLocationLabel(g_games[sel]):"No game selected";
    renderGrid(sel,top,location.c_str());
    SDL_Delay(8);
  }
  g_active=&g_global;
  if(launch) commitAll();
  storeSave(g_global,LAUNCHER_INI);
  storeSave(g_recent,RECENT_INI);

  bool willChain=false;
  std::string emulatorNro;
  if(launch&&envHasNextLoad()){
    Store effective=g_global;
    std::string gameCRCPath;
    if(!launchKey.empty()){
      std::string profile=std::string(GAMECFG_DIR)+"/"+launchKey+".ini";
      if(!regularFileExists(profile)&&launchLegacyUnique&&!launchLegacyKey.empty()) profile=std::string(GAMECFG_DIR)+"/"+launchLegacyKey+".ini";
      Store overrides; storeLoad(overrides,profile.c_str());
      for(const auto &entry:overrides.kv) storeSet(effective,entry.k.c_str(),entry.v.c_str());
      gameCRCPath=toEmu(std::string(GAMECRC_DIR)+"/"+launchKey+".ini");
    }
    normalizeLsfgStore(effective);
    removeLegacySmcSettings(effective);
    removeLegacyCheatGate(effective);
    applyGlobalRetroAchievementsSettings(effective);
    std::string build=storeGet(effective,"Wrapper/CoreBuild","4248");
    if(build!="4248"&&build!="3668") build="4248";
    std::string renderer=strcmp(storeGet(effective,"EmuCore/GS/Renderer","14"),"12")==0?"gl":"vk";
    const bool disableThreadedPresentation=!strcmp(
        storeGet(effective,"EmuCore/GS/DisableThreadedPresentation","false"),"true");
    if(build=="3668"){
      storeSet(effective,"EmuCore/GS/ThreadedPresentation",
               disableThreadedPresentation?"false":"true");
      storeRemove(effective,"EmuCore/GS/DisableThreadedPresentation");
    } else {
      storeSet(effective,"EmuCore/GS/DisableThreadedPresentation",
               disableThreadedPresentation?"true":"false");
      storeRemove(effective,"EmuCore/GS/ThreadedPresentation");
    }
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    std::string coreSource="romfs:/cores/emucore_"+build+".so";
    std::string coreDestination=std::string(CORES_DIR)+"/libemucore_"+build+".so";
    std::string emulatorSource="romfs:/emu/NetherSX2_nx_"+renderer+".nro";
    std::string emulatorDestination=std::string(EMU_HOST_DIR)+"/NetherSX2_nx_"+renderer+".nro";
    emulatorNro=std::string(EMU_HOST_DIR)+"/NetherSX2_nx_"+renderer+".nro";
    bool haveCore=ensureCore(coreSource.c_str(),coreDestination.c_str(),build);
    bool haveEmulator=ensureEmu(emulatorSource.c_str(),emulatorDestination.c_str());
    bool haveResources=ensureResources(build);
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(haveCore){
      std::string corePath="/switch/nethersx2/cores/libemucore_"+build+".so";
      storeSet(g_global,"Wrapper/CoreSo",corePath.c_str());
      storeSet(effective,"Wrapper/CoreSo",corePath.c_str());
    }
    storeSet(effective,"Wrapper/BootBIOS",launchBios?"true":"false");
    storeSet(effective,"EmuCore/DiscPath",launchBios?"":toEmu(launchPath).c_str());
    storeSet(effective,"Folders/Cheats",toEmu(CHEATS_DIR).c_str());
    storeSet(effective,"EmuCore/EnableCheats","true");
    storeRemove(effective,"Wrapper/LauncherPath");
    storeRemove(effective,"Wrapper/GameConfigPath");
    storeRemove(effective,"Wrapper/GameCRCPath");
    const std::string launcherPath=launcherNroPath();
    if(!launcherPath.empty()) storeSet(effective,"Wrapper/LauncherPath",launcherPath.c_str());
    if(!gameCRCPath.empty()) storeSet(effective,"Wrapper/GameCRCPath",gameCRCPath.c_str());
    const bool lsfgRequested=!strcmp(storeGet(effective,"Wrapper/LSFGEnabled","false"),"true");
    const char *lsfgWarning=nullptr;
    if(lsfgRequested&&renderer!="vk"){
      storeSet(effective,"Wrapper/LSFGEnabled","false");
      lsfgWarning="LSFG disabled for this launch: Vulkan is required";
    } else if(lsfgRequested&&!lsfgDllInstalled()){
      storeSet(effective,"Wrapper/LSFGEnabled","false");
      lsfgWarning="LSFG disabled: Lossless.dll is missing";
    } else if(lsfgRequested){
      // LSFG 2x must receive unique frames, not the duplicate presents used to
      // refresh 30 FPS PS2 titles at 60 Hz.
      storeSet(effective,"EmuCore/GS/SkipDuplicateFrames","true");
    }
    bool configSaved=storeSave(effective,EMU_INI);
    willChain=haveCore&&haveEmulator&&haveResources&&configSaved;
    if(willChain&&lsfgWarning){
      toast(lsfgWarning);
      SDL_Delay(1800);
    } else if(!willChain){
      if(!haveResources) toast("Could not extract NetherSX2 resources (SD full?)");
      else if(!haveCore||!haveEmulator) toast("Could not extract emulator files (SD full?)");
      else toast("Could not write the launch configuration");
      SDL_Delay(1800);
    }
  }

  cleanupLauncher();
  if(willChain) envSetNextLoad(emulatorNro.c_str(),emulatorNro.c_str());
  return 0;
}
