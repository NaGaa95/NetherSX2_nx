#pragma once

#include <cstdint>
#include <string>

enum class LauncherUpdateState
{
	Idle,
	Checking,
	UpdateAvailable,
	UpToDate,
	Downloading,
	ReadyToInstall,
	Installing,
	Installed,
	Cancelled,
	Error,
};

struct LauncherReleaseInfo
{
	std::string tag;
	std::string name;
	std::string notes;
	std::string pageUrl;
	std::string assetName;
	std::string assetUrl;
	std::string assetDigest;
	std::uint64_t assetSize{};
};

struct LauncherUpdateSnapshot
{
	LauncherUpdateState state{LauncherUpdateState::Idle};
	LauncherReleaseInfo release;
	std::string error;
	std::uint64_t downloaded{};
	std::uint64_t total{};
};

using LauncherUpdateWakeCallback = void (*)(void* user_data);

const char* LauncherUpdate_BuiltReleaseTag();
bool LauncherUpdate_IsNewer(const std::string& candidate, const std::string& installed);

bool LauncherUpdate_StartCheck(const std::string& installedTag);
bool LauncherUpdate_StartDownload(const std::string& launcherPath);
bool LauncherUpdate_InstallDownloaded(const std::string& launcherPath);
void LauncherUpdate_Cancel();
LauncherUpdateSnapshot LauncherUpdate_GetSnapshot();
void LauncherUpdate_SetWakeCallback(LauncherUpdateWakeCallback callback,
                                    void* user_data = nullptr);
void LauncherUpdate_Shutdown();

bool LauncherUpdate_RecoverInstallation(const std::string& launcherPath, std::string& error);
