// Platform seam: Runtime Vulkan driver loader (System / Turnip / PanVK / Mock ICD)
// and minimal Vulkan RHI backend (`docs/PLATFORM_PLAN.md`, slice P4).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
// and carries no PE/PDB citations.

#include "driver_loader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#elif __has_include("../../runtime/android/third_party/vulkan/vulkan.h")
#include "../../runtime/android/third_party/vulkan/vulkan.h"
#else
#error "Missing Vulkan headers (expected vendored headers in runtime/android/third_party)"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wse::platform {

namespace {

typedef PFN_vkVoidFunction(VKAPI_PTR* PFN_vk_icdGetInstanceProcAddr)(
    VkInstance instance, const char* pName);
typedef PFN_vkVoidFunction(VKAPI_PTR* PFN_vk_icdGetPhysicalDeviceProcAddr)(
    VkInstance instance, const char* pName);
typedef VkResult(VKAPI_PTR* PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(
    std::uint32_t* pVersion);

// -----------------------------------------------------------------------------
// Cross-platform shared library helpers
// -----------------------------------------------------------------------------

#if defined(_WIN32)
struct LibHandle {
    HMODULE mod = nullptr;

    [[nodiscard]] bool open(const char* path, bool global_scope) noexcept {
        (void)global_scope;
        mod = ::LoadLibraryA(path);
        return mod != nullptr;
    }

    template <typename Fn>
    void load_sym(const char* name, Fn& out) const noexcept {
        out = nullptr;
        if (mod == nullptr) {
            return;
        }
        FARPROC sym = ::GetProcAddress(mod, name);
        if (sym != nullptr) {
            out = reinterpret_cast<Fn>(sym);
        }
    }

    void close() noexcept {
        if (mod != nullptr) {
            ::FreeLibrary(mod);
            mod = nullptr;
        }
    }

    [[nodiscard]] static std::string last_error() {
        const DWORD err = ::GetLastError();
        return "Win32 LoadLibrary error " + std::to_string(err);
    }
};
#else
struct LibHandle {
    void* mod = nullptr;

    [[nodiscard]] bool open(const char* path, bool global_scope) noexcept {
        const int flags = RTLD_NOW | (global_scope ? RTLD_GLOBAL : RTLD_LOCAL);
        mod = ::dlopen(path, flags);
        return mod != nullptr;
    }

    template <typename Fn>
    void load_sym(const char* name, Fn& out) const noexcept {
        out = nullptr;
        if (mod == nullptr) {
            return;
        }
        void* sym = ::dlsym(mod, name);
        static_assert(sizeof(out) == sizeof(sym), "unexpected function pointer size");
        std::memcpy(&out, &sym, sizeof(out));
    }

    void close() noexcept {
        if (mod != nullptr) {
            ::dlclose(mod);
            mod = nullptr;
        }
    }

    [[nodiscard]] static std::string last_error() {
        const char* msg = ::dlerror();
        return msg != nullptr ? std::string(msg) : std::string("unknown dlopen error");
    }
};
#endif

// -----------------------------------------------------------------------------
// In-process Mock Vulkan ICD v5 (for host/CI testing & unavailable hardware)
// -----------------------------------------------------------------------------

constexpr std::uintptr_t kIcdLoaderMagic = 0x01CDC0DEU;
constexpr std::uint32_t kMaxSupportedIcdVersion = 5U;

struct MockInstance;

struct MockPhysicalDevice {
    std::uintptr_t loader_magic = kIcdLoaderMagic;
    MockInstance* instance = nullptr;
};

struct MockInstance {
    std::uintptr_t loader_magic = kIcdLoaderMagic;
    VulkanMockConfig config{};
    std::uint32_t negotiated_version = kMaxSupportedIcdVersion;
    MockPhysicalDevice physical_device{};
};

struct MockDevice {
    std::uintptr_t loader_magic = kIcdLoaderMagic;
    MockPhysicalDevice* phys = nullptr;
    std::uint64_t next_id = 100U;
    std::uintptr_t queue_magic = kIcdLoaderMagic;
};

// Thread-local config passed from `VulkanDriverLoader::load_mock_icd()` into
// `mock_vkCreateInstance()`, after which `MockInstance` owns a copy of it.
thread_local const VulkanMockConfig* g_tls_mock_config = nullptr;
thread_local std::uint32_t g_tls_mock_icd_version = kMaxSupportedIcdVersion;

VKAPI_ATTR VkResult VKAPI_CALL mock_vk_icdNegotiateLoaderICDInterfaceVersion(
    std::uint32_t* pSupportedVersion) {
    if (pSupportedVersion == nullptr || *pSupportedVersion == 0U) {
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    const std::uint32_t negotiated = (*pSupportedVersion < kMaxSupportedIcdVersion)
                                         ? *pSupportedVersion
                                         : kMaxSupportedIcdVersion;
    *pSupportedVersion = negotiated;
    g_tls_mock_icd_version = negotiated;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkEnumerateInstanceVersion(std::uint32_t* pApiVersion) {
    if (pApiVersion == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pApiVersion = (g_tls_mock_config != nullptr) ? g_tls_mock_config->api_version
                                                : VK_MAKE_API_VERSION(0, 1, 3, 0);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    std::uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    (void)pProperties;
    if (pLayerName != nullptr) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    if (pPropertyCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    (void)pAllocator;
    if (pCreateInfo == nullptr || pInstance == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto* inst = new MockInstance{};
    if (g_tls_mock_config != nullptr) {
        inst->config = *g_tls_mock_config;
    }
    inst->negotiated_version = g_tls_mock_icd_version;
    inst->physical_device.loader_magic = kIcdLoaderMagic;
    inst->physical_device.instance = inst;
    *pInstance = reinterpret_cast<VkInstance>(inst);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mock_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {
    (void)pAllocator;
    if (instance != VK_NULL_HANDLE) {
        delete reinterpret_cast<MockInstance*>(instance);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkEnumeratePhysicalDevices(
    VkInstance instance,
    std::uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices) {
    if (instance == VK_NULL_HANDLE || pPhysicalDeviceCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto* inst = reinterpret_cast<MockInstance*>(instance);
    if (pPhysicalDevices == nullptr) {
        *pPhysicalDeviceCount = 1U;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceCount == 0U) {
        return VK_INCOMPLETE;
    }
    pPhysicalDevices[0] = reinterpret_cast<VkPhysicalDevice>(&inst->physical_device);
    *pPhysicalDeviceCount = 1U;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mock_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties) {
    if (physicalDevice == VK_NULL_HANDLE || pProperties == nullptr) {
        return;
    }
    const auto* phys = reinterpret_cast<const MockPhysicalDevice*>(physicalDevice);
    const VulkanMockConfig& cfg = phys->instance->config;

    std::memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion = cfg.api_version;
    pProperties->driverVersion = cfg.driver_version;
    pProperties->vendorID = cfg.vendor_id;
    pProperties->deviceID = cfg.device_id;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    pProperties->limits.maxImageDimension2D = 4096U;
    const std::size_t copy_len =
        (std::min)(cfg.device_name.size(), static_cast<std::size_t>(VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1U));
    std::memcpy(pProperties->deviceName, cfg.device_name.data(), copy_len);
    pProperties->deviceName[copy_len] = '\0';
}

VKAPI_ATTR void VKAPI_CALL mock_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice,
    std::uint32_t* pQueueFamilyPropertyCount,
    VkQueueFamilyProperties* pQueueFamilyProperties) {
    (void)physicalDevice;
    if (pQueueFamilyPropertyCount == nullptr) {
        return;
    }
    if (pQueueFamilyProperties == nullptr) {
        *pQueueFamilyPropertyCount = 1U;
        return;
    }
    if (*pQueueFamilyPropertyCount == 0U) {
        return;
    }
    std::memset(pQueueFamilyProperties, 0, sizeof(VkQueueFamilyProperties));
    pQueueFamilyProperties[0].queueFlags =
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    pQueueFamilyProperties[0].queueCount = 1U;
    pQueueFamilyProperties[0].timestampValidBits = 64U;
    pQueueFamilyProperties[0].minImageTransferGranularity = {1U, 1U, 1U};
    *pQueueFamilyPropertyCount = 1U;
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    (void)pCreateInfo;
    (void)pAllocator;
    if (physicalDevice == VK_NULL_HANDLE || pDevice == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto* dev = new MockDevice{};
    dev->phys = reinterpret_cast<MockPhysicalDevice*>(physicalDevice);
    *pDevice = reinterpret_cast<VkDevice>(dev);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mock_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    (void)pAllocator;
    if (device != VK_NULL_HANDLE) {
        delete reinterpret_cast<MockDevice*>(device);
    }
}

VKAPI_ATTR void VKAPI_CALL mock_vkGetDeviceQueue(
    VkDevice device,
    std::uint32_t queueFamilyIndex,
    std::uint32_t queueIndex,
    VkQueue* pQueue) {
    (void)queueFamilyIndex;
    (void)queueIndex;
    if (device == VK_NULL_HANDLE || pQueue == nullptr) {
        return;
    }
    auto* dev = reinterpret_cast<MockDevice*>(device);
    *pQueue = reinterpret_cast<VkQueue>(&dev->queue_magic);
}

template <typename VkHandle>
VkHandle allocate_mock_handle(MockDevice* dev) noexcept {
    const std::uint64_t id = ++dev->next_id;
    VkHandle out{};
    static_assert(sizeof(out) <= sizeof(id), "handle exceeds uint64");
    std::memcpy(&out, &id, sizeof(out));
    return out;
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkCreateCommandPool(
    VkDevice device,
    const VkCommandPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkCommandPool* pCommandPool) {
    (void)pCreateInfo;
    (void)pAllocator;
    if (device == VK_NULL_HANDLE || pCommandPool == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pCommandPool = allocate_mock_handle<VkCommandPool>(reinterpret_cast<MockDevice*>(device));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mock_vkDestroyCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    const VkAllocationCallbacks* pAllocator) {
    (void)device;
    (void)commandPool;
    (void)pAllocator;
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkCreateBuffer(
    VkDevice device,
    const VkBufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkBuffer* pBuffer) {
    (void)pAllocator;
    if (device == VK_NULL_HANDLE || pCreateInfo == nullptr || pBuffer == nullptr ||
        pCreateInfo->size == 0U) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pBuffer = allocate_mock_handle<VkBuffer>(reinterpret_cast<MockDevice*>(device));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mock_vkDestroyBuffer(
    VkDevice device,
    VkBuffer buffer,
    const VkAllocationCallbacks* pAllocator) {
    (void)device;
    (void)buffer;
    (void)pAllocator;
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage) {
    (void)pAllocator;
    if (device == VK_NULL_HANDLE || pCreateInfo == nullptr || pImage == nullptr ||
        pCreateInfo->extent.width == 0U || pCreateInfo->extent.height == 0U) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pImage = allocate_mock_handle<VkImage>(reinterpret_cast<MockDevice*>(device));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mock_vkDestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator) {
    (void)device;
    (void)image;
    (void)pAllocator;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mock_vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName) {
    (void)device;
    if (pName == nullptr) {
        return nullptr;
    }
    const std::string_view name(pName);
    if (name == "vkGetDeviceProcAddr") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkGetDeviceProcAddr);
    }
    if (name == "vkDestroyDevice") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkDestroyDevice);
    }
    if (name == "vkGetDeviceQueue") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkGetDeviceQueue);
    }
    if (name == "vkCreateCommandPool") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkCreateCommandPool);
    }
    if (name == "vkDestroyCommandPool") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkDestroyCommandPool);
    }
    if (name == "vkCreateBuffer") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkCreateBuffer);
    }
    if (name == "vkDestroyBuffer") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkDestroyBuffer);
    }
    if (name == "vkCreateImage") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkCreateImage);
    }
    if (name == "vkDestroyImage") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkDestroyImage);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mock_vk_icdGetInstanceProcAddr(
    VkInstance instance,
    const char* pName) {
    if (pName == nullptr) {
        return nullptr;
    }
    const std::string_view name(pName);
    if (name == "vkEnumerateInstanceVersion") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkEnumerateInstanceVersion);
    }
    if (name == "vkEnumerateInstanceExtensionProperties") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkEnumerateInstanceExtensionProperties);
    }
    if (name == "vkCreateInstance") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkCreateInstance);
    }
    if (instance == VK_NULL_HANDLE) {
        return nullptr;
    }
    if (name == "vkDestroyInstance") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkDestroyInstance);
    }
    if (name == "vkEnumeratePhysicalDevices") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkEnumeratePhysicalDevices);
    }
    if (name == "vkGetPhysicalDeviceProperties") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkGetPhysicalDeviceProperties);
    }
    if (name == "vkGetPhysicalDeviceQueueFamilyProperties") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkGetPhysicalDeviceQueueFamilyProperties);
    }
    if (name == "vkCreateDevice") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkCreateDevice);
    }
    return mock_vkGetDeviceProcAddr(VK_NULL_HANDLE, pName);
}

template <typename VkHandle>
std::uint64_t handle_to_u64(VkHandle h) noexcept {
    std::uint64_t out = 0;
    static_assert(sizeof(h) <= sizeof(out), "handle exceeds uint64");
    std::memcpy(&out, &h, sizeof(h));
    return out;
}

template <typename VkHandle>
VkHandle u64_to_handle(std::uint64_t val) noexcept {
    VkHandle h{};
    static_assert(sizeof(h) <= sizeof(val), "handle exceeds uint64");
    std::memcpy(&h, &val, sizeof(h));
    return h;
}

void write_u16_le(std::vector<std::byte>& dst, std::size_t off, std::uint16_t v) noexcept {
    dst[off + 0U] = static_cast<std::byte>(v & 0xFFU);
    dst[off + 1U] = static_cast<std::byte>((v >> 8U) & 0xFFU);
}

void write_u32_le(std::vector<std::byte>& dst, std::size_t off, std::uint32_t v) noexcept {
    dst[off + 0U] = static_cast<std::byte>(v & 0xFFU);
    dst[off + 1U] = static_cast<std::byte>((v >> 8U) & 0xFFU);
    dst[off + 2U] = static_cast<std::byte>((v >> 16U) & 0xFFU);
    dst[off + 3U] = static_cast<std::byte>((v >> 24U) & 0xFFU);
}

}  // namespace

std::string format_vulkan_version(std::uint32_t vk_version) {
    const std::uint32_t major = VK_API_VERSION_MAJOR(vk_version);
    const std::uint32_t minor = VK_API_VERSION_MINOR(vk_version);
    const std::uint32_t patch = VK_API_VERSION_PATCH(vk_version);
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

struct VulkanDriverLoader::Impl {
    LibHandle hw_stub_lib{};
    LibHandle icd_lib{};
    DriverMode mode = DriverMode::system;
    bool loaded = false;
    bool is_mock = false;
    bool used_direct_icd = false;
    std::uint32_t negotiated_icd_version = 0;
    std::string loaded_library{};
    VulkanMockConfig mock_config{};

    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vk_icdGetPhysicalDeviceProcAddr gpdpa = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;

    PFN_vkDestroyInstance fn_destroy_instance = nullptr;
    PFN_vkDestroyDevice fn_destroy_device = nullptr;
    PFN_vkDestroyCommandPool fn_destroy_command_pool = nullptr;
    PFN_vkCreateBuffer fn_create_buffer = nullptr;
    PFN_vkDestroyBuffer fn_destroy_buffer = nullptr;
    PFN_vkCreateImage fn_create_image = nullptr;
    PFN_vkDestroyImage fn_destroy_image = nullptr;

    std::unordered_set<std::uint64_t> live_buffers{};
    std::unordered_set<std::uint64_t> live_images{};
    VulkanDriverProbeResult probe{};

    template <typename Fn>
    void resolve_inst(const char* name, Fn& out) const noexcept {
        out = nullptr;
        if (gipa != nullptr && instance != VK_NULL_HANDLE) {
            out = reinterpret_cast<Fn>(gipa(instance, name));
        }
    }

    template <typename Fn>
    void resolve_dev(PFN_vkGetDeviceProcAddr gdpa, const char* name, Fn& out) const noexcept {
        out = nullptr;
        if (gdpa != nullptr && device != VK_NULL_HANDLE) {
            out = reinterpret_cast<Fn>(gdpa(device, name));
        }
        if (out == nullptr && gipa != nullptr && instance != VK_NULL_HANDLE) {
            out = reinterpret_cast<Fn>(gipa(instance, name));
        }
    }

    void destroy_vulkan_objects() noexcept {
        if (device != VK_NULL_HANDLE) {
            for (const std::uint64_t h : live_buffers) {
                if (fn_destroy_buffer != nullptr) {
                    fn_destroy_buffer(device, u64_to_handle<VkBuffer>(h), nullptr);
                }
            }
            for (const std::uint64_t h : live_images) {
                if (fn_destroy_image != nullptr) {
                    fn_destroy_image(device, u64_to_handle<VkImage>(h), nullptr);
                }
            }
            if (command_pool != VK_NULL_HANDLE && fn_destroy_command_pool != nullptr) {
                fn_destroy_command_pool(device, command_pool, nullptr);
            }
            if (fn_destroy_device != nullptr) {
                fn_destroy_device(device, nullptr);
            }
        }
        live_buffers.clear();
        live_images.clear();
        command_pool = VK_NULL_HANDLE;
        graphics_queue = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        physical_device = VK_NULL_HANDLE;

        if (instance != VK_NULL_HANDLE && fn_destroy_instance != nullptr) {
            fn_destroy_instance(instance, nullptr);
        }
        instance = VK_NULL_HANDLE;

        fn_destroy_instance = nullptr;
        fn_destroy_device = nullptr;
        fn_destroy_command_pool = nullptr;
        fn_create_buffer = nullptr;
        fn_destroy_buffer = nullptr;
        fn_create_image = nullptr;
        fn_destroy_image = nullptr;
    }

    void unload() noexcept {
        destroy_vulkan_objects();
        icd_lib.close();
        hw_stub_lib.close();
        loaded = false;
        is_mock = false;
        used_direct_icd = false;
        negotiated_icd_version = 0;
        loaded_library.clear();
        gipa = nullptr;
        gpdpa = nullptr;
        probe = VulkanDriverProbeResult{};
    }
};

VulkanDriverLoader::VulkanDriverLoader() : impl_(std::make_unique<Impl>()) {}
VulkanDriverLoader::~VulkanDriverLoader() { unload(); }
VulkanDriverLoader::VulkanDriverLoader(VulkanDriverLoader&&) noexcept = default;
VulkanDriverLoader& VulkanDriverLoader::operator=(VulkanDriverLoader&&) noexcept = default;

bool VulkanDriverLoader::load_system(std::string* error_out) {
    unload();
    impl_->mode = DriverMode::system;

#if defined(_WIN32)
    static constexpr const char* kCandidates[] = {"vulkan-1.dll"};
#else
    static constexpr const char* kCandidates[] = {"libvulkan.so.1", "libvulkan.so"};
#endif

    std::string last_err;
    for (const char* name : kCandidates) {
        if (impl_->icd_lib.open(name, false)) {
            impl_->loaded_library = name;
            break;
        }
        last_err = LibHandle::last_error();
    }
    if (impl_->loaded_library.empty()) {
        if (error_out != nullptr) {
            *error_out = "failed to load system Vulkan loader: " + last_err;
        }
        return false;
    }

    impl_->icd_lib.load_sym("vkGetInstanceProcAddr", impl_->gipa);
    if (impl_->gipa == nullptr) {
        if (error_out != nullptr) {
            *error_out = impl_->loaded_library + " does not export vkGetInstanceProcAddr";
        }
        unload();
        return false;
    }

    impl_->loaded = true;
    return true;
}

bool VulkanDriverLoader::load_custom_driver(DriverMode mode,
                                            const std::string& icd_so_path,
                                            const std::string& hw_stub_so_path,
                                            std::string* error_out) {
    unload();
    if (icd_so_path.empty()) {
        if (error_out != nullptr) {
            *error_out = "custom ICD path is empty";
        }
        return false;
    }

    impl_->mode = mode;

    // Preload `libpanvk_hw.so` into the global namespace when supplied so
    // patched `libvulkan_panfrost.so` resolves its hw_get_module dependency cleanly.
    if (!hw_stub_so_path.empty()) {
        if (!impl_->hw_stub_lib.open(hw_stub_so_path.c_str(), true)) {
            if (error_out != nullptr) {
                *error_out = "failed to preload " + hw_stub_so_path + ": " +
                             LibHandle::last_error();
            }
            unload();
            return false;
        }
    }

    if (!impl_->icd_lib.open(icd_so_path.c_str(), false)) {
        if (error_out != nullptr) {
            *error_out = "failed to open Vulkan ICD " + icd_so_path + ": " +
                         LibHandle::last_error();
        }
        unload();
        return false;
    }
    impl_->loaded_library = icd_so_path;

    PFN_vk_icdGetInstanceProcAddr icd_gipa = nullptr;
    PFN_vk_icdNegotiateLoaderICDInterfaceVersion icd_negotiate = nullptr;
    impl_->icd_lib.load_sym("vk_icdGetInstanceProcAddr", icd_gipa);
    impl_->icd_lib.load_sym("vk_icdNegotiateLoaderICDInterfaceVersion", icd_negotiate);

    if (icd_gipa != nullptr) {
        if (icd_negotiate != nullptr) {
            std::uint32_t ver = kMaxSupportedIcdVersion;
            const VkResult neg_res = icd_negotiate(&ver);
            if (neg_res != VK_SUCCESS) {
                if (error_out != nullptr) {
                    *error_out = "ICD refused loader interface version 5 (VkResult " +
                                 std::to_string(static_cast<int>(neg_res)) + ")";
                }
                unload();
                return false;
            }
            impl_->negotiated_icd_version = ver;
        } else {
            impl_->negotiated_icd_version = 1U;
        }
        if (impl_->negotiated_icd_version >= 2U) {
            impl_->icd_lib.load_sym("vk_icdGetPhysicalDeviceProcAddr", impl_->gpdpa);
        }
        impl_->gipa = icd_gipa;
        impl_->used_direct_icd = true;
        impl_->loaded = true;
        return true;
    }

    impl_->icd_lib.load_sym("vkGetInstanceProcAddr", impl_->gipa);
    if (impl_->gipa == nullptr) {
        if (error_out != nullptr) {
            *error_out = icd_so_path +
                         " exports neither vk_icdGetInstanceProcAddr nor vkGetInstanceProcAddr";
        }
        unload();
        return false;
    }

    impl_->used_direct_icd = false;
    impl_->loaded = true;
    return true;
}

bool VulkanDriverLoader::load_mock_icd(const VulkanMockConfig& config) {
    unload();
    impl_->mode = config.mode;
    impl_->is_mock = true;
    impl_->mock_config = config;
    impl_->loaded_library = "mock://" + config.device_name;
    impl_->used_direct_icd = config.direct_icd;

    if (config.direct_icd) {
        std::uint32_t ver = kMaxSupportedIcdVersion;
        if (mock_vk_icdNegotiateLoaderICDInterfaceVersion(&ver) != VK_SUCCESS) {
            unload();
            return false;
        }
        impl_->negotiated_icd_version = ver;
    }
    impl_->gipa = &mock_vk_icdGetInstanceProcAddr;
    impl_->loaded = true;
    return true;
}

bool VulkanDriverLoader::probe_and_create_device(VulkanDriverProbeResult& out) {
    out = VulkanDriverProbeResult{};
    out.mode = impl_->mode;
    out.used_direct_icd = impl_->used_direct_icd;
    out.is_mock = impl_->is_mock;
    out.negotiated_icd_version = impl_->negotiated_icd_version;
    out.loaded_library = impl_->loaded_library;

    const auto fail = [&](std::string msg) -> bool {
        out.ok = false;
        out.error = std::move(msg);
        impl_->destroy_vulkan_objects();
        impl_->probe = out;
        return false;
    };

    if (!impl_->loaded || impl_->gipa == nullptr) {
        return fail("Vulkan driver library is not loaded");
    }

    impl_->destroy_vulkan_objects();

    if (impl_->is_mock) {
        g_tls_mock_config = &impl_->mock_config;
        g_tls_mock_icd_version = impl_->negotiated_icd_version;
    }

    auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(impl_->gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (create_instance == nullptr) {
        g_tls_mock_config = nullptr;
        return fail("missing vkCreateInstance entry point");
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "WSE2-Android";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 1, 1, 5);
    app_info.pEngineName = "WSE2";
    app_info.engineVersion = VK_MAKE_API_VERSION(0, 1, 1, 5);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo inst_info{};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pApplicationInfo = &app_info;

    const VkResult inst_res = create_instance(&inst_info, nullptr, &impl_->instance);
    g_tls_mock_config = nullptr;
    if (inst_res != VK_SUCCESS || impl_->instance == VK_NULL_HANDLE) {
        return fail("vkCreateInstance failed with VkResult " +
                    std::to_string(static_cast<int>(inst_res)));
    }

    impl_->resolve_inst("vkDestroyInstance", impl_->fn_destroy_instance);
    PFN_vkEnumeratePhysicalDevices enum_phys = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_phys_props = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_q_props = nullptr;
    PFN_vkCreateDevice create_dev = nullptr;
    impl_->resolve_inst("vkEnumeratePhysicalDevices", enum_phys);
    impl_->resolve_inst("vkGetPhysicalDeviceProperties", get_phys_props);
    impl_->resolve_inst("vkGetPhysicalDeviceQueueFamilyProperties", get_q_props);
    impl_->resolve_inst("vkCreateDevice", create_dev);

    if (enum_phys == nullptr || get_phys_props == nullptr || get_q_props == nullptr ||
        create_dev == nullptr) {
        return fail("Vulkan instance is missing required physical-device entry points");
    }

    std::uint32_t phys_count = 0;
    VkResult phys_res = enum_phys(impl_->instance, &phys_count, nullptr);
    if ((phys_res != VK_SUCCESS && phys_res != VK_INCOMPLETE) || phys_count == 0U) {
        return fail("no Vulkan physical devices available (VkResult " +
                    std::to_string(static_cast<int>(phys_res)) + ")");
    }
    std::vector<VkPhysicalDevice> phys_list(phys_count);
    phys_res = enum_phys(impl_->instance, &phys_count, phys_list.data());
    if ((phys_res != VK_SUCCESS && phys_res != VK_INCOMPLETE) || phys_list.empty()) {
        return fail("vkEnumeratePhysicalDevices returned no usable handles");
    }

    const auto score_device_type = [](VkPhysicalDeviceType t) noexcept -> int {
        switch (t) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                return 4;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                return 3;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                return 2;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                return 1;
            default:
                return 0;
        }
    };

    int best_score = -1;
    std::uint32_t chosen_q_family = 0;
    VkPhysicalDeviceProperties chosen_props{};

    for (VkPhysicalDevice candidate : phys_list) {
        if (candidate == VK_NULL_HANDLE) {
            continue;
        }
        VkPhysicalDeviceProperties props{};
        get_phys_props(candidate, &props);

        std::uint32_t q_count = 0;
        get_q_props(candidate, &q_count, nullptr);
        if (q_count == 0U) {
            continue;
        }
        std::vector<VkQueueFamilyProperties> q_props(q_count);
        get_q_props(candidate, &q_count, q_props.data());

        for (std::uint32_t qi = 0; qi < q_count; ++qi) {
            if ((q_props[qi].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U &&
                q_props[qi].queueCount > 0U) {
                const int sc = score_device_type(props.deviceType);
                if (sc > best_score) {
                    best_score = sc;
                    impl_->physical_device = candidate;
                    chosen_q_family = qi;
                    chosen_props = props;
                }
                break;
            }
        }
    }

    if (impl_->physical_device == VK_NULL_HANDLE) {
        return fail("no Vulkan physical device exposes a graphics queue family");
    }

    const float q_priority = 1.0f;
    VkDeviceQueueCreateInfo q_info{};
    q_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q_info.queueFamilyIndex = chosen_q_family;
    q_info.queueCount = 1U;
    q_info.pQueuePriorities = &q_priority;

    VkDeviceCreateInfo dev_info{};
    dev_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_info.queueCreateInfoCount = 1U;
    dev_info.pQueueCreateInfos = &q_info;

    const VkResult dev_res =
        create_dev(impl_->physical_device, &dev_info, nullptr, &impl_->device);
    if (dev_res != VK_SUCCESS || impl_->device == VK_NULL_HANDLE) {
        return fail("vkCreateDevice failed with VkResult " +
                    std::to_string(static_cast<int>(dev_res)));
    }

    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    impl_->resolve_inst("vkGetDeviceProcAddr", gdpa);

    impl_->resolve_dev(gdpa, "vkDestroyDevice", impl_->fn_destroy_device);
    impl_->resolve_dev(gdpa, "vkDestroyCommandPool", impl_->fn_destroy_command_pool);
    impl_->resolve_dev(gdpa, "vkCreateBuffer", impl_->fn_create_buffer);
    impl_->resolve_dev(gdpa, "vkDestroyBuffer", impl_->fn_destroy_buffer);
    impl_->resolve_dev(gdpa, "vkCreateImage", impl_->fn_create_image);
    impl_->resolve_dev(gdpa, "vkDestroyImage", impl_->fn_destroy_image);

    PFN_vkGetDeviceQueue get_queue = nullptr;
    PFN_vkCreateCommandPool create_pool = nullptr;
    impl_->resolve_dev(gdpa, "vkGetDeviceQueue", get_queue);
    impl_->resolve_dev(gdpa, "vkCreateCommandPool", create_pool);

    if (get_queue != nullptr) {
        get_queue(impl_->device, chosen_q_family, 0U, &impl_->graphics_queue);
    }
    if (create_pool != nullptr) {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = chosen_q_family;
        const VkResult pool_res =
            create_pool(impl_->device, &pool_info, nullptr, &impl_->command_pool);
        if (pool_res != VK_SUCCESS) {
            return fail("vkCreateCommandPool failed with VkResult " +
                        std::to_string(static_cast<int>(pool_res)));
        }
    }

    out.ok = true;
    out.api_version = chosen_props.apiVersion;
    out.driver_version = chosen_props.driverVersion;
    out.vendor_id = chosen_props.vendorID;
    out.device_id = chosen_props.deviceID;
    out.graphics_queue_family = chosen_q_family;
    out.device_name = chosen_props.deviceName;
    impl_->probe = out;
    return true;
}

void VulkanDriverLoader::unload() noexcept {
    if (impl_) {
        impl_->unload();
    }
}

bool VulkanDriverLoader::is_loaded() const noexcept {
    return impl_ && impl_->loaded;
}

bool VulkanDriverLoader::has_device() const noexcept {
    return impl_ && impl_->device != VK_NULL_HANDLE;
}

const VulkanDriverProbeResult& VulkanDriverLoader::probe_result() const noexcept {
    static const VulkanDriverProbeResult kEmpty{};
    return impl_ ? impl_->probe : kEmpty;
}

bool VulkanDriverLoader::create_vk_buffer(std::size_t size_bytes,
                                          BufferUsage usage,
                                          std::uint64_t& out_vk_handle,
                                          std::string* error_out) {
    out_vk_handle = 0;
    if (!has_device() || impl_->fn_create_buffer == nullptr) {
        return true;  // Graceful no-op when optional device handle proc is absent.
    }

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = static_cast<VkDeviceSize>(size_bytes);
    switch (usage) {
        case BufferUsage::vertex:
            info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            break;
        case BufferUsage::index:
            info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            break;
        case BufferUsage::uniform:
            info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            break;
    }
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buf = VK_NULL_HANDLE;
    const VkResult res = impl_->fn_create_buffer(impl_->device, &info, nullptr, &buf);
    if (res != VK_SUCCESS || buf == VK_NULL_HANDLE) {
        if (error_out != nullptr) {
            *error_out = "vkCreateBuffer failed with VkResult " +
                         std::to_string(static_cast<int>(res));
        }
        return false;
    }

    out_vk_handle = handle_to_u64(buf);
    impl_->live_buffers.insert(out_vk_handle);
    return true;
}

void VulkanDriverLoader::destroy_vk_buffer(std::uint64_t vk_handle) noexcept {
    if (vk_handle == 0 || !has_device()) {
        return;
    }
    if (impl_->live_buffers.erase(vk_handle) > 0 && impl_->fn_destroy_buffer != nullptr) {
        impl_->fn_destroy_buffer(impl_->device, u64_to_handle<VkBuffer>(vk_handle), nullptr);
    }
}

bool VulkanDriverLoader::create_vk_image_rgba8(std::uint32_t width,
                                               std::uint32_t height,
                                               std::uint64_t& out_vk_handle,
                                               std::string* error_out) {
    out_vk_handle = 0;
    if (!has_device() || impl_->fn_create_image == nullptr) {
        return true;
    }

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1U};
    info.mipLevels = 1U;
    info.arrayLayers = 1U;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage img = VK_NULL_HANDLE;
    const VkResult res = impl_->fn_create_image(impl_->device, &info, nullptr, &img);
    if (res != VK_SUCCESS || img == VK_NULL_HANDLE) {
        if (error_out != nullptr) {
            *error_out = "vkCreateImage failed with VkResult " +
                         std::to_string(static_cast<int>(res));
        }
        return false;
    }

    out_vk_handle = handle_to_u64(img);
    impl_->live_images.insert(out_vk_handle);
    return true;
}

void VulkanDriverLoader::destroy_vk_image(std::uint64_t vk_handle) noexcept {
    if (vk_handle == 0 || !has_device()) {
        return;
    }
    if (impl_->live_images.erase(vk_handle) > 0 && impl_->fn_destroy_image != nullptr) {
        impl_->fn_destroy_image(impl_->device, u64_to_handle<VkImage>(vk_handle), nullptr);
    }
}

std::size_t VulkanDriverLoader::live_vk_buffers() const noexcept {
    return impl_ ? impl_->live_buffers.size() : 0U;
}

std::size_t VulkanDriverLoader::live_vk_images() const noexcept {
    return impl_ ? impl_->live_images.size() : 0U;
}

// -----------------------------------------------------------------------------
// VulkanRhiDevice
// -----------------------------------------------------------------------------

VulkanRhiDevice::VulkanRhiDevice(std::shared_ptr<VulkanDriverLoader> loader)
    : loader_(std::move(loader)) {
    if (loader_ && loader_->has_device()) {
        const auto& pr = loader_->probe_result();
        rasterizer_.set_identity(pr.device_name, pr.mode, pr.api_version);
    } else {
        rasterizer_.set_identity("Vulkan Uninitialized Device", DriverMode::system, 0U);
    }
}

VulkanRhiDevice::~VulkanRhiDevice() {
    if (loader_) {
        for (const auto& [h, vk_h] : vk_buffers_) {
            (void)h;
            loader_->destroy_vk_buffer(vk_h);
        }
        for (const auto& [h, vk_h] : vk_textures_) {
            (void)h;
            loader_->destroy_vk_image(vk_h);
        }
    }
    vk_buffers_.clear();
    vk_textures_.clear();
}

std::string VulkanRhiDevice::device_name() const {
    return rasterizer_.device_name();
}

DriverMode VulkanRhiDevice::driver_mode() const noexcept {
    return rasterizer_.driver_mode();
}

std::uint32_t VulkanRhiDevice::api_version() const noexcept {
    return rasterizer_.api_version();
}

const VulkanDriverProbeResult& VulkanRhiDevice::probe_result() const noexcept {
    static const VulkanDriverProbeResult kEmpty{};
    return loader_ ? loader_->probe_result() : kEmpty;
}

BufferHandle VulkanRhiDevice::create_buffer(BufferUsage usage,
                                            std::span<const std::uint8_t> data,
                                            std::string* error_out) {
    if (!loader_ || !loader_->has_device()) {
        if (error_out != nullptr) {
            *error_out = "VulkanRhiDevice requires an initialized VulkanDriverLoader device";
        }
        return 0;
    }
    const BufferHandle h = rasterizer_.create_buffer(usage, data, error_out);
    if (h == 0) {
        return 0;
    }
    std::uint64_t vk_h = 0;
    if (!loader_->create_vk_buffer(data.size(), usage, vk_h, error_out)) {
        rasterizer_.destroy_buffer(h);
        return 0;
    }
    if (vk_h != 0) {
        vk_buffers_.emplace(h, vk_h);
    }
    return h;
}

void VulkanRhiDevice::destroy_buffer(BufferHandle handle) {
    const auto it = vk_buffers_.find(handle);
    if (it != vk_buffers_.end()) {
        if (loader_) {
            loader_->destroy_vk_buffer(it->second);
        }
        vk_buffers_.erase(it);
    }
    rasterizer_.destroy_buffer(handle);
}

TextureHandle VulkanRhiDevice::create_texture_rgba8(std::uint32_t width,
                                                    std::uint32_t height,
                                                    std::span<const std::uint8_t> rgba8,
                                                    std::string* error_out) {
    if (!loader_ || !loader_->has_device()) {
        if (error_out != nullptr) {
            *error_out = "VulkanRhiDevice requires an initialized VulkanDriverLoader device";
        }
        return 0;
    }
    const TextureHandle h = rasterizer_.create_texture_rgba8(width, height, rgba8, error_out);
    if (h == 0) {
        return 0;
    }
    std::uint64_t vk_h = 0;
    if (!loader_->create_vk_image_rgba8(width, height, vk_h, error_out)) {
        rasterizer_.destroy_texture(h);
        return 0;
    }
    if (vk_h != 0) {
        vk_textures_.emplace(h, vk_h);
    }
    return h;
}

void VulkanRhiDevice::destroy_texture(TextureHandle handle) {
    const auto it = vk_textures_.find(handle);
    if (it != vk_textures_.end()) {
        if (loader_) {
            loader_->destroy_vk_image(it->second);
        }
        vk_textures_.erase(it);
    }
    rasterizer_.destroy_texture(handle);
}

PipelineHandle VulkanRhiDevice::create_pipeline(const PipelineDesc& desc,
                                                std::string* error_out) {
    return rasterizer_.create_pipeline(desc, error_out);
}

void VulkanRhiDevice::destroy_pipeline(PipelineHandle handle) {
    rasterizer_.destroy_pipeline(handle);
}

bool VulkanRhiDevice::render_pass_and_readback(const RenderPassDesc& pass,
                                               std::span<const MeshDrawCall> draws,
                                               std::vector<std::uint8_t>& out_rgba8,
                                               std::string* error_out) {
    if (!loader_ || !loader_->has_device()) {
        if (error_out != nullptr) {
            *error_out = "VulkanRhiDevice requires an initialized VulkanDriverLoader device";
        }
        return false;
    }
    return rasterizer_.render_pass_and_readback(pass, draws, out_rgba8, error_out);
}

std::size_t VulkanRhiDevice::live_vk_buffers() const noexcept {
    return loader_ ? loader_->live_vk_buffers() : 0U;
}

std::size_t VulkanRhiDevice::live_vk_images() const noexcept {
    return loader_ ? loader_->live_vk_images() : 0U;
}

VulkanDriverProbeResult run_vulkan_mesh_smoke_test(
    DriverMode mode,
    const std::string& custom_icd_path,
    const std::string& hw_stub_path,
    bool allow_mock_fallback,
    std::uint64_t& out_frame_hash,
    std::string* error_out) {
    out_frame_hash = 0;
    auto loader = std::make_shared<VulkanDriverLoader>();
    VulkanDriverProbeResult probe{};

    bool loaded = false;
    std::string load_err;
    if (mode == DriverMode::system) {
        loaded = loader->load_system(&load_err);
    } else if (!custom_icd_path.empty()) {
        loaded = loader->load_custom_driver(mode, custom_icd_path, hw_stub_path, &load_err);
    } else {
        load_err = "no custom Vulkan ICD path imported for " + std::string(driver_mode_name(mode));
    }

    if (loaded) {
        if (!loader->probe_and_create_device(probe)) {
            loaded = false;
            load_err = probe.error;
        }
    }

    if (!loaded && allow_mock_fallback) {
        VulkanMockConfig cfg{};
        cfg.mode = mode;
        cfg.direct_icd = (mode != DriverMode::system);
        if (mode == DriverMode::turnip) {
            cfg.device_name = "Turnip Adreno (TM) 740 (Mock ICD)";
            cfg.vendor_id = 0x5143U;
            cfg.device_id = 0x0740U;
        } else if (mode == DriverMode::panvk) {
            cfg.device_name = "Mali-G610 PanVK v3.7.0 (Mock ICD)";
            cfg.vendor_id = 0x13B5U;
            cfg.device_id = 0x0610U;
        } else {
            cfg.device_name = "System Vulkan (Mock Fallback ICD)";
            cfg.vendor_id = 0x1AE0U;
            cfg.device_id = 0x0001U;
        }
        (void)loader->load_mock_icd(cfg);
        (void)loader->probe_and_create_device(probe);
        if (!load_err.empty()) {
            probe.error = load_err;
        }
    } else if (!loaded) {
        probe.ok = false;
        probe.mode = mode;
        probe.error = load_err;
        if (error_out != nullptr) {
            *error_out = load_err;
        }
        return probe;
    }

    // Build a synthetic static `.brf` textured quad + valid 4x4 BC1 `.dds` image.
    wse::asset::BrfMesh mesh{};
    mesh.name = "p4_smoke_quad";
    mesh.positions = {
        {-0.6f, -0.6f, 0.0f},
        {0.6f, -0.6f, 0.0f},
        {0.6f, 0.6f, 0.0f},
        {-0.6f, 0.6f, 0.0f},
    };
    const std::array<std::uint32_t, 4> corner_colors{
        0xFFFFFFFFU, 0xFFFFE0C0U, 0xFFC0FFE0U, 0xFFC0E0FFU};
    const std::array<wse::asset::Vec2f, 4> corner_uvs{
        wse::asset::Vec2f{0.0f, 1.0f},
        wse::asset::Vec2f{1.0f, 1.0f},
        wse::asset::Vec2f{1.0f, 0.0f},
        wse::asset::Vec2f{0.0f, 0.0f},
    };
    for (std::size_t i = 0; i < 4U; ++i) {
        wse::asset::BrfFaceCorner c{};
        c.vertex_index = static_cast<std::int32_t>(i);
        c.color = corner_colors[i];
        c.normal = {0.0f, 0.0f, 1.0f};
        c.uv = corner_uvs[i];
        c.uv2 = c.uv;
        mesh.face_corners.push_back(c);
    }
    mesh.faces.push_back(wse::asset::BrfMeshFace{{0, 1, 2}});
    mesh.faces.push_back(wse::asset::BrfMeshFace{{0, 2, 3}});

    std::vector<std::byte> dds_bytes(136U, std::byte{0});
    dds_bytes[0] = std::byte{'D'};
    dds_bytes[1] = std::byte{'D'};
    dds_bytes[2] = std::byte{'S'};
    dds_bytes[3] = std::byte{' '};
    write_u32_le(dds_bytes, 4U, 124U);
    write_u32_le(dds_bytes, 8U, 0x1007U);
    write_u32_le(dds_bytes, 12U, 4U);
    write_u32_le(dds_bytes, 16U, 4U);
    write_u32_le(dds_bytes, 20U, 8U);
    write_u32_le(dds_bytes, 76U, 32U);
    write_u32_le(dds_bytes, 80U, 0x4U);  // DDPF_FOURCC
    dds_bytes[84] = std::byte{'D'};
    dds_bytes[85] = std::byte{'X'};
    dds_bytes[86] = std::byte{'T'};
    dds_bytes[87] = std::byte{'1'};
    write_u32_le(dds_bytes, 108U, 0x1000U);
    // BC1 block: color0 = 0xF800 (red), color1 = 0x07E0 (green), alternating 2-bit selector.
    write_u16_le(dds_bytes, 128U, 0xF800U);
    write_u16_le(dds_bytes, 130U, 0x07E0U);
    write_u32_le(dds_bytes, 132U, 0x55005500U);

    wse::asset::DdsImageView dds{};
    if (!wse::asset::parse_dds(dds_bytes, dds, error_out)) {
        probe.ok = false;
        if (error_out != nullptr && !error_out->empty()) {
            probe.error = *error_out;
        }
        return probe;
    }

    float mvp[16]{};
    make_orbit_mvp_matrix(1.0f, 1.8f, 0.0f, 0.0f, mvp);

    VulkanRhiDevice rhi(loader);
    std::vector<std::uint8_t> frame;
    if (!render_textured_brf_mesh_frame(rhi, mesh, dds, 64U, 64U, mvp, frame, error_out,
                                        CullMode::none)) {
        probe.ok = false;
        if (error_out != nullptr && !error_out->empty()) {
            probe.error = *error_out;
        }
        return probe;
    }
    out_frame_hash = hash_rgba8_frame(frame);
    return probe;
}

}  // namespace wse::platform
