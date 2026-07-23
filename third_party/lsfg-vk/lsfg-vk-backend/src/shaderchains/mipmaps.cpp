/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "mipmaps.hpp"
#include "../helpers/utils.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk::backend;

Mipmaps::Mipmaps(const Ctx& ctx,
        const std::pair<vk::Image, vk::Image>& sourceImages) {
    // create output images for base and 6 mips
    this->images.reserve(7);
    for (uint32_t i = 0; i < 7; i++)
       this->images.emplace_back(ctx.vk,
            backend::shift_extent(ctx.flowExtent, i), VK_FORMAT_R8_UNORM);

    // The original DLL shader creates every level in one 32x32 workgroup.
    // Split it into small NVK-safe stages while keeping identical resources.
    this->lumaSets.reserve(2);
    this->lumaSets.emplace_back(ManagedShaderBuilder()
        .sampled(sourceImages.first)
        .storage(this->images.at(0))
        .sampler(ctx.bnbSampler)
        .buffer(ctx.constantBuffer)
        .build(ctx.vk, ctx.pool, ctx.shaders.get().mipmaps));
    this->lumaSets.emplace_back(ManagedShaderBuilder()
        .sampled(sourceImages.second)
        .storage(this->images.at(0))
        .sampler(ctx.bnbSampler)
        .buffer(ctx.constantBuffer)
        .build(ctx.vk, ctx.pool, ctx.shaders.get().mipmaps));

    this->downsampleSets.reserve(6);
    for (size_t i = 0; i < 6; ++i) {
        this->downsampleSets.emplace_back(ManagedShaderBuilder()
            .sampled(this->images.at(i))
            .storage(this->images.at(i + 1))
            .sampler(ctx.bnbSampler)
            .build(ctx.vk, ctx.pool,
                ctx.shaders.get().mipmap_downsample));
    }

    // Both replacement shaders use an 8x8 local size.
    this->lumaDispatchExtent =
        backend::add_shift_extent(ctx.flowExtent, 7, 3);
    for (size_t i = 0; i < this->downsampleDispatchExtents.size(); ++i) {
        const VkExtent2D outputExtent =
            backend::shift_extent(ctx.flowExtent, static_cast<uint32_t>(i + 1));
        this->downsampleDispatchExtents.at(i) =
            backend::add_shift_extent(outputExtent, 7, 3);
    }
}

void Mipmaps::prepare(std::vector<VkImage>& images) const {
    for (const auto& img : this->images)
        images.push_back(img.handle());
}

void Mipmaps::render(const vk::Vulkan& vk, const vk::CommandBuffer& cmd,
        size_t idx) const {
    this->lumaSets.at(idx % 2).dispatch(
        vk, cmd, this->lumaDispatchExtent);

    for (size_t i = 0; i < this->downsampleSets.size(); ++i)
        this->downsampleSets.at(i).dispatch(
            vk, cmd, this->downsampleDispatchExtents.at(i));
}
