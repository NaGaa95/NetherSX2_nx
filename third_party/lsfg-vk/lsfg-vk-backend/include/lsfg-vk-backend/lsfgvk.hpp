/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace vk { class Vulkan; }

namespace lsfgvk::backend {

    class [[gnu::visibility("default")]] ContextImpl;
    class [[gnu::visibility("default")]] InstanceImpl;

    using Context = ContextImpl;

    /// Existing Vulkan objects owned by the embedding application. The backend
    /// borrows every handle; it only owns resources it creates itself.
    struct BorrowedDevice {
        VkInstance instance{};
        VkPhysicalDevice physicalDevice{};
        VkDevice device{};
        uint32_t queueFamilyIndex{};
        VkQueue queue{};
        PFN_vkGetInstanceProcAddr getInstanceProcAddr{};
        std::optional<std::filesystem::path> pipelineCachePath{};
    };

    ///
    /// Primitive exception class that deliveres a detailed error message
    ///
    class [[gnu::visibility("default")]] error : public std::runtime_error {
    public:
        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        /// @param inner Inner exception.
        ///
        explicit error(const std::string &msg, const std::exception &inner);

        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        ///
        explicit error(const std::string &msg);

        error(const error &) = default;
        error &operator=(const error &) = default;
        error(error &&) = default;
        error &operator=(error &&) = default;
        ~error() override;
    };

    /// Function type for picking a device based on its name and IDs
    using DevicePicker = std::function<bool(
        const std::string& deviceName,
        std::pair<const std::string&, const std::string&> ids, // (vendor ID, device ID) 0xXXXX format
        const std::optional<std::string>& pci // (bus:slot.func) if available, no padded zeros
    )>;

    ///
    /// Main entry point of the library
    ///
    class [[gnu::visibility("default")]] Instance {
    public:
        ///
        /// Create a lsfg-vk instance
        ///
        /// @param devicePicker Function that picks a physical device based on some identifiers.
        /// @param shaderDllPath Path to the Lossless.dll file to load shaders from.
        /// @param allowLowPrecision Whether to load low-precision (FP16) shaders if supported.
        ///
        /// @throws backend::error on failure
        ///
        Instance(
            const DevicePicker& devicePicker,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision
        );

        /// Create an instance on an application-owned Vulkan device. This is
        /// used by the Horizon port, where external-memory FDs do not exist.
        Instance(
            const BorrowedDevice& device,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision
        );

        ///
        /// Open a frame generation context.
        ///
        /// The VkFormat of the exchanged images is inferred from whether hdr is true or false:
        /// - false: VK_FORMAT_R8G8B8A8_UNORM
        /// - true: VK_FORMAT_R16G16B16A16_SFLOAT
        ///
        /// The application and library must keep track of the frame index. When the next frame
        /// is ready, signal the syncFd with one increment (with the first trigger being 1).
        /// Each generated frame will increment the semaphore by one:
        /// - Application signals 1 -> Start generating with (curr, next) source images
        /// - Library signals 1 -> First frame between (curr, next) is ready
        /// - Library signals N -> N-th frame between (curr, next) is ready
        /// - Application signals N+1 -> Start generating with (next, curr) source images
        ///
        /// @param sourceFds Pair of file descriptors for the source images alternated between.
        /// @param destFds Vector with file descriptors to import output images from.
        /// @param syncFd File descriptor for the timeline semaphore used for synchronization.
        /// @param width Width of the images.
        /// @param height Height of the images.
        /// @param hdr Whether the images are HDR.
        /// @param flow Motion flow factor.
        /// @param perf Whether to enable performance mode.
        ///
        /// @throws backend::error on failure
        ///
        Context& openContext(
            std::pair<int, int> sourceFds,
            const std::vector<int>& destFds,
            int syncFd,
            uint32_t width, uint32_t height,
            bool hdr, float flow, bool perf
        );

        /// Open a same-device context with backend-owned source/destination
        /// images and a local timeline semaphore.
        Context& openLocalContext(
            uint32_t width, uint32_t height,
            bool hdr, float flow, bool perf,
            size_t generatedFrameCount,
            uint32_t transferQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED
        );

        ///
        /// Schedule a new set of generated frames.
        ///
        /// @param context Context to use.
        /// @throws backend::error on failure
        ///
        void scheduleFrames(Context& context);

        ///
        /// Close a frame generation context
        ///
        /// @param context Context to close.
        ///
        void closeContext(const Context& context);

        /// Accessors used by an in-process presentation bridge.
        [[nodiscard]] const vk::Vulkan& vulkan() const;
        [[nodiscard]] VkImage sourceImage(const Context& context, size_t index) const;
        [[nodiscard]] VkImage destinationImage(const Context& context, size_t index) const;
        [[nodiscard]] VkSemaphore syncSemaphore(const Context& context) const;
        [[nodiscard]] uint64_t nextSourceValue(const Context& context) const;
        [[nodiscard]] uint64_t generatedValue(const Context& context, size_t index) const;

        // Non-copyable and non-movable
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;
        Instance(Instance&&) = delete;
        Instance& operator=(Instance&&) = delete;
        virtual ~Instance();
    private:
        std::unique_ptr<InstanceImpl> m_impl;

        std::vector<std::unique_ptr<Context>> m_contexts;
    };

    ///
    /// Make all lsfg-vk instances leaking.
    /// This is to workaround a bug in the Vulkan loader, which
    /// makes it impossible to destroy Vulkan instances and devices.
    ///
    void makeLeaking();

}
