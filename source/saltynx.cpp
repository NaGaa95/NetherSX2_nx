#include "saltynx.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace {
constexpr uint32_t magic = 0x465053;
constexpr size_t page_size = 0x1000;
constexpr u64 allocation_size = 310;

struct ResolutionCalls {
    uint16_t width, height, calls;
} __attribute__((packed));

struct FpsBlock {
    uint32_t magic;
    uint8_t fps;
    float average;
    bool active;
    uint8_t locked, mode, zero_sync, patch, api;
    uint32_t ticks[10];
    uint8_t buffers, set_buffers, active_buffers, set_active_buffers, display_sync;
    ResolutionCalls render[8], viewport[8];
    bool original_refresh, no_docked_60, suspend;
    uint8_t refresh;
    float read_speed;
    uint8_t locked_docked;
    uint64_t frame_number;
    int8_t expected_buffers;
} __attribute__((packed));

static_assert(sizeof(FpsBlock) == 174);
static_assert(offsetof(FpsBlock, api) == 14);
static_assert(offsetof(FpsBlock, ticks) == 15);
static_assert(offsetof(FpsBlock, render) == 60);
static_assert(offsetof(FpsBlock, viewport) == 108);
static_assert(offsetof(FpsBlock, frame_number) == 165);

Thread publisher{};
UEvent wake{};
bool running = false;
std::atomic<bool> stopping{false};
std::atomic<uint64_t> frame_count{0}, previous_tick{0};
std::atomic<uint32_t> resolution{0};
std::atomic<uint8_t> graphics_api{0};
std::array<std::atomic<uint32_t>, 10> frame_ticks{};
SharedMemory shared{};
uint8_t* page = nullptr;
volatile FpsBlock* block = nullptr;

volatile FpsBlock* find_block() {
    if (!page)
        return nullptr;
    for (size_t offset = 0; offset + sizeof(FpsBlock) <= page_size; offset += 4) {
        uint32_t value;
        std::memcpy(&value, page + offset, sizeof(value));
        if (value == magic)
            return reinterpret_cast<volatile FpsBlock*>(page + offset);
    }
    return nullptr;
}

bool make_session_room(bool early) {
    u64 raw = 0;
    if (R_FAILED(svcGetInfo(&raw, InfoType_ResourceLimit, INVALID_HANDLE, 0)))
        return false;
    const Handle limit = Handle(raw);
    s64 current = 0, maximum = 0;
    bool room = R_FAILED(svcGetResourceLimitCurrentValue(&current, limit, LimitableResource_Sessions))
        || R_FAILED(svcGetResourceLimitLimitValue(&maximum, limit, LimitableResource_Sessions))
        || current < maximum;
    if (!room && envIsSyscallHinted(0x7E))
        room = R_SUCCEEDED(svcSetResourceLimitLimitValue(limit, LimitableResource_Sessions, maximum + 4));
    svcCloseHandle(limit);
    return !room && early;
}

void attach(bool early = false) {
    if ((block = find_block()))
        return;
    const bool release_sm = make_session_room(early);
    if (release_sm)
        smExit();
    for (const char* name : {"SaltySD", "InjectServ"}) {
        Handle handle = INVALID_HANDLE;
        if (R_FAILED(svcConnectToNamedPort(&handle, name)))
            continue;
        Service service{};
        service.session = handle;
        if (!page) {
            Handle memory = INVALID_HANDLE;
            const Result result = serviceDispatch(&service, 7, .in_send_pid = true,
                .out_handle_attrs = {SfOutHandleAttr_HipcCopy}, .out_handles = &memory);
            if (R_SUCCEEDED(result)) {
                shmemLoadRemote(&shared, memory, page_size, Perm_Rw);
                if (R_SUCCEEDED(shmemMap(&shared)))
                    page = static_cast<uint8_t*>(shmemGetAddr(&shared));
                else
                    shmemClose(&shared);
            }
        }
        block = find_block();
        if (page && !block) {
            u64 offset = 0;
            if (R_SUCCEEDED(serviceDispatchInOut(&service, 6, allocation_size, offset, .in_send_pid = true))
                && offset <= page_size - allocation_size && offset % 4 == 0) {
                std::memset(page + offset, 0, allocation_size);
                block = reinterpret_cast<volatile FpsBlock*>(page + offset);
                block->magic = magic;
            }
        }
        const u64 zero = 0;
        serviceDispatchIn(&service, 0, zero, .in_send_pid = true);
        svcCloseHandle(handle);
        if (block)
            break;
    }
    if (release_sm)
        smInitialize();
}

void publish() {
    if (!block || block->magic != magic)
        return;
    const uint64_t frames = frame_count.load(std::memory_order_acquire);
    const uint64_t last = previous_tick.load(std::memory_order_relaxed);
    const uint64_t frequency = armGetSystemTickFreq();
    const bool recent = last && armGetSystemTick() - last < frequency;
    uint64_t sum = 0;
    unsigned count = 0;
    for (const auto& sample : frame_ticks) {
        const auto ticks = sample.load(std::memory_order_relaxed);
        if (ticks) {
            sum += ticks;
            ++count;
        }
    }
    const float average = recent && sum ? float(double(frequency) * count / sum) : 0.0f;
    const uint32_t estimate = average > 0 ? uint32_t(average + 0.5f) : 0;
    block->fps = uint8_t(std::min(estimate, 255u));
    block->average = average;
    block->frame_number = frames;
    block->api = graphics_api.load(std::memory_order_relaxed);
    const uint32_t fallback = estimate ? uint32_t(frequency / estimate) : uint32_t(frequency);
    for (unsigned i = 0; i < frame_ticks.size(); ++i) {
        const auto ticks = frame_ticks[i].load(std::memory_order_relaxed);
        block->ticks[i] = recent && ticks ? ticks : fallback;
    }
    const uint32_t size = resolution.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < 8; ++i) {
        block->render[i].calls = 0;
        block->viewport[i].calls = 0;
    }
    if (size) {
        block->render[0].width = uint16_t(size);
        block->viewport[0].width = uint16_t(size);
        block->render[0].height = uint16_t(size >> 16);
        block->viewport[0].height = uint16_t(size >> 16);
        block->render[0].calls = uint16_t(std::clamp(estimate, 1u, 65534u));
        block->viewport[0].calls = uint16_t(std::clamp(estimate, 1u, 65534u));
    }
    block->active = frames != 0;
}

void run(void*) {
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x7)))
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 0x7);
    unsigned attempts = 0;
    while (!stopping.load(std::memory_order_acquire)) {
        if ((!block || block->magic != magic) && attempts++ % 20 == 0)
            attach();
        publish();
        waitSingle(waiterForUEvent(&wake), 50'000'000);
    }
    if (block && block->magic == magic) {
        block->fps = 0;
        block->average = 0.0f;
        block->active = false;
    }
}
}

void saltynx_start(void) {
    if (running)
        return;
    frame_count.store(0, std::memory_order_relaxed);
    previous_tick.store(0, std::memory_order_relaxed);
    resolution.store(0, std::memory_order_relaxed);
    graphics_api.store(0, std::memory_order_relaxed);
    for (auto& ticks : frame_ticks)
        ticks.store(0, std::memory_order_relaxed);
    attach(true);
    stopping.store(false, std::memory_order_relaxed);
    ueventCreate(&wake, true);
    if (R_SUCCEEDED(threadCreate(&publisher, run, nullptr, nullptr, 0x8000, 0x3B, -2))) {
        if (R_SUCCEEDED(threadStart(&publisher)))
            running = true;
        else
            threadClose(&publisher);
    }
    if (!running && page) {
        shmemClose(&shared);
        page = nullptr;
        block = nullptr;
    }
}

void saltynx_stop(void) {
    if (!running)
        return;
    stopping.store(true, std::memory_order_release);
    ueventSignal(&wake);
    threadWaitForExit(&publisher);
    threadClose(&publisher);
    if (page)
        shmemClose(&shared);
    page = nullptr;
    block = nullptr;
    running = false;
}

void saltynx_set_resolution(uint32_t width, uint32_t height) {
    if (width && height)
        resolution.store(std::min(width, 65535u) | (std::min(height, 65535u) << 16),
            std::memory_order_relaxed);
}

void saltynx_frame(uint8_t api) {
    const auto now = armGetSystemTick();
    const auto previous = previous_tick.exchange(now, std::memory_order_relaxed);
    const auto index = frame_count.fetch_add(1, std::memory_order_acq_rel);
    if (previous && now > previous) {
        const auto delta = now - previous;
        if (delta >= armGetSystemTickFreq()) {
            for (auto& ticks : frame_ticks)
                ticks.store(0, std::memory_order_relaxed);
        } else {
            frame_ticks[index % frame_ticks.size()].store(uint32_t(std::min(delta, uint64_t(UINT32_MAX))),
                std::memory_order_relaxed);
        }
    }
    graphics_api.store(api, std::memory_order_relaxed);
}
