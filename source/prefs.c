/* prefs.c -- native key/value store backing the Android SharedPreferences.
 *
 * libemucore.so reads every setting through PreferenceHelpers JNI callbacks
 * (getDefaultSharedPreferences + SharedPreferences.get{Boolean,Int,Float,String}
 * + Editor.put*). Keys arrive as flat "Section/Key" strings (e.g.
 * "EmuCore/GS/Renderer"). We back that with a tiny in-memory map persisted to
 * nethersx2.ini, seeded with the defaults needed to boot the OpenGL renderer.
 *
 * Plain stdio + string code: no libnx dependency, so it stays trivially
 * testable and robust against the core hammering it during init.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "prefs.h"
#include "config.h"
#include "util.h"

// Fixed-cap map. The core only reads a few dozen keys to boot and the full
// AetherSX2 settings set is well under this; a flat array keeps lookup simple
// and avoids any allocation during the latency-sensitive init path.
#define PREFS_MAX_ENTRIES 512
#define PREFS_KEY_LEN     128
#define PREFS_VAL_LEN     256

typedef struct {
  char key[PREFS_KEY_LEN];
  char val[PREFS_VAL_LEN];
} PrefEntry;

static PrefEntry s_map[PREFS_MAX_ENTRIES];
static int       s_count;
static char      s_ini_path[512];

// disc path stashed before seeding (from the launcher's EmuCore/DiscPath); empty
// => use the config.h default. Kept separate so prefs_set_disc_path can run before init.
static char s_disc_path[PREFS_VAL_LEN];

// ---------------------------------------------------------------------------
// core map ops
// ---------------------------------------------------------------------------

// returns the entry index for an exact (case-sensitive) key match, or -1.
static int prefs_find(const char *key) {
  if (!key)
    return -1;
  for (int i = 0; i < s_count; ++i) {
    if (strcmp(s_map[i].key, key) == 0)
      return i;
  }
  return -1;
}

// insert or overwrite. Truncates safely to the fixed field widths -- the core's
// keys/values are all short, but we never want an overrun from a stray ini line.
static void prefs_put(const char *key, const char *val) {
  if (!key || !val)
    return;

  int i = prefs_find(key);
  if (i < 0) {
    if (s_count >= PREFS_MAX_ENTRIES) {

      return;
    }
    i = s_count++;
    snprintf(s_map[i].key, sizeof(s_map[i].key), "%s", key);
  }
  snprintf(s_map[i].val, sizeof(s_map[i].val), "%s", val);
}

// seed a default only if the key is absent -- never clobber a user value that
// was already present in the ini.
static void prefs_seed(const char *key, const char *val) {
  if (prefs_find(key) < 0)
    prefs_put(key, val);
}

// ---------------------------------------------------------------------------
// ini parsing
// ---------------------------------------------------------------------------

static char *trim(char *s) {
  if (!s)
    return s;
  while (*s && isspace((unsigned char)*s))
    ++s;
  if (*s == 0)
    return s;
  char *end = s + strlen(s) - 1;
  while (end > s && isspace((unsigned char)*end))
    *end-- = 0;
  return s;
}

// Parse our flat ini: lines of "Section/Key = value", '#' (or ';') comments,
// blank lines ignored. We deliberately do NOT honour [Section] headers -- the
// keys the core uses are already the full "Section/Key" path, so storing them
// flat keeps lookup a single strcmp.
static void prefs_parse_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {

    return;
  }

  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char *p = trim(line);
    if (p[0] == 0 || p[0] == '#' || p[0] == ';')
      continue;
    // skip any [Section] header line defensively; we use flat keys.
    if (p[0] == '[')
      continue;

    char *eq = strchr(p, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *key = trim(p);
    char *val = trim(eq + 1);
    if (key[0] == 0)
      continue;
    prefs_put(key, val);
  }

  fclose(f);
}

// ---------------------------------------------------------------------------
// default seeding (boot-critical settings for the OpenGL path)
// ---------------------------------------------------------------------------

static void prefs_seed_defaults(void) {
  char buf[32];

  // Folders: absolute SD paths. DataRoot itself is established at runtime via
  // the faked getDataDirectory(); these override the per-folder leaf defaults
  // so the core's BIOS/save/cache scans hit known dirs. Leaf names match the
  // bare tokens in libemucore (bios/snaps/sstates/memcards/...). config.h only
  // defines a few of these as macros, so the rest are built off DATA_ROOT.
  prefs_seed("Folders/Bios",          BIOS_DIR);
  prefs_seed("Folders/Snapshots",     DATA_ROOT "/snaps");
  prefs_seed("Folders/Savestates",    DATA_ROOT "/sstates");
  prefs_seed("Folders/MemoryCards",   DATA_ROOT "/memcards");
  prefs_seed("Folders/Cache",         CACHE_DIR);
  prefs_seed("Folders/Textures",      DATA_ROOT "/textures");
  prefs_seed("Folders/Covers",        DATA_ROOT "/covers");
  prefs_seed("Folders/GameSettings",  DATA_ROOT "/gamesettings");
  prefs_seed("Folders/InputProfiles", DATA_ROOT "/inputprofiles");
  // Cheats live alongside the resources (cheats_ni/ws.zip ship in assets/).
  prefs_seed("Folders/Cheats",        RESOURCES_DIR);
  prefs_seed("Folders/Logs",          DATA_ROOT "/logs");
  prefs_seed("Folders/Resources",     RESOURCES_DIR);

  // GPU / renderer: OpenGL (12) or Vulkan/NVK (14), chosen at build time
  // (config.h GS_RENDERER, set by the Makefile). Stored as the integer enum
  // form; the PreferenceHelpers shim returns it for GetIntValue.
  snprintf(buf, sizeof(buf), "%d", GS_RENDERER);
  prefs_seed("EmuCore/GS/Renderer", buf);
  prefs_seed("EmuCore/GS/upscale_multiplier", "1");  // native res; bump later
  // PCSX2 parses AspectRatio as an enum NAME, not an index. "1" was rejected every
  // load ("(LoadSettings) Warning: Unrecognized value '1' on key 'AspectRatio'"),
  // silently falling back to the default. Valid: "Stretch", "Auto 4:3/3:2",
  // "4:3", "16:9". Editable in nethersx2.ini.
  prefs_seed("EmuCore/GS/AspectRatio", "4:3");
  prefs_seed("EmuCore/GS/VsyncEnable", "0");
#if GS_RENDERER == GS_RENDERER_VK
  // Keep presentation work off the GS thread by default. The two bundled cores
  // use opposite names for the same state.
  prefs_seed("EmuCore/GS/DisableThreadedPresentation", "0");
  prefs_seed("EmuCore/GS/ThreadedPresentation", "1");
#endif
  prefs_seed("EmuCore/GS/SkipDuplicateFrames", "false");

  // Experimental in-process LSFG-VK bridge. It is deliberately opt-in and the
  // proprietary shader DLL is never bundled; users provide their own file.
  prefs_seed("Wrapper/LSFGEnabled", "false");
  prefs_seed("Wrapper/LSFGFlowScale", "0.25");
  prefs_seed("Wrapper/LSFGPerformance", "true");
  prefs_seed("Wrapper/FastmemMode", "hybrid");
  // core
  prefs_seed("EmuCore/EnableCheats", "0");
  // Fast boot: read the disc's SYSTEM.CNF and boot the game's ELF directly,
  // skipping the BIOS animation. Without it the core full-boots the BIOS, which
  // in our headless (no-UI) setup never hands off to the disc -- it boots to the
  // BIOS OSDSYS (reported as a date-format 'serial') then the VM cleanly stops.
  // The core gates on contains() before getBoolean(), so this MUST be seeded
  // (present) for the value to be read at all.
  // Re-confirmed 2026-07-10: with this "0" the BIOS loads rom0:OSDSYS and parks
  // there -- the disc ELF is never loaded. Must stay "1". (Also forced in main.c.)
  prefs_seed("EmuCore/EnableFastBoot", "1");
  // fastCDVD DISABLED (also forced false in main.c): it breaks CDVD command timing
  // and hangs games in sceCdInit's cdvdfsv RPC bind. Disc reads work without it.
  prefs_seed("EmuCore/Speedhacks/fastCDVD", "false");

  // Core's own logging, all off for release: the EE/IOP console + verbose paths
  // format many strings per frame and each line hits an SD-backed log, capping
  // FPS. The core gates getBoolean behind contains(), so these must be seeded to
  // be read; main.c re-forces them off each boot.
  prefs_seed("Logging/EnableSystemConsole", "0");
  prefs_seed("Logging/EnableFileLogging", "0");
  prefs_seed("Logging/EnableVerbose", "0");
  prefs_seed("Logging/EnableEEConsole", "0");
  prefs_seed("Logging/EnableIOPConsole", "0");

  // first-boot / wizard: we have no Java UI, so pretend the setup wizard ran.
  prefs_seed("UI/HasRunWizard", "true");

  // boot disc: the path stashed via prefs_set_disc_path wins; otherwise the
  // config.h default. (The VM is told to boot this once the core is up.)
  prefs_seed("EmuCore/DiscPath",
             s_disc_path[0] ? s_disc_path : DEFAULT_DISC_PATH);
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void prefs_set_disc_path(const char *path) {
  if (path && path[0])
    snprintf(s_disc_path, sizeof(s_disc_path), "%s", path);
  else
    s_disc_path[0] = 0;
}

void prefs_init(const char *ini_path) {
  s_count = 0;
  memset(s_map, 0, sizeof(s_map));

  // remember where to flush back to.
  snprintf(s_ini_path, sizeof(s_ini_path), "%s",
           ini_path ? ini_path : PREFS_PATH);

  // load existing user values first, then fill in any missing boot defaults so
  // we never override what the user set.
  prefs_parse_file(s_ini_path);
  prefs_seed_defaults();
  prefs_remove("Wrapper/LSFGDllPath");
  const float lsfg_flow = prefs_get_float("Wrapper/LSFGFlowScale", 0.25f);
  if (lsfg_flow != 0.25f && lsfg_flow != 0.5f)
    prefs_set_float("Wrapper/LSFGFlowScale", lsfg_flow > 0.375f ? 0.5f : 0.25f);
}

void prefs_save(void) {
  if (s_ini_path[0] == 0)
    return;

  FILE *f = fopen(s_ini_path, "w");
  if (!f) {

    return;
  }

  fputs("# NetherSX2 settings -- auto-written; edit with the emulator stopped\n",
        f);
  // Insertion order: stable and readable; the core never relies on ordering.
  for (int i = 0; i < s_count; ++i)
    fprintf(f, "%s = %s\n", s_map[i].key, s_map[i].val);

  fclose(f);
}

bool prefs_contains(const char *key) {
  return prefs_find(key) >= 0;
}

bool prefs_get_bool(const char *key, bool def) {
  int i = prefs_find(key);
  if (i < 0)
    return def;
  const char *v = s_map[i].val;
  if (!strcmp(v, "true") || !strcmp(v, "1") ||
      !strcmp(v, "True") || !strcmp(v, "TRUE"))
    return true;
  if (!strcmp(v, "false") || !strcmp(v, "0") ||
      !strcmp(v, "False") || !strcmp(v, "FALSE"))
    return false;
  return def;  // unrecognised => fall back rather than guess
}

int prefs_get_int(const char *key, int def) {
  int i = prefs_find(key);
  return (i < 0) ? def : atoi(s_map[i].val);
}

float prefs_get_float(const char *key, float def) {
  int i = prefs_find(key);
  return (i < 0) ? def : (float)atof(s_map[i].val);
}

const char *prefs_get_string(const char *key, const char *def) {
  int i = prefs_find(key);
  // pointer stays valid until the entry is overwritten/removed or prefs_save
  // rewrites the file (which does not touch the in-memory buffers).
  return (i < 0) ? def : s_map[i].val;
}

void prefs_set_bool(const char *key, bool v) {
  prefs_put(key, v ? "true" : "false");
}

void prefs_set_int(const char *key, int v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", v);
  prefs_put(key, buf);
}

void prefs_set_float(const char *key, float v) {
  char buf[64];
  // %g keeps it compact and round-trips through atof for the core's needs.
  snprintf(buf, sizeof(buf), "%g", v);
  prefs_put(key, buf);
}

void prefs_set_string(const char *key, const char *v) {
  prefs_put(key, v ? v : "");
}

void prefs_remove(const char *key) {
  int i = prefs_find(key);
  if (i < 0)
    return;
  // compact: move the last entry into the hole (order isn't significant).
  s_map[i] = s_map[s_count - 1];
  memset(&s_map[s_count - 1], 0, sizeof(s_map[s_count - 1]));
  --s_count;
}
