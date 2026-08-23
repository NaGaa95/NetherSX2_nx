#include "forwarder.h"
#include "forwarder_structs.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <turbojpeg.h>

std::string g_forwarderSelfPath;

namespace {

#define FWD_TRY(x) do { const Result _rc_ = (x); if (R_FAILED(_rc_)) return _rc_; } while (0)

constexpr u8 HEADER_KEK_SRC[0x10] = {
    0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D, 0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A};
constexpr u8 HEADER_KEY_SRC[0x20] = {
    0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
    0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2};

Result derive_header_key(u8 header_key[0x20])
{
    u8 header_kek[0x20];
    FWD_TRY(splCryptoGenerateAesKek(HEADER_KEK_SRC, 0, 0, header_kek));
    FWD_TRY(splCryptoGenerateAesKey(header_kek, HEADER_KEY_SRC, header_key));
    FWD_TRY(splCryptoGenerateAesKey(header_kek, HEADER_KEY_SRC + 0x10, header_key + 0x10));
    return 0;
}

Service g_nsAppSrv;
bool g_nsAppInitialized{};

Result ns_app_init()
{
    Result rc = nsInitialize();
    if (R_FAILED(rc))
        return rc;
    if (hosversionAtLeast(3, 0, 0)) {
        rc = nsGetApplicationManagerInterface(&g_nsAppSrv);
        if (R_FAILED(rc)) {
            nsExit();
            return rc;
        }
    } else {
        g_nsAppSrv = *nsGetServiceSession_ApplicationManagerInterface();
    }
    g_nsAppInitialized = true;
    return 0;
}

void ns_app_exit()
{
    if (!g_nsAppInitialized)
        return;
    serviceClose(&g_nsAppSrv);
    nsExit();
    g_nsAppSrv = {};
    g_nsAppInitialized = false;
}

Result ns_push_application_record(u64 tid, const FwdContentStorageRecord *records, u32 count)
{
    const struct {
        u8 last_modified_event;
        u8 padding[0x7];
        u64 tid;
    } in = {0x3 /* Installed */, {0}, tid};
    return serviceDispatchIn(&g_nsAppSrv, 16, in,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
        .buffers = {{records, sizeof(*records) * count}});
}

Result ns_invalidate_control_cache(u64 tid)
{
    return serviceDispatchIn(&g_nsAppSrv, 404, tid);
}

constexpr u32 IVFC_MAX_LEVEL = 6;
constexpr u32 IVFC_HASH_BLOCK_SIZE = 0x4000;
constexpr u32 PFS0_EXEFS_HASH_BLOCK_SIZE = 0x10000;
constexpr u32 PFS0_META_HASH_BLOCK_SIZE = 0x1000;
constexpr u32 PFS0_PADDING_SIZE = 0x200;
constexpr u32 ROMFS_ENTRY_EMPTY = 0xFFFFFFFF;
constexpr u32 ROMFS_FILEPARTITION_OFS = 0x200;

struct BufHelper {
    BufHelper() = default;
    void write(const void *data, u64 size)
    {
        if (size > std::numeric_limits<size_t>::max() ||
            offset > std::numeric_limits<size_t>::max() - static_cast<size_t>(size))
            throw std::length_error("forwarder buffer is too large");
        const size_t end = static_cast<size_t>(offset + size);
        if (end > buf.size())
            buf.resize(end);
        if (size)
            std::memcpy(buf.data() + static_cast<size_t>(offset), data, static_cast<size_t>(size));
        offset += size;
    }
    void seek(u64 where_to) { offset = where_to; }
    auto tell() const { return offset; }
    std::vector<u8> buf;
    u64 offset{};
};

struct NcaEntry {
    NcaEntry(const BufHelper &b, NcmContentType t) : data{b.buf}, type{(u8)t}
    {
        sha256CalculateHash(hash, data.data(), data.size());
    }
    const std::vector<u8> data;
    const u8 type;
    u8 hash[SHA256_HASH_SIZE];
};

struct FileEntry {
    std::string name;
    std::vector<u8> data;
};
using FileEntries = std::vector<FileEntry>;

void add_file_entry(FileEntries &entries, const char *name, const void *data, u64 size)
{
    if (!name || size > std::numeric_limits<size_t>::max() || (size && !data))
        throw std::invalid_argument("invalid forwarder file");
    FileEntry e;
    e.name = name;
    e.data.resize(static_cast<size_t>(size));
    if (size)
        std::memcpy(e.data.data(), data, static_cast<size_t>(size));
    entries.emplace_back(std::move(e));
}
u64 write_padding(BufHelper &buf, u64 off, u64 block)
{
    const u64 size = block - (off % block);
    if (size) {
        std::vector<u8> padding(size);
        buf.write(padding.data(), padding.size());
    }
    return size;
}

u32 align32(u32 offset, u32 alignment)
{
    const u32 mask = ~(alignment - 1);
    return (offset + (alignment - 1)) & mask;
}
u64 align64(u64 offset, u64 alignment)
{
    const u64 mask = ~(u64)(alignment - 1);
    return (offset + (alignment - 1)) & mask;
}

u32 calc_path_hash(u32 parent, const u8 *path, u32 start, u32 path_len)
{
    u32 hash = parent ^ 123456789;
    for (u32 i = 0; i < path_len; i++) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= path[start + i];
    }
    return hash;
}

u32 romfs_get_hash_table_count(u32 num_entries)
{
    if (num_entries < 3)
        return 3;
    else if (num_entries < 19)
        return num_entries | 1;
    u32 count = num_entries;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 ||
           count % 11 == 0 || count % 13 == 0 || count % 17 == 0)
        count++;
    return count;
}

void build_romfs_into_file(const FileEntries &entries, BufHelper &buf)
{
    if (entries.size() > std::numeric_limits<u32>::max())
        throw std::length_error("too many RomFS files");
    const u32 fileCount = static_cast<u32>(entries.size());
    const u32 dirHashCount = romfs_get_hash_table_count(1);
    const u32 fileHashCount = romfs_get_hash_table_count(fileCount);
    const u64 dirHashTableSize = sizeof(u32) * dirHashCount;
    const u64 fileHashTableSize = sizeof(u32) * fileHashCount;
    std::vector<u32> dir_hash_table(dirHashCount, ROMFS_ENTRY_EMPTY);
    std::vector<u32> file_hash_table(fileHashCount, ROMFS_ENTRY_EMPTY);
    std::vector<u32> fileOffsets(fileCount);
    std::vector<u64> dataOffsets(fileCount);

    u64 fileTableSize = 0;
    u64 filePartitionSize = 0;
    for (u32 i = 0; i < fileCount; i++) {
        const auto &entry = entries[i];
        if (entry.name.size() < 2 || entry.name[0] != '/' ||
            entry.name.find('/', 1) != std::string::npos ||
            entry.name.size() - 1 > std::numeric_limits<u32>::max() - 3)
            throw std::invalid_argument("invalid root RomFS path");
        const u32 nameSize = static_cast<u32>(entry.name.size() - 1);
        if (fileTableSize > std::numeric_limits<u32>::max() ||
            sizeof(romfs_file) + align32(nameSize, 4) > std::numeric_limits<u32>::max() - fileTableSize)
            throw std::length_error("RomFS file table is too large");
        fileOffsets[i] = static_cast<u32>(fileTableSize);
        fileTableSize += sizeof(romfs_file) + align32(nameSize, 4);
        filePartitionSize = align64(filePartitionSize, 0x10);
        dataOffsets[i] = filePartitionSize;
        if (entry.data.size() > std::numeric_limits<u64>::max() - filePartitionSize)
            throw std::length_error("RomFS data is too large");
        filePartitionSize += entry.data.size();
    }

    std::vector<u8> dirTable(sizeof(romfs_dir));
    std::vector<u8> fileTable(static_cast<size_t>(fileTableSize));
    romfs_dir root{};
    root.parent = 0;
    root.sibling = ROMFS_ENTRY_EMPTY;
    root.childDir = ROMFS_ENTRY_EMPTY;
    root.childFile = fileCount ? fileOffsets[0] : ROMFS_ENTRY_EMPTY;
    const u32 rootHash = calc_path_hash(0, nullptr, 0, 0);
    root.nextHash = dir_hash_table[rootHash % dirHashCount];
    root.nameLen = 0;
    dir_hash_table[rootHash % dirHashCount] = 0;
    std::memcpy(dirTable.data(), &root, sizeof(root));

    for (u32 i = 0; i < fileCount; i++) {
        const auto &source = entries[i];
        const u32 nameSize = static_cast<u32>(source.name.size() - 1);
        romfs_file entry{};
        entry.parent = 0;
        entry.sibling = i + 1 < fileCount ? fileOffsets[i + 1] : ROMFS_ENTRY_EMPTY;
        entry.dataOff = dataOffsets[i];
        entry.dataSize = source.data.size();
        const u32 hash = calc_path_hash(0, reinterpret_cast<const u8 *>(source.name.data()), 1, nameSize);
        entry.nextHash = file_hash_table[hash % fileHashCount];
        entry.nameLen = nameSize;
        file_hash_table[hash % fileHashCount] = fileOffsets[i];
        std::memcpy(fileTable.data() + fileOffsets[i], &entry, sizeof(entry));
        std::memcpy(fileTable.data() + fileOffsets[i] + sizeof(entry), source.name.data() + 1, nameSize);
    }

    romfs_header header{};
    if (filePartitionSize > std::numeric_limits<u64>::max() - ROMFS_FILEPARTITION_OFS - 3)
        throw std::length_error("RomFS offsets are too large");
    header.headerSize = sizeof(header);
    header.fileHashTableSize = fileHashTableSize;
    header.fileTableSize = fileTableSize;
    header.dirHashTableSize = dirHashTableSize;
    header.dirTableSize = dirTable.size();
    header.fileDataOff = ROMFS_FILEPARTITION_OFS;
    header.dirHashTableOff = align64(filePartitionSize + ROMFS_FILEPARTITION_OFS, 4);
    header.dirTableOff = header.dirHashTableOff + dirHashTableSize;
    header.fileHashTableOff = header.dirTableOff + dirTable.size();
    header.fileTableOff = header.fileHashTableOff + fileHashTableSize;

    buf.write(&header, sizeof(header));
    for (u32 i = 0; i < fileCount; i++) {
        buf.seek(dataOffsets[i] + ROMFS_FILEPARTITION_OFS);
        buf.write(entries[i].data.data(), entries[i].data.size());
    }
    buf.seek(header.dirHashTableOff);
    buf.write(dir_hash_table.data(), dirHashTableSize);
    buf.write(dirTable.data(), dirTable.size());
    buf.write(file_hash_table.data(), fileHashTableSize);
    buf.write(fileTable.data(), fileTable.size());
}

std::vector<u8> romfs_build(const FileEntries &entries, u64 *out_size)
{
    BufHelper buf;
    build_romfs_into_file(entries, buf);
    buf.seek(buf.buf.size());
    *out_size = buf.tell();
    write_padding(buf, buf.tell(), IVFC_HASH_BLOCK_SIZE);
    return buf.buf;
}

std::vector<u8> build_pfs0(const FileEntries &entries)
{
    BufHelper buf;
    struct Pfs0Header { u32 magic; u32 total_files; u32 string_table_size; u32 padding; } header{};
    struct Pfs0FileTable { u64 data_offset; u64 data_size; u32 name_offset; u32 padding; };
    std::vector<Pfs0FileTable> file_table(entries.size());
    std::vector<char> string_table;

    u64 string_offset{}, data_offset{};
    for (u32 i = 0; i < entries.size(); i++) {
        file_table[i].data_offset = data_offset;
        file_table[i].data_size = entries[i].data.size();
        file_table[i].name_offset = string_offset;
        file_table[i].padding = 0;
        string_table.resize(string_offset + entries[i].name.length() + 1);
        std::memcpy(string_table.data() + string_offset, entries[i].name.c_str(), entries[i].name.length() + 1);
        data_offset += entries[i].data.size();
        string_offset += entries[i].name.length() + 1;
    }
    string_table.resize((string_table.size() + 0x1F) & ~0x1F);

    header.magic = 0x30534650;
    header.total_files = entries.size();
    header.string_table_size = string_table.size();
    buf.write(&header, sizeof(header));
    buf.write(file_table.data(), sizeof(Pfs0FileTable) * file_table.size());
    buf.write(string_table.data(), string_table.size());
    for (const auto &e : entries)
        buf.write(e.data.data(), e.data.size());
    return buf.buf;
}

std::vector<u8> build_pfs0_hash_table(const std::vector<u8> &pfs0, u32 block_size)
{
    BufHelper buf;
    u8 hash[SHA256_HASH_SIZE];
    u32 read_size = block_size;
    for (u32 i = 0; i < pfs0.size(); i += read_size) {
        if (i + read_size >= pfs0.size())
            read_size = pfs0.size() - i;
        sha256CalculateHash(hash, pfs0.data() + i, read_size);
        buf.write(hash, sizeof(hash));
    }
    return buf.buf;
}

std::vector<u8> build_master_hash(const std::vector<u8> &data)
{
    std::vector<u8> hash(SHA256_HASH_SIZE);
    sha256CalculateHash(hash.data(), data.data(), data.size());
    return hash;
}

void write_nca_padding(BufHelper &buf) { write_padding(buf, buf.tell(), 0x200); }

void nca_encrypt_header(nca::Header *header, const u8 *key)
{
    Aes128XtsContext ctx{};
    aes128XtsContextCreate(&ctx, key, key + 0x10, true);
    u8 sector{};
    for (u64 pos = 0; pos < 0xC00; pos += 0x200) {
        aes128XtsContextResetSector(&ctx, sector++, true);
        aes128XtsEncrypt(&ctx, (u8 *)header + pos, (const u8 *)header + pos, 0x200);
    }
}

void write_nca_section(nca::Header &h, u8 index, u64 start, u64 end)
{
    auto &s = h.fs_table[index];
    s.media_start_offset = start / 0x200;
    s.media_end_offset = end / 0x200;
    s._0x8[0] = 0x1;
}

void write_nca_fs_header_pfs0(nca::Header &h, u8 index, const std::vector<u8> &master_hash, u64 hash_table_size, u32 block_size)
{
    auto &fs = h.fs_header[index];
    fs.version = 0x2;
    fs.fs_type = nca::FileSystemType_PFS0;
    fs.hash_type = nca::HashType_HierarchicalSha256;
    fs.encryption_type = nca::EncryptionType_None;
    fs.hash_data.hierarchical_sha256_data.layer_count = 0x2;
    fs.hash_data.hierarchical_sha256_data.block_size = block_size;
    fs.hash_data.hierarchical_sha256_data.hash_layer.size = hash_table_size;
    std::memcpy(fs.hash_data.hierarchical_sha256_data.master_hash, master_hash.data(), master_hash.size());
    sha256CalculateHash(&h.fs_header_hash[index], &fs, sizeof(fs));
}

void write_nca_fs_header_romfs(nca::Header &h, u8 index)
{
    auto &fs = h.fs_header[index];
    fs.version = 0x2;
    fs.fs_type = nca::FileSystemType_RomFS;
    fs.hash_type = nca::HashType_HierarchicalIntegrity;
    fs.encryption_type = nca::EncryptionType_None;
    fs.hash_data.integrity_meta_info.magic = 0x43465649; // IVFC
    fs.hash_data.integrity_meta_info.version = 0x20000;
    fs.hash_data.integrity_meta_info.master_hash_size = SHA256_HASH_SIZE;
    fs.hash_data.integrity_meta_info.info_level_hash.max_layers = 0x7;
    fs.hash_data.integrity_meta_info.info_level_hash.levels[5].block_size = 0x0E; // 0x4000
    sha256CalculateHash(&h.fs_header_hash[index], &fs, sizeof(fs));
}

void write_nca_pfs0(nca::Header &h, u8 index, const FileEntries &entries, u32 block_size, BufHelper &buf)
{
    const auto pfs0 = build_pfs0(entries);
    const auto pfs0_hash_table = build_pfs0_hash_table(pfs0, block_size);
    const auto pfs0_master_hash = build_master_hash(pfs0_hash_table);

    buf.write(pfs0_hash_table.data(), pfs0_hash_table.size());
    const auto padding_size = write_padding(buf, pfs0_hash_table.size(), PFS0_PADDING_SIZE);

    h.fs_header[index].hash_data.hierarchical_sha256_data.pfs0_layer.offset = pfs0_hash_table.size() + padding_size;
    h.fs_header[index].hash_data.hierarchical_sha256_data.pfs0_layer.size = pfs0.size();

    buf.write(pfs0.data(), pfs0.size());
    write_nca_padding(buf);

    const auto section_start = index == 0 ? sizeof(h) : h.fs_table[index - 1].media_end_offset * 0x200;
    write_nca_section(h, index, section_start, buf.tell());
    write_nca_fs_header_pfs0(h, index, pfs0_master_hash, pfs0_hash_table.size(), block_size);
}

std::vector<u8> ivfc_create_level(const std::vector<u8> &src)
{
    BufHelper buf;
    u8 hash[SHA256_HASH_SIZE];
    u64 read_size = IVFC_HASH_BLOCK_SIZE;
    for (u32 i = 0; i < src.size(); i += read_size) {
        if (i + read_size >= src.size())
            read_size = src.size() - i;
        sha256CalculateHash(hash, src.data() + i, read_size);
        buf.write(hash, sizeof(hash));
    }
    write_padding(buf, buf.tell(), IVFC_HASH_BLOCK_SIZE);
    return buf.buf;
}

void write_nca_romfs(nca::Header &h, u8 index, const FileEntries &entries, BufHelper &buf)
{
    auto &fs = h.fs_header[index];
    auto &meta_info = fs.hash_data.integrity_meta_info;
    auto &info_level_hash = meta_info.info_level_hash;

    std::vector<u8> ivfc[IVFC_MAX_LEVEL];
    ivfc[5] = romfs_build(entries, &info_level_hash.levels[5].hash_data_size);
    for (int b = 4; b >= 0; b--) {
        ivfc[b] = ivfc_create_level(ivfc[b + 1]);
        info_level_hash.levels[b].hash_data_size = ivfc[b].size();
        info_level_hash.levels[b].block_size = 0x0E;
    }
    info_level_hash.levels[0].logical_offset = 0;
    for (int i = 1; i <= 5; i++)
        info_level_hash.levels[i].logical_offset = info_level_hash.levels[i - 1].logical_offset + info_level_hash.levels[i - 1].hash_data_size;

    for (const auto &iv : ivfc)
        buf.write(iv.data(), iv.size());
    write_nca_padding(buf);

    const auto ivfc_master_hash = build_master_hash(ivfc[0]);
    std::memcpy(meta_info.master_hash, ivfc_master_hash.data(), sizeof(meta_info.master_hash));

    const auto section_start = index == 0 ? sizeof(h) : h.fs_table[index - 1].media_end_offset * 0x200;
    write_nca_section(h, index, section_start, buf.tell());
    write_nca_fs_header_romfs(h, index);
}

void write_nca_header_encrypted(nca::Header &h, u64 tid, const u8 *header_key, nca::ContentType type, BufHelper &buf)
{
    h.magic = nca::HeaderMagic;
    h.distribution_type = nca::DistributionType_System;
    h.content_type = type;
    h.program_id = tid;
    h.sdk_version = 0x000C1100;
    h.size = buf.tell();
    nca_encrypt_header(&h, header_key);
    buf.seek(0);
    buf.write(&h, sizeof(h));
}

NcaEntry create_program_nca(u64 tid, const u8 *key, const FileEntries &exefs)
{
    BufHelper buf;
    nca::Header h{};
	buf.write(&h, sizeof(h));
	write_nca_pfs0(h, 0, exefs, PFS0_EXEFS_HASH_BLOCK_SIZE, buf);
	write_nca_header_encrypted(h, tid, key, nca::ContentType_Program, buf);
    return {buf, NcmContentType_Program};
}

NcaEntry create_control_nca(u64 tid, const u8 *key, const FileEntries &romfs)
{
    BufHelper buf;
    nca::Header h{};
    buf.write(&h, sizeof(h));
    write_nca_romfs(h, 0, romfs, buf);
    write_nca_header_encrypted(h, tid, key, nca::ContentType_Control, buf);
    return {buf, NcmContentType_Control};
}

struct MetaResult {
    std::vector<u8> nca;
    u8 hash[SHA256_HASH_SIZE]{};
    NcmContentMetaKey key{};
    FwdContentStorageRecord record{};
    std::vector<u8> data;
    std::vector<NcmContentId> contentIds;
};

MetaResult create_meta_nca(u64 tid, const u8 *key, NcmStorageId storage_id, const std::vector<NcaEntry> &ncas)
{
    constexpr size_t contentCount = 2;
    if (ncas.size() != contentCount)
        throw std::invalid_argument("forwarder requires program and control content");

    CnmtHeader cnmt_header{};
    NcmApplicationMetaExtendedHeader cnmt_extended{};
    NcmPackagedContentInfo packaged_content_info[contentCount]{};
    u8 digest[0x20]{};

    cnmt_header.title_id = tid;
    cnmt_header.title_version = 0;
    cnmt_header.meta_type = NcmContentMetaType_Application;
    cnmt_header.meta_header.extended_header_size = sizeof(cnmt_extended);
    cnmt_header.meta_header.content_count = contentCount;
    cnmt_header.meta_header.content_meta_count = 0x1;
    cnmt_header.meta_header.attributes = 0x0;
    cnmt_header.meta_header.storage_id = storage_id;
    cnmt_extended.patch_id = cnmt_header.title_id | 0x800;

    for (size_t i = 0; i < contentCount; i++) {
        std::memcpy(packaged_content_info[i].hash, ncas[i].hash, sizeof(packaged_content_info[i].hash));
        std::memcpy(&packaged_content_info[i].info.content_id, ncas[i].hash, sizeof(packaged_content_info[i].info.content_id));
        packaged_content_info[i].info.content_type = ncas[i].type;
        ncmU64ToContentInfoSize(ncas[i].data.size(), &packaged_content_info[i].info);
    }

    BufHelper cnmt_buf;
    cnmt_buf.write(&cnmt_header, sizeof(cnmt_header));
    cnmt_buf.write(&cnmt_extended, sizeof(cnmt_extended));
    cnmt_buf.write(&packaged_content_info, sizeof(packaged_content_info));
    cnmt_buf.write(digest, sizeof(digest));

    FileEntries cnmt;
    char cnmt_name[34];
    std::snprintf(cnmt_name, sizeof(cnmt_name), "Application_%016lX.cnmt", tid);
    add_file_entry(cnmt, cnmt_name, cnmt_buf.buf.data(), cnmt_buf.buf.size());

    BufHelper buf;
    nca::Header h{};
    buf.write(&h, sizeof(h));
    write_nca_pfs0(h, 0, cnmt, PFS0_META_HASH_BLOCK_SIZE, buf);
    write_nca_header_encrypted(h, tid, key, nca::ContentType_Meta, buf);

    NcaEntry nca_entry{buf, NcmContentType_Meta};

    MetaResult r;
    r.nca = nca_entry.data;
    std::memcpy(r.hash, nca_entry.hash, sizeof(r.hash));

    NcmContentMetaHeader out_header = cnmt_header.meta_header;
    out_header.content_count++; // + meta itself
    out_header.storage_id = 0;

    r.key.id = cnmt_header.title_id;
    r.key.version = cnmt_header.title_version;
    r.key.type = cnmt_header.meta_type;
    r.key.install_type = NcmContentInstallType_Full;

    r.record.key = r.key;
    r.record.storage_id = storage_id;

    std::vector<NcmContentInfo> infos(contentCount+1);
    std::memcpy(&infos[0].content_id, nca_entry.hash, sizeof(infos[0].content_id));
    infos[0].content_type = nca_entry.type;
    infos[0].attr = 0;
    ncmU64ToContentInfoSize(nca_entry.data.size(), &infos[0]);
    infos[0].id_offset = 0;
    infos[1] = packaged_content_info[0].info;
    infos[2] = packaged_content_info[1].info;
    BufHelper metadata;
    metadata.write(&out_header,sizeof(out_header));
    metadata.write(&cnmt_extended,sizeof(cnmt_extended));
    metadata.write(infos.data(),infos.size()*sizeof(infos[0]));
    r.data=std::move(metadata.buf);
    for(const auto &info:infos) r.contentIds.push_back(info.content_id);
    return r;
}

bool npdm_patch_kc(std::vector<u8> &npdm, u32 off, u32 size, u32 bitmask, u32 value)
{
    if ((size & 3) != 0 || off > npdm.size() || size > npdm.size() - off)
        return false;
    const u32 pattern = BIT(bitmask) - 1;
    const u32 mask = BIT(bitmask) | pattern;
    for (u32 i = 0; i < size; i += 4) {
        u32 cup;
        std::memcpy(&cup, npdm.data() + off + i, sizeof(cup));
        if ((cup & mask) == pattern) {
            cup = value | pattern;
            std::memcpy(npdm.data() + off + i, &cup, sizeof(cup));
            return true;
        }
    }
    return false;
}

bool patch_npdm(std::vector<u8> &npdm, u64 tid)
{
    if (npdm.size() < sizeof(npdm::Meta))
        return false;
    npdm::Meta meta{};
    npdm::Aci0 aci0{};
    npdm::Acid acid{};
    std::memcpy(&meta, npdm.data(), sizeof(meta));
    if (meta.aci0_offset > npdm.size() || sizeof(aci0) > npdm.size() - meta.aci0_offset ||
        meta.acid_offset > npdm.size() || sizeof(acid) > npdm.size() - meta.acid_offset)
        return false;
    std::memcpy(&aci0, npdm.data() + meta.aci0_offset, sizeof(aci0));
    std::memcpy(&acid, npdm.data() + meta.acid_offset, sizeof(acid));
    const u64 aciKacOffset = static_cast<u64>(meta.aci0_offset) + aci0.kac_offset;
    const u64 acidKacOffset = static_cast<u64>(meta.acid_offset) + acid.kac_offset;
    if (aciKacOffset > npdm.size() || aci0.kac_size > npdm.size() - aciKacOffset ||
        acidKacOffset > npdm.size() || acid.kac_size > npdm.size() - acidKacOffset)
        return false;

    aci0.program_id = tid;
    acid.program_id_min = tid;
    acid.program_id_max = tid;

    // Atmosphere 1.8+ requires bit 19 in both KAC sections.
    u64 ver{};
    if (R_SUCCEEDED(splInitialize())) {
        splGetConfig((SplConfigItem)65000 /* ExosphereVersion */, &ver);
        splExit();
    }
    ver >>= 40;
    if (ver >= MAKEHOSVERSION(1, 8, 0)) {
        if (!npdm_patch_kc(npdm, static_cast<u32>(aciKacOffset), aci0.kac_size, 16, BIT(19)) ||
            !npdm_patch_kc(npdm, static_cast<u32>(acidKacOffset), acid.kac_size, 16, BIT(19)))
            return false;
    }

    std::memcpy(npdm.data(), &meta, sizeof(meta));
    std::memcpy(npdm.data() + meta.aci0_offset, &aci0, sizeof(aci0));
    std::memcpy(npdm.data() + meta.acid_offset, &acid, sizeof(acid));
    return true;
}

void patch_nacp(NacpStruct &nacp, const std::string &name, const std::string &author, u64 tid)
{
    for (auto &lang : nacp.lang) {
        if (!name.empty()) {
            std::memset(lang.name, 0, sizeof(lang.name));
            std::strncpy(lang.name, name.c_str(), sizeof(lang.name) - 1);
        }
        if (!author.empty()) {
            std::memset(lang.author, 0, sizeof(lang.author));
            std::strncpy(lang.author, author.c_str(), sizeof(lang.author) - 1);
        }
    }
    // The bundled default.nacp remains the source of truth for the launcher and
    // version metadata. Per-game shortcuts may override their displayed author.
    nacp.startup_user_account = 0x00;
    nacp.user_account_switch_lock = 0x00;
    nacp.add_on_content_registration_type = 0x01;
    nacp.screenshot = 0;
    nacp.video_capture = 0x2;
    nacp.logo_type = 0x2;
    nacp.logo_handling = 0x0;
    nacp.data_loss_confirmation = 0x0;
    nacp.required_network_service_license_on_launch = 0x0;
    nacp.application_error_code_category = 0;
    nacp.presence_group_id = tid;
    nacp.save_data_owner_id = tid;
    nacp.pseudo_device_id_seed = tid;
    nacp.add_on_content_base_id = tid ^ 0x1000;
    for (auto &id : nacp.local_communication_id)
        id = tid;
    nacp.play_log_policy = 0x0;
    nacp.play_log_query_capability = 0x0;
    nacp.user_account_save_data_size = 0x0;
    nacp.user_account_save_data_journal_size = 0x0;
    nacp.device_save_data_size = 0x0;
    nacp.device_save_data_journal_size = 0x0;
    nacp.user_account_save_data_size_max = 0x0;
    nacp.user_account_save_data_journal_size_max = 0x0;
    nacp.device_save_data_size_max = 0x0;
    nacp.device_save_data_journal_size_max = 0x0;
}

bool readFile(const std::string &path, std::vector<u8> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = fseek(f, 0, SEEK_END) == 0;
    const long sz = ok ? ftell(f) : -1;
    ok = sz > 0 && sz <= 64 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        out.resize(static_cast<size_t>(sz));
        ok = fread(out.data(), 1, out.size(), f) == out.size() && !ferror(f);
    }
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        out.clear();
    return ok;
}

bool makeNacpIcon(const std::string &src, std::vector<u8> &out)
{
    SDL_Surface *img = IMG_Load(src.c_str());
    if (!img) return false;
    SDL_Surface *rgb = SDL_CreateRGBSurfaceWithFormat(0, 256, 256, 24, SDL_PIXELFORMAT_RGB24);
    if (!rgb) { SDL_FreeSurface(img); return false; }
    SDL_FillRect(rgb, nullptr, 0);
    SDL_Rect d{0, 0, 256, 256};
    SDL_BlitScaled(img, nullptr, rgb, &d);
    SDL_FreeSurface(img);

    tjhandle tj = tjInitCompress();
    if (!tj) { SDL_FreeSurface(rgb); return false; }
    unsigned char *jpg = nullptr; unsigned long jpgSize = 0;
    int rc = tjCompress2(tj, (unsigned char *)rgb->pixels, 256, rgb->pitch, 256, TJPF_RGB,
                         &jpg, &jpgSize, TJSAMP_420, 90, 0);
    bool ok = (rc == 0) && jpgSize > 0 && jpgSize < 0x20000;
    if (ok) { out.assign(jpg, jpg + jpgSize); }
    if (jpg) tjFree(jpg);
    tjDestroy(tj);
    SDL_FreeSurface(rgb);
    return ok;
}

constexpr Result kForwarderIoError = MAKERESULT(299, 1);

enum class ForwarderStage {
    CryptoService,
    ContentService,
    ApplicationService,
    HeaderKey,
    Configuration,
    Permissions,
    Build,
    Content,
    Metadata,
    ConfigurationCommit,
    ApplicationRecord,
};

bool queryRegularFile(const std::string &path, bool &exists)
{
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        exists = true;
        return S_ISREG(st.st_mode);
    }
    exists = false;
    return errno == ENOENT;
}

bool validateForwarderConfig(const std::string &path)
{
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 3 || st.st_size > 2046)
        return false;
    std::vector<char> data(static_cast<size_t>(st.st_size));
    FILE *file = fopen(path.c_str(), "rb");
    if (!file)
        return false;
    bool read = fread(data.data(), 1, data.size(), file) == data.size() && !ferror(file);
    if (fclose(file) != 0)
        read = false;
    if (!read)
        return false;
    const auto first = std::find(data.begin(), data.end(), '\0');
    if (first == data.end() || first == data.begin() ||
        static_cast<size_t>(first - data.begin()) >= FS_MAX_PATH)
        return false;
    const auto second = std::find(first + 1, data.end(), '\0');
    return second != data.end() && second == data.end() - 1 && second != first + 1;
}

std::string resolveLauncherPath()
{
    std::string path = g_forwarderSelfPath;
    if (path.rfind("/switch/", 0) == 0)
        path.insert(0, "sdmc:");
    else if (path.rfind("switch/", 0) == 0)
        path.insert(0, "sdmc:/");
    if (path.rfind("sdmc:/", 0) != 0 || path.size() >= FS_MAX_PATH ||
        path.find_first_of("\"\r\n") != std::string::npos)
        return {};
    std::string extension = path.size() >= 4 ? path.substr(path.size() - 4) : std::string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool exists = false;
    if (extension == ".nro" && queryRegularFile(path, exists) && exists)
        return path;
    return {};
}

struct ForwarderConfigTransaction
{
    std::string path;
    std::string temp;
    std::string backup;
    bool hadPrevious{};
    bool installed{};
    bool completed{};

    ~ForwarderConfigTransaction()
    {
        if (!completed)
            rollback();
    }

    Result stage(u64 tid, const std::string &nroPath, const std::string &args)
    {
        struct stat directoryStat{};
        if (mkdir("sdmc:/switch/nethersx2/forwarders", 0777) != 0 &&
            (errno != EEXIST || stat("sdmc:/switch/nethersx2/forwarders", &directoryStat) != 0 ||
             !S_ISDIR(directoryStat.st_mode)))
            return kForwarderIoError;

        char pathBuffer[128];
        const int length = snprintf(pathBuffer, sizeof(pathBuffer),
            "sdmc:/switch/nethersx2/forwarders/%016llx.cfg", static_cast<unsigned long long>(tid));
        if (length < 0 || static_cast<size_t>(length) >= sizeof(pathBuffer))
            return kForwarderIoError;
        path = pathBuffer;
        temp = path + ".tmp";
        backup = path + ".old";

        bool currentExists = false, backupExists = false, tempExists = false;
        if (!queryRegularFile(path, currentExists) || !queryRegularFile(backup, backupExists) ||
            !queryRegularFile(temp, tempExists))
            return kForwarderIoError;
        if (!currentExists && backupExists) {
            if (!validateForwarderConfig(backup) || rename(backup.c_str(), path.c_str()) != 0)
                return kForwarderIoError;
            currentExists = true;
            backupExists = false;
            fsdevCommitDevice("sdmc");
        } else if (currentExists && backupExists) {
            const bool currentValid = validateForwarderConfig(path);
            const bool backupValid = validateForwarderConfig(backup);
            if (!currentValid && backupValid) {
                if (remove(path.c_str()) != 0 || rename(backup.c_str(), path.c_str()) != 0)
                    return kForwarderIoError;
            } else if (currentValid) {
                if (remove(backup.c_str()) != 0)
                    return kForwarderIoError;
            } else {
                return kForwarderIoError;
            }
            fsdevCommitDevice("sdmc");
        }
        if (tempExists && remove(temp.c_str()) != 0)
            return kForwarderIoError;
        hadPrevious = currentExists;
        if (hadPrevious && !validateForwarderConfig(path))
            return kForwarderIoError;

        FILE *file = fopen(temp.c_str(), "wb");
        if (!file)
            return kForwarderIoError;
        bool ok = fwrite(nroPath.c_str(), 1, nroPath.size() + 1, file) == nroPath.size() + 1 &&
                  fwrite(args.c_str(), 1, args.size() + 1, file) == args.size() + 1;
        if (fflush(file) != 0 || fsync(fileno(file)) != 0)
            ok = false;
        if (fclose(file) != 0)
            ok = false;
        if (!ok) {
            remove(temp.c_str());
            return kForwarderIoError;
        }
        if (!validateForwarderConfig(temp)) {
            remove(temp.c_str());
            return kForwarderIoError;
        }
        fsdevCommitDevice("sdmc");
        return 0;
    }

    Result commit()
    {
        if (!validateForwarderConfig(temp))
            return kForwarderIoError;
        bool backupExists = false;
        if (!queryRegularFile(backup, backupExists) ||
            (backupExists && remove(backup.c_str()) != 0))
            return kForwarderIoError;
        if (hadPrevious && rename(path.c_str(), backup.c_str()) != 0)
            return kForwarderIoError;
        if (rename(temp.c_str(), path.c_str()) != 0) {
            if (hadPrevious)
                rename(backup.c_str(), path.c_str());
            return kForwarderIoError;
        }
        installed = true;
        fsdevCommitDevice("sdmc");
        if (!validateForwarderConfig(path)) {
            rollback();
            return kForwarderIoError;
        }
        return 0;
    }

    void rollback()
    {
        remove(temp.c_str());
        if (!installed)
            return;
        remove(path.c_str());
        if (hadPrevious)
            rename(backup.c_str(), path.c_str());
        installed = false;
        fsdevCommitDevice("sdmc");
    }

    void finish()
    {
        remove(temp.c_str());
        remove(backup.c_str());
        completed = true;
        installed = false;
        fsdevCommitDevice("sdmc");
    }
};

struct ForwarderWriteItem
{
    const u8 *data;
    size_t size;
    const u8 *hash;
};

void removeRegisteredContent(NcmStorageId storageId, const std::vector<NcmContentId> &contentIds)
{
    if (contentIds.empty())
        return;
    NcmContentStorage storage{};
    if (R_FAILED(ncmOpenContentStorage(&storage, storageId)))
        return;
    for (const auto &contentId : contentIds)
        ncmContentStorageDelete(&storage, &contentId);
    ncmContentStorageClose(&storage);
}

struct RegisteredContentGuard
{
    NcmStorageId storageId;
    const std::vector<NcmContentId> &contentIds;
    bool active{true};

    ~RegisteredContentGuard()
    {
        if (active)
            removeRegisteredContent(storageId, contentIds);
    }

    void release() { active = false; }
};

Result registerContent(NcmStorageId storageId, const std::vector<ForwarderWriteItem> &items,
                       std::vector<NcmContentId> &createdContent)
{
    NcmContentStorage storage{};
    Result rc = ncmOpenContentStorage(&storage, storageId);
    if (R_FAILED(rc))
        return rc;

    for (const auto &item : items) {
        if (item.size > static_cast<size_t>(std::numeric_limits<s64>::max())) {
            rc = kForwarderIoError;
            break;
        }
        NcmContentId contentId{};
        std::memcpy(&contentId, item.hash, sizeof(contentId));
        bool alreadyPresent = false;
        rc = ncmContentStorageHas(&storage, &alreadyPresent, &contentId);
        if (R_FAILED(rc))
            break;
        if (alreadyPresent)
            continue;

        NcmPlaceHolderId placeholderId{};
        bool placeholderCreated = false;
        rc = ncmContentStorageGeneratePlaceHolderId(&storage, &placeholderId);
        if (R_SUCCEEDED(rc)) {
            ncmContentStorageDeletePlaceHolder(&storage, &placeholderId);
            rc = ncmContentStorageCreatePlaceHolder(&storage, &contentId, &placeholderId,
                                                     static_cast<s64>(item.size));
            placeholderCreated = R_SUCCEEDED(rc);
        }
        if (R_SUCCEEDED(rc))
            rc = ncmContentStorageWritePlaceHolder(&storage, &placeholderId, 0, item.data, item.size);
        if (R_SUCCEEDED(rc) && hosversionAtLeast(3, 0, 0))
            rc = ncmContentStorageFlushPlaceHolder(&storage);
        if (R_SUCCEEDED(rc))
            rc = ncmContentStorageRegister(&storage, &contentId, &placeholderId);
        if (R_FAILED(rc)) {
            if (placeholderCreated)
                ncmContentStorageDeletePlaceHolder(&storage, &placeholderId);
            break;
        }
        createdContent.push_back(contentId);
    }
    ncmContentStorageClose(&storage);
    if (R_FAILED(rc))
        removeRegisteredContent(storageId, createdContent);
    return rc;
}

struct ContentMetaBackup
{
    bool existed{};
    std::vector<u8> data;
    std::vector<NcmContentId> contentIds;
};

bool contentIdsEqual(const NcmContentId &left,const NcmContentId &right)
{
    return std::memcmp(&left,&right,sizeof(left))==0;
}

bool readForwarderConfig(const std::string &path,std::string &nroPath,std::string &args)
{
    if(!validateForwarderConfig(path))return false;
    struct stat st{};
    if(stat(path.c_str(),&st)!=0)return false;
    std::vector<char> data(static_cast<size_t>(st.st_size));
    FILE *file=fopen(path.c_str(),"rb");
    if(!file)return false;
    const bool ok=fread(data.data(),1,data.size(),file)==data.size()&&!ferror(file);
    if(fclose(file)!=0||!ok)return false;
    const auto first=std::find(data.begin(),data.end(),'\0');
    nroPath.assign(data.begin(),first);
    args.assign(first+1,data.end()-1);
    return true;
}

struct LegacyForwarderConfig { u64 tid{}; std::string path; };

std::vector<LegacyForwarderConfig> findLegacyForwarders(const std::string &gameKey,u64 stableTid)
{
    constexpr const char *directoryPath="sdmc:/switch/nethersx2/forwarders";
    std::vector<LegacyForwarderConfig> found;
    DIR *directory=opendir(directoryPath);
    if(!directory)return found;
    const std::string suffix=" -g \""+gameKey+"\"";
    struct dirent *entry=nullptr;
    while(found.size()<32&&(entry=readdir(directory))){
        const std::string filename=entry->d_name;
        if(filename.size()!=20||filename.compare(16,4,".cfg")!=0)continue;
        char *end=nullptr;
        const std::string stem=filename.substr(0,16);
        const unsigned long long parsed=strtoull(stem.c_str(),&end,16);
        if(!end||*end)continue;
        const u64 tid=static_cast<u64>(parsed);
        if(tid==stableTid||(tid&UINT64_C(0xff00000000000fff))!=UINT64_C(0x0500000000000000))continue;
        const std::string path=std::string(directoryPath)+"/"+filename;
        std::string oldNro,oldArgs;
        if(!readForwarderConfig(path,oldNro,oldArgs)||oldNro.empty()||oldArgs!=oldNro+suffix)continue;
        found.push_back({tid,path});
    }
    closedir(directory);
    return found;
}

void removeLegacyForwarders(const std::vector<LegacyForwarderConfig> &legacy)
{
    for(const auto &item:legacy){
        if(R_SUCCEEDED(nsDeleteApplicationCompletely(item.tid))){
            remove(item.path.c_str());
            remove((item.path+".old").c_str());
            remove((item.path+".tmp").c_str());
        }
    }
    if(!legacy.empty())fsdevCommitDevice("sdmc");
}

bool containsContentId(const std::vector<NcmContentId> &contentIds,const NcmContentId &contentId)
{
    return std::any_of(contentIds.begin(),contentIds.end(),[&](const auto &candidate){
        return contentIdsEqual(candidate,contentId);
    });
}

Result snapshotContentIds(NcmContentMetaDatabase &database,const NcmContentMetaKey &key,
                          const std::vector<u8> &metadata,std::vector<NcmContentId> &contentIds)
{
    if(metadata.size()<sizeof(NcmContentMetaHeader)) return kForwarderIoError;
    NcmContentMetaHeader header{};
    std::memcpy(&header,metadata.data(),sizeof(header));
    const size_t prefix=sizeof(header)+header.extended_header_size;
    if(prefix<sizeof(header)||prefix>metadata.size()||
       header.content_count>(metadata.size()-prefix)/sizeof(NcmContentInfo))
        return kForwarderIoError;
    contentIds.clear();
    if(!header.content_count) return 0;
    std::vector<NcmContentInfo> infos(header.content_count);
    s32 written=0;
    Result rc=ncmContentMetaDatabaseListContentInfo(&database,&written,infos.data(),
        static_cast<s32>(infos.size()),&key,0);
    if(R_FAILED(rc)) return rc;
    if(written!=static_cast<s32>(infos.size())) return kForwarderIoError;
    for(const auto &info:infos) contentIds.push_back(info.content_id);
    return 0;
}

Result snapshotContentMeta(NcmContentMetaDatabase &database,const NcmContentMetaKey &key,
                           ContentMetaBackup &backup)
{
    backup={};
    Result rc=ncmContentMetaDatabaseHas(&database,&backup.existed,&key);
    if(R_FAILED(rc)||!backup.existed) return rc;
    u64 size=0;
    rc=ncmContentMetaDatabaseGetSize(&database,&size,&key);
    if(R_SUCCEEDED(rc)&&(size==0||size>1024*1024)) rc=kForwarderIoError;
    if(R_FAILED(rc)) return rc;
    backup.data.resize(static_cast<size_t>(size));
    u64 received=0;
    rc=ncmContentMetaDatabaseGet(&database,&key,&received,backup.data.data(),backup.data.size());
    if(R_SUCCEEDED(rc)&&received!=backup.data.size()) rc=kForwarderIoError;
    if(R_SUCCEEDED(rc)) rc=snapshotContentIds(database,key,backup.data,backup.contentIds);
    return rc;
}

Result restoreContentMeta(NcmContentMetaDatabase &database, const NcmContentMetaKey &key,
                          const ContentMetaBackup &backup)
{
    Result rc = backup.existed
        ? ncmContentMetaDatabaseSet(&database, &key, backup.data.data(), backup.data.size())
        : ncmContentMetaDatabaseRemove(&database, &key);
    if (R_SUCCEEDED(rc))
        rc = ncmContentMetaDatabaseCommit(&database);
    return rc;
}

Result installContentMeta(NcmStorageId storageId, const MetaResult &meta,
                          ContentMetaBackup &backup, bool &safeToRemoveContent)
{
    if(meta.data.empty()) return kForwarderIoError;
    safeToRemoveContent = true;
    NcmContentMetaDatabase database{};
    Result rc = ncmOpenContentMetaDatabase(&database, storageId);
    if (R_FAILED(rc))
        return rc;

    bool snapshotReady = false;
    bool writeAttempted = false;
    rc = snapshotContentMeta(database,meta.key,backup);
    if (R_SUCCEEDED(rc)) {
        snapshotReady = true;
        writeAttempted = true;
        rc = ncmContentMetaDatabaseSet(&database, &meta.key, meta.data.data(), meta.data.size());
    }
    if (R_SUCCEEDED(rc))
        rc = ncmContentMetaDatabaseCommit(&database);
    if (R_FAILED(rc) && snapshotReady && writeAttempted &&
        R_FAILED(restoreContentMeta(database, meta.key, backup)))
        safeToRemoveContent = false;
    ncmContentMetaDatabaseClose(&database);
    return rc;
}

std::vector<NcmContentId> findReplacedContent(const std::vector<NcmContentId> &previous,
                                              const std::vector<NcmContentId> &current)
{
    std::vector<NcmContentId> replaced;
    for(const auto &contentId:previous)
        if(!containsContentId(current,contentId)&&!containsContentId(replaced,contentId))
            replaced.push_back(contentId);
    return replaced;
}

void removeOrphanedContent(NcmStorageId storageId,const std::vector<NcmContentId> &contentIds)
{
    if(contentIds.empty()) return;
    NcmContentMetaDatabase database{};
    if(R_FAILED(ncmOpenContentMetaDatabase(&database,storageId))) return;
    NcmContentStorage storage{};
    if(R_FAILED(ncmOpenContentStorage(&storage,storageId))){
        ncmContentMetaDatabaseClose(&database); return;
    }
    for(const auto &contentId:contentIds){
        bool orphaned=false;
        if(R_FAILED(ncmContentMetaDatabaseLookupOrphanContent(&database,&orphaned,&contentId,1))) break;
        if(!orphaned) continue;
        bool present=false;
        if(R_FAILED(ncmContentStorageHas(&storage,&present,&contentId))) break;
        if(present&&R_FAILED(ncmContentStorageDelete(&storage,&contentId))) break;
    }
    ncmContentStorageClose(&storage);
    ncmContentMetaDatabaseClose(&database);
}

bool rollbackContentMeta(NcmStorageId storageId, const NcmContentMetaKey &key,
                         const ContentMetaBackup &backup)
{
    NcmContentMetaDatabase database{};
    if (R_FAILED(ncmOpenContentMetaDatabase(&database, storageId)))
        return false;
    const Result rc = restoreContentMeta(database, key, backup);
    ncmContentMetaDatabaseClose(&database);
    return R_SUCCEEDED(rc);
}

Result install_forwarder(const std::string &gameKey, const std::string &name,
                         const std::string &author,
                         bool launcherOnly,
                         const std::vector<u8> &nsoData, std::vector<u8> npdmData, NacpStruct nacp,
                         const std::vector<u8> &iconJpeg, ForwarderStage &stage)
{
    constexpr NcmStorageId storageId = NcmStorageId_SdCard;
    u8 headerKey[0x20];
    stage = ForwarderStage::HeaderKey;
    FWD_TRY(derive_header_key(headerKey));

    stage = ForwarderStage::Configuration;
    const std::string nroPath = resolveLauncherPath();
    if (nroPath.empty())
        return kForwarderIoError;
    const bool quotePath = nroPath.find_first_of(" \t") != std::string::npos;
    const std::string launcherArgument=quotePath ? "\""+nroPath+"\"" : nroPath;
    const std::string args = launcherOnly ? launcherArgument :
                             launcherArgument+" -g \""+gameKey+"\"";
    if (nroPath.empty() || nroPath.size() >= FS_MAX_PATH ||
        nroPath.size() + args.size() + 2 > 2046)
        return kForwarderIoError;

    u64 hashData[SHA256_HASH_SIZE / sizeof(u64)];
    const std::string hashSource = launcherOnly ? "nethersx2-launcher" :
                                                  "nethersx2-forwarder:"+gameKey;
    sha256CalculateHash(hashData, hashSource.data(), hashSource.length());
    const u64 tid = 0x0500000000000000 | (hashData[0] & 0x00FFFFFFFFFFF000);
    const std::vector<LegacyForwarderConfig> legacyForwarders=
        launcherOnly?std::vector<LegacyForwarderConfig>{}:findLegacyForwarders(gameKey,tid);

    ForwarderConfigTransaction config;
    FWD_TRY(config.stage(tid, nroPath, args));

    stage = ForwarderStage::Permissions;
    if (!patch_npdm(npdmData, tid)) {
        config.rollback();
        return kForwarderIoError;
    }
    stage = ForwarderStage::Build;
    std::vector<NcaEntry> ncaEntries;
    {
        FileEntries exefs;
        add_file_entry(exefs, "main", nsoData.data(), nsoData.size());
        add_file_entry(exefs, "main.npdm", npdmData.data(), npdmData.size());
        ncaEntries.emplace_back(create_program_nca(tid, headerKey, exefs));
    }
    {
        patch_nacp(nacp, name, author, tid);
        FileEntries romfs;
        add_file_entry(romfs, "/control.nacp", &nacp, sizeof(nacp));
        add_file_entry(romfs, "/icon_AmericanEnglish.dat", iconJpeg.data(), iconJpeg.size());
        ncaEntries.emplace_back(create_control_nca(tid, headerKey, romfs));
    }
    const auto meta = create_meta_nca(tid, headerKey, storageId, ncaEntries);

    std::vector<ForwarderWriteItem> writeItems;
    for (const auto &entry : ncaEntries)
        writeItems.push_back({entry.data.data(), entry.data.size(), entry.hash});
    writeItems.push_back({meta.nca.data(), meta.nca.size(), meta.hash});

    std::vector<NcmContentId> createdContent;
    stage = ForwarderStage::Content;
    Result rc = registerContent(storageId, writeItems, createdContent);
    if (R_FAILED(rc)) {
        config.rollback();
        return rc;
    }
    RegisteredContentGuard contentGuard{storageId, createdContent};

    ContentMetaBackup metaBackup;
    bool safeToRemoveContent = true;
    stage = ForwarderStage::Metadata;
    rc = installContentMeta(storageId, meta, metaBackup, safeToRemoveContent);
    if (R_FAILED(rc)) {
        if (!safeToRemoveContent)
            contentGuard.release();
        config.rollback();
        return rc;
    }
    contentGuard.release();

    stage = ForwarderStage::ConfigurationCommit;
    rc = config.commit();
    if (R_FAILED(rc)) {
        const bool restored = rollbackContentMeta(storageId, meta.key, metaBackup);
        if (restored)
            removeRegisteredContent(storageId, createdContent);
        config.rollback();
        return rc;
    }

    stage = ForwarderStage::ApplicationRecord;
    nsDeleteApplicationEntity(tid);
    rc = ns_push_application_record(tid, &meta.record, 1);
    if (R_FAILED(rc)) {
        config.rollback();
        const bool restored = rollbackContentMeta(storageId, meta.key, metaBackup);
        nsDeleteApplicationEntity(tid);
        if (restored && metaBackup.existed) {
            FwdContentStorageRecord oldRecord{meta.key, static_cast<u8>(storageId), {0}};
            ns_push_application_record(tid, &oldRecord, 1);
        }
        if (restored)
            removeRegisteredContent(storageId, createdContent);
        return rc;
    }

    ns_invalidate_control_cache(tid);
    config.finish();
    removeOrphanedContent(storageId,findReplacedContent(metaBackup.contentIds,meta.contentIds));
    removeLegacyForwarders(legacyForwarders);
    return 0;
}

const char *forwarderStageError(ForwarderStage stage)
{
    switch (stage) {
    case ForwarderStage::CryptoService: return "Could not initialize the console crypto service";
    case ForwarderStage::ContentService: return "Could not initialize SD content services";
    case ForwarderStage::ApplicationService: return "Could not initialize HOME Menu application services";
    case ForwarderStage::HeaderKey: return "Could not derive the forwarder header key";
    case ForwarderStage::Configuration: return "Could not stage the shortcut launch configuration";
    case ForwarderStage::Permissions: return "Could not patch the forwarder's required permissions";
    case ForwarderStage::Build: return "Could not build the shortcut content";
    case ForwarderStage::Content: return "Could not write shortcut content to the SD card";
    case ForwarderStage::Metadata: return "Could not register shortcut metadata";
    case ForwarderStage::ConfigurationCommit: return "Could not commit the shortcut launch configuration";
    case ForwarderStage::ApplicationRecord: return "Could not register the shortcut with HOME Menu";
    }
    return "Shortcut installation failed";
}

} // namespace

std::string launcherNroPath()
{
    return resolveLauncherPath();
}

static bool forwarder_create_common(const std::string &gameKey, const std::string &name,
                                    const std::string &author,const std::string &iconImgPath,
                                    bool launcherOnly,
                                    char *err, std::size_t errSize)
{
    if (err && errSize) err[0] = '\0';

    const bool validKey = !gameKey.empty() && gameKey.size() <= 255 &&
        std::all_of(gameKey.begin(), gameKey.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        });
    if (!validKey) {
        if (err && errSize) snprintf(err, errSize, "The game key is not safe for a shortcut.");
        return false;
    }

	std::vector<u8> nso, npdm, nacpRaw, iconJpeg;
	if (!readFile("romfs:/fwd/hbl.nso", nso) || !readFile("romfs:/fwd/hbl.npdm", npdm) ||
        !readFile("romfs:/fwd/default.nacp", nacpRaw) || nacpRaw.size() < sizeof(NacpStruct)) {
        if (err && errSize) snprintf(err, errSize, "Forwarder assets missing.");
        return false;
    }
    if (!makeNacpIcon(iconImgPath, iconJpeg)) {
        if (err && errSize) snprintf(err, errSize, "Failed to convert icon.");
        return false;
    }

    NacpStruct nacp;
    std::memcpy(&nacp, nacpRaw.data(), sizeof(nacp));

    bool cryptoInitialized = false;
    bool ncmInitialized = false;
    ForwarderStage stage = ForwarderStage::CryptoService;
    Result rc = splCryptoInitialize();
    if (R_SUCCEEDED(rc)) {
        cryptoInitialized = true;
        stage = ForwarderStage::ContentService;
        rc = ncmInitialize();
    }
    if (R_SUCCEEDED(rc)) {
        ncmInitialized = true;
        stage = ForwarderStage::ApplicationService;
        rc = ns_app_init();
    }
    if (R_SUCCEEDED(rc)) {
        try {
            rc = install_forwarder(gameKey, name.empty() ? "NetherSX2" : name,author,launcherOnly,
                                   nso, npdm, nacp, iconJpeg, stage);
        } catch (...) {
            rc = kForwarderIoError;
        }
    }

    ns_app_exit();
    if (ncmInitialized)
        ncmExit();
    if (cryptoInitialized)
        splCryptoExit();

    if (R_FAILED(rc)) {
        if (err && errSize) {
            const char *hint = (stage == ForwarderStage::Content || stage == ForwarderStage::Metadata)
                ? ". Check SD space and CFW signature patches." : ".";
            snprintf(err, errSize, "%s (0x%08X)%s", forwarderStageError(stage), rc, hint);
        }
        return false;
    }
    return true;
}

bool forwarder_create(const std::string &gameKey, const std::string &name, const std::string &author,
                      const std::string &iconImgPath, char *err, std::size_t errSize)
{
    return forwarder_create_common(gameKey,name,author.empty()?"NetherSX2":author,
                                   iconImgPath,false,err,errSize);
}

bool forwarder_create_launcher(char *err,std::size_t errSize)
{
    return forwarder_create_common("launcher","NetherSX2","","romfs:/logo.png",true,err,errSize);
}
