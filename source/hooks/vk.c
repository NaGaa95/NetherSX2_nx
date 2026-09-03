/* vk.c -- Vulkan (Mesa NVK) bridge. See vk.h for the why.
 *
 * Three functional shims sit between the core and NVK:
 *   1. vkCreateAndroidSurfaceKHR -> NVK's VI surface (the core has no VI path).
 *   2. vkQueuePresentKHR         -> count presented frames (main.c's liveness /
 *                                   CPU-boost-off trigger; the GL swap counter
 *                                   never moves on the Vulkan path).
 *   3. vkGetPhysicalDeviceMemoryProperties2 -> report the full heap as the
 *                                   VK_EXT_memory_budget budget. NVK reports the
 *                                   free-system-memory figure, which our newlib
 *                                   heap has eaten; VMA then believes it is over
 *                                   budget and refuses EVERY allocation without
 *                                   calling Vulkan -> the GS spins in bad_alloc
 *                                   and the screen stays black. This is the fix.
 * Plus vkCreateInstance / vkEnumerateInstanceExtensionProperties, which swap the
 * core's VK_KHR_android_surface for NVK's real VK_NN_vi_surface.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "vk.h"

#ifdef USE_VULKAN

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include "../util.h"
#include "../config.h"
#include "../prefs.h"
#include "../lsfg/lsfg_bridge.h"

volatile int vk_present_count = 0;

#ifdef NETHERSX2_VK_DIAGNOSTIC
static Mutex vk_diag_mutex;
static FILE *vk_diag_file;

static FILE *
vk_diag_open_locked(void) {
  if (!vk_diag_file) {
    mkdir(DATA_ROOT, 0777);
    vk_diag_file = fopen(DATA_ROOT "/nethersx2-vulkan.log", "a");
    if (vk_diag_file)
      setvbuf(vk_diag_file, NULL, _IOLBF, 0);
  }
  return vk_diag_file;
}

void
vk_diag_reset(void) {
  mutexLock(&vk_diag_mutex);
  if (vk_diag_file) {
    fclose(vk_diag_file);
    vk_diag_file = NULL;
  }
  mkdir(DATA_ROOT, 0777);
  vk_diag_file = fopen(DATA_ROOT "/nethersx2-vulkan.log", "w");
  if (vk_diag_file) {
    setvbuf(vk_diag_file, NULL, _IOLBF, 0);
    fprintf(vk_diag_file, "NetherSX2 Vulkan diagnostic %s\n", NETHERSX2_VERSION);
    fflush(vk_diag_file);
    fsync(fileno(vk_diag_file));
  }
  mutexUnlock(&vk_diag_mutex);
}

void
vk_diag_note(const char *format, ...) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);

  mutexLock(&vk_diag_mutex);
  FILE *file = vk_diag_open_locked();
  if (file) {
    fprintf(file, "[%lld.%03lld] ", (long long)time.tv_sec,
            (long long)(time.tv_nsec / 1000000));
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fflush(file);
    fsync(fileno(file));
  }
  mutexUnlock(&vk_diag_mutex);
}

void
vk_diag_exception(uint64_t pc, uint64_t far, uint32_t esr,
                  uint64_t sp, uint64_t frame_pointer, uint64_t link_register) {
  char report[512];
  const int length = snprintf(
      report, sizeof(report),
      "NetherSX2 fatal CPU exception\n"
      "pc=0x%016llx far=0x%016llx esr=0x%08x\n"
      "sp=0x%016llx fp=0x%016llx lr=0x%016llx\n",
      (unsigned long long)pc, (unsigned long long)far, esr,
      (unsigned long long)sp, (unsigned long long)frame_pointer,
      (unsigned long long)link_register);
  const int fd = open(DATA_ROOT "/nethersx2-exception.log",
                      O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd >= 0) {
    if (length > 0)
      (void)write(fd, report, (size_t)length < sizeof(report) ?
                             (size_t)length : sizeof(report));
    fsync(fd);
    close(fd);
  }
}
#endif

typedef struct {
  VkQueue queue;
  uint32_t family;
} QueueRecord;

#define MAX_TRACKED_QUEUES 8

static VkInstance       tracked_instance;
static VkPhysicalDevice tracked_physical_device;
static VkDevice         tracked_device;
static uint32_t         tracked_instance_api = VK_API_VERSION_1_0;
static QueueRecord      tracked_queues[MAX_TRACKED_QUEUES];
static uint32_t         tracked_queue_count;
static uint32_t         lsfg_main_queue_family = VK_QUEUE_FAMILY_IGNORED;

static VkSwapchainKHR tracked_swapchain;
static VkExtent2D     tracked_swapchain_extent;
static int            tracked_swapchain_lsfg_compatible;

static LsfgNxRuntime *lsfg_runtime;
static int lsfg_init_attempted;
static int lsfg_device_capable;
static int lsfg_session_prepared;
static int lsfg_enabled_requested;
static int lsfg_runtime_available;
/* Measure source timing before LSFG; two 60 Hz FIFO presents would halve a
 * native 50/60 FPS game. */
static uint64_t lsfg_last_source_present_ns;
static double lsfg_source_interval_ns;
static unsigned lsfg_source_samples;
static unsigned lsfg_high_fps_slow_samples;
static int lsfg_previous_requested;
static int lsfg_rate_decision = -1; /* -1: measuring, 0: generate, 1: passthrough */

static PFN_vkCreateDevice real_create_device;
static PFN_vkGetDeviceQueue real_get_device_queue;
static PFN_vkGetDeviceQueue2 real_get_device_queue2;
static PFN_vkCreateSwapchainKHR real_create_swapchain;
static PFN_vkGetSwapchainImagesKHR real_get_swapchain_images;
static PFN_vkDestroySwapchainKHR real_destroy_swapchain;
static PFN_vkDestroyDevice real_destroy_device;
static PFN_vkCreatePipelineCache real_create_pipeline_cache;
static PFN_vkDestroyPipelineCache real_destroy_pipeline_cache;
static PFN_vkGetPipelineCacheData real_get_pipeline_cache_data;
#ifdef NETHERSX2_VK_DIAGNOSTIC
static PFN_vkQueueSubmit real_queue_submit;
static PFN_vkQueueSubmit2 real_queue_submit2;
static PFN_vkAcquireNextImageKHR real_acquire_next_image;
static unsigned vk_diag_submit_calls;
static unsigned vk_diag_submit2_calls;
static unsigned vk_diag_acquire_calls;
static unsigned vk_diag_swapchain_image_calls;
#endif

static int lsfg_requested(void) {
  return __atomic_load_n(&lsfg_enabled_requested, __ATOMIC_ACQUIRE);
}

int vk_lsfg_is_available(void) {
  return __atomic_load_n(&lsfg_runtime_available, __ATOMIC_ACQUIRE);
}

int vk_lsfg_is_enabled(void) {
  return lsfg_requested();
}

int vk_lsfg_is_high_fps_passthrough(void) {
  return lsfg_requested() && lsfg_rate_decision == 1;
}

void vk_lsfg_request_enabled(int enabled) {
  __atomic_store_n(&lsfg_enabled_requested, enabled != 0, __ATOMIC_RELEASE);
}

static const char *lsfg_dll_path(void) {
  return DATA_ROOT "/lsfg/Lossless.dll";
}

static int file_readable(const char *path) {
  if (!path || !path[0]) return 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

static void lsfg_destroy_runtime(void) {
  if (lsfg_runtime) {
    lsfg_nx_destroy(lsfg_runtime);
    lsfg_runtime = NULL;
  }
}

static void remember_queue(VkQueue queue, uint32_t family) {
  if (!queue) return;
  for (uint32_t i = 0; i < tracked_queue_count; ++i) {
    if (tracked_queues[i].queue == queue) {
      tracked_queues[i].family = family;
      return;
    }
  }
  if (tracked_queue_count < MAX_TRACKED_QUEUES) {
    tracked_queues[tracked_queue_count].queue = queue;
    tracked_queues[tracked_queue_count].family = family;
    ++tracked_queue_count;
  }
}

static int find_queue_family(VkQueue queue, uint32_t *family) {
  for (uint32_t i = 0; i < tracked_queue_count; ++i) {
    if (tracked_queues[i].queue == queue) {
      if (family) *family = tracked_queues[i].family;
      return 1;
    }
  }
  return 0;
}

static int has_device_extension(VkPhysicalDevice physical_device,
                                const char *wanted) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(physical_device, NULL, &count, NULL) != VK_SUCCESS)
    return 0;
  VkExtensionProperties *properties = count ? malloc(sizeof(*properties) * count) : NULL;
  if (count && !properties) return 0;
  VkResult result = vkEnumerateDeviceExtensionProperties(
      physical_device, NULL, &count, properties);
  int found = 0;
  if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
    for (uint32_t i = 0; i < count; ++i) {
      if (!strcmp(properties[i].extensionName, wanted)) {
        found = 1;
        break;
      }
    }
  }
  free(properties);
  return found;
}

static int extension_enabled(const VkDeviceCreateInfo *create_info,
                             const char *wanted) {
  for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i)
    if (!strcmp(create_info->ppEnabledExtensionNames[i], wanted)) return 1;
  return 0;
}

static VkResult find_lsfg_main_queue_family(
    VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo *create_info,
    uint32_t *main_family) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, NULL);
  if (!count) return VK_ERROR_FEATURE_NOT_PRESENT;

  VkQueueFamilyProperties *properties = malloc(sizeof(*properties) * count);
  if (!properties) return VK_ERROR_OUT_OF_HOST_MEMORY;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, properties);

  uint32_t main = VK_QUEUE_FAMILY_IGNORED;

  /* NVK's graphics/present queue supports the fused LSFG transfer work. */
  for (uint32_t pass = 0; pass < 2 && main == VK_QUEUE_FAMILY_IGNORED; ++pass) {
    const VkQueueFlags wanted = pass ? VK_QUEUE_COMPUTE_BIT : VK_QUEUE_GRAPHICS_BIT;
    for (uint32_t i = 0; i < create_info->queueCreateInfoCount; ++i) {
      const uint32_t family = create_info->pQueueCreateInfos[i].queueFamilyIndex;
      if (family < count && create_info->pQueueCreateInfos[i].queueCount &&
          (properties[family].queueFlags & wanted)) {
        main = family;
        break;
      }
    }
  }

  free(properties);
  *main_family = main;
  return main == VK_QUEUE_FAMILY_IGNORED ?
      VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
}

static void reset_tracked_swapchain(void) {
  lsfg_destroy_runtime();
  tracked_swapchain = VK_NULL_HANDLE;
  memset(&tracked_swapchain_extent, 0, sizeof(tracked_swapchain_extent));
  tracked_swapchain_lsfg_compatible = 0;
  lsfg_init_attempted = 0;
  lsfg_last_source_present_ns = 0;
  lsfg_source_interval_ns = 0.0;
  lsfg_source_samples = 0;
  lsfg_high_fps_slow_samples = 0;
  lsfg_previous_requested = 0;
  lsfg_rate_decision = -1;
  __atomic_store_n(&lsfg_runtime_available, 0, __ATOMIC_RELEASE);
}

static uint64_t lsfg_monotonic_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
  return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static uint64_t lsfg_observe_source_present(void) {
  const uint64_t now = lsfg_monotonic_ns();
  uint64_t interval = 0;
  if (now && lsfg_last_source_present_ns && now > lsfg_last_source_present_ns) {
    interval = now - lsfg_last_source_present_ns;
    /* Ignore menu stalls, swapchain transitions, and implausibly short bursts. */
    if (interval >= UINT64_C(4000000) && interval <= UINT64_C(100000000)) {
      if (!lsfg_source_samples)
        lsfg_source_interval_ns = (double)interval;
      else
        lsfg_source_interval_ns =
            lsfg_source_interval_ns * 0.875 + (double)interval * 0.125;
      if (lsfg_source_samples < 120) ++lsfg_source_samples;
    } else {
      interval = 0;
    }
  }
  lsfg_last_source_present_ns = now;
  return interval;
}

static int lsfg_source_is_high_rate(void) {
  /* Conservative boundary between 30 FPS and a slightly dipping 50/60 FPS game. */
  return lsfg_source_samples >= 8 &&
         lsfg_source_interval_ns > 0.0 &&
         lsfg_source_interval_ns < 30000000.0;
}

// surface: Android create-info -> NVK VI surface. ci->window is the core's
// ANativeWindow*, which our ANativeWindow_fromSurface_fake made == a libnx
// NWindow*, so it passes straight through to vkCreateViSurfaceNN.
static VkResult VKAPI_CALL
vkCreateAndroidSurfaceKHR_shim(VkInstance inst,
                               const VkAndroidSurfaceCreateInfoKHR *ci,
                               const VkAllocationCallbacks *alloc,
                               VkSurfaceKHR *out) {
  VkViSurfaceCreateInfoNN vi = {
      .sType  = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
      .pNext  = NULL,
      .flags  = 0,
      .window = (void *)ci->window, // NWindow* -- no conversion
  };
  VkResult r = vkCreateViSurfaceNN(inst, &vi, alloc, out);

#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateViSurfaceNN window=%p result=%d surface=%p",
               vi.window, r, out && r == VK_SUCCESS ? (void *)*out : NULL);
#endif

  return r;
}

// LSFG device features are prepared only for launches enabled in the SDL
// launcher. Runtime frame generation still begins Off and is controlled
// independently by the in-game overlay.
static VkResult VKAPI_CALL
vkCreateDevice_shim(VkPhysicalDevice physical_device,
                    const VkDeviceCreateInfo *create_info,
                    const VkAllocationCallbacks *alloc,
                    VkDevice *out) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateDevice begin physical=%p queues=%u extensions=%u lsfg_pref=%d",
               (void *)physical_device, create_info->queueCreateInfoCount,
               create_info->enabledExtensionCount, lsfg_session_prepared);
#endif
  PFN_vkCreateDevice create_fn = real_create_device ? real_create_device : vkCreateDevice;
  int prepare_lsfg = lsfg_session_prepared;
  if (prepare_lsfg && !file_readable(lsfg_dll_path())) {
    prepare_lsfg = 0;
    lsfg_session_prepared = 0;
    vk_lsfg_request_enabled(0);
  }
  VkResult result = VK_SUCCESS;
  int augmented = 0;
  int configure_lsfg = prepare_lsfg;
  uint32_t main_family = VK_QUEUE_FAMILY_IGNORED;
  VkResult configuration_error = VK_SUCCESS;

  VkDeviceCreateInfo modified = *create_info;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
      .pNext = (void *)create_info->pNext,
      .timelineSemaphore = VK_TRUE,
  };
  VkPhysicalDeviceTimelineSemaphoreFeatures *existing_timeline = NULL;
  VkPhysicalDeviceVulkan12Features *existing_vulkan12 = NULL;
  VkBool32 old_timeline_value = VK_FALSE;

  const char **extensions = NULL;

  if (configure_lsfg) {
    configuration_error = find_lsfg_main_queue_family(
        physical_device, create_info, &main_family);
    if (configuration_error != VK_SUCCESS) {
      configure_lsfg = 0;
    }
  }

  if (configure_lsfg) {
    for (const VkBaseInStructure *next = create_info->pNext;
         next; next = next->pNext) {
      if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
        existing_timeline = (VkPhysicalDeviceTimelineSemaphoreFeatures *)(uintptr_t)next;
        old_timeline_value = existing_timeline->timelineSemaphore;
        existing_timeline->timelineSemaphore = VK_TRUE;
        augmented = 1;
        break;
      }
      if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
        existing_vulkan12 = (VkPhysicalDeviceVulkan12Features *)(uintptr_t)next;
        old_timeline_value = existing_vulkan12->timelineSemaphore;
        existing_vulkan12->timelineSemaphore = VK_TRUE;
        augmented = 1;
        break;
      }
    }
    if (!existing_timeline && !existing_vulkan12) {
      modified.pNext = &timeline_feature;
      augmented = 1;
    }

    if (tracked_instance_api < VK_API_VERSION_1_2 &&
        !extension_enabled(create_info, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
      if (!has_device_extension(physical_device,
                                VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
        configuration_error = VK_ERROR_EXTENSION_NOT_PRESENT;
        configure_lsfg = 0;
      } else {
        extensions = malloc(sizeof(*extensions) *
                            (create_info->enabledExtensionCount + 1));
        if (!extensions) {
          configuration_error = VK_ERROR_OUT_OF_HOST_MEMORY;
          configure_lsfg = 0;
        } else {
          for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i)
            extensions[i] = create_info->ppEnabledExtensionNames[i];
          extensions[create_info->enabledExtensionCount] =
              VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
          modified.enabledExtensionCount = create_info->enabledExtensionCount + 1;
          modified.ppEnabledExtensionNames = extensions;
        }
      }
    }
  }

  /* A late setup failure must leave the application's original feature chain
   * untouched when LSFG was Off and normal device creation is allowed. */
  if (!configure_lsfg) {
    if (existing_timeline)
      existing_timeline->timelineSemaphore = old_timeline_value;
    if (existing_vulkan12)
      existing_vulkan12->timelineSemaphore = old_timeline_value;
  }

  if (prepare_lsfg && !configure_lsfg) {
    result = configuration_error != VK_SUCCESS ? configuration_error :
        VK_ERROR_FEATURE_NOT_PRESENT;
  } else {
    result = create_fn(physical_device,
        configure_lsfg ? &modified : create_info, alloc, out);
  }

  if (existing_timeline) existing_timeline->timelineSemaphore = old_timeline_value;
  if (existing_vulkan12) existing_vulkan12->timelineSemaphore = old_timeline_value;
  free(extensions);

  lsfg_device_capable = 0;
  if (result == VK_SUCCESS) {
    tracked_physical_device = physical_device;
    tracked_device = *out;
    tracked_queue_count = 0;
    lsfg_main_queue_family = VK_QUEUE_FAMILY_IGNORED;
    reset_tracked_swapchain();

    if (configure_lsfg && augmented) {
      lsfg_main_queue_family = main_family;
      lsfg_device_capable = 1;
    }
  }
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateDevice end result=%d device=%p lsfg_capable=%d family=%u",
               result, out && result == VK_SUCCESS ? (void *)*out : NULL,
               lsfg_device_capable, lsfg_main_queue_family);
#endif
  return result;
}

static void VKAPI_CALL
vkGetDeviceQueue_shim(VkDevice device, uint32_t queue_family_index,
                      uint32_t queue_index, VkQueue *out) {
  PFN_vkGetDeviceQueue get_fn = real_get_device_queue ?
      real_get_device_queue : vkGetDeviceQueue;
  get_fn(device, queue_family_index, queue_index, out);
  if (out) remember_queue(*out, queue_family_index);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkGetDeviceQueue family=%u index=%u queue=%p",
               queue_family_index, queue_index, out ? (void *)*out : NULL);
#endif
}

static void VKAPI_CALL
vkGetDeviceQueue2_shim(VkDevice device, const VkDeviceQueueInfo2 *queue_info,
                       VkQueue *out) {
  PFN_vkGetDeviceQueue2 get_fn = real_get_device_queue2 ?
      real_get_device_queue2 : vkGetDeviceQueue2;
  get_fn(device, queue_info, out);
  if (queue_info && out) remember_queue(*out, queue_info->queueFamilyIndex);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkGetDeviceQueue2 family=%u index=%u queue=%p",
               queue_info ? queue_info->queueFamilyIndex : UINT32_MAX,
               queue_info ? queue_info->queueIndex : UINT32_MAX,
               out ? (void *)*out : NULL);
#endif
}

#ifdef NETHERSX2_VK_DIAGNOSTIC
static VkResult VKAPI_CALL
vkQueueSubmit_shim(VkQueue queue, uint32_t submit_count,
                   const VkSubmitInfo *submits, VkFence fence) {
  PFN_vkQueueSubmit submit = real_queue_submit ? real_queue_submit : vkQueueSubmit;
  VkResult result = submit(queue, submit_count, submits, fence);
  const unsigned call = ++vk_diag_submit_calls;
  if (call <= 3 || result != VK_SUCCESS)
    vk_diag_note("vkQueueSubmit call=%u count=%u fence=%p result=%d",
                 call, submit_count, (void *)fence, result);
  return result;
}

static VkResult VKAPI_CALL
vkQueueSubmit2_shim(VkQueue queue, uint32_t submit_count,
                    const VkSubmitInfo2 *submits, VkFence fence) {
  PFN_vkQueueSubmit2 submit = real_queue_submit2 ? real_queue_submit2 : vkQueueSubmit2;
  VkResult result = submit(queue, submit_count, submits, fence);
  const unsigned call = ++vk_diag_submit2_calls;
  if (call <= 3 || result != VK_SUCCESS)
    vk_diag_note("vkQueueSubmit2 call=%u count=%u fence=%p result=%d",
                 call, submit_count, (void *)fence, result);
  return result;
}

static VkResult VKAPI_CALL
vkAcquireNextImageKHR_shim(VkDevice device, VkSwapchainKHR swapchain,
                           uint64_t timeout, VkSemaphore semaphore,
                           VkFence fence, uint32_t *image_index) {
  PFN_vkAcquireNextImageKHR acquire = real_acquire_next_image ?
      real_acquire_next_image : vkAcquireNextImageKHR;
  VkResult result = acquire(device, swapchain, timeout, semaphore, fence,
                            image_index);
  const unsigned call = ++vk_diag_acquire_calls;
  if (call <= 4 || (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR))
    vk_diag_note("vkAcquireNextImageKHR call=%u result=%d image=%u semaphore=%p fence=%p",
                 call, result,
                 image_index && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ?
                     *image_index : UINT32_MAX,
                 (void *)semaphore, (void *)fence);
  return result;
}

static VkResult VKAPI_CALL
vkGetSwapchainImagesKHR_shim(VkDevice device, VkSwapchainKHR swapchain,
                             uint32_t *count, VkImage *images) {
  PFN_vkGetSwapchainImagesKHR get_images = real_get_swapchain_images ?
      real_get_swapchain_images : vkGetSwapchainImagesKHR;
  VkResult result = get_images(device, swapchain, count, images);
  const unsigned call = ++vk_diag_swapchain_image_calls;
  if (call <= 4 || (result != VK_SUCCESS && result != VK_INCOMPLETE))
    vk_diag_note("vkGetSwapchainImagesKHR call=%u fill=%d result=%d count=%u",
                 call, images != NULL, result, count ? *count : 0);
  return result;
}
#endif

static VkResult VKAPI_CALL
vkCreateSwapchainKHR_shim(VkDevice device,
                          const VkSwapchainCreateInfoKHR *create_info,
                          const VkAllocationCallbacks *alloc,
                          VkSwapchainKHR *out) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateSwapchainKHR begin old=%p extent=%ux%u format=%u usage=0x%x images=%u mode=%u",
               (void *)create_info->oldSwapchain,
               create_info->imageExtent.width, create_info->imageExtent.height,
               create_info->imageFormat, create_info->imageUsage,
               create_info->minImageCount, create_info->presentMode);
#endif
  PFN_vkCreateSwapchainKHR create_fn = real_create_swapchain ?
      real_create_swapchain : vkCreateSwapchainKHR;

  if (create_info->oldSwapchain == tracked_swapchain)
    reset_tracked_swapchain();

  VkSwapchainCreateInfoKHR modified = *create_info;
  int compatible = 0;
  const int requested = lsfg_requested();
  const int want_lsfg = lsfg_device_capable;
  const char *dll_path = lsfg_dll_path();
  const int have_dll = want_lsfg && file_readable(dll_path);

  if (requested && (!want_lsfg || !have_dll))
    return VK_ERROR_FEATURE_NOT_PRESENT;

  if (have_dll) {
    VkSurfaceCapabilitiesKHR capabilities;
    VkFormatProperties swapchain_properties;
    VkFormatProperties rgba_properties;
    VkResult cap_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        tracked_physical_device, create_info->surface, &capabilities);
    vkGetPhysicalDeviceFormatProperties(tracked_physical_device,
        create_info->imageFormat, &swapchain_properties);
    vkGetPhysicalDeviceFormatProperties(tracked_physical_device,
        VK_FORMAT_R8G8B8A8_UNORM, &rgba_properties);
    const VkImageUsageFlags transfer_usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags copy_features =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const int queues_valid =
        lsfg_main_queue_family != VK_QUEUE_FAMILY_IGNORED;
    if (cap_result == VK_SUCCESS && queues_valid &&
        (capabilities.supportedUsageFlags & transfer_usage) == transfer_usage &&
        (swapchain_properties.optimalTilingFeatures & copy_features) == copy_features &&
        (rgba_properties.optimalTilingFeatures & copy_features) == copy_features) {
      modified.imageUsage |= transfer_usage;
      // Give the doubled FIFO stream all image headroom exposed by VI.
      uint32_t desired = modified.minImageCount + 2;
      if (capabilities.maxImageCount && desired > capabilities.maxImageCount)
        desired = capabilities.maxImageCount;
      if (desired > modified.minImageCount) modified.minImageCount = desired;
      modified.presentMode = VK_PRESENT_MODE_FIFO_KHR;
      compatible = 1;
    }
  }

  if (requested && want_lsfg && have_dll && !compatible)
    return VK_ERROR_FORMAT_NOT_SUPPORTED;

  VkResult result = create_fn(device, compatible ? &modified : create_info, alloc, out);
  if (result == VK_SUCCESS) {
    reset_tracked_swapchain();
    tracked_swapchain = *out;
    tracked_swapchain_extent = create_info->imageExtent;
    tracked_swapchain_lsfg_compatible = compatible;
    __atomic_store_n(&lsfg_runtime_available, compatible, __ATOMIC_RELEASE);
  }
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateSwapchainKHR end result=%d swapchain=%p compatible=%d actual_usage=0x%x actual_images=%u actual_mode=%u",
               result, out && result == VK_SUCCESS ? (void *)*out : NULL,
               compatible, compatible ? modified.imageUsage : create_info->imageUsage,
               compatible ? modified.minImageCount : create_info->minImageCount,
               compatible ? modified.presentMode : create_info->presentMode);
#endif
  return result;
}

static void VKAPI_CALL
vkDestroySwapchainKHR_shim(VkDevice device, VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks *alloc) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkDestroySwapchainKHR swapchain=%p", (void *)swapchain);
#endif
  if (swapchain == tracked_swapchain) reset_tracked_swapchain();
  PFN_vkDestroySwapchainKHR destroy_fn = real_destroy_swapchain ?
      real_destroy_swapchain : vkDestroySwapchainKHR;
  destroy_fn(device, swapchain, alloc);
}

static void VKAPI_CALL
vkDestroyDevice_shim(VkDevice device, const VkAllocationCallbacks *alloc) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkDestroyDevice device=%p", (void *)device);
#endif
  if (device == tracked_device) {
    reset_tracked_swapchain();
    tracked_device = VK_NULL_HANDLE;
    tracked_physical_device = VK_NULL_HANDLE;
    tracked_queue_count = 0;
    lsfg_main_queue_family = VK_QUEUE_FAMILY_IGNORED;
    lsfg_device_capable = 0;
  }
  PFN_vkDestroyDevice destroy_fn = real_destroy_device ?
      real_destroy_device : vkDestroyDevice;
  destroy_fn(device, alloc);
}

// present counter (main.c reads vk_present_count).
static PFN_vkQueuePresentKHR real_qpresent = NULL;

static VkResult
vkQueuePresentKHR_native(VkQueue queue, const VkPresentInfoKHR *present_info) {
  PFN_vkQueuePresentKHR present = real_qpresent ? real_qpresent : vkQueuePresentKHR;
  VkResult result = present(queue, present_info);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  if (vk_present_count <= 4 || result != VK_SUCCESS)
    vk_diag_note("vkQueuePresentKHR call=%d swaps=%u result=%d",
                 vk_present_count,
                 present_info ? present_info->swapchainCount : 0, result);
#endif
  return result;
}

static int lsfg_try_create(VkQueue queue) {
  if (lsfg_runtime) return 1;
  if (lsfg_init_attempted || !tracked_swapchain_lsfg_compatible ||
      !tracked_device || !tracked_physical_device || !tracked_instance)
    return 0;

  lsfg_init_attempted = 1;
  uint32_t queue_family = 0;
  if (!find_queue_family(queue, &queue_family)) return 0;
  if (queue_family != lsfg_main_queue_family) return 0;

  PFN_vkGetSwapchainImagesKHR get_images = real_get_swapchain_images ?
      real_get_swapchain_images : vkGetSwapchainImagesKHR;
  uint32_t image_count = 0;
  VkResult result = get_images(tracked_device, tracked_swapchain,
                               &image_count, NULL);
  if (result != VK_SUCCESS || image_count < 3) return 0;

  VkImage *images = malloc(sizeof(*images) * image_count);
  if (!images) return 0;
  result = get_images(tracked_device, tracked_swapchain, &image_count, images);
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    free(images);
    return 0;
  }

  float flow_scale = prefs_get_float("Wrapper/LSFGFlowScale", 0.25f);
  if (flow_scale != 0.25f && flow_scale != 0.5f)
    flow_scale = 0.25f;

  LsfgNxCreateInfo info = {
      .instance = tracked_instance,
      .physical_device = tracked_physical_device,
      .device = tracked_device,
      .queue = queue,
      .queue_family_index = queue_family,
      .get_instance_proc_addr = vkGetInstanceProcAddr,
      .swapchain = tracked_swapchain,
      .extent = tracked_swapchain_extent,
      .swapchain_images = images,
      .swapchain_image_count = image_count,
      .shader_dll_path = lsfg_dll_path(),
      .flow_scale = flow_scale,
      .performance_mode = prefs_get_bool("Wrapper/LSFGPerformance", true),
  };
  lsfg_runtime = lsfg_nx_create(&info);
  free(images);

  return lsfg_runtime != NULL;
}

static VkResult VKAPI_CALL
vkQueuePresentKHR_shim(VkQueue q, const VkPresentInfoKHR *pi) {
  ++vk_present_count;
#ifdef NETHERSX2_VK_DIAGNOSTIC
  if (vk_present_count <= 4)
    vk_diag_note("vkQueuePresentKHR enter call=%d waits=%u swaps=%u lsfg=%d",
                 vk_present_count, pi ? pi->waitSemaphoreCount : 0,
                 pi ? pi->swapchainCount : 0, lsfg_requested());
#endif
  const int requested = lsfg_requested();
  uint64_t source_interval = 0;

  /* FIFO back-pressure invalidates source timing after generation starts. */
  if (!requested || lsfg_rate_decision != 0)
    source_interval = lsfg_observe_source_present();

  if (requested != lsfg_previous_requested) {
    if (requested) {
      lsfg_rate_decision = lsfg_source_samples >= 8 ?
          (lsfg_source_is_high_rate() ? 1 : 0) : -1;
      lsfg_high_fps_slow_samples = 0;
    } else {
      lsfg_rate_decision = -1;
      lsfg_last_source_present_ns = 0;
      lsfg_source_interval_ns = 0.0;
      lsfg_source_samples = 0;
      lsfg_high_fps_slow_samples = 0;
    }
    lsfg_previous_requested = requested;
  }

  if (!requested && lsfg_runtime) {
    lsfg_destroy_runtime();
    lsfg_init_attempted = 0;
  }

  if (requested && lsfg_rate_decision < 0 && lsfg_source_samples >= 8)
    lsfg_rate_decision = lsfg_source_is_high_rate() ? 1 : 0;

  if (requested && lsfg_rate_decision == 1) {
    /* Engage LSFG after a sustained transition from 50/60 to 25/30 FPS. */
    if (source_interval >= UINT64_C(31500000))
      ++lsfg_high_fps_slow_samples;
    else if (source_interval)
      lsfg_high_fps_slow_samples = 0;
    if (lsfg_high_fps_slow_samples < 16)
      return vkQueuePresentKHR_native(q, pi);
    lsfg_rate_decision = 0;
    lsfg_high_fps_slow_samples = 0;
  }

  /* Keep native presentation active until source-rate classification completes. */
  if (requested && lsfg_rate_decision < 0)
    return vkQueuePresentKHR_native(q, pi);

  if (pi && pi->swapchainCount == 1 && pi->pSwapchains &&
      pi->pSwapchains[0] == tracked_swapchain &&
      requested) {
    if (!tracked_swapchain_lsfg_compatible || !lsfg_try_create(q))
      return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (lsfg_nx_present(lsfg_runtime, q, pi, &result)) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
      if (vk_present_count <= 4 || result != VK_SUCCESS)
        vk_diag_note("lsfg present call=%d result=%d", vk_present_count, result);
#endif
      return result;
    }
#ifdef NETHERSX2_VK_DIAGNOSTIC
    vk_diag_note("lsfg present call=%d failed before queue", vk_present_count);
#endif
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  return vkQueuePresentKHR_native(q, pi);
}

// memory-budget override (the black-screen fix -- see file header).
static PFN_vkGetPhysicalDeviceMemoryProperties2 real_memprops2 = NULL;
static void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties2_shim(VkPhysicalDevice pd,
                                          VkPhysicalDeviceMemoryProperties2 *props) {
  if (!real_memprops2) return;
  real_memprops2(pd, props);
  uint32_t heaps = props->memoryProperties.memoryHeapCount;
  for (VkBaseOutStructure *p = (VkBaseOutStructure *)props->pNext; p; p = p->pNext) {
    if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
      continue;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = (void *)p;
    for (uint32_t i = 0; i < heaps && i < VK_MAX_MEMORY_HEAPS; i++) {
      VkDeviceSize heap = props->memoryProperties.memoryHeaps[i].size;
      if (b->heapBudget[i] < heap) b->heapBudget[i] = heap;
    }
  }
}

#define VK_PIPELINE_CACHE_FILE CACHE_DIR "/vk_pipeline.bin"

static VkResult VKAPI_CALL
vkCreatePipelineCache_shim(VkDevice dev, const VkPipelineCacheCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAlloc, VkPipelineCache *pCache) {
  if (!real_create_pipeline_cache)
    return VK_ERROR_INITIALIZATION_FAILED;

  VkPipelineCacheCreateInfo ci = *pCreateInfo;
  void *cache_data = NULL;

  if (ci.initialDataSize == 0 || !ci.pInitialData) {
    FILE *f = fopen(VK_PIPELINE_CACHE_FILE, "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      if (sz > 32) {
        fseek(f, 0, SEEK_SET);
        cache_data = malloc((size_t)sz);
        if (cache_data && fread(cache_data, 1, (size_t)sz, f) == (size_t)sz) {
          ci.initialDataSize = (size_t)sz;
          ci.pInitialData = cache_data;
        } else if (cache_data) {
          free(cache_data);
          cache_data = NULL;
        }
      }
      fclose(f);
    }
  }

  VkResult r = real_create_pipeline_cache(dev, &ci, pAlloc, pCache);
  if (cache_data) free(cache_data);
  return r;
}

static void VKAPI_CALL
vkDestroyPipelineCache_shim(VkDevice dev, VkPipelineCache cache, const VkAllocationCallbacks *pAlloc) {
  if (!real_destroy_pipeline_cache)
    return;

  if (cache != VK_NULL_HANDLE && real_get_pipeline_cache_data) {
    size_t size = 0;
    if (real_get_pipeline_cache_data(dev, cache, &size, NULL) == VK_SUCCESS && size > 32) {
      void *buf = malloc(size);
      if (buf) {
        if (real_get_pipeline_cache_data(dev, cache, &size, buf) == VK_SUCCESS) {
          mkdir(DATA_ROOT, 0777);
          mkdir(CACHE_DIR, 0777);
          FILE *f = fopen(VK_PIPELINE_CACHE_FILE ".tmp", "wb");
          if (f) {
            fwrite(buf, 1, size, f);
            fclose(f);
            rename(VK_PIPELINE_CACHE_FILE ".tmp", VK_PIPELINE_CACHE_FILE);
          }
        }
        free(buf);
      }
    }
  }

  real_destroy_pipeline_cache(dev, cache, pAlloc);
}

// device proc addr wrapper: swap in our present shim; forward everything else to
// NVK's real GDPA unchanged.
static PFN_vkVoidFunction VKAPI_CALL
vk_gdpa_hook(VkDevice dev, const char *name) {
  PFN_vkVoidFunction fn = vkGetDeviceProcAddr(dev, name);
  if (!name) return fn;
#ifdef NETHERSX2_VK_DIAGNOSTIC
  if (!strcmp(name, "vkQueueSubmit")) {
    real_queue_submit = (PFN_vkQueueSubmit)fn;
    return (PFN_vkVoidFunction)vkQueueSubmit_shim;
  }
  if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR")) {
    real_queue_submit2 = (PFN_vkQueueSubmit2)fn;
    return fn ? (PFN_vkVoidFunction)vkQueueSubmit2_shim : NULL;
  }
  if (!strcmp(name, "vkAcquireNextImageKHR")) {
    real_acquire_next_image = (PFN_vkAcquireNextImageKHR)fn;
    return (PFN_vkVoidFunction)vkAcquireNextImageKHR_shim;
  }
#endif
  if (!strcmp(name, "vkGetDeviceQueue")) {
    real_get_device_queue = (PFN_vkGetDeviceQueue)fn;
    return (PFN_vkVoidFunction)vkGetDeviceQueue_shim;
  }
  if (!strcmp(name, "vkGetDeviceQueue2")) {
    real_get_device_queue2 = (PFN_vkGetDeviceQueue2)fn;
    return (PFN_vkVoidFunction)vkGetDeviceQueue2_shim;
  }
  if (!strcmp(name, "vkCreateSwapchainKHR")) {
    real_create_swapchain = (PFN_vkCreateSwapchainKHR)fn;
    return (PFN_vkVoidFunction)vkCreateSwapchainKHR_shim;
  }
  if (!strcmp(name, "vkGetSwapchainImagesKHR")) {
    real_get_swapchain_images = (PFN_vkGetSwapchainImagesKHR)fn;
#ifdef NETHERSX2_VK_DIAGNOSTIC
    return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR_shim;
#else
    return fn;
#endif
  }
  if (!strcmp(name, "vkDestroySwapchainKHR")) {
    real_destroy_swapchain = (PFN_vkDestroySwapchainKHR)fn;
    return (PFN_vkVoidFunction)vkDestroySwapchainKHR_shim;
  }
  if (!strcmp(name, "vkDestroyDevice")) {
    real_destroy_device = (PFN_vkDestroyDevice)fn;
    return (PFN_vkVoidFunction)vkDestroyDevice_shim;
  }
  if (!strcmp(name, "vkCreatePipelineCache")) {
    real_create_pipeline_cache = (PFN_vkCreatePipelineCache)fn;
    return (PFN_vkVoidFunction)vkCreatePipelineCache_shim;
  }
  if (!strcmp(name, "vkDestroyPipelineCache")) {
    real_destroy_pipeline_cache = (PFN_vkDestroyPipelineCache)fn;
    return (PFN_vkVoidFunction)vkDestroyPipelineCache_shim;
  }
  if (!strcmp(name, "vkGetPipelineCacheData")) {
    real_get_pipeline_cache_data = (PFN_vkGetPipelineCacheData)fn;
    return fn;
  }
  if (!strcmp(name, "vkQueuePresentKHR")) {
    real_qpresent = (PFN_vkQueuePresentKHR)fn;
    return (PFN_vkVoidFunction)vkQueuePresentKHR_shim;
  }
  return fn;
}

// instance proc addr wrapper -- registered in the import table as
// "vkGetInstanceProcAddr". Routes the Android surface, device-proc-addr, present
// and the memory-budget override; everything else is NVK's real GIPA.
PFN_vkVoidFunction VKAPI_CALL
vk_gipa_hook(VkInstance inst, const char *name) {
  if (name) {
    if (!strcmp(name, "vkCreateInstance"))
      return (PFN_vkVoidFunction)vkCreateInstance_hook;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
      return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties_hook;
    if (!strcmp(name, "vkCreateAndroidSurfaceKHR"))
      return (PFN_vkVoidFunction)vkCreateAndroidSurfaceKHR_shim;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)vk_gdpa_hook;
#ifdef NETHERSX2_VK_DIAGNOSTIC
    if (!strcmp(name, "vkQueueSubmit")) {
      real_queue_submit = (PFN_vkQueueSubmit)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkQueueSubmit_shim;
    }
    if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR")) {
      real_queue_submit2 = (PFN_vkQueueSubmit2)vkGetInstanceProcAddr(inst, name);
      return real_queue_submit2 ? (PFN_vkVoidFunction)vkQueueSubmit2_shim : NULL;
    }
    if (!strcmp(name, "vkAcquireNextImageKHR")) {
      real_acquire_next_image =
          (PFN_vkAcquireNextImageKHR)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkAcquireNextImageKHR_shim;
    }
#endif
    if (!strcmp(name, "vkCreateDevice")) {
      real_create_device = (PFN_vkCreateDevice)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkCreateDevice_shim;
    }
    if (!strcmp(name, "vkGetDeviceQueue")) {
      real_get_device_queue = (PFN_vkGetDeviceQueue)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkGetDeviceQueue_shim;
    }
    if (!strcmp(name, "vkGetDeviceQueue2")) {
      real_get_device_queue2 = (PFN_vkGetDeviceQueue2)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkGetDeviceQueue2_shim;
    }
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
      real_create_swapchain = (PFN_vkCreateSwapchainKHR)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkCreateSwapchainKHR_shim;
    }
    if (!strcmp(name, "vkGetSwapchainImagesKHR")) {
      real_get_swapchain_images = (PFN_vkGetSwapchainImagesKHR)vkGetInstanceProcAddr(inst, name);
#ifdef NETHERSX2_VK_DIAGNOSTIC
      return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR_shim;
#else
      return (PFN_vkVoidFunction)real_get_swapchain_images;
#endif
    }
    if (!strcmp(name, "vkDestroySwapchainKHR")) {
      real_destroy_swapchain = (PFN_vkDestroySwapchainKHR)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkDestroySwapchainKHR_shim;
    }
    if (!strcmp(name, "vkDestroyDevice")) {
      real_destroy_device = (PFN_vkDestroyDevice)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkDestroyDevice_shim;
    }
    if (!strcmp(name, "vkCreatePipelineCache")) {
      real_create_pipeline_cache = (PFN_vkCreatePipelineCache)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkCreatePipelineCache_shim;
    }
    if (!strcmp(name, "vkDestroyPipelineCache")) {
      real_destroy_pipeline_cache = (PFN_vkDestroyPipelineCache)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkDestroyPipelineCache_shim;
    }
    if (!strcmp(name, "vkGetPipelineCacheData")) {
      real_get_pipeline_cache_data = (PFN_vkGetPipelineCacheData)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)real_get_pipeline_cache_data;
    }
    if (!strcmp(name, "vkQueuePresentKHR")) {
      real_qpresent = (PFN_vkQueuePresentKHR)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkQueuePresentKHR_shim;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") ||
        !strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR")) {
      PFN_vkVoidFunction f = vkGetInstanceProcAddr(inst, name);
      if (!f) return NULL; // don't advertise what NVK doesn't have
      real_memprops2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)f;
      return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties2_shim;
    }
  }
  return vkGetInstanceProcAddr(inst, name);
}

// enumerate instance extensions -- advertise VK_KHR_android_surface (which NVK
// does NOT provide) so the core's availability check passes. Swapped for the real
// VK_NN_vi_surface at vkCreateInstance below.
VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties_hook(const char *layer, uint32_t *pCount,
                                            VkExtensionProperties *pProps) {
  if (pProps == NULL) {
    VkResult r = vkEnumerateInstanceExtensionProperties(layer, pCount, NULL);
    if (r == VK_SUCCESS) (*pCount)++; // reserve a slot for the injected name
    return r;
  }
  uint32_t want = *pCount, got = want ? want - 1 : 0;
  VkResult r = vkEnumerateInstanceExtensionProperties(layer, &got, pProps);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
  VkExtensionProperties inj = { VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, 6 };
  if (got < want) { pProps[got++] = inj; *pCount = got; return VK_SUCCESS; }
  *pCount = got;
  return VK_INCOMPLETE;
}

// create instance -- swap the core's VK_KHR_android_surface for the real
// VK_NN_vi_surface so NVK accepts the instance and owns the VI surface extension.
VkResult VKAPI_CALL
vkCreateInstance_hook(const VkInstanceCreateInfo *ci,
                      const VkAllocationCallbacks *alloc, VkInstance *out) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateInstance begin api=%u app=%s engine=%s extensions=%u",
               ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0,
               ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName ?
                   ci->pApplicationInfo->pApplicationName : "(null)",
               ci->pApplicationInfo && ci->pApplicationInfo->pEngineName ?
                   ci->pApplicationInfo->pEngineName : "(null)",
               ci->enabledExtensionCount);
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
    vk_diag_note("instance extension[%u]=%s", i,
                 ci->ppEnabledExtensionNames[i]);
#endif
  int lsfg_launch_prepared =
      prefs_get_bool("Wrapper/LSFGEnabled", false) &&
      file_readable(lsfg_dll_path());

  uint32_t n = ci->enabledExtensionCount;
  const char **ext = malloc(sizeof(char *) * (n ? n : 1));
  for (uint32_t i = 0; i < n; i++) {
    ext[i] = strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)
                 ? ci->ppEnabledExtensionNames[i]
                 : VK_NN_VI_SURFACE_EXTENSION_NAME;
  }
  VkInstanceCreateInfo m = *ci;
  m.ppEnabledExtensionNames = ext;
  VkResult r = vkCreateInstance(&m, alloc, out);
  free((void *)ext);

  if (r == VK_SUCCESS) {
    tracked_instance = *out;
    tracked_instance_api = (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion) ?
        ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    tracked_physical_device = VK_NULL_HANDLE;
    tracked_device = VK_NULL_HANDLE;
    tracked_queue_count = 0;
    lsfg_main_queue_family = VK_QUEUE_FAMILY_IGNORED;
    lsfg_device_capable = 0;
    lsfg_session_prepared = lsfg_launch_prepared;
#ifdef NETHERSX2_VK_DIAGNOSTIC
    vk_diag_submit_calls = 0;
    vk_diag_submit2_calls = 0;
    vk_diag_acquire_calls = 0;
    vk_diag_swapchain_image_calls = 0;
#endif
    // The launcher switch is an availability gate, not the initial runtime
    // state. Every game starts with frame generation disabled until the user
    // explicitly enables it from the in-game overlay.
    vk_lsfg_request_enabled(0);
    reset_tracked_swapchain();
  }

#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateInstance end result=%d instance=%p lsfg_prepared=%d",
               r, out && r == VK_SUCCESS ? (void *)*out : NULL,
               lsfg_launch_prepared);
#endif

  return r;
}

#endif // USE_VULKAN
