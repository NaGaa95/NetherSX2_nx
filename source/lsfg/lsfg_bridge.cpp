/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg_bridge.h"

#ifdef USE_VULKAN

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

VkImageMemoryBarrier imageBarrier(VkImage image,
        VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
        VkImageLayout oldLayout, VkImageLayout newLayout) {
    return {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = sourceAccess,
        .dstAccessMask = destinationAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
}

struct FrameSlot {
    explicit FrameSlot(const vk::Vulkan& vulkan)
        : frame(vulkan), restore(vulkan), completion(vulkan), acquired(vulkan),
          generated(vulkan), original(vulkan) {}

    vk::CommandBuffer frame;
    vk::CommandBuffer restore;
    vk::Fence completion;
    vk::Semaphore acquired;
    vk::Semaphore generated;
    vk::Semaphore original;
    bool pending{};
};

} // namespace

struct LsfgNxRuntime {
    explicit LsfgNxRuntime(const LsfgNxCreateInfo& info)
        : swapchain(info.swapchain), extent(info.extent) {
        if (!info.instance || !info.physical_device || !info.device || !info.queue ||
                info.queue_family_index == VK_QUEUE_FAMILY_IGNORED ||
                !info.get_instance_proc_addr || !info.swapchain ||
                !info.swapchain_images || !info.swapchain_image_count ||
                !info.extent.width || !info.extent.height ||
                !info.shader_dll_path || !*info.shader_dll_path)
            throw std::runtime_error("incomplete LSFG create info");

        swapchainImages.assign(info.swapchain_images,
            info.swapchain_images + info.swapchain_image_count);

        float flowScale = info.flow_scale;
        if (!std::isfinite(flowScale) || flowScale < 0.125F || flowScale > 0.5F)
            flowScale = 0.25F;

        const lsfgvk::backend::BorrowedDevice borrowed{
            .instance = info.instance,
            .physicalDevice = info.physical_device,
            .device = info.device,
            .queueFamilyIndex = info.queue_family_index,
            .queue = info.queue,
            .getInstanceProcAddr = info.get_instance_proc_addr,
            .pipelineCachePath = std::filesystem::path(
                "/switch/nethersx2/cache/lsfg-vk-pipeline-cache.bin")
        };

        backend = std::make_unique<lsfgvk::backend::Instance>(
            borrowed, std::filesystem::path(info.shader_dll_path), false);
        context = &backend->openLocalContext(
            extent.width, extent.height,
            false, 1.0F / flowScale, info.performance_mode, 1,
            VK_QUEUE_FAMILY_IGNORED);
        vulkan = &backend->vulkan();

        if (!vulkan->df().AcquireNextImageKHR || !vulkan->df().QueuePresentKHR)
            throw std::runtime_error("swapchain entry points are unavailable");

        const size_t slotCount = std::max<size_t>(swapchainImages.size(), 3);
        slots.reserve(slotCount);
        for (size_t i = 0; i < slotCount; ++i)
            slots.emplace_back(*vulkan);
    }

    ~LsfgNxRuntime() {
        if (vulkan && vulkan->df().DeviceWaitIdle)
            (void)vulkan->df().DeviceWaitIdle(vulkan->dev());
    }

    LsfgNxRuntime(const LsfgNxRuntime&) = delete;
    LsfgNxRuntime& operator=(const LsfgNxRuntime&) = delete;

    [[nodiscard]] bool handles(VkSwapchainKHR candidate) const {
        return candidate == swapchain;
    }

    VkResult present(VkQueue queue, const VkPresentInfoKHR& info) {
        if (queue != vulkan->queue())
            throw std::runtime_error("present queue differs from borrowed LSFG queue");
        if (info.swapchainCount != 1 || info.pSwapchains[0] != swapchain)
            throw std::runtime_error("unsupported multi-swapchain presentation");

        const uint32_t originalImageIndex = info.pImageIndices[0];
        if (originalImageIndex >= swapchainImages.size())
            throw std::runtime_error("swapchain image index out of range");

        FrameSlot& slot = prepareSlot();

        /* Warm up the previous-frame source slot before the first interpolation. */
        const size_t sourceIndex = static_cast<size_t>((realFrameIndex + 1U) & 1U);
        const VkImage sourceImage = backend->sourceImage(*context, sourceIndex);

        std::vector<VkSemaphore> applicationWaits;
        if (info.waitSemaphoreCount) {
            applicationWaits.assign(info.pWaitSemaphores,
                info.pWaitSemaphores + info.waitSemaphoreCount);
        }

        slot.frame.begin(*vulkan);
        recordCapture(slot.frame, swapchainImages.at(originalImageIndex), sourceImage,
            sourceInitialized.at(sourceIndex));

        if (realFrameIndex == 0) {
            slot.frame.end(*vulkan);
            slot.frame.submit(*vulkan,
                std::move(applicationWaits), VK_NULL_HANDLE, 0,
                {slot.original.handle()}, VK_NULL_HANDLE, 0,
                slot.completion.handle());
            sourceInitialized.at(sourceIndex) = true;
            slot.pending = true;
            ++realFrameIndex;
            return presentOriginal(
                queue, info, originalImageIndex, slot.original.handle(), true);
        }

        backend->recordFrame(*context, slot.frame);

        /* VI permits one acquired image: present generated, reacquire, then restore real. */
        recordOutput(slot.frame, backend->destinationImage(*context, 0),
            swapchainImages.at(originalImageIndex));
        slot.frame.end(*vulkan);
        slot.frame.submit(*vulkan,
            std::move(applicationWaits), VK_NULL_HANDLE, 0,
            {slot.generated.handle()}, VK_NULL_HANDLE, 0);
        sourceInitialized.at(sourceIndex) = true;

        const VkResult generatedResult = presentGenerated(
            queue, info, originalImageIndex, slot.generated.handle());
        if (generatedResult != VK_SUCCESS && generatedResult != VK_SUBOPTIMAL_KHR)
            return generatedResult;

        uint32_t restoredImageIndex{};
        const VkResult acquireResult = vulkan->df().AcquireNextImageKHR(
            vulkan->dev(), swapchain, UINT64_MAX,
            slot.acquired.handle(), VK_NULL_HANDLE, &restoredImageIndex);
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("vkAcquireNextImageKHR after generated present failed: " +
                std::to_string(acquireResult));
        }
        if (restoredImageIndex >= swapchainImages.size())
            throw std::runtime_error("acquired original-frame image index out of range");

        slot.restore.begin(*vulkan);
        recordOriginal(slot.restore, sourceImage, swapchainImages.at(restoredImageIndex));
        slot.restore.end(*vulkan);
        slot.restore.submit(*vulkan,
            {slot.acquired.handle()}, VK_NULL_HANDLE, 0,
            {slot.original.handle()}, VK_NULL_HANDLE, 0,
            slot.completion.handle());
        slot.pending = true;

        const VkResult originalResult = presentOriginal(
            queue, info, restoredImageIndex, slot.original.handle(), false);
        ++realFrameIndex;
        return originalResult;
    }

private:
    FrameSlot& prepareSlot() {
        FrameSlot& slot = slots.at(slotCursor++ % slots.size());
        if (slot.pending) {
            if (!slot.completion.wait(*vulkan))
                throw std::runtime_error("timeout waiting for LSFG frame slot");
            slot.pending = false;
        }
        slot.completion.reset(*vulkan);
        return slot;
    }

    void recordCapture(const vk::CommandBuffer& command, VkImage swapchainImage,
            VkImage sourceImage, bool sourceWasInitialized) const {
        command.copyImage(*vulkan,
            {
                imageBarrier(swapchainImage,
                    VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                imageBarrier(sourceImage,
                    sourceWasInitialized ? VK_ACCESS_SHADER_READ_BIT : 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    sourceWasInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            },
            {swapchainImage, sourceImage}, extent,
            {
                imageBarrier(swapchainImage,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                imageBarrier(sourceImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)
            });
    }

    void recordOutput(const vk::CommandBuffer& command, VkImage generatedImage,
            VkImage swapchainImage) const {
        command.copyImage(*vulkan,
            {
                imageBarrier(generatedImage,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                imageBarrier(swapchainImage,
                    VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            },
            {generatedImage, swapchainImage}, extent,
            {
                imageBarrier(generatedImage,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
                imageBarrier(swapchainImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            });
    }

    void recordOriginal(const vk::CommandBuffer& command, VkImage sourceImage,
            VkImage swapchainImage) const {
        command.copyImage(*vulkan,
            {
                imageBarrier(sourceImage,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                imageBarrier(swapchainImage,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            },
            {sourceImage, swapchainImage}, extent,
            {
                imageBarrier(sourceImage,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
                imageBarrier(swapchainImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            });
    }

    VkResult presentGenerated(VkQueue queue, const VkPresentInfoKHR& original,
            uint32_t imageIndex, VkSemaphore waitSemaphore) const {
        VkPresentInfoKHR generated = original;
        generated.waitSemaphoreCount = 1;
        generated.pWaitSemaphores = &waitSemaphore;
        generated.pImageIndices = &imageIndex;
        generated.pResults = nullptr;
        return vulkan->df().QueuePresentKHR(queue, &generated);
    }

    VkResult presentOriginal(VkQueue queue, const VkPresentInfoKHR& original,
            uint32_t imageIndex, VkSemaphore waitSemaphore,
            bool keepNextChain) const {
        VkPresentInfoKHR output = original;
        output.pNext = keepNextChain ? original.pNext : nullptr;
        output.waitSemaphoreCount = 1;
        output.pWaitSemaphores = &waitSemaphore;
        output.pImageIndices = &imageIndex;
        return vulkan->df().QueuePresentKHR(queue, &output);
    }

    VkSwapchainKHR swapchain{};
    VkExtent2D extent{};
    std::vector<VkImage> swapchainImages;

    std::unique_ptr<lsfgvk::backend::Instance> backend;
    lsfgvk::backend::Context *context{};
    const vk::Vulkan *vulkan{};
    std::vector<FrameSlot> slots;

    std::array<bool, 2> sourceInitialized{false, false};
    uint64_t realFrameIndex{};
    size_t slotCursor{};
};

extern "C" LsfgNxRuntime *lsfg_nx_create(const LsfgNxCreateInfo *info) {
    if (!info) return nullptr;

    try {
        auto runtime = std::make_unique<LsfgNxRuntime>(*info);
        return runtime.release();
    } catch (...) {
        return nullptr;
    }
}

extern "C" void lsfg_nx_destroy(LsfgNxRuntime *runtime) {
    delete runtime;
}

extern "C" bool lsfg_nx_present(LsfgNxRuntime *runtime, VkQueue queue,
        const VkPresentInfoKHR *presentInfo, VkResult *result) {
    if (!runtime || !presentInfo || !result ||
            presentInfo->swapchainCount != 1 || !presentInfo->pSwapchains ||
            !presentInfo->pImageIndices ||
            !runtime->handles(presentInfo->pSwapchains[0]))
        return false;

    try {
        *result = runtime->present(queue, *presentInfo);
        return true;
    } catch (...) {
    }

    *result = VK_ERROR_INITIALIZATION_FAILED;
    return true;
}

#endif /* USE_VULKAN */
