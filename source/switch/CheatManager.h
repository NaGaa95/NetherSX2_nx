#ifndef NETHERSX2_NX_CHEAT_MANAGER_H
#define NETHERSX2_NX_CHEAT_MANAGER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NX_CHEAT_MAX_ENTRIES 256
#define NX_CHEAT_NAME_SIZE 160

typedef struct NxCheatEntry {
  char name[NX_CHEAT_NAME_SIZE];
  unsigned patch_count;
  unsigned enabled_count;
} NxCheatEntry;

typedef struct NxCheatList {
  NxCheatEntry entries[NX_CHEAT_MAX_ENTRIES];
  size_t count;
  int file_exists;
  int truncated;
} NxCheatList;

// Reads named PNACH sections and legacy "// name" code blocks. A missing file
// is not an error: file_exists is cleared and the returned list is empty.
int nx_cheat_load(const char *path, NxCheatList *list);

// Enables/disables every patch= line belonging to one visible list entry. The
// PNACH is replaced atomically and all non-code text is preserved.
int nx_cheat_set_enabled(const char *path, size_t entry, int enabled);

#ifdef __cplusplus
}
#endif

#endif
