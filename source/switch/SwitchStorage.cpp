// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived in part from Cemu-NX storage code (MPL-2.0).

#include "SwitchStorage.h"

#include <switch.h>
#include <usbhsfs.h>

#include <ctime>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include <sys/iosupport.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SwitchStorage
{
namespace
{
constexpr std::size_t SMB_READ_AHEAD_MIN_BYTES = 64 * 1024;
constexpr std::size_t SMB_READ_AHEAD_MAX_BYTES = 1024 * 1024;
constexpr std::size_t SMB_READ_AHEAD_BUDGET = 16 * 1024 * 1024;
constexpr unsigned SMB_RANDOM_READ_BYPASS_COUNT = 3;
constexpr auto SMB_DIRECTORY_CACHE_LIFETIME = std::chrono::seconds(3);
constexpr std::size_t SMB_DIRECTORY_CACHE_LIMIT = 32;
constexpr std::size_t SMB_DIRECTORY_ENTRY_LIMIT = 4096;

struct SmbMount;
struct SmbDevice;

struct CachedDirectoryEntry
{
  std::string name;
  struct stat info{};
};

struct CachedDirectory
{
  std::vector<CachedDirectoryEntry> entries;
  std::chrono::steady_clock::time_point expires;
  bool complete = false;
};

struct SmbFile
{
  // devoptab allocates this structure as unconstructed storage, so keep the
  // owning shared_ptr in a separately constructed object.  It pins the mount
  // until close even after its device has been retired.
  std::shared_ptr<SmbMount>* lifetime = nullptr;
  SmbMount* mount = nullptr;
  smb2fh* handle = nullptr;
  std::uint8_t* read_ahead = nullptr;
  std::size_t read_ahead_capacity = 0;
  std::size_t read_ahead_offset = 0;
  std::size_t read_ahead_size = 0;
  std::size_t read_ahead_window = SMB_READ_AHEAD_MIN_BYTES;
  unsigned sequential_reads = 0;
  unsigned random_read_bypass = 0;
  std::uint64_t position = 0;
  std::uint64_t context_generation = 0;
  int flags = 0;
  bool opened_once = false;
  char path[PATH_MAX]{};
};

struct SmbDir
{
  std::shared_ptr<SmbMount>* lifetime = nullptr;
  SmbMount* mount = nullptr;
  smb2dir* handle = nullptr;
  std::vector<CachedDirectoryEntry>* entries = nullptr;
  std::size_t entry_index = 0;
  std::uint64_t context_generation = 0;
  bool from_cache = false;
  bool complete = false;
  char path[PATH_MAX]{};
};

struct SmbMount
{
  SmbShare config;
  SmbDevice* device = nullptr;
  smb2_context* context = nullptr;
  std::atomic<SmbConnectionState> state{SmbConnectionState::Disconnected};
  std::atomic_bool retired{false};
  bool connected = false;
  std::uint64_t context_generation = 0;
  std::string last_error;
  std::mutex io_mutex;
  std::size_t read_ahead_bytes = 0;
  std::unordered_map<std::string, CachedDirectory> directory_cache;

  ~SmbMount()
  {
    if (!context)
      return;
    if (connected)
      smb2_disconnect_share(context);
    smb2_destroy_context(context);
  }
};

// Newlib retains the devoptab pointer in open descriptors.  Keep registration
// records at stable addresses for the process lifetime; a remount receives a
// fresh record so a callback already dispatched for an old registration can
// never acquire the replacement connection by accident.
struct SmbDevice
{
  std::string device_name;
  std::string root_path;
  devoptab_t devoptab{};
  std::shared_ptr<SmbMount> mount;
};

std::mutex s_mount_mutex;
std::vector<std::shared_ptr<SmbMount>> s_smb_mounts;
std::vector<std::unique_ptr<SmbDevice>> s_smb_devices;
bool s_usb_initialized = false;
std::atomic<std::uint64_t> s_usb_generation{0};
std::mutex s_usb_mutex;
std::vector<UsbHsFsDevice> s_usb_devices;
std::mutex s_usb_callback_mutex;
UsbStatusCallback s_usb_callback = nullptr;
void* s_usb_callback_data = nullptr;

void UsbStatusChanged(const UsbHsFsDevice* devices, u32 count, void*)
{
  {
    std::lock_guard lock(s_usb_mutex);
    s_usb_devices.clear();
    if (devices && count)
      s_usb_devices.assign(devices, devices + count);
    s_usb_generation.fetch_add(1, std::memory_order_release);
  }

  // Serialize invocation with registration changes so unregistering also
  // fences the caller-owned userdata. Never hold the USB snapshot mutex while
  // calling application code.
  std::lock_guard callback_lock(s_usb_callback_mutex);
  if (s_usb_callback)
    s_usb_callback(s_usb_callback_data);
}

int Fail(_reent* reent, int error)
{
  if (reent)
    reent->_errno = error > 0 ? error : EIO;
  return -1;
}

bool ValidId(const std::string& id)
{
  if (id.empty() || id.size() > 16)
    return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  });
}

std::string Trim(std::string value)
{
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::unordered_map<std::string, std::string> ReadIni(const std::string& path)
{
  std::unordered_map<std::string, std::string> values;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return values;
  char line[4096];
  while (std::fgets(line, sizeof(line), file))
  {
    std::string text = Trim(line);
    if (text.empty() || text.front() == '#' || text.front() == ';' || text.front() == '[')
      continue;
    const auto separator = text.find('=');
    if (separator == std::string::npos)
      continue;
    std::string key = Trim(text.substr(0, separator));
    if (!key.empty())
      values[std::move(key)] = Trim(text.substr(separator + 1));
  }
  std::fclose(file);
  return values;
}

std::string ValueFor(const std::unordered_map<std::string, std::string>& values,
                     const std::string& key)
{
  const auto iterator = values.find(key);
  return iterator == values.end() ? std::string{} : iterator->second;
}

std::string DeviceNameForId(const std::string& id)
{
  return ValidId(id) ? "nxsmb_" + id : std::string{};
}

bool FixPath(const char* source, char* destination, std::size_t destination_size)
{
  if (!source || !destination || destination_size == 0)
    return false;
  const char* colon = std::strchr(source, ':');
  if (!colon)
    return false;
  const char* input = colon + 1;
  while (*input == '/')
    ++input;

  std::size_t length = 0;
  bool slash = false;
  for (; *input; ++input)
  {
    if (*input == '/')
    {
      if (slash)
        continue;
      slash = true;
    }
    else
    {
      slash = false;
    }
    if (length + 1 >= destination_size)
      return false;
    destination[length++] = *input;
  }
  while (length && destination[length - 1] == '/')
    --length;
  destination[length] = '\0';
  return true;
}

bool IsRootPath(const char* path)
{
  const char* colon = path ? std::strchr(path, ':') : nullptr;
  if (!colon)
    return false;
  ++colon;
  while (*colon == '/')
    ++colon;
  return *colon == '\0';
}

void FillStat(struct stat* output, const smb2_stat_64& input)
{
  std::memset(output, 0, sizeof(*output));
  switch (input.smb2_type)
  {
  case SMB2_TYPE_FILE:
    output->st_mode = S_IFREG | 0666;
    break;
  case SMB2_TYPE_DIRECTORY:
    output->st_mode = S_IFDIR | 0777;
    break;
  case SMB2_TYPE_LINK:
    output->st_mode = S_IFLNK | 0777;
    break;
  default:
    output->st_mode = S_IFREG | 0444;
    break;
  }
  output->st_ino = input.smb2_ino;
  output->st_nlink = input.smb2_nlink ? input.smb2_nlink : 1;
  output->st_size = static_cast<off_t>(input.smb2_size);
  output->st_atime = input.smb2_atime;
  output->st_mtime = input.smb2_mtime;
  output->st_ctime = input.smb2_ctime;
  output->st_blksize = 65536;
}

std::shared_ptr<SmbMount> MountFrom(_reent* reent)
{
  auto* device = reent ? static_cast<SmbDevice*>(reent->deviceData) : nullptr;
  if (!device)
    return {};
  std::lock_guard lock(s_mount_mutex);
  std::shared_ptr<SmbMount> mount = device->mount;
  if (!mount || mount->retired.load(std::memory_order_acquire))
    return {};
  return mount;
}

std::shared_ptr<SmbMount> MountFrom(const SmbFile* file)
{
  return file && file->lifetime ? *file->lifetime : std::shared_ptr<SmbMount>{};
}

std::shared_ptr<SmbMount> MountFrom(const SmbDir* directory)
{
  return directory && directory->lifetime ? *directory->lifetime : std::shared_ptr<SmbMount>{};
}

bool MountRetired(const SmbMount* mount)
{
  return !mount || mount->retired.load(std::memory_order_acquire);
}

bool PinMount(SmbFile* file, std::shared_ptr<SmbMount> mount)
{
  file->lifetime = new (std::nothrow) std::shared_ptr<SmbMount>(std::move(mount));
  if (!file->lifetime)
    return false;
  file->mount = file->lifetime->get();
  return true;
}

bool PinMount(SmbDir* directory, std::shared_ptr<SmbMount> mount)
{
  directory->lifetime = new (std::nothrow) std::shared_ptr<SmbMount>(std::move(mount));
  if (!directory->lifetime)
    return false;
  directory->mount = directory->lifetime->get();
  return true;
}

void ReleaseMount(SmbFile* file)
{
  auto* lifetime = file->lifetime;
  *file = {};
  delete lifetime;
}

void ReleaseMount(SmbDir* directory)
{
  auto* lifetime = directory->lifetime;
  *directory = {};
  delete lifetime;
}

void DisconnectSmbUnlocked(SmbMount* mount)
{
  ++mount->context_generation;
  if (mount->context)
  {
    if (mount->connected)
      smb2_disconnect_share(mount->context);
    smb2_destroy_context(mount->context);
    mount->context = nullptr;
  }
  mount->connected = false;
  mount->directory_cache.clear();
}

bool IsReconnectableError(SmbMount* mount, int result)
{
  const int error = result < 0 ? -result : result;
  if (error == ENOTCONN || error == ECONNRESET || error == ECONNABORTED || error == EPIPE ||
      error == ETIMEDOUT || error == EHOSTUNREACH || error == ENETDOWN ||
      error == ENETUNREACH)
  {
    return true;
  }
  // libsmb2's synchronous wait path returns plain -1 for socket/poll/service
  // failures. Do not confuse an ordinary remote EPERM with that case.
  if (result != -1 || !mount || !mount->context)
    return false;
  if (smb2_get_fd(mount->context) < 0)
    return true;
  const char* detail = smb2_get_error(mount->context);
  return detail && (std::strstr(detail, "Poll failed") || std::strstr(detail, "Timeout") ||
                    std::strstr(detail, "service failed") ||
                    std::strstr(detail, "connection"));
}

void SetSmbError(SmbMount* mount, const char* fallback)
{
  const char* detail = mount->context ? smb2_get_error(mount->context) : nullptr;
  mount->last_error = detail && *detail ? detail : fallback;
}

void RecordSmbIoFailure(SmbMount* mount, int result)
{
  if (MountRetired(mount) || !IsReconnectableError(mount, result))
    return;
  SetSmbError(mount, "The SMB connection was lost");
  mount->connected = false;
  mount->state.store(SmbConnectionState::Failed, std::memory_order_release);
}

struct SmbConnectResult
{
  bool complete = false;
  int status = -ECONNABORTED;
};

void SmbConnectCallback(smb2_context*, int status, void*, void* private_data)
{
  auto* result = static_cast<SmbConnectResult*>(private_data);
  result->status = status;
  result->complete = true;
}

bool ConnectSmbUnlocked(SmbMount* mount, bool reconnect,
                        const std::atomic_bool* cancel = nullptr)
{
  if (MountRetired(mount))
    return false;
  mount->state.store(reconnect ? SmbConnectionState::Reconnecting :
                                 SmbConnectionState::Connecting,
                     std::memory_order_release);
  // Every context replacement invalidates all libsmb2 handles, including a
  // failed reconnect. Open file/directory states use this generation to avoid
  // dereferencing handles owned by a destroyed context.
  DisconnectSmbUnlocked(mount);

  mount->context = smb2_init_context();
  if (!mount->context)
  {
    mount->last_error = "Could not create the SMB client";
    mount->state.store(SmbConnectionState::Failed, std::memory_order_release);
    return false;
  }
  smb2_set_security_mode(mount->context, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_timeout(mount->context, 6);
  if (!mount->config.user.empty())
    smb2_set_user(mount->context, mount->config.user.c_str());
  if (!mount->config.password.empty())
    smb2_set_password(mount->context, mount->config.password.c_str());
  if (!mount->config.domain.empty())
    smb2_set_domain(mount->context, mount->config.domain.c_str());

  SmbConnectResult connection;
  int result = smb2_connect_share_async(
      mount->context, mount->config.server.c_str(), mount->config.share.c_str(),
      mount->config.user.empty() ? nullptr : mount->config.user.c_str(), SmbConnectCallback,
      &connection);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while (result >= 0 && !connection.complete)
  {
    if (MountRetired(mount) || (cancel && cancel->load(std::memory_order_acquire)))
    {
      mount->last_error = MountRetired(mount) ? "SMB share was unmounted" :
                                               "SMB connection cancelled";
      mount->state.store(SmbConnectionState::Disconnected, std::memory_order_release);
      smb2_destroy_context(mount->context);
      mount->context = nullptr;
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
      mount->last_error = "SMB connection timed out";
      mount->state.store(SmbConnectionState::Failed, std::memory_order_release);
      smb2_destroy_context(mount->context);
      mount->context = nullptr;
      return false;
    }
    pollfd descriptor{};
    descriptor.fd = smb2_get_fd(mount->context);
    descriptor.events = static_cast<short>(smb2_which_events(mount->context));
    const int poll_result = ::poll(&descriptor, 1, 100);
    if (poll_result < 0)
    {
      if (errno == EINTR)
        continue;
      result = -errno;
      break;
    }
    if (poll_result > 0 && smb2_service(mount->context, descriptor.revents) < 0)
    {
      result = -EIO;
      break;
    }
  }
  if (result >= 0)
    result = connection.status;
  if (MountRetired(mount))
  {
    mount->last_error = "SMB share was unmounted";
    mount->state.store(SmbConnectionState::Disconnected, std::memory_order_release);
    smb2_destroy_context(mount->context);
    mount->context = nullptr;
    return false;
  }
  if (result < 0)
  {
    SetSmbError(mount, "Could not connect to the SMB share");
    mount->state.store(SmbConnectionState::Failed, std::memory_order_release);
    return false;
  }

  mount->connected = true;
  mount->directory_cache.clear();
  mount->last_error.clear();
  mount->state.store(SmbConnectionState::Connected, std::memory_order_release);
  return true;
}

bool EnsureSmbConnectedUnlocked(SmbMount* mount)
{
  if (MountRetired(mount))
    return false;
  if (mount->context &&
      mount->state.load(std::memory_order_acquire) == SmbConnectionState::Connected)
  {
    return true;
  }
  return ConnectSmbUnlocked(mount, true);
}

void PruneDirectoryCache(SmbMount* mount)
{
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(mount->directory_cache,
                [&](const auto& item) { return item.second.expires <= now; });
  while (mount->directory_cache.size() > SMB_DIRECTORY_CACHE_LIMIT)
  {
    const auto oldest = std::ranges::min_element(
        mount->directory_cache, {}, [](const auto& item) { return item.second.expires; });
    if (oldest == mount->directory_cache.end())
      break;
    mount->directory_cache.erase(oldest);
  }
}

void CacheDirectory(SmbDir* directory)
{
  if (!directory->entries || directory->from_cache || !directory->complete ||
      directory->entries->size() > SMB_DIRECTORY_ENTRY_LIMIT)
  {
    return;
  }
  PruneDirectoryCache(directory->mount);
  CachedDirectory cached;
  cached.entries = *directory->entries;
  cached.expires = std::chrono::steady_clock::now() + SMB_DIRECTORY_CACHE_LIFETIME;
  cached.complete = true;
  directory->mount->directory_cache.insert_or_assign(directory->path, std::move(cached));
  PruneDirectoryCache(directory->mount);
}

bool EnsureReadAhead(SmbFile* file, std::size_t capacity)
{
  capacity = std::clamp(capacity, SMB_READ_AHEAD_MIN_BYTES, SMB_READ_AHEAD_MAX_BYTES);
  if (file->read_ahead_capacity >= capacity)
    return true;
  const std::size_t growth = capacity - file->read_ahead_capacity;
  if (file->mount->read_ahead_bytes > SMB_READ_AHEAD_BUDGET ||
      growth > SMB_READ_AHEAD_BUDGET - file->mount->read_ahead_bytes)
    return false;
  void* memory = std::realloc(file->read_ahead, capacity);
  if (!memory)
    return false;
  file->read_ahead = static_cast<std::uint8_t*>(memory);
  file->read_ahead_capacity = capacity;
  file->mount->read_ahead_bytes += growth;
  return true;
}

void ReleaseReadAhead(SmbFile* file)
{
  if (!file->read_ahead)
    return;
  std::free(file->read_ahead);
  file->read_ahead = nullptr;
  file->mount->read_ahead_bytes -= file->read_ahead_capacity;
  file->read_ahead_capacity = 0;
  file->read_ahead_offset = 0;
  file->read_ahead_size = 0;
}

bool ReopenFileUnlocked(SmbFile* file)
{
  if (!file->mount->context ||
      file->mount->state.load(std::memory_order_acquire) != SmbConnectionState::Connected)
  {
    return false;
  }
  // Reconnecting must only reopen the existing object. Replaying creation-time
  // flags can truncate a partially copied file or make an O_EXCL handle fail
  // after the first connection was lost. Access, append and synchronization
  // flags remain intact; append positioning is enforced by SmbWrite below.
  const int reopen_flags =
      file->opened_once ? file->flags & ~(O_CREAT | O_EXCL | O_TRUNC) : file->flags;
  file->handle = smb2_open(file->mount->context, file->path, reopen_flags);
  if (!file->handle)
    return false;
  file->opened_once = true;
  file->context_generation = file->mount->context_generation;
  file->read_ahead_offset = file->read_ahead_size = 0;
  if (file->position == 0)
    return true;
  if (file->position > static_cast<std::uint64_t>(LLONG_MAX))
  {
    smb2_close(file->mount->context, file->handle);
    file->handle = nullptr;
    return false;
  }
  std::uint64_t actual = 0;
  const int result = smb2_lseek(file->mount->context, file->handle,
                                static_cast<std::int64_t>(file->position), SEEK_SET, &actual);
  if (result >= 0 && actual == file->position)
    return true;
  smb2_close(file->mount->context, file->handle);
  file->handle = nullptr;
  return false;
}

bool ReconnectAndReopenFileUnlocked(SmbFile* file)
{
  file->handle = nullptr;  // The old handle belongs to the context being destroyed.
  return ConnectSmbUnlocked(file->mount, true) && ReopenFileUnlocked(file);
}

bool ReopenDirectoryUnlocked(SmbDir* directory)
{
  if (!directory->mount->context ||
      directory->mount->state.load(std::memory_order_acquire) != SmbConnectionState::Connected)
  {
    return false;
  }
  directory->handle = smb2_opendir(directory->mount->context, directory->path);
  if (!directory->handle)
    return false;
  directory->context_generation = directory->mount->context_generation;
  // Replaying entries is safe for a read-only enumeration. Skip entries that
  // were already returned so callers do not see duplicates after reconnect.
  for (std::size_t i = 0; i < directory->entry_index; ++i)
  {
    if (!smb2_readdir(directory->mount->context, directory->handle))
      return false;
  }
  return true;
}

bool ReconnectAndReopenDirectoryUnlocked(SmbDir* directory)
{
  directory->handle = nullptr;
  return ConnectSmbUnlocked(directory->mount, true) && ReopenDirectoryUnlocked(directory);
}

bool PathFailureNeedsReconnect(SmbMount* mount, const char* path)
{
  if (!mount->context ||
      mount->state.load(std::memory_order_acquire) != SmbConnectionState::Connected)
  {
    return true;
  }
  smb2_stat_64 info{};
  return IsReconnectableError(mount, smb2_stat(mount->context, path, &info));
}

int SynchronizeFilePosition(SmbFile* file)
{
  if (file->read_ahead_size == 0)
    return 0;
  if (file->context_generation != file->mount->context_generation)
    return ReopenFileUnlocked(file) ? 0 : -EIO;
  if (file->position > static_cast<std::uint64_t>(LLONG_MAX))
    return -EOVERFLOW;
  std::uint64_t position = 0;
  const int result = smb2_lseek(file->mount->context, file->handle,
                                static_cast<std::int64_t>(file->position), SEEK_SET, &position);
  if (result >= 0)
  {
    file->read_ahead_offset = 0;
    file->read_ahead_size = 0;
  }
  return result;
}

int SmbOpen(_reent* reent, void* state, const char* source, int flags, int)
{
  std::shared_ptr<SmbMount> mount = MountFrom(reent);
  auto* file = static_cast<SmbFile*>(state);
  *file = {};
  if (!mount)
    return Fail(reent, ENODEV);
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
    return Fail(reent, ENAMETOOLONG);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if (!PinMount(file, mount))
    return Fail(reent, ENOMEM);
  file->flags = flags;
  std::snprintf(file->path, sizeof(file->path), "%s", path);
  if (!ReopenFileUnlocked(file) &&
      (!PathFailureNeedsReconnect(mount.get(), path) || !ReconnectAndReopenFileUnlocked(file)))
  {
    ReleaseMount(file);
    return Fail(reent, EIO);
  }
  if ((flags & O_APPEND) != 0)
  {
    std::uint64_t position = 0;
    const int result = smb2_lseek(mount->context, file->handle, 0, SEEK_END, &position);
    if (result < 0)
    {
      smb2_close(mount->context, file->handle);
      ReleaseMount(file);
      return Fail(reent, -result);
    }
    file->position = position;
  }
  reent->_errno = 0;
  return 0;
}

int SmbClose(_reent* reent, void* state)
{
  auto* file = static_cast<SmbFile*>(state);
  std::shared_ptr<SmbMount> mount = MountFrom(file);
  if (!mount)
    return Fail(reent, EBADF);
  std::lock_guard lock(mount->io_mutex);
  ReleaseReadAhead(file);
  const bool close_current =
      !MountRetired(mount.get()) && file->handle &&
      file->context_generation == mount->context_generation &&
      mount->state.load(std::memory_order_acquire) == SmbConnectionState::Connected;
  const int result = close_current ? smb2_close(mount->context, file->handle) : 0;
  if (result < 0)
    RecordSmbIoFailure(mount.get(), result);
  ReleaseMount(file);
  if (result < 0)
    return Fail(reent, -result);
  reent->_errno = 0;
  return 0;
}

ssize_t SmbRead(_reent* reent, void* state, char* output, std::size_t length)
{
  auto* file = static_cast<SmbFile*>(state);
  std::shared_ptr<SmbMount> mount = MountFrom(file);
  if (!mount)
    return Fail(reent, EBADF);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if ((!file->handle || file->context_generation != file->mount->context_generation) &&
      !ReopenFileUnlocked(file) && !ReconnectAndReopenFileUnlocked(file))
    return Fail(reent, EIO);
  std::size_t total = 0;
  bool retried = false;
  if (file->read_ahead_offset < file->read_ahead_size)
  {
    const std::size_t cached =
        std::min(length, file->read_ahead_size - file->read_ahead_offset);
    std::memcpy(output, file->read_ahead + file->read_ahead_offset, cached);
    file->read_ahead_offset += cached;
    file->position += cached;
    total += cached;
    if (file->read_ahead_offset == file->read_ahead_size)
    {
      file->read_ahead_offset = file->read_ahead_size = 0;
      file->read_ahead_window =
          std::min(file->read_ahead_window * 2, SMB_READ_AHEAD_MAX_BYTES);
    }
  }
  while (total < length)
  {
    const std::size_t remaining = length - total;
    const std::size_t maximum =
        std::max<std::size_t>(1, smb2_get_max_read_size(file->mount->context));
    const bool sequential = file->random_read_bypass == 0 && file->sequential_reads > 0;
    const std::size_t wanted = std::min(file->read_ahead_window, maximum);
    const bool use_read_ahead = sequential && remaining < wanted && EnsureReadAhead(file, wanted);
    const std::size_t amount =
        std::min(use_read_ahead ? wanted : remaining, maximum);
    std::uint8_t* destination = use_read_ahead ? file->read_ahead :
                                                reinterpret_cast<std::uint8_t*>(output + total);
    int result = smb2_read(file->mount->context, file->handle, destination, amount);
    if (result < 0 && !retried && IsReconnectableError(file->mount, result))
    {
      retried = true;
      if (ReconnectAndReopenFileUnlocked(file))
        result = smb2_read(file->mount->context, file->handle, destination, amount);
    }
    if (result < 0)
    {
      RecordSmbIoFailure(file->mount, result);
      return total ? static_cast<ssize_t>(total) : Fail(reent, -result);
    }
    if (result == 0)
      break;
    ++file->sequential_reads;
    if (file->random_read_bypass > 0)
      --file->random_read_bypass;
    if (use_read_ahead)
    {
      file->read_ahead_offset = 0;
      file->read_ahead_size = static_cast<std::size_t>(result);
      const std::size_t copied = std::min(remaining, file->read_ahead_size);
      std::memcpy(output + total, file->read_ahead, copied);
      file->read_ahead_offset = copied;
      file->position += copied;
      total += copied;
      if (file->read_ahead_offset == file->read_ahead_size)
      {
        file->read_ahead_offset = file->read_ahead_size = 0;
        file->read_ahead_window =
            std::min(file->read_ahead_window * 2, SMB_READ_AHEAD_MAX_BYTES);
      }
      break;
    }
    const std::size_t bytes_read = static_cast<std::size_t>(result);
    file->position += bytes_read;
    total += bytes_read;
    if (bytes_read < amount)
      break;
  }
  reent->_errno = 0;
  return static_cast<ssize_t>(total);
}

ssize_t SmbWrite(_reent* reent, void* state, const char* input, std::size_t length)
{
  auto* file = static_cast<SmbFile*>(state);
  std::shared_ptr<SmbMount> mount = MountFrom(file);
  if (!mount || !file->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if (file->context_generation != file->mount->context_generation && !ReopenFileUnlocked(file))
    return Fail(reent, EIO);
  const int synchronized = SynchronizeFilePosition(file);
  if (synchronized < 0)
    return Fail(reent, -synchronized);
  if (length > 0 && (file->flags & O_APPEND) != 0)
  {
    // libsmb2 does not implement O_APPEND itself. Seek on every write so a
    // reconnect (or another writer extending the file) cannot turn an append
    // handle into an overwrite at its previously cached position.
    std::uint64_t end_position = 0;
    const int seek_result =
        smb2_lseek(file->mount->context, file->handle, 0, SEEK_END, &end_position);
    if (seek_result < 0)
    {
      RecordSmbIoFailure(file->mount, seek_result);
      return Fail(reent, -seek_result);
    }
    file->position = end_position;
  }
  const std::size_t maximum =
      std::max<std::size_t>(1, smb2_get_max_write_size(file->mount->context));
  std::size_t total = 0;
  while (total < length)
  {
    const std::size_t amount = std::min(length - total, maximum);
    const int result = smb2_write(file->mount->context, file->handle,
                                  reinterpret_cast<const std::uint8_t*>(input + total), amount);
    if (result < 0)
    {
      RecordSmbIoFailure(file->mount, result);
      return total ? static_cast<ssize_t>(total) : Fail(reent, -result);
    }
    if (result == 0)
      return total ? static_cast<ssize_t>(total) : Fail(reent, EIO);
    total += static_cast<std::size_t>(result);
    file->position += static_cast<std::size_t>(result);
  }
  if (total > 0)
    file->mount->directory_cache.clear();
  reent->_errno = 0;
  return static_cast<ssize_t>(total);
}

bool ComputeSeekTarget(std::uint64_t base, off_t delta, std::uint64_t* target)
{
  if (delta >= 0)
  {
    const auto positive = static_cast<std::uint64_t>(delta);
    if (positive > static_cast<std::uint64_t>(LLONG_MAX) ||
        base > static_cast<std::uint64_t>(LLONG_MAX) - positive)
    {
      return false;
    }
    *target = base + positive;
    return true;
  }

  // Avoid negating the most-negative off_t value, which is undefined signed
  // overflow. -(delta + 1) is always representable.
  const auto magnitude = static_cast<std::uint64_t>(-(delta + 1)) + 1;
  if (magnitude > base)
    return false;
  *target = base - magnitude;
  return true;
}

off_t SmbSeek(_reent* reent, void* state, off_t position, int origin)
{
  auto* file = static_cast<SmbFile*>(state);
  std::shared_ptr<SmbMount> mount = MountFrom(file);
  if (!mount)
  {
    Fail(reent, EBADF);
    return static_cast<off_t>(-1);
  }
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
  {
    Fail(reent, ENODEV);
    return static_cast<off_t>(-1);
  }
  if ((!file->handle || file->context_generation != file->mount->context_generation) &&
      !ReopenFileUnlocked(file) && !ReconnectAndReopenFileUnlocked(file))
  {
    Fail(reent, EIO);
    return static_cast<off_t>(-1);
  }
  std::uint64_t result_position = 0;
  if (origin == SEEK_SET || origin == SEEK_CUR)
  {
    const std::uint64_t base = origin == SEEK_SET ? 0 : file->position;
    std::uint64_t target = 0;
    if (!ComputeSeekTarget(base, position, &target))
    {
      Fail(reent, position < 0 ? EINVAL : EOVERFLOW);
      return static_cast<off_t>(-1);
    }
    if (target > static_cast<std::uint64_t>(LLONG_MAX))
    {
      Fail(reent, EOVERFLOW);
      return static_cast<off_t>(-1);
    }
    if (file->read_ahead_size > 0)
    {
      const std::uint64_t cache_start = file->position - file->read_ahead_offset;
      const std::uint64_t cache_end = cache_start + file->read_ahead_size;
      if (target >= cache_start && target <= cache_end)
      {
        file->read_ahead_offset = static_cast<std::size_t>(target - cache_start);
        if (target != file->position)
        {
          file->sequential_reads = 0;
          file->random_read_bypass = SMB_RANDOM_READ_BYPASS_COUNT;
          file->read_ahead_window = SMB_READ_AHEAD_MIN_BYTES;
        }
        file->position = target;
        reent->_errno = 0;
        return static_cast<off_t>(target);
      }
    }
    const int result = smb2_lseek(file->mount->context, file->handle,
                                  static_cast<std::int64_t>(target), SEEK_SET, &result_position);
    if (result < 0)
    {
      RecordSmbIoFailure(file->mount, result);
      Fail(reent, -result);
      return static_cast<off_t>(-1);
    }
  }
  else
  {
    const int result =
        smb2_lseek(file->mount->context, file->handle, position, origin, &result_position);
    if (result < 0)
    {
      RecordSmbIoFailure(file->mount, result);
      Fail(reent, -result);
      return static_cast<off_t>(-1);
    }
  }
  if (result_position > static_cast<std::uint64_t>(LLONG_MAX))
  {
    Fail(reent, EOVERFLOW);
    return static_cast<off_t>(-1);
  }
  file->read_ahead_offset = file->read_ahead_size = 0;
  if (result_position != file->position)
  {
    file->sequential_reads = 0;
    file->random_read_bypass = SMB_RANDOM_READ_BYPASS_COUNT;
    file->read_ahead_window = SMB_READ_AHEAD_MIN_BYTES;
  }
  file->position = result_position;
  reent->_errno = 0;
  return static_cast<off_t>(result_position);
}

int SmbFstat(_reent* reent, void* state, struct stat* output)
{
  auto* file = static_cast<SmbFile*>(state);
  std::shared_ptr<SmbMount> mount = MountFrom(file);
  if (!mount || !output)
    return Fail(reent, EBADF);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if ((!file->handle || file->context_generation != file->mount->context_generation) &&
      !ReopenFileUnlocked(file) && !ReconnectAndReopenFileUnlocked(file))
    return Fail(reent, EIO);
  smb2_stat_64 info{};
  int result = smb2_fstat(file->mount->context, file->handle, &info);
  if (result < 0 && IsReconnectableError(file->mount, result) &&
      ReconnectAndReopenFileUnlocked(file))
    result = smb2_fstat(file->mount->context, file->handle, &info);
  if (result < 0)
  {
    RecordSmbIoFailure(file->mount, result);
    return Fail(reent, -result);
  }
  FillStat(output, info);
  reent->_errno = 0;
  return 0;
}

bool FindCachedStat(SmbMount* mount, const std::string& path, struct stat* output)
{
  PruneDirectoryCache(mount);
  const std::size_t slash = path.find_last_of('/');
  const std::string parent = slash == std::string::npos ? std::string{} : path.substr(0, slash);
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const auto cached = mount->directory_cache.find(parent);
  if (cached == mount->directory_cache.end())
    return false;
  const auto entry = std::ranges::find(cached->second.entries, name, &CachedDirectoryEntry::name);
  if (entry == cached->second.entries.end() || S_ISLNK(entry->info.st_mode))
    return false;
  *output = entry->info;
  return true;
}

int SmbStat(_reent* reent, const char* source, struct stat* output)
{
  std::shared_ptr<SmbMount> mount = MountFrom(reent);
  if (!mount || !output)
    return Fail(reent, EINVAL);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if (IsRootPath(source))
  {
    std::memset(output, 0, sizeof(*output));
    output->st_mode = S_IFDIR | 0777;
    output->st_nlink = 1;
    reent->_errno = 0;
    return 0;
  }
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
    return Fail(reent, ENAMETOOLONG);
  if (FindCachedStat(mount.get(), path, output))
  {
    reent->_errno = 0;
    return 0;
  }
  if (!EnsureSmbConnectedUnlocked(mount.get()))
    return Fail(reent, EIO);
  smb2_stat_64 info{};
  int result = smb2_stat(mount->context, path, &info);
  if (result < 0 && IsReconnectableError(mount.get(), result) &&
      ConnectSmbUnlocked(mount.get(), true))
    result = smb2_stat(mount->context, path, &info);
  if (result < 0)
  {
    RecordSmbIoFailure(mount.get(), result);
    return Fail(reent, -result);
  }
  FillStat(output, info);
  reent->_errno = 0;
  return 0;
}

template <typename Operation>
int PathOperation(_reent* reent, const char* source, Operation operation)
{
  std::shared_ptr<SmbMount> mount = MountFrom(reent);
  if (!mount)
    return Fail(reent, ENODEV);
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
    return Fail(reent, ENAMETOOLONG);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if (!EnsureSmbConnectedUnlocked(mount.get()))
    return Fail(reent, EIO);
  const int result = operation(mount.get(), path);
  if (result < 0)
  {
    RecordSmbIoFailure(mount.get(), result);
    return Fail(reent, -result);
  }
  mount->directory_cache.clear();
  reent->_errno = 0;
  return 0;
}

int SmbUnlink(_reent* reent, const char* path)
{
  return PathOperation(reent, path,
                       [](SmbMount* mount, const char* fixed) {
                         return smb2_unlink(mount->context, fixed);
                       });
}

int SmbMkdir(_reent* reent, const char* path, int)
{
  return PathOperation(reent, path,
                       [](SmbMount* mount, const char* fixed) {
                         return smb2_mkdir(mount->context, fixed);
                       });
}

int SmbRmdir(_reent* reent, const char* path)
{
  return PathOperation(reent, path,
                       [](SmbMount* mount, const char* fixed) {
                         return smb2_rmdir(mount->context, fixed);
                       });
}

int SmbRename(_reent* reent, const char* source, const char* destination)
{
  std::shared_ptr<SmbMount> mount = MountFrom(reent);
  if (!mount)
    return Fail(reent, ENODEV);
  char old_path[PATH_MAX]{}, new_path[PATH_MAX]{};
  if (!FixPath(source, old_path, sizeof(old_path)) ||
      !FixPath(destination, new_path, sizeof(new_path)))
    return Fail(reent, ENAMETOOLONG);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if (!EnsureSmbConnectedUnlocked(mount.get()))
    return Fail(reent, EIO);
  const int result = smb2_rename(mount->context, old_path, new_path);
  if (result < 0)
  {
    RecordSmbIoFailure(mount.get(), result);
    return Fail(reent, -result);
  }
  mount->directory_cache.clear();
  reent->_errno = 0;
  return 0;
}

DIR_ITER* SmbDirOpen(_reent* reent, DIR_ITER* state, const char* source)
{
  std::shared_ptr<SmbMount> mount = MountFrom(reent);
  auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
  if (!mount || !directory)
  {
    Fail(reent, EINVAL);
    return nullptr;
  }
  *directory = {};
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
  {
    Fail(reent, ENAMETOOLONG);
    return nullptr;
  }
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
  {
    Fail(reent, ENODEV);
    return nullptr;
  }
  if (!PinMount(directory, mount))
  {
    Fail(reent, ENOMEM);
    return nullptr;
  }
  std::snprintf(directory->path, sizeof(directory->path), "%s", path);
  PruneDirectoryCache(mount.get());
  if (const auto cached = mount->directory_cache.find(path);
      cached != mount->directory_cache.end() && cached->second.complete)
  {
    directory->entries =
        new (std::nothrow) std::vector<CachedDirectoryEntry>(cached->second.entries);
    if (!directory->entries)
    {
      ReleaseMount(directory);
      Fail(reent, ENOMEM);
      return nullptr;
    }
    directory->from_cache = true;
    directory->complete = true;
    reent->_errno = 0;
    return state;
  }

  directory->entries = new (std::nothrow) std::vector<CachedDirectoryEntry>();
  if (!directory->entries)
  {
    ReleaseMount(directory);
    Fail(reent, ENOMEM);
    return nullptr;
  }
  if (!ReopenDirectoryUnlocked(directory) &&
      (!PathFailureNeedsReconnect(mount.get(), path) ||
       !ReconnectAndReopenDirectoryUnlocked(directory)))
  {
    delete directory->entries;
    ReleaseMount(directory);
    Fail(reent, EIO);
    return nullptr;
  }
  PruneDirectoryCache(mount.get());
  mount->directory_cache.insert_or_assign(path,CachedDirectory{
      {},std::chrono::steady_clock::now()+SMB_DIRECTORY_CACHE_LIFETIME,false});
  reent->_errno = 0;
  return state;
}

int SmbDirReset(_reent* reent, DIR_ITER* state)
{
  auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
  std::shared_ptr<SmbMount> mount = MountFrom(directory);
  if (!mount || !directory->entries)
    return Fail(reent, EBADF);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  directory->entry_index = 0;
  if (directory->from_cache)
  {
    reent->_errno = 0;
    return 0;
  }
  directory->entries->clear();
  directory->complete = false;
  if (!directory->handle ||
      directory->context_generation != directory->mount->context_generation)
  {
    directory->handle = nullptr;
    if (!ReopenDirectoryUnlocked(directory) && !ReconnectAndReopenDirectoryUnlocked(directory))
      return Fail(reent, EIO);
  }
  else
  {
    smb2_rewinddir(directory->mount->context, directory->handle);
  }
  reent->_errno = 0;
  return 0;
}

int SmbDirNext(_reent* reent, DIR_ITER* state, char* name, struct stat* output)
{
  auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
  std::shared_ptr<SmbMount> mount = MountFrom(directory);
  if (!mount || !directory->entries || !name || !output)
    return Fail(reent, EBADF);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if (directory->from_cache)
  {
    if (directory->entry_index >= directory->entries->size())
      return Fail(reent, ENOENT);
    const CachedDirectoryEntry& entry = (*directory->entries)[directory->entry_index++];
    std::snprintf(name, NAME_MAX, "%s", entry.name.c_str());
    *output = entry.info;
    reent->_errno = 0;
    return 0;
  }
  if (!directory->handle ||
      directory->context_generation != directory->mount->context_generation)
  {
    directory->handle = nullptr;
    if (!ReopenDirectoryUnlocked(directory) && !ReconnectAndReopenDirectoryUnlocked(directory))
      return Fail(reent, EIO);
  }
  const smb2dirent* entry =
      smb2_readdir(directory->mount->context, directory->handle);
  if (!entry)
  {
    directory->complete = true;
    CacheDirectory(directory);
    return Fail(reent, ENOENT);
  }
  std::snprintf(name, NAME_MAX, "%s", entry->name);
  // QUERY_DIRECTORY already supplied this metadata. Feeding it directly to
  // devoptab avoids a separate SMB stat request for every library entry.
  FillStat(output, entry->st);
  if (directory->entries->size() < SMB_DIRECTORY_ENTRY_LIMIT + 1)
  {
    const CachedDirectoryEntry cachedEntry{entry->name,*output};
    directory->entries->push_back(cachedEntry);
    auto &incremental=mount->directory_cache[directory->path];
    if(incremental.entries.size()<SMB_DIRECTORY_ENTRY_LIMIT)
      incremental.entries.push_back(cachedEntry);
    incremental.expires=std::chrono::steady_clock::now()+SMB_DIRECTORY_CACHE_LIFETIME;
    incremental.complete=false;
  }
  ++directory->entry_index;
  reent->_errno = 0;
  return 0;
}

int SmbDirClose(_reent* reent, DIR_ITER* state)
{
  auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
  std::shared_ptr<SmbMount> mount = MountFrom(directory);
  if (!mount || !directory->entries)
    return Fail(reent, EBADF);
  std::lock_guard lock(mount->io_mutex);
  if (!MountRetired(mount.get()) && !directory->from_cache && directory->handle &&
      directory->context_generation == mount->context_generation &&
      mount->state.load(std::memory_order_acquire) == SmbConnectionState::Connected)
  {
    smb2_closedir(mount->context, directory->handle);
  }
  if (!MountRetired(mount.get()))
    CacheDirectory(directory);
  delete directory->entries;
  ReleaseMount(directory);
  reent->_errno = 0;
  return 0;
}

int SmbStatvfs(_reent* reent, const char* source, struct statvfs* output)
{
  std::shared_ptr<SmbMount> mount = MountFrom(reent);
  if (!mount || !output)
    return Fail(reent, EINVAL);
  char path[PATH_MAX]{};
  if (!FixPath(source, path, sizeof(path)))
    return Fail(reent, ENAMETOOLONG);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if (!EnsureSmbConnectedUnlocked(mount.get()))
    return Fail(reent, EIO);
  struct smb2_statvfs info{};
  int result = smb2_statvfs(mount->context, path, &info);
  if (result < 0 && IsReconnectableError(mount.get(), result) &&
      ConnectSmbUnlocked(mount.get(), true))
    result = smb2_statvfs(mount->context, path, &info);
  if (result < 0)
  {
    RecordSmbIoFailure(mount.get(), result);
    return Fail(reent, -result);
  }
  std::memset(output, 0, sizeof(*output));
  output->f_bsize = info.f_bsize;
  output->f_frsize = info.f_frsize;
  output->f_blocks = info.f_blocks;
  output->f_bfree = info.f_bfree;
  output->f_bavail = info.f_bavail;
  output->f_files = info.f_files;
  output->f_ffree = info.f_ffree;
  output->f_favail = info.f_favail;
  output->f_fsid = info.f_fsid;
  output->f_flag = info.f_flag;
  output->f_namemax = info.f_namemax;
  reent->_errno = 0;
  return 0;
}

int SmbTruncate(_reent* reent, void* state, off_t length)
{
  auto* file = static_cast<SmbFile*>(state);
  std::shared_ptr<SmbMount> mount = MountFrom(file);
  if (!mount || !file->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if (file->context_generation != file->mount->context_generation && !ReopenFileUnlocked(file))
    return Fail(reent, EIO);
  const int synchronized = SynchronizeFilePosition(file);
  if (synchronized < 0)
    return Fail(reent, -synchronized);
  const int result = smb2_ftruncate(file->mount->context, file->handle, length);
  if (result < 0)
  {
    RecordSmbIoFailure(file->mount, result);
    return Fail(reent, -result);
  }
  file->mount->directory_cache.clear();
  reent->_errno = 0;
  return 0;
}

int SmbSync(_reent* reent, void* state)
{
  auto* file = static_cast<SmbFile*>(state);
  std::shared_ptr<SmbMount> mount = MountFrom(file);
  if (!mount || !file->handle)
    return Fail(reent, EBADF);
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
    return Fail(reent, ENODEV);
  if (file->context_generation != file->mount->context_generation && !ReopenFileUnlocked(file))
    return Fail(reent, EIO);
  const int result = smb2_fsync(file->mount->context, file->handle);
  if (result < 0)
  {
    RecordSmbIoFailure(file->mount, result);
    return Fail(reent, -result);
  }
  reent->_errno = 0;
  return 0;
}

void HashBytes(std::uint64_t* hash, const void* data, std::size_t size)
{
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < size; ++i)
  {
    *hash ^= bytes[i];
    *hash *= 1099511628211ULL;
  }
}

template <typename T>
void HashInteger(std::uint64_t* hash, T value)
{
  for (std::size_t i = 0; i < sizeof(T); ++i)
  {
    const std::uint8_t byte = static_cast<std::uint8_t>(value & 0xff);
    HashBytes(hash, &byte, 1);
    value >>= 8;
  }
}

template <std::size_t Size>
void HashText(std::uint64_t* hash, const char (&text)[Size])
{
  HashBytes(hash, text, strnlen(text, Size));
  const std::uint8_t separator = 0;
  HashBytes(hash, &separator, 1);
}

std::string FormatUsbId(const char* prefix, std::uint64_t hash)
{
  char output[32];
  std::snprintf(output, sizeof(output), "%s-%016llx", prefix,
                static_cast<unsigned long long>(hash));
  return output;
}

std::string UsbPhysicalId(const UsbHsFsDevice& device)
{
  std::uint64_t hash = 14695981039346656037ULL;
  HashInteger(&hash, device.vid);
  HashInteger(&hash, device.pid);
  HashText(&hash, device.serial_number);
  // Some inexpensive enclosures expose no serial. Model and capacity make the
  // fallback deterministic and substantially reduce accidental collisions.
  if (!device.serial_number[0])
  {
    HashText(&hash, device.manufacturer);
    HashText(&hash, device.product_name);
    HashInteger(&hash, device.capacity);
  }
  return FormatUsbId("usbdev", hash);
}

std::string UsbVolumeId(const UsbHsFsDevice& device)
{
  std::uint64_t hash = 14695981039346656037ULL;
  const std::string physical_id = UsbPhysicalId(device);
  HashBytes(&hash, physical_id.data(), physical_id.size());
  HashInteger(&hash, device.lun);
  HashInteger(&hash, device.fs_idx);
  HashInteger(&hash, device.fs_type);
  HashInteger(&hash, device.capacity);
  return FormatUsbId("usbvol", hash);
}

Location MakeUsbLocation(const UsbHsFsDevice& device)
{
  Location location;
  location.id = UsbVolumeId(device);
  location.physical_id = UsbPhysicalId(device);
  location.mount_alias = device.name;
  location.path = device.name;
  if (!location.path.empty() && location.path.back() != '/')
    location.path += '/';
  location.serial_number.assign(device.serial_number,
                                strnlen(device.serial_number, sizeof(device.serial_number)));
  location.vendor_id = device.vid;
  location.product_id = device.pid;
  location.lun = device.lun;
  location.partition = device.fs_idx;
  location.filesystem_type = device.fs_type;
  location.capacity = device.capacity;

  const std::uint64_t gib = device.capacity / (1024ULL * 1024ULL * 1024ULL);
  char label[256];
  std::snprintf(label, sizeof(label), "%s - %s%s%s (%llu GiB)", device.name,
                LIBUSBHSFS_FS_TYPE_STR(device.fs_type), device.product_name[0] ? " - " : "",
                device.product_name, static_cast<unsigned long long>(gib));
  location.label = label;
  return location;
}
}  // namespace

void SetUsbStatusCallback(UsbStatusCallback callback, void* user_data)
{
  std::lock_guard lock(s_usb_callback_mutex);
  s_usb_callback = callback;
  s_usb_callback_data = callback ? user_data : nullptr;
}

std::string SmbRootPath(const std::string& id)
{
  const std::string device_name = DeviceNameForId(id);
  return device_name.empty() ? std::string{} : device_name + ":/";
}

std::string SmbBrowsePath(const SmbShare& share)
{
  std::string result = SmbRootPath(share.id);
  if (result.empty() || share.path.empty())
    return result;
  std::string path = share.path;
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && path.front() == '/')
    path.erase(path.begin());
  while (!path.empty() && path.back() == '/')
    path.pop_back();
  return path.empty() ? result : result + path;
}

bool InitializeUsb(std::string* error)
{
  {
    std::lock_guard lock(s_mount_mutex);
    if (s_usb_initialized)
      return true;
    usbHsFsSetFileSystemMountFlags(UsbHsFsMountFlags_None);
    const Result result = usbHsFsInitialize(0);
    if (R_FAILED(result))
    {
      if (error)
      {
        char message[80];
        std::snprintf(message, sizeof(message), "USB initialization failed (0x%08x)", result);
        *error = message;
      }
      return false;
    }
    s_usb_initialized = true;
    usbHsFsSetPopulateCallback(UsbStatusChanged, nullptr);
  }
  // The status callback can wake application code, so perform the initial
  // snapshot after releasing the storage registry mutex as well.
  std::array<UsbHsFsDevice, 32> devices{};
  const u32 count = usbHsFsListMountedDevices(devices.data(), devices.size());
  UsbStatusChanged(devices.data(), count, nullptr);
  return true;
}

std::uint64_t UsbStatusGeneration()
{
  return s_usb_generation.load(std::memory_order_acquire);
}

UsbSnapshot GetUsbSnapshot()
{
  UsbSnapshot snapshot;
  {
    std::lock_guard lock(s_mount_mutex);
    if (!s_usb_initialized)
      return snapshot;
  }
  std::lock_guard lock(s_usb_mutex);
  snapshot.generation = s_usb_generation.load(std::memory_order_acquire);
  snapshot.locations.reserve(s_usb_devices.size());
  for (const UsbHsFsDevice& device : s_usb_devices)
  {
    if (device.name[0])
      snapshot.locations.emplace_back(MakeUsbLocation(device));
  }
  return snapshot;
}

std::vector<Location> ListUsbLocations()
{
  return GetUsbSnapshot().locations;
}

std::string ResolveUsbPath(const std::string& id)
{
  const auto locations = ListUsbLocations();
  const auto location = std::ranges::find(locations, id, &Location::id);
  return location == locations.end() ? std::string{} : location->path;
}

bool SafelyEjectUsb(const std::string& id, std::string* error)
{
  {
    std::lock_guard lock(s_mount_mutex);
    if (!s_usb_initialized)
    {
      if (error)
        *error = "USB storage is not initialized";
      return false;
    }
  }

  UsbHsFsDevice target{};
  bool found = false;
  {
    std::lock_guard lock(s_usb_mutex);
    for (const UsbHsFsDevice& device : s_usb_devices)
    {
      if (UsbVolumeId(device) == id || UsbPhysicalId(device) == id)
      {
        target = device;
        found = true;
        break;
      }
    }
  }
  if (!found)
  {
    if (error)
      *error = "The USB drive is no longer connected";
    return false;
  }
  if (!usbHsFsUnmountDevice(&target, true))
  {
    if (error)
      *error = "Could not safely eject the USB drive; close files using it and try again";
    return false;
  }
  return true;
}

bool MountSmb(const SmbShare& share, std::string* error, const std::atomic_bool* cancel)
{
  if (!ValidId(share.id) || share.server.empty() || share.share.empty())
  {
    if (error)
      *error = "SMB share settings are incomplete";
    return false;
  }
  std::shared_ptr<SmbMount> existing;
  {
    std::lock_guard lock(s_mount_mutex);
    const auto iterator = std::ranges::find_if(
        s_smb_mounts, [&](const auto& mount) { return mount->config.id == share.id; });
    if (iterator != s_smb_mounts.end())
      existing = *iterator;
  }
  if (existing)
  {
    std::unique_lock lock(existing->io_mutex);
    if (MountRetired(existing.get()))
    {
      lock.unlock();
      return MountSmb(share, error, cancel);
    }
    if (existing->state.load(std::memory_order_acquire) == SmbConnectionState::Connected &&
        existing->config.server == share.server && existing->config.share == share.share &&
        existing->config.user == share.user && existing->config.password == share.password &&
        existing->config.domain == share.domain)
    {
      return true;
    }
    existing->config = share;
    const bool result = ConnectSmbUnlocked(existing.get(), true, cancel);
    if (!result && error)
      *error = existing->last_error;
    return result;
  }

  auto mount = std::make_shared<SmbMount>();
  mount->config = share;
  // Network negotiation can take several seconds. Never hold the registry
  // mutex here: USB callbacks, status queries and other shares remain usable.
  if (!ConnectSmbUnlocked(mount.get(), false, cancel))
  {
    if (error)
      *error = mount->last_error;
    return false;
  }

  std::shared_ptr<SmbMount> raced_mount;
  {
    std::lock_guard lock(s_mount_mutex);
    const auto existing_mount = std::ranges::find_if(
        s_smb_mounts, [&](const auto& current) { return current->config.id == share.id; });
    if (existing_mount != s_smb_mounts.end())
    {
      raced_mount = *existing_mount;
    }
    else
    {
      auto device = std::make_unique<SmbDevice>();
      device->device_name = DeviceNameForId(share.id);
      device->root_path = device->device_name + ":/";
      device->mount = mount;
      device->devoptab.name = device->device_name.c_str();
      device->devoptab.structSize = sizeof(SmbFile);
      device->devoptab.open_r = SmbOpen;
      device->devoptab.close_r = SmbClose;
      device->devoptab.write_r = SmbWrite;
      device->devoptab.read_r = SmbRead;
      device->devoptab.seek_r = SmbSeek;
      device->devoptab.fstat_r = SmbFstat;
      device->devoptab.stat_r = SmbStat;
      device->devoptab.unlink_r = SmbUnlink;
      device->devoptab.rename_r = SmbRename;
      device->devoptab.mkdir_r = SmbMkdir;
      device->devoptab.dirStateSize = sizeof(SmbDir);
      device->devoptab.diropen_r = SmbDirOpen;
      device->devoptab.dirreset_r = SmbDirReset;
      device->devoptab.dirnext_r = SmbDirNext;
      device->devoptab.dirclose_r = SmbDirClose;
      device->devoptab.statvfs_r = SmbStatvfs;
      device->devoptab.ftruncate_r = SmbTruncate;
      device->devoptab.fsync_r = SmbSync;
      device->devoptab.deviceData = device.get();
      device->devoptab.rmdir_r = SmbRmdir;
      device->devoptab.lstat_r = SmbStat;
      mount->device = device.get();
      if (AddDevice(&device->devoptab) < 0)
      {
        mount->device = nullptr;
        if (error)
          *error = "No free filesystem slot is available for the SMB share";
        return false;
      }
      s_smb_devices.emplace_back(std::move(device));
      s_smb_mounts.emplace_back(mount);
      return true;
    }
  }

  // Another worker finished mounting this ID while we negotiated. Dispose of
  // our duplicate connection outside the registry lock, then report/recover
  // the published mount's actual state.
  mount.reset();
  std::unique_lock lock(raced_mount->io_mutex);
  if (MountRetired(raced_mount.get()))
  {
    lock.unlock();
    return MountSmb(share, error, cancel);
  }
  if (raced_mount->state.load(std::memory_order_acquire) == SmbConnectionState::Connected)
    return true;
  const bool result = ConnectSmbUnlocked(raced_mount.get(), true, cancel);
  if (!result && error)
    *error = raced_mount->last_error;
  return result;
}

bool UnmountSmb(const std::string& id)
{
  std::shared_ptr<SmbMount> removed;
  {
    std::lock_guard lock(s_mount_mutex);
    const auto iterator = std::ranges::find_if(
        s_smb_mounts, [&](const auto& mount) { return mount->config.id == id; });
    if (iterator == s_smb_mounts.end())
      return true;
    (*iterator)->retired.store(true, std::memory_order_release);
    (*iterator)->state.store(SmbConnectionState::Disconnected, std::memory_order_release);
    SmbDevice* const device = (*iterator)->device;
    if (device)
    {
      device->mount.reset();
      RemoveDevice(device->root_path.c_str());
    }
    removed = std::move(*iterator);
    s_smb_mounts.erase(iterator);
  }
  // Wait for any operation that already pinned the mount, then invalidate all
  // remote handles and tear down the connection outside the registry mutex.
  // Open devoptab states retain shared ownership and close locally afterward.
  std::lock_guard lock(removed->io_mutex);
  DisconnectSmbUnlocked(removed.get());
  return true;
}

bool IsSmbMounted(const std::string& id)
{
  std::lock_guard lock(s_mount_mutex);
  return std::ranges::any_of(s_smb_mounts,
                             [&](const auto& mount) { return mount->config.id == id; });
}

SmbConnectionState GetSmbConnectionState(const std::string& id)
{
  std::lock_guard lock(s_mount_mutex);
  const auto mount =
      std::ranges::find_if(s_smb_mounts, [&](const auto& item) { return item->config.id == id; });
  return mount == s_smb_mounts.end() ? SmbConnectionState::Disconnected :
                                      (*mount)->state.load(std::memory_order_acquire);
}

bool ReconnectSmb(const std::string& id, std::string* error, const std::atomic_bool* cancel)
{
  std::shared_ptr<SmbMount> mount;
  {
    std::lock_guard lock(s_mount_mutex);
    const auto iterator = std::ranges::find_if(
        s_smb_mounts, [&](const auto& item) { return item->config.id == id; });
    if (iterator == s_smb_mounts.end())
    {
      if (error)
        *error = "The SMB share is not mounted";
      return false;
    }
    mount = *iterator;
  }
  std::lock_guard lock(mount->io_mutex);
  if (MountRetired(mount.get()))
  {
    if (error)
      *error = "The SMB share is not mounted";
    return false;
  }
  const bool result = ConnectSmbUnlocked(mount.get(), true, cancel);
  if (!result && error)
    *error = mount->last_error;
  return result;
}

std::vector<SmbShare> LoadSmbShares(const std::string& ini_path)
{
  const auto values = ReadIni(ini_path);
  const int count = std::clamp(std::atoi(ValueFor(values, "Storage/SmbCount").c_str()), 0, 8);
  std::vector<SmbShare> shares;
  shares.reserve(count);
  for (int index = 0; index < count; ++index)
  {
    const std::string prefix = "Storage/Smb" + std::to_string(index);
    SmbShare share;
    share.id = ValueFor(values, prefix + "Id");
    share.name = ValueFor(values, prefix + "Name");
    share.server = ValueFor(values, prefix + "Server");
    share.share = ValueFor(values, prefix + "Share");
    share.path = ValueFor(values, prefix + "Path");
    share.user = ValueFor(values, prefix + "User");
    share.password = ValueFor(values, prefix + "Password");
    share.domain = ValueFor(values, prefix + "Domain");
    const std::string automatic = ValueFor(values, prefix + "AutoMount");
    share.autoMount = automatic.empty() || automatic == "1" || automatic == "true";
    if (ValidId(share.id) && !share.server.empty() && !share.share.empty())
      shares.emplace_back(std::move(share));
  }
  return shares;
}

void InitializeFromConfig(const std::string& ini_path, bool initialize_usb,
                          std::vector<std::string>* errors)
{
  std::string error;
  if (initialize_usb && !InitializeUsb(&error) && errors)
    errors->emplace_back(std::move(error));
  for (const auto& share : LoadSmbShares(ini_path))
  {
    if (!share.autoMount)
      continue;
    error.clear();
    if (!MountSmb(share, &error) && errors)
      errors->emplace_back((share.name.empty() ? share.share : share.name) + ": " + error);
  }
}

void Shutdown()
{
  SetUsbStatusCallback(nullptr);
  std::vector<std::shared_ptr<SmbMount>> mounts;
  bool shutdown_usb = false;
  {
    std::lock_guard lock(s_mount_mutex);
    for (auto& mount : s_smb_mounts)
    {
      mount->retired.store(true, std::memory_order_release);
      mount->state.store(SmbConnectionState::Disconnected, std::memory_order_release);
      if (mount->device)
      {
        mount->device->mount.reset();
        RemoveDevice(mount->device->root_path.c_str());
      }
    }
    mounts.swap(s_smb_mounts);
    shutdown_usb = s_usb_initialized;
    s_usb_initialized = false;
  }
  for (const auto& mount : mounts)
  {
    std::lock_guard lock(mount->io_mutex);
    DisconnectSmbUnlocked(mount.get());
  }
  mounts.clear();  // Open descriptors can still retain retired mount objects.
  if (shutdown_usb)
  {
    usbHsFsSetPopulateCallback(nullptr, nullptr);
    usbHsFsExit();
    std::lock_guard lock(s_usb_mutex);
    s_usb_devices.clear();
    s_usb_generation.fetch_add(1, std::memory_order_release);
  }
}
}  // namespace SwitchStorage
