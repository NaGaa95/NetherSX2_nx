/* patches.c -- binary patches applied to the writable core image
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>

#include "../config.h"
#include "../util.h"
#include "../hooks.h"
#include "../so_util.h"
#include "../prefs.h"

extern so_module emu_mod; // libemucore.so (defined in main.c)

#define INSN_NOP        0xd503201fu
#define INSN_MOV_X9_XZR 0xaa1f03e9u
#define INSN_MOV_X8_XZR 0xaa1f03e8u

typedef struct { uint32_t vaddr, expect, insn; const char *what; } Patch;
static const Patch g_patches_4248[] = {
  { 0x83a3d0, 0x5400c3cb, INSN_NOP,        "s1 b.lt(len)" },
  { 0x83a3e8, 0x940c7546, INSN_NOP,        "s1 bl memchr" },
  { 0x83a418, 0xaa0b0129, INSN_MOV_X9_XZR, "s1 force match" },
  { 0x83a774, 0x5400a6ab, INSN_NOP,        "s2 b.lt(len)" },
  { 0x83a78c, 0x940c745d, INSN_NOP,        "s2 bl memchr" },
  { 0x83a7bc, 0xaa0b0129, INSN_MOV_X9_XZR, "s2 force match" },
  { 0x83b8c0, 0x34001c58, INSN_NOP,        "s3 class-name match (cbz w24)" },
  { 0x83bbd0, 0x340003d7, INSN_NOP,        "s4 string match (cbz w23)" },
  { 0x83ce74, 0x54002b2b, INSN_NOP,        "vm b.lt(len)" },
  { 0x83ce8c, 0x940c6a9d, INSN_NOP,        "vm bl memchr" },
  { 0x83ceb8, 0xaa0a0108, INSN_MOV_X8_XZR, "vm force match" },
  // Emit the version tag expected by the GameDB cache loader.
  { 0x2e3fc8, 0x528c86ea, 0x528cad0a, "gamedb cache ver lo" },
  { 0x2e3fcc, 0x72a726ca, 0x72a52c8a, "gamedb cache ver hi" },
  // Center aspect-fitted content.
  { 0x61c924, 0x54000141, INSN_NOP, "center display X" },
  { 0x61c964, 0x54000221, INSN_NOP, "center display Y" },
  // Horizon cannot write-protect the ashmem-backed EE RAM for SMC detection.
  { 0x711180, 0xb940054a, 0x5280004a, "SMC: force ProtMode_Manual" },
  // Android exposes 0.25x, but this core revision clamps the GS multiplier to 0.5x.
  { 0x4bab84, 0x1e2c1002, 0x1e2a1002, "GS upscale minimum 0.25x" },
};

static const Patch g_patches_3668[] = {
  { 0x82d0c0, 0x5400c3eb, INSN_NOP,        "s1 b.lt(len)" },
  { 0x82d0d8, 0x940c68aa, INSN_NOP,        "s1 bl memchr" },
  { 0x82d108, 0xaa0b0129, INSN_MOV_X9_XZR, "s1 force match" },
  { 0x82d464, 0x5400a6cb, INSN_NOP,        "s2 b.lt(len)" },
  { 0x82d47c, 0x940c67c1, INSN_NOP,        "s2 bl memchr" },
  { 0x82d4ac, 0xaa0b0129, INSN_MOV_X9_XZR, "s2 force match" },
  { 0x82e5b4, 0x34001c58, INSN_NOP,        "s3 class-name match (cbz w24)" },
  { 0x82e8c4, 0x340003d7, INSN_NOP,        "s4 string match (cbz w23)" },
  { 0x82fb64, 0x54002c2b, INSN_NOP,        "vm b.lt(len)" },
  { 0x82fb7c, 0x940c5e01, INSN_NOP,        "vm bl memchr" },
  { 0x82fba8, 0xaa0a0108, INSN_MOV_X8_XZR, "vm force match" },
  { 0x2e3378, 0x528c26cb, 0x528d2e6b, "gamedb cache ver lo" },
  { 0x2e337c, 0x72a7068b, 0x72a52c6b, "gamedb cache ver hi" },
  { 0x62a204, 0x54000141, INSN_NOP, "center display X" },
  { 0x62a244, 0x54000221, INSN_NOP, "center display Y" },
  { 0x6f6c10, 0xb940054a, 0x5280004a, "SMC: force ProtMode_Manual" },
  { 0x4ab4a8, 0x1e2c1002, 0x1e2a1002, "GS upscale minimum 0.25x" },
};

// Ignore the core's boot-time RequestStop until application shutdown.
static void *g_real_reqstop = 0;
static uintptr_t g_reqstop_slot;
volatile int g_allow_stop;

static void reqstop_hook(void *thisptr) {
  if (!g_allow_stop) return;
  ((void (*)(void *))g_real_reqstop)(thisptr);
}

CoreVersion g_core_version = CORE_VER_UNKNOWN;

int core_is_3668(void) { return g_core_version == CORE_VER_V22N_3668; }

int core_shutdown_mtgs(void) {
  typedef struct {
    uintptr_t object;
    uintptr_t destructor;
  } MtgsOffsets;
  static const MtgsOffsets offsets_4248 = {0xc6adff0, 0x4add70};
  static const MtgsOffsets offsets_3668 = {0xc6a0e80, 0x49ed60};

  const MtgsOffsets *offsets = g_core_version == CORE_VER_V22N_4248 ? &offsets_4248 :
                               g_core_version == CORE_VER_V22N_3668 ? &offsets_3668 : NULL;
  if (!offsets || offsets->object + 0xb0 > emu_mod.load_size ||
      offsets->destructor + 8 > emu_mod.load_size)
    return -1;

  const uintptr_t base = (uintptr_t)emu_mod.load_virtbase;
  const uint32_t *code = (const uint32_t *)(base + offsets->destructor);
  if (code[0] != 0xa9bd7bfd || code[1] != 0xf9000bf5)
    return -1;

  void *object = (void *)(base + offsets->object);
  if (!*(void **)((uintptr_t)object + 0xa8))
    return 0;

  ((void (*)(void *))(base + offsets->destructor))(object);
  return 1;
}

static int in_range(uint32_t off, uint32_t sz) {
  return (uint64_t)off + sz <= (uint64_t)emu_mod.load_size;
}

static uint32_t a64_branch(uint32_t from, uint32_t to) {
  return 0x14000000u | (((to - from) >> 2) & 0x03ffffffu);
}

static uint32_t a64_adrp(unsigned reg, uint32_t from, uint32_t target) {
  const int64_t delta = ((int64_t)(target & ~0xfffu) -
                         (int64_t)(from & ~0xfffu)) >> 12;
  return 0x90000000u | ((uint32_t)(delta & 3) << 29) |
         ((uint32_t)((delta >> 2) & 0x7ffff) << 5) | reg;
}

static void patch_quick_menu_center(void) {
  typedef struct {
    uint32_t site;
    uint32_t cave;
    uint32_t imgui_io;
  } CenterPatch;
  static const CenterPatch patch_4248 = {0x7df77c, 0x88a15c, 0xc6cb5c0};
  static const CenterPatch patch_3668 = {0x7d4d8c, 0x87e7ec, 0xc6be458};
  const CenterPatch *patch = core_is_3668() ? &patch_3668 :
                             g_core_version == CORE_VER_V22N_4248 ? &patch_4248 : NULL;
  if (!patch || !in_range(patch->site, 4) || !in_range(patch->cave, 84))
    return;

  uint32_t *const site = (uint32_t *)((uintptr_t)emu_mod.load_base + patch->site);
  uint32_t *const cave = (uint32_t *)((uintptr_t)emu_mod.load_base + patch->cave);
  if (*site != 0x2d7327afu)
    return;
  for (unsigned i = 0; i < 21; i++)
    if (cave[i] != 0)
      return;

  uint32_t stub[21] = {
    0x2d7327af, 0x39400340, 0x7100a01f, 0x540001e1,
    0xf8401340, 0x580001c1, 0xeb01001f, 0x54000161,
    0,          0,          0xbd401002, 0x1e2b3842,
    0x1e2c1003, 0x1e23084f, 0xbd401402, 0x1e2e3842,
    0x1e230849, 0x2d3327af, 0,          0x6874656e,
    0x78737265,
  };
  stub[8] = a64_adrp(0, patch->cave + 32, patch->imgui_io);
  stub[9] = 0xf9400000u | (((patch->imgui_io & 0xfff) >> 3) << 10);
  stub[18] = a64_branch(patch->cave + 72, patch->site + 4);
  memcpy(cave, stub, sizeof(stub));
  *site = a64_branch(patch->site, patch->cave);
}

static void detect_core_version(void) {
  g_core_version = CORE_VER_UNKNOWN;
  if (in_range(0xd4436, 24) &&
      !memcmp((const char *)((uintptr_t)emu_mod.load_base + 0xd4436), "v2.2n-4248", 10))
    g_core_version = CORE_VER_V22N_4248;
  else if (in_range(0xff01f, 24) &&
           !memcmp((const char *)((uintptr_t)emu_mod.load_base + 0xff01f), "v2.2n-3668", 10))
    g_core_version = CORE_VER_V22N_3668;
}

void patch_game(void) {
  detect_core_version();

  if (g_core_version == CORE_VER_UNKNOWN)
    return;

  const Patch *tbl; int total;
  if (core_is_3668()) {
    tbl = g_patches_3668; total = (int)(sizeof(g_patches_3668) / sizeof(*g_patches_3668));
    g_reqstop_slot = 0xb6f920;
  } else {
    tbl = g_patches_4248; total = (int)(sizeof(g_patches_4248) / sizeof(*g_patches_4248));
    g_reqstop_slot = 0xb80558;
  }

  const int smc = prefs_get_bool("Wrapper/EESmcCheck", true);

  for (int i = 0; i < total; i++) {
    const Patch *pt = &tbl[i];
    if (!smc && !strncmp(pt->what, "SMC:", 4)) {

      continue;
    }
    if (!in_range(pt->vaddr, 4)) {

      continue;
    }
    volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)emu_mod.load_base + pt->vaddr);
    if (*p != pt->expect) {

      continue;
    }
    *p = pt->insn;
  }

  patch_quick_menu_center();

  if (!in_range((uint32_t)g_reqstop_slot, 8))
    return;
  volatile uintptr_t *slot =
      (volatile uintptr_t *)((uintptr_t)emu_mod.load_base + g_reqstop_slot);
  g_real_reqstop = (void *)*slot;
  *slot = (uintptr_t)&reqstop_hook;

}
