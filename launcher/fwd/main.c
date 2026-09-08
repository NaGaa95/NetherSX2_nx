// Derived from nx-hbloader; see LICENSE.md.
#include <switch.h>
#include <string.h>

#define EXIT_DETECTION_STR "__NETHERSX2_FORWARDER_EXIT__"
#ifndef FWD_CFG_DIR
#define FWD_CFG_DIR "/switch/nethersx2/forwarders/"
#endif
_Static_assert(sizeof(FWD_CFG_DIR) + 20 <= 64, "forwarder config path is too long");

static char g_argv[2048] = {0};
static char g_nextArgv[2048] = {0};
static char g_nextNroPath[FS_MAX_PATH] = {0};
static char g_defaultArgv[2048] = {0};
static char g_defaultNroPath[FS_MAX_PATH] = {0};

static const char g_noticeText[] = { "NetherSX2 " VERSION };

static u64 g_nroSize = 0;
static NroHeader g_nroHeader = {0};

static enum {
    CodeMemoryUnavailable    = 0,
    CodeMemoryForeignProcess = BIT(0),
    CodeMemorySameProcess    = BIT(0) | BIT(1),
} g_codeMemoryCapability = CodeMemoryUnavailable;

static void*  g_heapAddr = {0};
static size_t g_heapSize = {0};

static Handle g_procHandle = {0};
static u128 g_userIdStorage = {0};
static u8 g_savedTls[0x100] = {0};

// Used by trampoline.s
u64 g_nroAddr = 0;
Result g_lastRet = 0;

void NX_NORETURN nroEntrypointTrampoline(const ConfigEntry* entries, u64 handle, u64 entrypoint);

// hbloader starts the NRO on the main thread's inherited affinity, which can be
// narrower than the NPDM grant. Widen it so the emulator can use core 3.
static void restoreMainThreadAffinity(void) {
    u64 core_mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, core_mask);
}

static void fix_nro_path(char* path) {
    if (!strncmp(path, "sdmc:/", 6)) {
        memmove(path, path + 5, strlen(path + 5) + 1);
    }
}

static void NX_NORETURN selfExit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc))
        goto fail0;

    Service applet, proxy, self;

    rc = smGetService(&applet, "appletOE");
    if (R_FAILED(rc))
        goto fail1;

    const u32 cmd_id = 0;
    const u64 reserved = 0;

    rc = serviceDispatchIn(&applet, cmd_id, reserved,
        .in_send_pid = true,
        .in_num_handles = 1,
        .in_handles = { g_procHandle },
        .out_num_objects = 1,
        .out_objects = &proxy,
    );
    if (R_FAILED(rc))
        goto fail2;

    rc = serviceDispatch(&proxy, 1,
        .out_num_objects = 1,
        .out_objects = &self,
    );
    if (R_FAILED(rc))
        goto fail3;

    rc = serviceDispatch(&self, 0);

    serviceClose(&self);

fail3:
    serviceClose(&proxy);

fail2:
    serviceClose(&applet);

fail1:
    smExit();

fail0:
    if (R_SUCCEEDED(rc)) {
        while(1) svcSleepThread(86400000000000ULL);
        svcExitProcess();
        __builtin_unreachable();
    } else {
        diagAbortWithResult(rc);
    }
}

static u64 calculateMaxHeapSize(void) {
    u64 size = 0;
    u64 mem_available = 0, mem_used = 0;

    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

    if (mem_available > mem_used+0x200000)
        size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
        size = 0x2000000*16;
    if (size > 0x6000000)
        size -= 0x6000000;

    return size;
}

static void setupHbHeap(void) {
    void* addr = NULL;
    u64 size = calculateMaxHeapSize();
    Result rc = svcSetHeapSize(&addr, size);

    if (R_FAILED(rc) || addr==NULL)
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 9));

    g_heapAddr = addr;
    g_heapSize = size;
}

static void procHandleReceiveThread(void* arg) {
    Handle session = (Handle)(uintptr_t)arg;
    Result rc;

    void* base = armGetTls();
    hipcMakeRequestInline(base);

    s32 idx = 0;
    rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 15));

    HipcParsedRequest r = hipcParseRequest(base);
    if (r.meta.num_copy_handles != 1)
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 17));

    g_procHandle = r.data.copy_handles[0];
    svcCloseHandle(session);
}

static void getOwnProcessHandle(void) {
    Result rc;

    Handle server_handle, client_handle;
    rc = svcCreateSession(&server_handle, &client_handle, 0, 0);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 12));

    Thread t;
    u8* stack = g_heapAddr;
    rc = threadCreate(&t, &procHandleReceiveThread, (void*)(uintptr_t)server_handle, stack, 0x1000, 0x20, 0);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 10));

    rc = threadStart(&t);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 13));

    hipcMakeRequestInline(armGetTls(),
        .num_copy_handles = 1,
    ).copy_handles[0] = CUR_PROCESS_HANDLE;

    // The server closes after receiving the copied handle, so no reply is expected.
    svcSendSyncRequest(client_handle);
    svcCloseHandle(client_handle);

    threadWaitForExit(&t);
    threadClose(&t);
}

static bool isKernel5xOrLater(void) {
    u64 dummy = 0;
    Result rc = svcGetInfo(&dummy, InfoType_UserExceptionContextAddress, INVALID_HANDLE, 0);
    return R_VALUE(rc) != KERNELRESULT(InvalidEnumValue);
}

static bool isKernel4x(void) {
    u64 dummy = 0;
    Result rc = svcGetInfo(&dummy, InfoType_InitialProcessIdRange, INVALID_HANDLE, 0);
    return R_VALUE(rc) != KERNELRESULT(InvalidEnumValue);
}

static void getCodeMemoryCapability(void) {
    if (detectMesosphere()) {
        g_codeMemoryCapability = CodeMemorySameProcess;
    } else if (isKernel5xOrLater()) {
        // An invalid operation distinguishes patched same-process support.
        Handle code;
        Result rc = svcCreateCodeMemory(&code, g_heapAddr, 0x1000);
        if (R_SUCCEEDED(rc)) {
            rc = svcControlCodeMemory(code, (CodeMapOperation)-1, 0, 0x1000, 0);
            svcCloseHandle(code);

            if (R_VALUE(rc) == KERNELRESULT(InvalidEnumValue))
                g_codeMemoryCapability = CodeMemorySameProcess;
            else
                g_codeMemoryCapability = CodeMemoryForeignProcess;
        }
    } else if (isKernel4x()) {
        g_codeMemoryCapability = CodeMemorySameProcess;
    } else {
        g_codeMemoryCapability = CodeMemoryUnavailable;
    }
}

void NX_NORETURN loadNro(void) {
    NroHeader* header = NULL;
    size_t rw_size = 0;
    Result rc = 0;

    memcpy((u8*)armGetTls() + 0x100, g_savedTls, 0x100);

    if (!strcmp(g_nextArgv, EXIT_DETECTION_STR)) {
        if (!strcmp(g_nextNroPath, g_defaultNroPath)) {
            selfExit();
        } else {
            strcpy(g_nextNroPath, g_defaultNroPath);
            strcpy(g_nextArgv, g_defaultArgv);
        }
    }

    if (g_nroSize) {
        header = &g_nroHeader;
        rw_size = header->segments[2].size + header->bss_size;
        rw_size = (rw_size+0xFFF) & ~0xFFF;

        if (R_FAILED(rc = svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PreUnloadDll, g_nroAddr, g_nroSize))) {
            diagAbortWithResult(rc);
        }

        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[0].file_off, ((u64) g_heapAddr) + header->segments[0].file_off, header->segments[0].size);

        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 24));

        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[1].file_off, ((u64) g_heapAddr) + header->segments[1].file_off, header->segments[1].size);

        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 25));

        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[2].file_off, ((u64) g_heapAddr) + header->segments[2].file_off, rw_size);

        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 26));

        if (R_FAILED(rc = svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PostUnloadDll, g_nroAddr, g_nroSize))) {
            diagAbortWithResult(rc);
        }

        g_nroAddr = g_nroSize = 0;
    } else {
        // The program ID selects its launcher configuration.
        u64 program_id = 0;
        if (R_FAILED(rc = svcGetInfo(&program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)))
            diagAbortWithResult(rc);

        char cfg_path[64];
        {
            static const char pre[] = FWD_CFG_DIR;
            static const char hexd[] = "0123456789abcdef";
            int p = 0;
            for (const char* c = pre; *c; c++) cfg_path[p++] = *c;
            for (int i = 60; i >= 0; i -= 4) cfg_path[p++] = hexd[(program_id >> i) & 0xF];
            cfg_path[p++] = '.'; cfg_path[p++] = 'c'; cfg_path[p++] = 'f'; cfg_path[p++] = 'g';
            cfg_path[p] = 0;
        }

        char cfg[2048];
        u64 br = 0;
        int ok = 0;
        FsFileSystem sd;
        if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
            FsFile f;
            if (R_SUCCEEDED(fsFsOpenFile(&sd, cfg_path, FsOpenMode_Read, &f))) {
                if (R_SUCCEEDED(fsFileRead(&f, 0, cfg, sizeof(cfg) - 2, FsReadOption_None, &br))) {
                    cfg[br] = 0; cfg[br + 1] = 0;
                    ok = 1;
                }
                fsFileClose(&f);
            }
            fsFsClose(&sd);
        }

        if (ok) {
            size_t path_len = strnlen(cfg, br);
            size_t arg_space = path_len < br ? (size_t)br - path_len - 1 : 0;
            const char* args = cfg + path_len + 1;
            size_t arg_len = arg_space ? strnlen(args, arg_space) : 0;
            ok = path_len > 0 && path_len < br && path_len < sizeof(g_nextNroPath) &&
                 arg_space > 0 && arg_len < arg_space && arg_len < sizeof(g_nextArgv);
            if (ok) {
                memcpy(g_nextNroPath, cfg, path_len + 1);
                memcpy(g_nextArgv, args, arg_len + 1);
            }
        }
        if (!ok) {
            strcpy(g_nextNroPath, "sdmc:/hbmenu.nro");
            strcpy(g_nextArgv, "sdmc:/hbmenu.nro");
        }

        strcpy(g_defaultNroPath, g_nextNroPath);
        strcpy(g_defaultArgv, g_nextArgv);
    }

    {
        char fixedNextNroPath[FS_MAX_PATH];
        strcpy(fixedNextNroPath, g_nextNroPath);
        fix_nro_path(fixedNextNroPath);

        memcpy(g_argv, g_nextArgv, sizeof(g_argv));
        if (R_FAILED(rc = svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PreLoadDll, (uintptr_t)g_argv, sizeof(g_argv)))) {
            diagAbortWithResult(rc);
        }

        uint8_t *nrobuf = (uint8_t*) g_heapAddr;
        NroStart*  start  = (NroStart*)  (nrobuf + 0);
        header = (NroHeader*) (nrobuf + sizeof(NroStart));

        FsFileSystem fs;
        if (R_FAILED(rc = fsOpenSdCardFileSystem(&fs))) {
            diagAbortWithResult(rc);
        }

        FsFile f;
        if (R_FAILED(rc = fsFsOpenFile(&fs, fixedNextNroPath, FsOpenMode_Read, &f))) {
            diagAbortWithResult(rc);
        }

        u64 bytes_read = 0;
        rc = fsFileRead(&f, 0, start, g_heapSize, FsReadOption_None, &bytes_read);
        fsFileClose(&f);
        fsFsClose(&fs);
        if (R_FAILED(rc))
            diagAbortWithResult(rc);
        if (bytes_read < sizeof(*start) + sizeof(*header) ||
            header->magic != NROHEADER_MAGIC ||
            header->size < sizeof(*start) + sizeof(*header) || header->size > bytes_read)
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 6));
    }

    for (int i = 0; i < 3; i++) {
        if (header->segments[i].file_off > header->size ||
            header->segments[i].size > header->size - header->segments[i].file_off)
        {
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 6));
        }
    }

    u64 rw_unaligned = (u64)header->segments[2].size + header->bss_size;
    u64 total_unaligned = (u64)header->size + header->bss_size;
    if (rw_unaligned > UINT64_MAX - 0xFFF || total_unaligned > UINT64_MAX - 0xFFF)
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 6));
    rw_size = (size_t)((rw_unaligned + 0xFFF) & ~0xFFFULL);
    const size_t total_size = (size_t)((total_unaligned + 0xFFF) & ~0xFFFULL);
    const u64 bss_offset = (u64)header->segments[2].file_off + header->segments[2].size;
    if (total_size > g_heapSize || bss_offset > total_size ||
        header->bss_size > total_size - bss_offset)
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 6));

    memset((u8*)g_heapAddr + bss_offset, 0, header->bss_size);

    // Copy header to elsewhere because we're going to unmap it next.
    memcpy(&g_nroHeader, header, sizeof(g_nroHeader));
    header = &g_nroHeader;

    virtmemLock();
    void* map_addr = virtmemFindCodeMemory(total_size, 0);
    if (!map_addr) {
        virtmemUnlock();
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 18));
    }
    rc = svcMapProcessCodeMemory(g_procHandle, (u64)map_addr, (u64)g_heapAddr, total_size);
    virtmemUnlock();

    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 18));

    rc = svcSetProcessMemoryPermission(
        g_procHandle, (u64)map_addr + header->segments[0].file_off, header->segments[0].size, Perm_R | Perm_X);

    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    rc = svcSetProcessMemoryPermission(
        g_procHandle, (u64)map_addr + header->segments[1].file_off, header->segments[1].size, Perm_R);

    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    rc = svcSetProcessMemoryPermission(
        g_procHandle, (u64)map_addr + header->segments[2].file_off, rw_size, Perm_Rw);

    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    const u64 nro_size = header->segments[2].file_off + rw_size;
    const u64 nro_heap_start = ((u64) g_heapAddr) + nro_size;
    const u64 nro_heap_size  = g_heapSize + (u64) g_heapAddr - (u64) nro_heap_start;
    #define M EntryFlag_IsMandatory

    static ConfigEntry entries[] = {
        { EntryType_MainThreadHandle,     0, {0, 0} },
        { EntryType_ProcessHandle,        0, {0, 0} },
        { EntryType_AppletType,           0, {AppletType_SystemApplication, EnvAppletFlags_ApplicationOverride} },
        { EntryType_OverrideHeap,         M, {0, 0} },
        { EntryType_Argv,                 0, {0, 0} },
        { EntryType_NextLoadPath,         0, {0, 0} },
        { EntryType_LastLoadResult,       0, {0, 0} },
        { EntryType_SyscallAvailableHint, 0, {UINT64_MAX, UINT64_MAX} },
        { EntryType_SyscallAvailableHint2, 0, {UINT64_MAX, 0} },
        { EntryType_RandomSeed,           0, {0, 0} },
        { EntryType_UserIdStorage,        0, {(u64)(uintptr_t)&g_userIdStorage, 0} },
        { EntryType_HosVersion,           0, {0, 0} },
        { EntryType_EndOfList,            0, {(u64)(uintptr_t)g_noticeText, sizeof(g_noticeText)} }
    };

    ConfigEntry *entry_Syscalls = &entries[7];

    if (!(g_codeMemoryCapability & BIT(0))) {
        entry_Syscalls->Value[0x4B/64] &= ~(1UL << (0x4B%64));
    }

    if (!(g_codeMemoryCapability & BIT(1))) {
        entry_Syscalls->Value[0x4C/64] &= ~(1UL << (0x4C%64));
    }

    entries[0].Value[0] = envGetMainThreadHandle();
    entries[1].Value[0] = g_procHandle;
    entries[3].Value[0] = nro_heap_start;
    entries[3].Value[1] = nro_heap_size;
    entries[4].Value[1] = (u64)(uintptr_t)&g_argv[0];
    entries[5].Value[0] = (u64)(uintptr_t)&g_nextNroPath[0];
    entries[5].Value[1] = (u64)(uintptr_t)&g_nextArgv[0];
    entries[6].Value[0] = g_lastRet;
    entries[9].Value[0] = randomGet64();
    entries[9].Value[1] = randomGet64();
    entries[11].Value[0] = hosversionGet();
    entries[11].Value[1] = hosversionIsAtmosphere() ? 0x41544d4f53504852UL : 0; // 'ATMOSPHR'

    g_nroAddr = (u64)map_addr;
    g_nroSize = nro_size;

    if (R_FAILED(rc = svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PostLoadDll, g_nroAddr, nro_size)))
        diagAbortWithResult(rc);

    strcpy(g_nextArgv, EXIT_DETECTION_STR);
    restoreMainThreadAffinity();
    nroEntrypointTrampoline(&entries[0], -1, g_nroAddr);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    memcpy(g_savedTls, (const u8*)armGetTls() + 0x100, 0x100);
    setupHbHeap();
    getOwnProcessHandle();
    getCodeMemoryCapability();
    loadNro();
}

u32 __nx_applet_type = AppletType_Application;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    fake_heap_start = NULL;
    fake_heap_end   = NULL;
}

void __appInit(void) {
    Result rc;

    // Atmosphère is encoded in the version passed to the loaded NRO.
    Handle dummy;
    rc = svcConnectToNamedPort(&dummy, "ams");
    u32 ams_flag = (R_VALUE(rc) != KERNELRESULT(NotFound)) ? BIT(31) : 0;
    if (R_SUCCEEDED(rc))
        svcCloseHandle(dummy);

    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(ams_flag | MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, LibnxError_InitFail_FS));

    smExit();
}

void __appExit(void) {}

// Abort unexpected libc exit paths.
void __wrap_exit(void) {
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 39));
}

void* __libnx_alloc(size_t size) {
    (void)size;
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 40));
}

void* __libnx_aligned_alloc(size_t alignment, size_t size) {
    (void)alignment;
    (void)size;
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 41));
}

void __libnx_free(void* p) {
    (void)p;
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 43));
}
