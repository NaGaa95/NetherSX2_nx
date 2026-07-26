/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Same-device LSFG-VK bridge for the Switch Vulkan renderer.
 *
 * The implementation is C++20, but this deliberately small C ABI keeps the
 * existing Vulkan hook in source/hooks/vk.c independent of the vendored C++
 * backend. LSFG owns only its private images, pipelines and synchronization;
 * every NetherSX2 Vulkan handle is borrowed.
 */
#ifndef NETHERSX2_LSFG_BRIDGE_H
#define NETHERSX2_LSFG_BRIDGE_H

#ifdef USE_VULKAN

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LsfgNxRuntime LsfgNxRuntime;

typedef struct LsfgNxCreateInfo {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family_index;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;

  VkSwapchainKHR swapchain;
  VkExtent2D extent;
  const VkImage *swapchain_images;
  uint32_t swapchain_image_count;

  const char *shader_dll_path;
  float flow_scale;
  bool performance_mode;
} LsfgNxCreateInfo;

LsfgNxRuntime *lsfg_nx_create(const LsfgNxCreateInfo *info);
void lsfg_nx_destroy(LsfgNxRuntime *runtime);

/*
 * Returns true when the bridge consumed the presentation and wrote result.
 * Runtime faults are reported as errors. They deliberately do not fall back to
 * an ordinary presentation while the user has LSFG enabled.
 */
bool lsfg_nx_present(LsfgNxRuntime *runtime, VkQueue queue,
                     const VkPresentInfoKHR *present_info, VkResult *result);

#ifdef __cplusplus
}
#endif

#endif /* USE_VULKAN */
#endif /* NETHERSX2_LSFG_BRIDGE_H */
