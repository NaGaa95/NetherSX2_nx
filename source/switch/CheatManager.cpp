#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "CheatManager.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {

constexpr size_t kMaximumPnachBytes = 4 * 1024 * 1024;
constexpr const char *kDisabledMarker = "// [NetherSX2-nx disabled] ";

enum PatchKind {
  kNotPatch,
  kActivePatch,
  kManagedDisabledPatch,
  kCommentedPatch,
};

struct Line {
  std::string text;
  std::string ending;
  int group = -1;
  size_t patch_offset = std::string::npos;
  PatchKind patch_kind = kNotPatch;
};

struct Group {
  std::string name;
  unsigned patch_count = 0;
  unsigned enabled_count = 0;
  unsigned managed_disabled_count = 0;
  unsigned commented_count = 0;
};

struct ParsedFile {
  std::vector<Line> lines;
  std::vector<Group> groups;
  std::vector<size_t> visible_groups;
};

size_t skip_space(const std::string &value, size_t offset = 0) {
  if (offset == 0 && value.size() >= 3 &&
      static_cast<unsigned char>(value[0]) == 0xef &&
      static_cast<unsigned char>(value[1]) == 0xbb &&
      static_cast<unsigned char>(value[2]) == 0xbf)
    offset = 3;
  while (offset < value.size() &&
         std::isspace(static_cast<unsigned char>(value[offset])))
    offset++;
  return offset;
}

std::string trim_copy(const std::string &value) {
  size_t first = skip_space(value);
  size_t last = value.size();
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
    last--;
  return value.substr(first, last - first);
}

bool starts_with_ci(const std::string &value, size_t offset, const char *prefix) {
  for (size_t index = 0; prefix[index]; index++) {
    if (offset + index >= value.size() ||
        std::tolower(static_cast<unsigned char>(value[offset + index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index])))
      return false;
  }
  return true;
}

std::string display_name(std::string name) {
  name = trim_copy(name);
  static const char *prefixes[] = {"cheats\\", "cheat\\", "patches\\", "patch\\"};
  for (const char *prefix : prefixes) {
    if (starts_with_ci(name, 0, prefix)) {
      name.erase(0, std::strlen(prefix));
      break;
    }
  }
  for (size_t offset = 0; (offset = name.find('\\', offset)) != std::string::npos;) {
    name.replace(offset, 1, " / ");
    offset += 3;
  }
  if (name.empty()) name = "Unnamed code";
  return name;
}

bool find_patch(const std::string &line, size_t *patch_offset, PatchKind *kind) {
  size_t offset = skip_space(line);
  if (starts_with_ci(line, offset, "patch=")) {
    *patch_offset = offset;
    *kind = kActivePatch;
    return true;
  }

  if (offset + 1 < line.size() && line[offset] == '/' && line[offset + 1] == '/')
    offset += 2;
  else if (offset < line.size() && (line[offset] == '#' || line[offset] == ';'))
    offset++;
  else
    return false;

  offset = skip_space(line, offset);
  bool managed = false;
  if (starts_with_ci(line, offset, "[NetherSX2-nx disabled]")) {
    managed = true;
    offset += std::strlen("[NetherSX2-nx disabled]");
    offset = skip_space(line, offset);
  }
  if (!starts_with_ci(line, offset, "patch=")) return false;
  *patch_offset = offset;
  *kind = managed ? kManagedDisabledPatch : kCommentedPatch;
  return true;
}

bool comment_heading(const std::string &line, std::string *heading) {
  size_t offset = skip_space(line);
  if (offset + 1 < line.size() && line[offset] == '/' && line[offset + 1] == '/')
    offset += 2;
  else if (offset < line.size() && (line[offset] == '#' || line[offset] == ';'))
    offset++;
  else
    return false;
  std::string value = trim_copy(line.substr(offset));
  if (value.empty() || starts_with_ci(value, 0, "patch=") ||
      starts_with_ci(value, 0, "[NetherSX2-nx disabled]"))
    return false;
  // Commented-out raw words are usually old values, not human-readable names.
  bool all_code = value.size() >= 8;
  for (char character : value) {
    if (!std::isxdigit(static_cast<unsigned char>(character)) &&
        !std::isspace(static_cast<unsigned char>(character))) {
      all_code = false;
      break;
    }
  }
  if (all_code) return false;
  *heading = display_name(value);
  return true;
}

int add_group(ParsedFile *parsed, const std::string &name, unsigned unnamed_index) {
  Group group;
  if (name.empty())
    group.name = "Unnamed code " + std::to_string(unnamed_index);
  else
    group.name = display_name(name);
  parsed->groups.push_back(std::move(group));
  return static_cast<int>(parsed->groups.size() - 1);
}

bool parse_bytes(const std::string &contents, ParsedFile *parsed) {
  parsed->lines.clear();
  parsed->groups.clear();
  parsed->visible_groups.clear();

  size_t position = 0;
  while (position < contents.size()) {
    const size_t start = position;
    while (position < contents.size() && contents[position] != '\r' && contents[position] != '\n')
      position++;
    Line line;
    line.text.assign(contents, start, position - start);
    if (position < contents.size()) {
      if (contents[position] == '\r' && position + 1 < contents.size() && contents[position + 1] == '\n') {
        line.ending = "\r\n";
        position += 2;
      } else {
        line.ending.assign(1, contents[position++]);
      }
    }
    parsed->lines.push_back(std::move(line));
  }
  if (contents.empty()) parsed->lines.clear();

  int section_group = -1;
  int legacy_group = -1;
  std::string pending_heading;
  bool legacy_break = true;
  unsigned unnamed_index = 1;

  for (Line &line : parsed->lines) {
    std::string trimmed = trim_copy(line.text);
    if (trimmed.size() >= 3 && trimmed.front() == '[' && trimmed.back() == ']') {
      section_group = add_group(parsed, trimmed.substr(1, trimmed.size() - 2), unnamed_index++);
      legacy_group = -1;
      pending_heading.clear();
      legacy_break = true;
      continue;
    }

    size_t patch_offset = std::string::npos;
    PatchKind patch_kind = kNotPatch;
    if (find_patch(line.text, &patch_offset, &patch_kind)) {
      int group = section_group;
      if (group < 0) {
        if (!pending_heading.empty()) {
          group = add_group(parsed, pending_heading, unnamed_index++);
          legacy_group = group;
          pending_heading.clear();
        } else if (legacy_group < 0 || legacy_break) {
          group = add_group(parsed, "", unnamed_index++);
          legacy_group = group;
        } else {
          group = legacy_group;
        }
      }
      line.group = group;
      line.patch_offset = patch_offset;
      line.patch_kind = patch_kind;
      Group &entry = parsed->groups[static_cast<size_t>(group)];
      if (patch_kind == kActivePatch) entry.enabled_count++;
      else if (patch_kind == kManagedDisabledPatch) entry.managed_disabled_count++;
      else entry.commented_count++;
      legacy_break = false;
      continue;
    }

    if (section_group < 0) {
      std::string heading;
      if (comment_heading(line.text, &heading)) {
        if (pending_heading.empty()) pending_heading = std::move(heading);
        legacy_break = true;
      } else if (trimmed.empty() && legacy_group >= 0) {
        legacy_break = true;
      }
    }
  }

  for (size_t index = 0; index < parsed->groups.size(); index++) {
    Group &group = parsed->groups[index];
    group.patch_count = group.enabled_count + group.managed_disabled_count;
    // A fully commented legacy block is a disabled cheat. Commented patch
    // lines inside an otherwise active block are usually alternatives or old
    // values and must not be enabled accidentally.
    if (group.patch_count == 0) group.patch_count = group.commented_count;
    if (parsed->groups[index].patch_count != 0)
      parsed->visible_groups.push_back(index);
  }
  return true;
}

bool recover_file(const char *path) {
  const std::string temporary = std::string(path) + ".tmp";
  const std::string previous = std::string(path) + ".old";
  struct stat current_info {}, previous_info {}, temporary_info {};
  bool current = stat(path, &current_info) == 0;
  const int current_error = current ? 0 : errno;
  bool old = stat(previous.c_str(), &previous_info) == 0;
  const int old_error = old ? 0 : errno;
  bool temporary_exists = stat(temporary.c_str(), &temporary_info) == 0;
  const int temporary_error = temporary_exists ? 0 : errno;
  if ((!current && current_error != ENOENT) || (!old && old_error != ENOENT) ||
      (!temporary_exists && temporary_error != ENOENT))
    return false;
  bool changed = false;
  if (!current && old) {
    if (std::rename(previous.c_str(), path) != 0) return false;
    current = true;
    old = false;
    changed = true;
  }
  if (temporary_exists) {
    if (std::remove(temporary.c_str()) != 0) return false;
    changed = true;
  }
  if (current && old) {
    if (std::remove(previous.c_str()) != 0) return false;
    changed = true;
  }
#ifdef __SWITCH__
  if (changed) fsdevCommitDevice("sdmc");
#else
  (void)changed;
#endif
  return true;
}

bool read_file(const char *path, ParsedFile *parsed, bool *exists) {
  *exists = false;
  if (!recover_file(path)) return false;
  struct stat info {};
  if (stat(path, &info) != 0) return errno == ENOENT;
  *exists = true;
  if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<size_t>(info.st_size) > kMaximumPnachBytes)
    return false;

  FILE *file = std::fopen(path, "rb");
  if (!file) return false;
  std::string contents(static_cast<size_t>(info.st_size), '\0');
  const bool ok = contents.empty() ||
                  std::fread(&contents[0], 1, contents.size(), file) == contents.size();
  std::fclose(file);
  return ok && parse_bytes(contents, parsed);
}

bool write_file(const char *path, const ParsedFile &parsed) {
  const std::string temporary = std::string(path) + ".tmp";
  const std::string previous = std::string(path) + ".old";
  std::remove(temporary.c_str());
  FILE *output = std::fopen(temporary.c_str(), "wb");
  if (!output) return false;
  bool ok = true;
  for (const Line &line : parsed.lines) {
    if ((!line.text.empty() &&
         std::fwrite(line.text.data(), 1, line.text.size(), output) != line.text.size()) ||
        (!line.ending.empty() &&
         std::fwrite(line.ending.data(), 1, line.ending.size(), output) != line.ending.size())) {
      ok = false;
      break;
    }
  }
  if (ok) ok = std::fflush(output) == 0 && fsync(fileno(output)) == 0;
  if (std::fclose(output) != 0) ok = false;
  if (!ok) {
    std::remove(temporary.c_str());
    return false;
  }

  std::remove(previous.c_str());
  if (std::rename(path, previous.c_str()) != 0) {
    std::remove(temporary.c_str());
    return false;
  }
  if (std::rename(temporary.c_str(), path) != 0) {
    std::rename(previous.c_str(), path);
    std::remove(temporary.c_str());
    return false;
  }
#ifdef __SWITCH__
  fsdevCommitDevice("sdmc");
#endif
  std::remove(previous.c_str());
#ifdef __SWITCH__
  fsdevCommitDevice("sdmc");
#endif
  return true;
}

} // namespace

extern "C" int nx_cheat_load(const char *path, NxCheatList *list) {
  if (!list) return 0;
  std::memset(list, 0, sizeof(*list));
  if (!path) return 0;
  ParsedFile parsed;
  bool exists = false;
  if (!read_file(path, &parsed, &exists)) return 0;
  list->file_exists = exists ? 1 : 0;
  const size_t count = std::min(parsed.visible_groups.size(),
                                static_cast<size_t>(NX_CHEAT_MAX_ENTRIES));
  list->count = count;
  list->truncated = parsed.visible_groups.size() > count ? 1 : 0;
  for (size_t index = 0; index < count; index++) {
    const Group &group = parsed.groups[parsed.visible_groups[index]];
    std::snprintf(list->entries[index].name, sizeof(list->entries[index].name),
                  "%s", group.name.c_str());
    list->entries[index].patch_count = group.patch_count;
    list->entries[index].enabled_count = group.enabled_count;
  }
  return 1;
}

extern "C" int nx_cheat_set_enabled(const char *path, size_t entry, int enabled) {
  if (!path) return 0;
  ParsedFile parsed;
  bool exists = false;
  if (!read_file(path, &parsed, &exists) || !exists || entry >= parsed.visible_groups.size())
    return 0;
  const int target = static_cast<int>(parsed.visible_groups[entry]);
  const bool turn_on = enabled != 0;
  const Group &target_group = parsed.groups[static_cast<size_t>(target)];
  const bool include_commented =
      target_group.enabled_count + target_group.managed_disabled_count == 0;
  bool changed = false;
  for (Line &line : parsed.lines) {
    if (line.group != target || line.patch_offset == std::string::npos ||
        (line.patch_kind == kCommentedPatch && !include_commented))
      continue;
    const bool currently_on = line.patch_kind == kActivePatch;
    if (currently_on == turn_on) continue;
    const size_t indentation = skip_space(line.text);
    const std::string patch = line.text.substr(line.patch_offset);
    line.text.erase(indentation);
    if (!turn_on) line.text += kDisabledMarker;
    line.text += patch;
    line.patch_kind = turn_on ? kActivePatch : kManagedDisabledPatch;
    changed = true;
  }
  return (!changed || write_file(path, parsed)) ? 1 : 0;
}
