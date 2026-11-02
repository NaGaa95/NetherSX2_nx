#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/hooks.h"
#include "../../source/so_util.h"

so_module emu_mod;
CoreVersion g_core_version;

typedef void (*PresentRect)(void *, const void *, const void *, void *,
                            const void *, unsigned, float, bool);
typedef struct {
  CoreVersion version;
  uint32_t table, destructor, present;
  unsigned slot;
} Profile;

#ifdef USE_VULKAN
static const Profile profiles[] = {
  {CORE_VER_V22N_4248, 0xb78c30, 0x661400, 0x662cb0, 24},
  {CORE_VER_V22N_3668, 0xb68890, 0x6883f0, 0x68a2d0, 28}
};
#else
static const Profile profiles[] = {
  {CORE_VER_V22N_4248, 0xb78a20, 0x64c290, 0x653ef0, 24},
  {CORE_VER_V22N_3668, 0xb686c0, 0x674240, 0x67bbf0, 28}
};
#endif

static unsigned checks, published, forwarded;
static uint32_t width, height;
static const void *expected_source;
static void *expected_destination;
static bool expected_linear;

static void check(bool condition) {
  if (!condition) {
    fprintf(stderr, "Resolution check %u failed\n", checks + 1);
    exit(EXIT_FAILURE);
  }
  ++checks;
}

void saltynx_set_resolution(uint32_t w, uint32_t h) {
  width = w;
  height = h;
  ++published;
}

static void original_present(void *device, const void *source,
                             const void *source_rect, void *destination,
                             const void *destination_rect, unsigned shader,
                             float time, bool linear) {
  check(device == (void *)0x1000 && source == expected_source &&
        source_rect == (void *)0x2000 && destination == expected_destination &&
        destination_rect == (void *)0x3000 && shader == 0x1234 &&
        time == 17.25f && linear == expected_linear);
  ++forwarded;
}

static void invoke(PresentRect present, const void *source, void *destination,
                   bool linear) {
  expected_source = source;
  expected_destination = destination;
  expected_linear = linear;
  const unsigned previous = forwarded;
  present((void *)0x1000, source, (void *)0x2000, destination,
          (void *)0x3000, 0x1234, 17.25f, linear);
  check(forwarded == previous + 1);
}

static void test_profile(const Profile *profile, uint8_t *image) {
  memset(image, 0, 0xb79000);
  emu_mod.load_base = image;
  emu_mod.load_virtbase = (void *)((uintptr_t)original_present - profile->present);
  emu_mod.load_size = 0xb79000;
  g_core_version = profile->version;
  const uintptr_t base = (uintptr_t)emu_mod.load_virtbase;
  uintptr_t *table = (uintptr_t *)(image + profile->table);
  table[0] = base + profile->destructor;
  table[profile->slot] = (uintptr_t)original_present;

  g_core_version = CORE_VER_UNKNOWN;
  check(!core_install_resolution_hook());
  g_core_version = profile->version;
  emu_mod.load_size = profile->table + profile->slot * sizeof(uintptr_t) + 7;
  check(!core_install_resolution_hook());
  emu_mod.load_size = 0xb79000;
  table[0] ^= 4;
  check(!core_install_resolution_hook());
  table[0] ^= 4;
  table[profile->slot] ^= 4;
  check(!core_install_resolution_hook());
  table[profile->slot] ^= 4;
  check(table[profile->slot] == (uintptr_t)original_present);
  check(core_install_resolution_hook());
  PresentRect present = (PresentRect)table[profile->slot];
  check(present != original_present);
  check(!core_install_resolution_hook());
  check(table[profile->slot] == (uintptr_t)present);

  union { uintptr_t alignment; uint8_t bytes[0x28]; } texture = {0};
  int32_t *size = (int32_t *)(texture.bytes + 0x10);
  const unsigned previous = published;
  size[0] = 512; size[1] = 448;
  invoke(present, &texture, NULL, false);
  check(width == 512 && height == 448 && published == previous + 1);
  size[0] = 1536; size[1] = 1344;
  invoke(present, &texture, NULL, true);
  check(width == 1536 && height == 1344 && published == previous + 2);
  size[0] = 640; size[1] = 480;
  invoke(present, &texture, NULL, false);
  check(width == 640 && height == 480 && published == previous + 3);
  size[0] = 1280; size[1] = 720;
  invoke(present, &texture, (void *)0x4000, true);
  invoke(present, NULL, NULL, true);
  size[0] = 0;
  invoke(present, &texture, NULL, false);
  size[0] = 512; size[1] = -1;
  invoke(present, &texture, NULL, true);
  check(width == 640 && height == 480 && published == previous + 3);
}

int main(void) {
  uint8_t *image = malloc(0xb79000);
  if (!image)
    return EXIT_FAILURE;
  check(!core_install_resolution_hook());
  for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
    test_profile(&profiles[i], image);
  free(image);
  printf("%u resolution checks passed\n", checks);
  return EXIT_SUCCESS;
}
