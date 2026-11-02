#ifndef USE_DEKO3D

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../hooks.h"
#include "../saltynx.h"
#include "../so_util.h"

extern so_module emu_mod;

typedef struct {
  uintptr_t *vtable;
  float scale[2];
  int32_t width, height;
} TextureSize;

_Static_assert(offsetof(TextureSize, width) == 0x10, "GS texture size ABI");

typedef void (*PresentRect)(void *, const TextureSize *, const void *,
                            void *, const void *, unsigned, float, bool);
static PresentRect real_present_rect;

static void present_rect_hook(void *device, const TextureSize *source,
                              const void *source_rect, void *destination,
                              const void *destination_rect, unsigned shader,
                              float time, bool linear) {
  if (source && !destination && source->width > 0 && source->height > 0)
    saltynx_set_resolution(source->width, source->height);
  real_present_rect(device, source, source_rect, destination,
                    destination_rect, shader, time, linear);
}

int core_install_resolution_hook(void) {
  typedef struct {
    uint32_t table, destructor, present;
    unsigned slot;
  } Profile;
#ifdef USE_VULKAN
  static const Profile profile_4248 = {0xb78c30, 0x661400, 0x662cb0, 24};
  static const Profile profile_3668 = {0xb68890, 0x6883f0, 0x68a2d0, 28};
#else
  static const Profile profile_4248 = {0xb78a20, 0x64c290, 0x653ef0, 24};
  static const Profile profile_3668 = {0xb686c0, 0x674240, 0x67bbf0, 28};
#endif
  const Profile *profile =
      g_core_version == CORE_VER_V22N_4248 ? &profile_4248 :
      g_core_version == CORE_VER_V22N_3668 ? &profile_3668 : NULL;
  if (!profile || !emu_mod.load_base || !emu_mod.load_virtbase)
    return 0;

  const size_t slot = profile->table + profile->slot * sizeof(uintptr_t);
  if (slot + sizeof(uintptr_t) > emu_mod.load_size ||
      profile->present >= emu_mod.load_size ||
      profile->destructor >= emu_mod.load_size)
    return 0;

  const uintptr_t base = (uintptr_t)emu_mod.load_virtbase;
  uintptr_t *table = (uintptr_t *)((uintptr_t)emu_mod.load_base + profile->table);
  if (table[0] != base + profile->destructor ||
      table[profile->slot] != base + profile->present)
    return 0;

  real_present_rect = (PresentRect)table[profile->slot];
  table[profile->slot] = (uintptr_t)present_rect_hook;
  return 1;
}

#endif
