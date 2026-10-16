/* vk_loader_shim.c -- minimal Vulkan loader for the ICD-only NXVK package.
 *
 * GENERATED FILE -- do not edit by hand. Regenerate with:
 *   python3 tools/gen_nvk_loader_shim.py \
 *     --vulkan-include "$DEVKITPRO/portlibs/switch/include/vulkan"
 *
 * libnvk.a in the nxvk portlib package defines exactly one public symbol,
 * vk_icdGetInstanceProcAddr: Horizon has no Vulkan loader, so nothing supplies
 * the public vk* entry points. The vendored 23-archive layout used to
 * (libvulkan_lite_instance/libvulkan_lite_runtime), which is why third-party
 * code calling them directly linked there.
 *
 * source/hooks/vk.c is the layer that faces libemucore.so: it calls the driver
 * directly (vkCreateInstance, vkCreateViSurfaceNN, vkGetPhysicalDevice*,
 * vkEnumerate*) and keeps raw-symbol fallbacks such as
 *   real_get_device_queue ? real_get_device_queue : vkGetDeviceQueue
 * which is a link-time reference even though the primary path resolves the same
 * function at runtime. third_party/lsfg-vk does NOT need this -- it dispatches
 * through its own vk.df()/vk.if() tables -- so the set below is exactly what
 * hooks/vk.c names. Each one gets a forwarder that resolves lazily through
 * vk_icdGetInstanceProcAddr and caches the result in a function-local static.
 *
 * Dispatch handles: global commands (vkCreateInstance, vkEnumerateInstance*) are
 * queried with VK_NULL_HANDLE because no instance exists yet. Everything else
 * uses the instance recorded by the vkCreateInstance forwarder -- Mesa's ICD
 * answers instance, device and queue level queries from an instance handle,
 * which is exactly what a loader does.
 *
 * Compiled only for the pkg-config link path: NETHERSX2_NVK_ICD_ONLY is defined
 * by the Makefile when nxvk.pc is present, never for the vendored vulkan/lib
 * tree, whose libvulkan_lite_* would collide with these definitions.
 */
#ifdef NETHERSX2_NVK_ICD_ONLY

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_vi.h>
#include <vulkan/vk_icd.h>

static VkInstance nvk_loader_instance = VK_NULL_HANDLE;

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex) {
  static PFN_vkAcquireNextImageKHR fn;
  if (!fn)
    fn = (PFN_vkAcquireNextImageKHR)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkAcquireNextImageKHR");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
  static PFN_vkCreateDevice fn;
  if (!fn)
    fn = (PFN_vkCreateDevice)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkCreateDevice");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
  static PFN_vkCreateInstance fn;
  if (!fn)
    fn = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  VkResult res = fn(pCreateInfo, pAllocator, pInstance);
  if (res == VK_SUCCESS && pInstance)
    nvk_loader_instance = *pInstance;
  return res;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain) {
  static PFN_vkCreateSwapchainKHR fn;
  if (!fn)
    fn = (PFN_vkCreateSwapchainKHR)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkCreateSwapchainKHR");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(device, pCreateInfo, pAllocator, pSwapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateViSurfaceNN(
    VkInstance instance,
    const VkViSurfaceCreateInfoNN* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface) {
  static PFN_vkCreateViSurfaceNN fn;
  if (!fn)
    fn = (PFN_vkCreateViSurfaceNN)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkCreateViSurfaceNN");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(instance, pCreateInfo, pAllocator, pSurface);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
  static PFN_vkDestroyDevice fn;
  if (!fn)
    fn = (PFN_vkDestroyDevice)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkDestroyDevice");
  if (!fn)
    return;
  fn(device, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {
  static PFN_vkDestroyInstance fn;
  if (!fn)
    fn = (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkDestroyInstance");
  if (!fn)
    return;
  fn(instance, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator) {
  static PFN_vkDestroySwapchainKHR fn;
  if (!fn)
    fn = (PFN_vkDestroySwapchainKHR)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkDestroySwapchainKHR");
  if (!fn)
    return;
  fn(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
  static PFN_vkEnumerateDeviceExtensionProperties fn;
  if (!fn)
    fn = (PFN_vkEnumerateDeviceExtensionProperties)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkEnumerateDeviceExtensionProperties");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
  static PFN_vkEnumerateInstanceExtensionProperties fn;
  if (!fn)
    fn = (PFN_vkEnumerateInstanceExtensionProperties)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(pLayerName, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount,
    VkLayerProperties* pProperties) {
  static PFN_vkEnumerateInstanceLayerProperties fn;
  if (!fn)
    fn = (PFN_vkEnumerateInstanceLayerProperties)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(
    uint32_t* pApiVersion) {
  static PFN_vkEnumerateInstanceVersion fn;
  if (!fn)
    fn = (PFN_vkEnumerateInstanceVersion)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(pApiVersion);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName) {
  static PFN_vkGetDeviceProcAddr fn;
  if (!fn)
    fn = (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkGetDeviceProcAddr");
  if (!fn)
    return (PFN_vkVoidFunction)0;
  return fn(device, pName);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue) {
  static PFN_vkGetDeviceQueue fn;
  if (!fn)
    fn = (PFN_vkGetDeviceQueue)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkGetDeviceQueue");
  if (!fn)
    return;
  fn(device, queueFamilyIndex, queueIndex, pQueue);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device,
    const VkDeviceQueueInfo2* pQueueInfo,
    VkQueue* pQueue) {
  static PFN_vkGetDeviceQueue2 fn;
  if (!fn)
    fn = (PFN_vkGetDeviceQueue2)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkGetDeviceQueue2");
  if (!fn)
    return;
  fn(device, pQueueInfo, pQueue);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkFormatProperties* pFormatProperties) {
  static PFN_vkGetPhysicalDeviceFormatProperties fn;
  if (!fn)
    fn = (PFN_vkGetPhysicalDeviceFormatProperties)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkGetPhysicalDeviceFormatProperties");
  if (!fn)
    return;
  fn(physicalDevice, format, pFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice,
    uint32_t* pQueueFamilyPropertyCount,
    VkQueueFamilyProperties* pQueueFamilyProperties) {
  static PFN_vkGetPhysicalDeviceQueueFamilyProperties fn;
  if (!fn)
    fn = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  if (!fn)
    return;
  fn(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
  static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR fn;
  if (!fn)
    fn = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(physicalDevice, surface, pSurfaceCapabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages) {
  static PFN_vkGetSwapchainImagesKHR fn;
  if (!fn)
    fn = (PFN_vkGetSwapchainImagesKHR)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkGetSwapchainImagesKHR");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo) {
  static PFN_vkQueuePresentKHR fn;
  if (!fn)
    fn = (PFN_vkQueuePresentKHR)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkQueuePresentKHR");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(queue, pPresentInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence) {
  static PFN_vkQueueSubmit fn;
  if (!fn)
    fn = (PFN_vkQueueSubmit)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkQueueSubmit");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence) {
  static PFN_vkQueueSubmit2 fn;
  if (!fn)
    fn = (PFN_vkQueueSubmit2)vk_icdGetInstanceProcAddr(nvk_loader_instance, "vkQueueSubmit2");
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(queue, submitCount, pSubmits, fence);
}


#endif /* NETHERSX2_NVK_ICD_ONLY */
