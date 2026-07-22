#include "SwitchStorage.h"

#include <switch.h>
#include <usbhsfs.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace SwitchStorage
{
namespace
{
constexpr size_t kSmbReadAheadBytes = 256 * 1024;
constexpr size_t kSmbReadAheadBudget = 8 * 1024 * 1024;

struct SmbMount;

struct SmbFile
{
	SmbMount* mount;
	smb2fh* handle;
	uint8_t* readAhead;
	size_t readAheadOffset;
	size_t readAheadSize;
	uint64_t position;
};

struct SmbDir
{
	SmbMount* mount;
	smb2dir* handle;
};

struct SmbMount
{
	SmbShare config;
	std::string deviceName;
	std::string rootPath;
	smb2_context* context = nullptr;
	bool connected = false;
	devoptab_t devoptab{};
	std::mutex ioMutex;
	size_t readAheadBytes = 0;

	~SmbMount()
	{
		if (context)
		{
			if (connected)
				smb2_disconnect_share(context);
			smb2_destroy_context(context);
		}
	}
};

bool EnsureReadAheadBuffer(SmbFile* file)
{
	if (file->readAhead)
		return true;
	if (file->mount->readAheadBytes > kSmbReadAheadBudget - kSmbReadAheadBytes)
		return false;
	file->readAhead = static_cast<uint8_t*>(std::malloc(kSmbReadAheadBytes));
	if (!file->readAhead)
		return false;
	file->mount->readAheadBytes += kSmbReadAheadBytes;
	return true;
}

void ReleaseReadAheadBuffer(SmbFile* file)
{
	if (!file->readAhead)
		return;
	std::free(file->readAhead);
	file->readAhead = nullptr;
	file->mount->readAheadBytes -= kSmbReadAheadBytes;
	file->readAheadOffset = 0;
	file->readAheadSize = 0;
}

int SynchronizeFilePosition(SmbFile* file)
{
	if (file->readAheadSize == 0)
		return 0;
	uint64_t resultPosition = 0;
	const int result = smb2_lseek(file->mount->context, file->handle,
		static_cast<int64_t>(file->position), SEEK_SET, &resultPosition);
	if (result >= 0)
	{
		file->readAheadOffset = 0;
		file->readAheadSize = 0;
	}
	return result;
}

std::mutex s_mountMutex;
std::vector<std::unique_ptr<SmbMount>> s_smbMounts;
bool s_usbInitialized = false;
std::atomic<uint64_t> s_usbGeneration{0};

void usbStatusChanged(const UsbHsFsDevice*, u32, void*)
{
	s_usbGeneration.fetch_add(1, std::memory_order_release);
}

int fail(_reent* reent, int error)
{
	reent->_errno = error > 0 ? error : EIO;
	return -1;
}

std::string trim(std::string value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

bool validId(const std::string& id)
{
	if (id.empty() || id.size() > 16)
		return false;
	return std::all_of(id.begin(), id.end(), [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		       (c >= '0' && c <= '9') || c == '_';
	});
}

std::string deviceNameForId(const std::string& id)
{
	return validId(id) ? "nxsmb_" + id : std::string{};
}

bool fixPath(const char* source, char* destination, size_t destinationSize)
{
	if (!source || !destination || destinationSize == 0)
		return false;
	const char* colon = std::strchr(source, ':');
	if (!colon)
		return false;
	const char* input = colon + 1;
	while (*input == '/')
		++input;

	size_t length = 0;
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
		if (length + 1 >= destinationSize)
			return false;
		destination[length++] = *input;
	}
	while (length && destination[length - 1] == '/')
		--length;
	destination[length] = '\0';
	return true;
}

bool isRootPath(const char* path)
{
	const char* colon = path ? std::strchr(path, ':') : nullptr;
	if (!colon)
		return false;
	++colon;
	while (*colon == '/')
		++colon;
	return *colon == '\0';
}

void fillStat(struct stat* output, const struct smb2_stat_64& input)
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

SmbMount* mountFrom(_reent* reent)
{
	return reent ? static_cast<SmbMount*>(reent->deviceData) : nullptr;
}

int smbOpen(_reent* reent, void* state, const char* source, int flags, int)
{
	auto* mount = mountFrom(reent);
	auto* file = static_cast<SmbFile*>(state);
	std::memset(file, 0, sizeof(*file));
	if (!mount)
		return fail(reent, ENODEV);
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	file->handle = smb2_open(mount->context, path, flags);
	if (!file->handle)
		return fail(reent, EIO);
	file->mount = mount;
	if (flags & O_APPEND)
	{
		uint64_t position = 0;
		const int result = smb2_lseek(mount->context, file->handle, 0, SEEK_END, &position);
		if (result < 0)
		{
			smb2_close(mount->context, file->handle);
			std::memset(file, 0, sizeof(*file));
			return fail(reent, -result);
		}
		file->position = position;
	}
	reent->_errno = 0;
	return 0;
}

int smbClose(_reent* reent, void* state)
{
	auto* file = static_cast<SmbFile*>(state);
	if (!file || !file->mount || !file->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(file->mount->ioMutex);
	ReleaseReadAheadBuffer(file);
	const int result = smb2_close(file->mount->context, file->handle);
	std::memset(file, 0, sizeof(*file));
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

ssize_t smbRead(_reent* reent, void* state, char* output, size_t length)
{
	auto* file = static_cast<SmbFile*>(state);
	if (!file || !file->mount || !file->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(file->mount->ioMutex);
	const size_t maximum = std::max<size_t>(1, smb2_get_max_read_size(file->mount->context));
	size_t total = 0;
	if (file->readAheadOffset < file->readAheadSize)
	{
		const size_t cached = std::min(length,
			file->readAheadSize - file->readAheadOffset);
		std::memcpy(output, file->readAhead + file->readAheadOffset, cached);
		file->readAheadOffset += cached;
		file->position += cached;
		total += cached;
		if (file->readAheadOffset == file->readAheadSize)
		{
			file->readAheadOffset = 0;
			file->readAheadSize = 0;
		}
	}
	while (total < length)
	{
		const size_t remaining = length - total;
		const bool useReadAhead = remaining < kSmbReadAheadBytes && EnsureReadAheadBuffer(file);
		const size_t amount = std::min(useReadAhead ? kSmbReadAheadBytes : remaining, maximum);
		uint8_t* destination = useReadAhead ? file->readAhead : reinterpret_cast<uint8_t*>(output + total);
		const int result = smb2_read(file->mount->context, file->handle, destination, amount);
		if (result < 0)
			return total ? static_cast<ssize_t>(total) : fail(reent, -result);
		if (result == 0)
			break;
		if (useReadAhead)
		{
			file->readAheadOffset = 0;
			file->readAheadSize = static_cast<size_t>(result);
			const size_t copied = std::min(remaining, file->readAheadSize);
			std::memcpy(output + total, file->readAhead, copied);
			file->readAheadOffset = copied;
			file->position += copied;
			total += copied;
			if (file->readAheadOffset == file->readAheadSize)
			{
				file->readAheadOffset = 0;
				file->readAheadSize = 0;
			}
			break;
		}
		const size_t bytesRead = static_cast<size_t>(result);
		file->position += bytesRead;
		total += bytesRead;
		if (bytesRead < amount)
			break;
	}
	reent->_errno = 0;
	return static_cast<ssize_t>(total);
}

ssize_t smbWrite(_reent* reent, void* state, const char* input, size_t length)
{
	auto* file = static_cast<SmbFile*>(state);
	if (!file || !file->mount || !file->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(file->mount->ioMutex);
	const int synchronized = SynchronizeFilePosition(file);
	if (synchronized < 0)
		return fail(reent, -synchronized);
	const size_t maximum = std::max<size_t>(1, smb2_get_max_write_size(file->mount->context));
	size_t total = 0;
	while (total < length)
	{
		const size_t amount = std::min(length - total, maximum);
		const int result = smb2_write(file->mount->context, file->handle,
		                              reinterpret_cast<const uint8_t*>(input + total), amount);
		if (result < 0)
			return total ? static_cast<ssize_t>(total) : fail(reent, -result);
		if (result == 0)
			return total ? static_cast<ssize_t>(total) : fail(reent, EIO);
		total += static_cast<size_t>(result);
		file->position += static_cast<size_t>(result);
	}
	reent->_errno = 0;
	return static_cast<ssize_t>(total);
}

off_t smbSeek(_reent* reent, void* state, off_t position, int origin)
{
	auto* file = static_cast<SmbFile*>(state);
	if (!file || !file->mount || !file->handle)
	{
		fail(reent, EBADF);
		return static_cast<off_t>(-1);
	}
	std::lock_guard<std::mutex> lock(file->mount->ioMutex);
	uint64_t resultPosition = 0;
	if (origin == SEEK_SET || origin == SEEK_CUR)
	{
		if (file->position > static_cast<uint64_t>(LLONG_MAX))
		{
			fail(reent, EOVERFLOW);
			return static_cast<off_t>(-1);
		}
		const int64_t base = origin == SEEK_SET ? 0 : static_cast<int64_t>(file->position);
		if (position > 0 && base > LLONG_MAX - position)
		{
			fail(reent, EOVERFLOW);
			return static_cast<off_t>(-1);
		}
		const int64_t target = base + position;
		if (target < 0)
		{
			fail(reent, EINVAL);
			return static_cast<off_t>(-1);
		}
		if (file->readAheadSize > 0)
		{
			const uint64_t cacheStart = file->position - file->readAheadOffset;
			const uint64_t cacheEnd = cacheStart + file->readAheadSize;
			if (static_cast<uint64_t>(target) >= cacheStart && static_cast<uint64_t>(target) <= cacheEnd)
			{
				file->readAheadOffset = static_cast<size_t>(static_cast<uint64_t>(target) - cacheStart);
				file->position = static_cast<uint64_t>(target);
				reent->_errno = 0;
				return static_cast<off_t>(target);
			}
		}
		const int result = smb2_lseek(file->mount->context, file->handle, target, SEEK_SET, &resultPosition);
		if (result < 0)
		{
			fail(reent, -result);
			return static_cast<off_t>(-1);
		}
	}
	else
	{
		const int result = smb2_lseek(file->mount->context, file->handle, position, origin, &resultPosition);
		if (result < 0)
		{
			fail(reent, -result);
			return static_cast<off_t>(-1);
		}
	}
	if (resultPosition > static_cast<uint64_t>(LLONG_MAX))
	{
		fail(reent, EOVERFLOW);
		return static_cast<off_t>(-1);
	}
	file->readAheadOffset = 0;
	file->readAheadSize = 0;
	file->position = resultPosition;
	reent->_errno = 0;
	return static_cast<off_t>(resultPosition);
}

int smbFstat(_reent* reent, void* state, struct stat* output)
{
	auto* file = static_cast<SmbFile*>(state);
	if (!file || !file->mount || !file->handle || !output)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(file->mount->ioMutex);
	struct smb2_stat_64 info{};
	const int result = smb2_fstat(file->mount->context, file->handle, &info);
	if (result < 0)
		return fail(reent, -result);
	fillStat(output, info);
	reent->_errno = 0;
	return 0;
}

int smbStat(_reent* reent, const char* source, struct stat* output)
{
	auto* mount = mountFrom(reent);
	if (!mount || !output)
		return fail(reent, EINVAL);
	if (isRootPath(source))
	{
		std::memset(output, 0, sizeof(*output));
		output->st_mode = S_IFDIR | 0777;
		output->st_nlink = 1;
		reent->_errno = 0;
		return 0;
	}
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	struct smb2_stat_64 info{};
	const int result = smb2_stat(mount->context, path, &info);
	if (result < 0)
		return fail(reent, -result);
	fillStat(output, info);
	reent->_errno = 0;
	return 0;
}

template <typename Operation>
int pathOperation(_reent* reent, const char* source, Operation operation)
{
	auto* mount = mountFrom(reent);
	if (!mount)
		return fail(reent, ENODEV);
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	const int result = operation(mount, path);
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

int smbUnlink(_reent* reent, const char* path)
{
	return pathOperation(reent, path, [](SmbMount* mount, const char* fixed) {
		return smb2_unlink(mount->context, fixed);
	});
}

int smbMkdir(_reent* reent, const char* path, int)
{
	return pathOperation(reent, path, [](SmbMount* mount, const char* fixed) {
		return smb2_mkdir(mount->context, fixed);
	});
}

int smbRmdir(_reent* reent, const char* path)
{
	return pathOperation(reent, path, [](SmbMount* mount, const char* fixed) {
		return smb2_rmdir(mount->context, fixed);
	});
}

int smbRename(_reent* reent, const char* source, const char* destination)
{
	auto* mount = mountFrom(reent);
	if (!mount)
		return fail(reent, ENODEV);
	char oldPath[PATH_MAX]{}, newPath[PATH_MAX]{};
	if (!fixPath(source, oldPath, sizeof(oldPath)) ||
	    !fixPath(destination, newPath, sizeof(newPath)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	const int result = smb2_rename(mount->context, oldPath, newPath);
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

DIR_ITER* smbDirOpen(_reent* reent, DIR_ITER* state, const char* source)
{
	auto* mount = mountFrom(reent);
	auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
	if (!mount || !directory)
	{
		fail(reent, EINVAL);
		return nullptr;
	}
	std::memset(directory, 0, sizeof(*directory));
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
	{
		fail(reent, ENAMETOOLONG);
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	directory->handle = smb2_opendir(mount->context, path);
	if (!directory->handle)
	{
		fail(reent, EIO);
		return nullptr;
	}
	directory->mount = mount;
	reent->_errno = 0;
	return state;
}

int smbDirReset(_reent* reent, DIR_ITER* state)
{
	auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
	if (!directory || !directory->mount || !directory->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(directory->mount->ioMutex);
	smb2_rewinddir(directory->mount->context, directory->handle);
	reent->_errno = 0;
	return 0;
}

int smbDirNext(_reent* reent, DIR_ITER* state, char* name, struct stat* output)
{
	auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
	if (!directory || !directory->mount || !directory->handle || !name || !output)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(directory->mount->ioMutex);
	const struct smb2dirent* entry = smb2_readdir(directory->mount->context, directory->handle);
	if (!entry)
		return fail(reent, ENOENT);
	std::snprintf(name, NAME_MAX, "%s", entry->name);
	fillStat(output, entry->st);
	reent->_errno = 0;
	return 0;
}

int smbDirClose(_reent* reent, DIR_ITER* state)
{
	auto* directory = state ? static_cast<SmbDir*>(state->dirStruct) : nullptr;
	if (!directory || !directory->mount || !directory->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(directory->mount->ioMutex);
	smb2_closedir(directory->mount->context, directory->handle);
	std::memset(directory, 0, sizeof(*directory));
	reent->_errno = 0;
	return 0;
}

int smbStatvfs(_reent* reent, const char* source, struct statvfs* output)
{
	auto* mount = mountFrom(reent);
	if (!mount || !output)
		return fail(reent, EINVAL);
	char path[PATH_MAX]{};
	if (!fixPath(source, path, sizeof(path)))
		return fail(reent, ENAMETOOLONG);
	std::lock_guard<std::mutex> lock(mount->ioMutex);
	struct smb2_statvfs info{};
	const int result = smb2_statvfs(mount->context, path, &info);
	if (result < 0)
		return fail(reent, -result);
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

int smbTruncate(_reent* reent, void* state, off_t length)
{
	auto* file = static_cast<SmbFile*>(state);
	if (!file || !file->mount || !file->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(file->mount->ioMutex);
	const int synchronized = SynchronizeFilePosition(file);
	if (synchronized < 0)
		return fail(reent, -synchronized);
	const int result = smb2_ftruncate(file->mount->context, file->handle, length);
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

int smbSync(_reent* reent, void* state)
{
	auto* file = static_cast<SmbFile*>(state);
	if (!file || !file->mount || !file->handle)
		return fail(reent, EBADF);
	std::lock_guard<std::mutex> lock(file->mount->ioMutex);
	const int result = smb2_fsync(file->mount->context, file->handle);
	if (result < 0)
		return fail(reent, -result);
	reent->_errno = 0;
	return 0;
}

std::unordered_map<std::string, std::string> readIni(const std::string& path)
{
	std::unordered_map<std::string, std::string> values;
	FILE* file = std::fopen(path.c_str(), "rb");
	if (!file)
		return values;
	char line[4096];
	while (std::fgets(line, sizeof(line), file))
	{
		std::string text = trim(line);
		if (text.empty() || text.front() == '#' || text.front() == ';' || text.front() == '[')
			continue;
		const auto separator = text.find('=');
		if (separator == std::string::npos)
			continue;
		std::string key = trim(text.substr(0, separator));
		if (!key.empty())
			values[key] = trim(text.substr(separator + 1));
	}
	std::fclose(file);
	return values;
}

std::string valueFor(const std::unordered_map<std::string, std::string>& values,
	                 const std::string& key)
{
	const auto iterator = values.find(key);
	return iterator == values.end() ? std::string{} : iterator->second;
}
} // namespace

std::string SmbRootPath(const std::string& id)
{
	const std::string deviceName = deviceNameForId(id);
	return deviceName.empty() ? std::string{} : deviceName + ":/";
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
	std::lock_guard<std::mutex> lock(s_mountMutex);
	if (s_usbInitialized)
		return true;
	usbHsFsSetFileSystemMountFlags(UsbHsFsMountFlags_None);
	const Result result = usbHsFsInitialize(0);
	if (R_FAILED(result))
	{
		if (error)
		{
			char message[64];
			std::snprintf(message, sizeof(message), "USB initialization failed (0x%08x)", result);
			*error = message;
		}
		return false;
	}
	s_usbInitialized = true;
	usbHsFsSetPopulateCallback(usbStatusChanged, nullptr);
	return true;
}

uint64_t UsbStatusGeneration()
{
	return s_usbGeneration.load(std::memory_order_acquire);
}

bool MountSmb(const SmbShare& share, std::string* error)
{
	std::lock_guard<std::mutex> lock(s_mountMutex);
	if (!validId(share.id) || share.server.empty() || share.share.empty())
	{
		if (error)
			*error = "SMB share settings are incomplete";
		return false;
	}
	for (const auto& mount : s_smbMounts)
	{
		if (mount->config.id == share.id)
			return true;
	}

	auto mount = std::make_unique<SmbMount>();
	mount->config = share;
	mount->deviceName = deviceNameForId(share.id);
	mount->rootPath = mount->deviceName + ":/";
	mount->context = smb2_init_context();
	if (!mount->context)
	{
		if (error)
			*error = "Could not create the SMB client";
		return false;
	}
	smb2_set_security_mode(mount->context, SMB2_NEGOTIATE_SIGNING_ENABLED);
	smb2_set_timeout(mount->context, 6);
	if (!share.user.empty())
		smb2_set_user(mount->context, share.user.c_str());
	if (!share.password.empty())
		smb2_set_password(mount->context, share.password.c_str());
	if (!share.domain.empty())
		smb2_set_domain(mount->context, share.domain.c_str());
	const int connected = smb2_connect_share(mount->context, share.server.c_str(),
	                                         share.share.c_str(),
	                                         share.user.empty() ? nullptr : share.user.c_str());
	if (connected < 0)
	{
		if (error)
		{
			const char* detail = smb2_get_error(mount->context);
			*error = detail && *detail ? detail : "Could not connect to the SMB share";
		}
		return false;
	}
	mount->connected = true;

	mount->devoptab.name = mount->deviceName.c_str();
	mount->devoptab.structSize = sizeof(SmbFile);
	mount->devoptab.open_r = smbOpen;
	mount->devoptab.close_r = smbClose;
	mount->devoptab.write_r = smbWrite;
	mount->devoptab.read_r = smbRead;
	mount->devoptab.seek_r = smbSeek;
	mount->devoptab.fstat_r = smbFstat;
	mount->devoptab.stat_r = smbStat;
	mount->devoptab.unlink_r = smbUnlink;
	mount->devoptab.rename_r = smbRename;
	mount->devoptab.mkdir_r = smbMkdir;
	mount->devoptab.dirStateSize = sizeof(SmbDir);
	mount->devoptab.diropen_r = smbDirOpen;
	mount->devoptab.dirreset_r = smbDirReset;
	mount->devoptab.dirnext_r = smbDirNext;
	mount->devoptab.dirclose_r = smbDirClose;
	mount->devoptab.statvfs_r = smbStatvfs;
	mount->devoptab.ftruncate_r = smbTruncate;
	mount->devoptab.fsync_r = smbSync;
	mount->devoptab.deviceData = mount.get();
	mount->devoptab.rmdir_r = smbRmdir;
	mount->devoptab.lstat_r = smbStat;
	if (AddDevice(&mount->devoptab) < 0)
	{
		if (error)
			*error = "No free filesystem slot is available for the SMB share";
		return false;
	}
	s_smbMounts.emplace_back(std::move(mount));
	return true;
}

bool UnmountSmb(const std::string& id)
{
	std::lock_guard<std::mutex> lock(s_mountMutex);
	const auto iterator = std::find_if(s_smbMounts.begin(), s_smbMounts.end(),
	                                  [&](const auto& mount) { return mount->config.id == id; });
	if (iterator == s_smbMounts.end())
		return true;
	RemoveDevice((*iterator)->rootPath.c_str());
	s_smbMounts.erase(iterator);
	return true;
}

bool IsSmbMounted(const std::string& id)
{
	std::lock_guard<std::mutex> lock(s_mountMutex);
	return std::any_of(s_smbMounts.begin(), s_smbMounts.end(),
	                   [&](const auto& mount) { return mount->config.id == id; });
}

std::vector<Location> ListUsbLocations()
{
	std::vector<Location> locations;
	std::lock_guard<std::mutex> lock(s_mountMutex);
	if (!s_usbInitialized)
		return locations;
	std::array<UsbHsFsDevice, 32> devices{};
	const u32 count = usbHsFsListMountedDevices(devices.data(), devices.size());
	locations.reserve(count);
	for (u32 index = 0; index < count; ++index)
	{
		const auto& device = devices[index];
		Location location;
		location.path = device.name;
		if (location.path.empty())
			continue;
		if (location.path.back() != '/')
			location.path += '/';
		const uint64_t gib = device.capacity / (1024ULL * 1024ULL * 1024ULL);
		char label[256];
		std::snprintf(label, sizeof(label), "%s - %s%s%s (%llu GiB)", device.name,
		              LIBUSBHSFS_FS_TYPE_STR(device.fs_type),
		              device.product_name[0] ? " - " : "",
		              device.product_name,
		              static_cast<unsigned long long>(gib));
		location.label = label;
		locations.emplace_back(std::move(location));
	}
	return locations;
}

std::vector<SmbShare> LoadSmbShares(const std::string& iniPath)
{
	const auto values = readIni(iniPath);
	const std::string countText = valueFor(values, "Storage/SmbCount");
	const int count = std::clamp(std::atoi(countText.c_str()), 0, 8);
	std::vector<SmbShare> shares;
	shares.reserve(count);
	for (int index = 0; index < count; ++index)
	{
		const std::string prefix = "Storage/Smb" + std::to_string(index);
		SmbShare share;
		share.id = valueFor(values, prefix + "Id");
		share.name = valueFor(values, prefix + "Name");
		share.server = valueFor(values, prefix + "Server");
		share.share = valueFor(values, prefix + "Share");
		share.path = valueFor(values, prefix + "Path");
		share.user = valueFor(values, prefix + "User");
		share.password = valueFor(values, prefix + "Password");
		share.domain = valueFor(values, prefix + "Domain");
		const std::string automatic = valueFor(values, prefix + "AutoMount");
		share.autoMount = automatic.empty() || automatic == "1" || automatic == "true";
		if (validId(share.id) && !share.server.empty() && !share.share.empty())
			shares.emplace_back(std::move(share));
	}
	return shares;
}

void InitializeFromConfig(const std::string& iniPath, bool initializeUsb,
                          std::vector<std::string>* errors)
{
	std::string error;
	if (initializeUsb && !InitializeUsb(&error) && errors)
		errors->emplace_back(std::move(error));
	for (const auto& share : LoadSmbShares(iniPath))
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
	std::lock_guard<std::mutex> lock(s_mountMutex);
	for (auto& mount : s_smbMounts)
		RemoveDevice(mount->rootPath.c_str());
	s_smbMounts.clear();
	if (s_usbInitialized)
	{
		usbHsFsSetPopulateCallback(nullptr, nullptr);
		usbHsFsExit();
		s_usbInitialized = false;
	}
}
} // namespace SwitchStorage
