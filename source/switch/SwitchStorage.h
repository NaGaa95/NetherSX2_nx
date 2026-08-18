// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace SwitchStorage
{
struct SmbShare
{
  std::string id;
  std::string name;
  std::string server;
  std::string share;
  std::string path;
  std::string user;
  std::string password;
  std::string domain;
  bool autoMount = true;
};

struct Location
{
  // Stable across umsN: renumbering.  When a disk has no serial number the
  // identifier is best-effort and also incorporates its model and capacity.
  std::string id;
  std::string path;
  std::string label;
  std::string mount_alias;
  std::string physical_id;
  std::string serial_number;
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  std::uint8_t lun = 0;
  std::uint32_t partition = 0;
  std::uint8_t filesystem_type = 0;
  std::uint64_t capacity = 0;
};

enum class SmbConnectionState
{
  Disconnected,
  Connecting,
  Connected,
  Reconnecting,
  Failed,
};

struct UsbSnapshot
{
  std::uint64_t generation = 0;
  std::vector<Location> locations;
};

using UsbStatusCallback = void (*)(void* user_data);

bool InitializeUsb(std::string* error = nullptr);
std::uint64_t UsbStatusGeneration();
// Registers a lightweight wake callback for USB topology changes. The callback
// runs on the libusbhsfs notification thread and must not block. Passing null
// unregisters it; when this function returns, an old callback is no longer in
// flight.
void SetUsbStatusCallback(UsbStatusCallback callback, void* user_data = nullptr);
// Returns the generation and volume list from one consistent callback snapshot.
// Diff Location::id against the previous snapshot for targeted hotplug updates.
UsbSnapshot GetUsbSnapshot();
std::vector<Location> ListUsbLocations();
// Resolves a stable Location::id to its current umsN:/ mount path.
std::string ResolveUsbPath(const std::string& id);
// Flushes/unmounts every partition on the physical disk containing id and
// stops its logical units. The disk may be unplugged after this returns true.
bool SafelyEjectUsb(const std::string& id, std::string* error = nullptr);

bool MountSmb(const SmbShare& share, std::string* error = nullptr,
              const std::atomic_bool* cancel = nullptr);
bool UnmountSmb(const std::string& id);
bool IsSmbMounted(const std::string& id);
SmbConnectionState GetSmbConnectionState(const std::string& id);
bool ReconnectSmb(const std::string& id, std::string* error = nullptr,
                  const std::atomic_bool* cancel = nullptr);
std::string SmbRootPath(const std::string& id);
std::string SmbBrowsePath(const SmbShare& share);
// Compatibility helpers used by the emulator-side launch bridge. The SDL
// launcher keeps its live Store model, while a directly launched core only has
// the persisted launcher.ini available.
std::vector<SmbShare> LoadSmbShares(const std::string& ini_path);
void InitializeFromConfig(const std::string& ini_path, bool initialize_usb = true,
                          std::vector<std::string>* errors = nullptr);

void Shutdown();
}  // namespace SwitchStorage
