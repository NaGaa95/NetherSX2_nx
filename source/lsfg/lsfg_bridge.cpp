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
#include <optional>
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

struct PresentSync {
    explicit PresentSync(const vk::Vulkan& vulkan)
        : generated(vulkan), original(vulkan) {}

    vk::Semaphore generated;
    vk::Semaphore original;
};

} // namespace

struct LsfgNxRuntime {
    explicit LsfgNxRuntime(const LsfgNxCreateInfo& info)
        : swapchain(info.swapchain),
          extent(info.extent) {
        if (!info.instance || !info.physical_device || !info.device || !info.queue ||
                !info.transfer_queue ||
                info.transfer_queue_family_index == VK_QUEUE_FAMILY_IGNORED ||
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
            info.transfer_queue_family_index);
        vulkan = &backend->vulkan();

        if (info.transfer_queue != info.queue ||
                info.transfer_queue_family_index != info.queue_family_index) {
            transferVulkan.emplace(
                info.instance, info.device, info.physical_device,
                info.transfer_queue_family_index, info.transfer_queue, false,
                vulkan->fi(), vulkan->df(), std::nullopt, std::nullopt);
            copyVulkan = &*transferVulkan;
        } else {
            copyVulkan = vulkan;
        }

        if (!vulkan->df().AcquireNextImageKHR || !vulkan->df().QueuePresentKHR)
            throw std::runtime_error("swapchain entry points are unavailable");

        captureCommand.emplace(*copyVulkan);
        outputCommand.emplace(*copyVulkan);
        originalCommand.emplace(*copyVulkan);
        frameFence.emplace(*vulkan);
        acquireSemaphore.emplace(*vulkan);

        const size_t syncCount = std::max<size_t>(swapchainImages.size(), 3);
        presentSyncs.reserve(syncCount);
        for (size_t i = 0; i < syncCount; ++i)
            presentSyncs.emplace_back(*vulkan);
    }

    ~LsfgNxRuntime() {
        if (vulkan && vulkan->df().DeviceWaitIdle)
            vulkan->df().DeviceWaitIdle(vulkan->dev());
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

        if (frameFencePending) {
            if (!frameFence->wait(*vulkan))
                throw std::runtime_error("timeout waiting for LSFG frame fence");
            frameFencePending = false;
        }
        frameFence->reset(*vulkan);

        /* Backend fidx 0 expects the current frame in source slot 0 and the
         * previous frame in slot 1. Since the bridge warms up one real frame
         * before backend fidx 0, start that warm-up in slot 1. */
        const size_t sourceIndex =
            static_cast<size_t>((realFrameIndex + 1U) & 1U);
        const VkImage sourceImage = backend->sourceImage(*context, sourceIndex);
        recordCapture(swapchainImages.at(originalImageIndex), sourceImage,
            sourceInitialized.at(sourceIndex));

        std::vector<VkSemaphore> applicationWaits;
        if (info.waitSemaphoreCount)
            applicationWaits.assign(info.pWaitSemaphores,
                info.pWaitSemaphores + info.waitSemaphoreCount);

        PresentSync& sync = presentSyncs.at(syncCursor++ % presentSyncs.size());

        /* Warm up both alternating source images before the first interpolation. */
        if (realFrameIndex == 0) {
            captureCommand->submit(*copyVulkan,
                std::move(applicationWaits), VK_NULL_HANDLE, 0,
                {sync.original.handle()}, VK_NULL_HANDLE, 0,
                frameFence->handle());
            sourceInitialized.at(sourceIndex) = true;
            frameFencePending = true;
            ++realFrameIndex;
            return presentOriginal(queue, info, originalImageIndex,
                sync.original.handle(), true);
        }

        const uint64_t sourceValue = backend->nextSourceValue(*context);
        const uint64_t generatedValue = backend->generatedValue(*context, 0);
        const VkSemaphore timeline = backend->syncSemaphore(*context);

        /* The copy submission signals the timeline before LSFG's compute work
         * consumes the captured source image. */
        captureCommand->submit(*copyVulkan,
            std::move(applicationWaits), VK_NULL_HANDLE, 0,
            {}, timeline, sourceValue);
        sourceInitialized.at(sourceIndex) = true;

        backend->scheduleFrames(*context);

        /* Nintendo's VI swapchain can expose only the surface minimum plus one
         * image. The application already owns that one image while inside its
         * present call, so trying to acquire another image here violates the
         * maximum-acquired-image rule and NVK reports VK_ERROR_OUT_OF_DATE_KHR.
         *
         * The real frame already resides in the current LSFG source image.
         * Reuse the application-owned image for the generated frame, release
         * it through presentation, then acquire one image and restore that
         * captured source into it. FIFO order remains generated -> real, while
         * at most one swapchain image is acquired at any point. */
        recordOutput(backend->destinationImage(*context, 0),
            swapchainImages.at(originalImageIndex), true);
        outputCommand->submit(*copyVulkan,
            {}, timeline, generatedValue,
            {sync.generated.handle()}, VK_NULL_HANDLE, 0);

        VkResult generatedResult = presentGenerated(
            queue, info, originalImageIndex, sync.generated.handle());
        if (generatedResult != VK_SUCCESS && generatedResult != VK_SUBOPTIMAL_KHR)
            return generatedResult;

        uint32_t restoredImageIndex{};
        const VkResult acquireResult = vulkan->df().AcquireNextImageKHR(
            vulkan->dev(), swapchain, UINT64_MAX,
            acquireSemaphore->handle(), VK_NULL_HANDLE,
            &restoredImageIndex);
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
            throw std::runtime_error("vkAcquireNextImageKHR after generated present failed: " +
                std::to_string(acquireResult));
        if (restoredImageIndex >= swapchainImages.size())
            throw std::runtime_error("acquired original-frame image index out of range");

        recordOriginal(sourceImage, swapchainImages.at(restoredImageIndex));
        originalCommand->submit(*copyVulkan,
            {acquireSemaphore->handle()}, timeline, generatedValue,
            {sync.original.handle()}, VK_NULL_HANDLE, 0,
            frameFence->handle());
        frameFencePending = true;

        VkResult originalResult = presentOriginal(
            queue, info, restoredImageIndex, sync.original.handle(), false);

        ++realFrameIndex;
        return originalResult;
    }

private:
    void recordCapture(VkImage swapchainImage, VkImage sourceImage,
            bool sourceWasInitialized) {
        const bool separateQueue = copyVulkan != vulkan;
        captureCommand->begin(*copyVulkan);
        captureCommand->copyImage(*copyVulkan,
            {
                imageBarrier(swapchainImage,
                    separateQueue ? 0 : VK_ACCESS_MEMORY_READ_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                imageBarrier(sourceImage,
                    separateQueue ? 0 :
                        (sourceWasInitialized ? VK_ACCESS_SHADER_READ_BIT : 0),
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    sourceWasInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            },
            {swapchainImage, sourceImage}, extent,
            {
                imageBarrier(swapchainImage,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    separateQueue ? 0 : VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                imageBarrier(sourceImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    separateQueue ? 0 : VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL)
            });
        captureCommand->end(*copyVulkan);
    }

    void recordOutput(VkImage generatedImage, VkImage swapchainImage,
            bool applicationOwned) {
        const bool separateQueue = copyVulkan != vulkan;
        outputCommand->begin(*copyVulkan);
        outputCommand->copyImage(*copyVulkan,
            {
                imageBarrier(generatedImage,
                    separateQueue ? 0 : VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                imageBarrier(swapchainImage,
                    applicationOwned && !separateQueue ?
                        VK_ACCESS_MEMORY_READ_BIT : 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    applicationOwned ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR :
                        VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            },
            {generatedImage, swapchainImage}, extent,
            {
                imageBarrier(generatedImage,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    separateQueue ? 0 : VK_ACCESS_SHADER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL),
                imageBarrier(swapchainImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    separateQueue ? 0 : VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            });
        outputCommand->end(*copyVulkan);
    }

    void recordOriginal(VkImage sourceImage, VkImage swapchainImage) {
        const bool separateQueue = copyVulkan != vulkan;
        originalCommand->begin(*copyVulkan);
        originalCommand->copyImage(*copyVulkan,
            {
                imageBarrier(sourceImage,
                    separateQueue ? 0 : VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                imageBarrier(swapchainImage,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            },
            {sourceImage, swapchainImage}, extent,
            {
                imageBarrier(sourceImage,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    separateQueue ? 0 : VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL),
                imageBarrier(swapchainImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    separateQueue ? 0 : VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            });
        originalCommand->end(*copyVulkan);
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
    std::optional<vk::Vulkan> transferVulkan;
    const vk::Vulkan *copyVulkan{};

    std::optional<vk::CommandBuffer> captureCommand;
    std::optional<vk::CommandBuffer> outputCommand;
    std::optional<vk::CommandBuffer> originalCommand;
    std::optional<vk::Fence> frameFence;
    std::optional<vk::Semaphore> acquireSemaphore;
    std::vector<PresentSync> presentSyncs;

    std::array<bool, 2> sourceInitialized{false, false};
    uint64_t realFrameIndex{};
    size_t syncCursor{};
    bool frameFencePending{};
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
