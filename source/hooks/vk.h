/* vk.h -- Vulkan (Mesa NVK) bridge for the NetherSX2 core's GSDeviceVK path.
 *
 * The core dlopen("libvulkan.so")s and bootstraps through vkGetInstanceProcAddr,
 * then creates its window surface with vkCreateAndroidSurfaceKHR(ANativeWindow*).
 * NVK on Switch has no VK_KHR_android_surface -- its real WSI is VK_NN_vi_surface
 * (vkCreateViSurfaceNN, .window = libnx NWindow*). Our ANativeWindow* already IS
 * an NWindow (ANativeWindow_fromSurface_fake -> nwindowGetDefault), so the bridge
 * intercepts the Android surface path and forwards it to NVK's VI surface with no
 * conversion. Compiled only under -DUSE_VULKAN (make RENDERER=VK).
 */
#ifndef HOOKS_VK_H
#define HOOKS_VK_H

#ifdef USE_VULKAN

#ifndef VK_USE_PLATFORM_VI_NN
#define VK_USE_PLATFORM_VI_NN
#endif
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_vi.h>
#include <vulkan/vulkan_android.h>

// bumped by our vkQueuePresentKHR wrapper; main.c reads it for the CPU-boost-off
// trigger (egl_swap_count stays 0 on the Vulkan path).
extern volatile int vk_present_count;

// Runtime LSFG control used by the in-game quick menu. Requests are consumed
// by the presentation thread so LSFG resources are never torn down while a
// queue submission is still in flight.
int vk_lsfg_is_available(void);
int vk_lsfg_is_enabled(void);
void vk_lsfg_request_enabled(int enabled);

// Registered in the core's import table (imports.c). Everything else the core
// discovers through vk_gipa_hook, which forwards to NVK's real vkGetInstanceProcAddr.
PFN_vkVoidFunction VKAPI_CALL vk_gipa_hook(VkInstance instance, const char *name);
VkResult VKAPI_CALL vkCreateInstance_hook(const VkInstanceCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkInstance *out);
VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties_hook(const char *layer,
        uint32_t *pCount, VkExtensionProperties *pProps);

#endif // USE_VULKAN
#endif // HOOKS_VK_H
