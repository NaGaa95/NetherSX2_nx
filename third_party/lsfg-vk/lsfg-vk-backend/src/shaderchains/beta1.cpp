/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "beta1.hpp"
#include "../helpers/utils.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk::backend;

Beta1::Beta1(const Ctx& ctx,
        const std::vector<vk::Image>& sourceImages) {
    const VkExtent2D extent = sourceImages.at(0).getExtent();

    // create temporary & output images
    this->tempImages0.reserve(2);
    this->tempImages1.reserve(2);
    for(uint32_t i = 0; i < 2; i++) {
        this->tempImages0.emplace_back(ctx.vk, extent);
        this->tempImages1.emplace_back(ctx.vk, extent);
    }

    this->images.reserve(6);
    for (uint32_t i = 0; i < 6; i++)
        this->images.emplace_back(ctx.vk,
            backend::shift_extent(extent, i),
            VK_FORMAT_R8_UNORM);

    // create descriptor sets
    const auto& shaders = (ctx.perf ?
        ctx.shaders.get().performance : ctx.shaders.get().quality).beta;
    this->sets.reserve(4);
    this->sets.emplace_back(ManagedShaderBuilder()
        .sampleds(sourceImages)
        .storages(this->tempImages0)
        .sampler(ctx.bnbSampler)
        .build(ctx.vk, ctx.pool, shaders.at(1)));
    this->sets.emplace_back(ManagedShaderBuilder()
        .sampleds(this->tempImages0)
        .storages(this->tempImages1)
        .sampler(ctx.bnbSampler)
        .build(ctx.vk, ctx.pool, shaders.at(2)));
    this->sets.emplace_back(ManagedShaderBuilder()
        .sampleds(this->tempImages1)
        .storages(this->tempImages0)
        .sampler(ctx.bnbSampler)
        .build(ctx.vk, ctx.pool, shaders.at(3)));
    this->sets.emplace_back(ManagedShaderBuilder()
        .sampleds(this->tempImages0)
        .storage(this->images.at(0))
        .sampler(ctx.bnbSampler)
        .buffer(ctx.constantBuffer)
        .build(ctx.vk, ctx.pool, shaders.at(4)));

    // The original finalizer also built five smaller levels inside a 32x32
    // shared-memory reduction. Use the proven 8x8 downsample pipeline instead.
    this->downsampleSets.reserve(5);
    for (size_t i = 0; i < 5; ++i) {
        this->downsampleSets.emplace_back(ManagedShaderBuilder()
            .sampled(this->images.at(i))
            .storage(this->images.at(i + 1))
            .sampler(ctx.bnbSampler)
            .build(ctx.vk, ctx.pool,
                ctx.shaders.get().mipmap_downsample));
    }

    // All four beta stages and the replacement downsampler use 8x8 groups.
    this->dispatchExtent0 = backend::add_shift_extent(extent, 7, 3);
    for (size_t i = 0; i < this->downsampleDispatchExtents.size(); ++i) {
        const VkExtent2D outputExtent =
            backend::shift_extent(extent, static_cast<uint32_t>(i + 1));
        this->downsampleDispatchExtents.at(i) =
            backend::add_shift_extent(outputExtent, 7, 3);
    }
}

void Beta1::prepare(std::vector<VkImage>& images) const {
    for (size_t i = 0; i < 2; i++) {
        images.push_back(this->tempImages0.at(i).handle());
        images.push_back(this->tempImages1.at(i).handle());
    }
    for (const auto& img : this->images)
        images.push_back(img.handle());
}

void Beta1::render(const vk::Vulkan& vk, const vk::CommandBuffer& cmd) const {
    for (size_t i = 0; i < this->sets.size(); ++i)
        this->sets.at(i).dispatch(vk, cmd,
            this->dispatchExtent0);

    for (size_t i = 0; i < this->downsampleSets.size(); ++i)
        this->downsampleSets.at(i).dispatch(
            vk, cmd, this->downsampleDispatchExtents.at(i));
}
