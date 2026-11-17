// Platform seam: Runtime Vulkan driver loader (System / Turnip / PanVK / Mock ICD)
// and minimal Vulkan RHI backend (`docs/PLATFORM_PLAN.md`, slice P4).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
// and carries no PE/PDB citations.

#include "driver_loader.h"

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/platform/rhi_brf.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

// Prefer the vendored headers so host, CI, and NDK builds compile against one
// Vulkan header version (the NDK sysroot ships its own, older `vulkan.h`).
#if __has_include("../../runtime/android/third_party/vulkan/vulkan.h")
#include "../../runtime/android/third_party/vulkan/vulkan.h"
#elif __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
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
typedef VkResult(VKAPI_PTR* PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(
    std::uint32_t* pVersion);

// -----------------------------------------------------------------------------
// Cross-platform shared library helpers
// -----------------------------------------------------------------------------

#if defined(_WIN32)
struct LibHandle {
    HMODULE mod = nullptr;

    // Custom driver: `path` is UTF-8 (LoadLibraryA would read it in the ANSI
    // code page and mangle non-ASCII user folders). An absolute path also lets
    // the driver's own DLL dependencies resolve from its directory.
    [[nodiscard]] bool open(const char* path, bool global_scope) noexcept {
        (void)global_scope;
        mod = nullptr;
        const int wide_len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
        if (wide_len <= 0) {
            ::SetLastError(ERROR_INVALID_NAME);
            return false;
        }
        std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
        if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), wide_len) <= 0) {
            ::SetLastError(ERROR_INVALID_NAME);
            return false;
        }
        const bool absolute = wide.size() > 2U && (wide[1] == L':' || (wide[0] == L'\\' && wide[1] == L'\\'));
        mod = ::LoadLibraryExW(wide.c_str(), nullptr, absolute ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
        return mod != nullptr;
    }

    // System Vulkan loader: search System32 only, never the application
    // directory or the current directory (DLL planting).
    [[nodiscard]] bool open_system(const char* name) noexcept {
        mod = ::LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
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

    [[nodiscard]] void* sym(const char* name) const noexcept {
        if (mod == nullptr) {
            return nullptr;
        }
        FARPROC p = ::GetProcAddress(mod, name);
        void* out = nullptr;
        static_assert(sizeof(out) == sizeof(p), "unexpected function pointer size");
        std::memcpy(&out, &p, sizeof(out));
        return out;
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
        int flags = RTLD_NOW | (global_scope ? RTLD_GLOBAL : RTLD_LOCAL);
#ifdef RTLD_NODELETE
        // Mesa drivers spawn util_queue / disk-cache threads and register exit
        // handlers; unmapping them on `close()` can crash later. Keep them mapped.
        flags |= RTLD_NODELETE;
#endif
        (void)::dlerror();  // Clear any stale error so last_error() reports this call.
        mod = ::dlopen(path, flags);
        return mod != nullptr;
    }

    // System Vulkan loader by soname (dynamic-linker search path).
    [[nodiscard]] bool open_system(const char* name) noexcept { return open(name, false); }

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

    // Raw symbol lookup (data symbols such as the Android `HMI` HAL module).
    [[nodiscard]] void* sym(const char* name) const noexcept {
        return (mod != nullptr) ? ::dlsym(mod, name) : nullptr;
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
    bool wsi_enabled = false;  // VK_KHR_surface + VK_KHR_android_surface enabled
};

struct MockDevice {
    std::uintptr_t loader_magic = kIcdLoaderMagic;
    MockPhysicalDevice* phys = nullptr;
    std::uint64_t next_id = 100U;
    std::uintptr_t queue_magic = kIcdLoaderMagic;
    std::unordered_set<std::uint64_t> live{};  // child objects not yet destroyed
};

// Process-wide mock object accounting (`mock_vulkan_object_stats`): lets tests
// prove the loader destroys every instance, device, and child object exactly once.
std::atomic<std::size_t> g_mock_live_objects{0};
std::atomic<std::size_t> g_mock_leaked_at_device_destroy{0};
std::atomic<std::size_t> g_mock_invalid_destroys{0};

// Source of `VulkanDriverLoader::device_generation` values, unique across every
// loader in the process (a move-assigned loader cannot repeat an old id).
std::atomic<std::uint64_t> g_next_device_generation{1};

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

bool mock_config_exposes_wsi(const VulkanMockConfig* cfg) noexcept {
    return cfg != nullptr && (!cfg->direct_icd || cfg->expose_android_surface_wsi);
}

constexpr std::array<const char*, 2> kMockWsiInstanceExts{"VK_KHR_surface",
                                                          "VK_KHR_android_surface"};
constexpr std::array<const char*, 1> kMockWsiDeviceExts{"VK_KHR_swapchain"};

// Standard two-call enumeration over `names` (empty when `expose` is false).
template <std::size_t N>
VkResult mock_enumerate_extensions(bool expose,
                                   const std::array<const char*, N>& names,
                                   std::uint32_t* pPropertyCount,
                                   VkExtensionProperties* pProperties) noexcept {
    const std::uint32_t total = expose ? static_cast<std::uint32_t>(N) : 0U;
    if (pProperties == nullptr) {
        *pPropertyCount = total;
        return VK_SUCCESS;
    }
    const std::uint32_t n = std::min<std::uint32_t>(*pPropertyCount, total);
    for (std::uint32_t i = 0U; i < n; ++i) {
        std::memset(&pProperties[i], 0, sizeof(VkExtensionProperties));
        const std::size_t len = std::min<std::size_t>(
            std::strlen(names[i]), VK_MAX_EXTENSION_NAME_SIZE - 1U);
        std::memcpy(pProperties[i].extensionName, names[i], len);
        pProperties[i].specVersion = 1U;
    }
    *pPropertyCount = n;
    return (n < total) ? VK_INCOMPLETE : VK_SUCCESS;
}

// VK_ERROR_EXTENSION_NOT_PRESENT unless every requested name is offered, as a
// conformant driver does. Sets `all_of_offered` when every offered name was enabled.
template <std::size_t N>
VkResult mock_check_enabled_extensions(bool expose,
                                       const std::array<const char*, N>& names,
                                       std::uint32_t count,
                                       const char* const* requested,
                                       bool& all_of_offered) noexcept {
    std::size_t matched = 0;
    for (std::uint32_t i = 0U; i < count; ++i) {
        if (requested == nullptr || requested[i] == nullptr) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        bool known = false;
        for (const char* n : names) {
            known = known || (expose && std::strcmp(n, requested[i]) == 0);
        }
        if (!known) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        ++matched;
    }
    all_of_offered = expose && matched == N;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    std::uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    if (pLayerName != nullptr) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    if (pPropertyCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return mock_enumerate_extensions(mock_config_exposes_wsi(g_tls_mock_config),
                                     kMockWsiInstanceExts, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    (void)pAllocator;
    if (pCreateInfo == nullptr || pInstance == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (g_tls_mock_config != nullptr && g_tls_mock_config->require_api_version_1_0) {
        const std::uint32_t requested_api =
            (pCreateInfo->pApplicationInfo != nullptr) ? pCreateInfo->pApplicationInfo->apiVersion
                                                       : VK_API_VERSION_1_0;
        if (requested_api != VK_API_VERSION_1_0) {
            return VK_ERROR_INCOMPATIBLE_DRIVER;
        }
    }
    if (g_tls_mock_config != nullptr && g_tls_mock_config->refuse_wsi_instance_extensions &&
        pCreateInfo->enabledExtensionCount != 0U) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    bool wsi_enabled = false;
    const VkResult ext_res = mock_check_enabled_extensions(
        mock_config_exposes_wsi(g_tls_mock_config), kMockWsiInstanceExts,
        pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames, wsi_enabled);
    if (ext_res != VK_SUCCESS) {
        return ext_res;
    }
    auto* inst = new MockInstance{};
    inst->wsi_enabled = wsi_enabled;
    g_mock_live_objects.fetch_add(1);
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
        g_mock_live_objects.fetch_sub(1);
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
    pProperties->limits.maxImageDimension2D = cfg.max_image_dimension_2d;
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

VKAPI_ATTR void VKAPI_CALL mock_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures) {
    if (physicalDevice == VK_NULL_HANDLE || pFeatures == nullptr) {
        return;
    }
    const auto* phys = reinterpret_cast<const MockPhysicalDevice*>(physicalDevice);
    std::memset(pFeatures, 0, sizeof(*pFeatures));
    pFeatures->textureCompressionBC = phys->instance->config.texture_compression_bc ? VK_TRUE : VK_FALSE;
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    std::uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    if (pLayerName != nullptr) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    if (physicalDevice == VK_NULL_HANDLE || pPropertyCount == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto* phys = reinterpret_cast<const MockPhysicalDevice*>(physicalDevice);
    return mock_enumerate_extensions(
        phys->instance->wsi_enabled && phys->instance->config.expose_swapchain, kMockWsiDeviceExts,
                                     pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    (void)pAllocator;
    if (physicalDevice == VK_NULL_HANDLE || pDevice == nullptr || pCreateInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto* phys = reinterpret_cast<const MockPhysicalDevice*>(physicalDevice);
    bool swapchain_enabled = false;
    // Device WSI extensions are offered only on an instance that enabled surface WSI.
    const VkResult ext_res = mock_check_enabled_extensions(
        phys->instance->wsi_enabled && phys->instance->config.expose_swapchain, kMockWsiDeviceExts,
        pCreateInfo->enabledExtensionCount,
        pCreateInfo->ppEnabledExtensionNames, swapchain_enabled);
    if (ext_res != VK_SUCCESS) {
        return ext_res;
    }
    auto* dev = new MockDevice{};
    g_mock_live_objects.fetch_add(1);
    dev->phys = reinterpret_cast<MockPhysicalDevice*>(physicalDevice);
    *pDevice = reinterpret_cast<VkDevice>(dev);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mock_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    (void)pAllocator;
    if (device != VK_NULL_HANDLE) {
        auto* dev = reinterpret_cast<MockDevice*>(device);
        // Vulkan requires every child object to be destroyed before its device.
        g_mock_leaked_at_device_destroy.fetch_add(dev->live.size());
        g_mock_live_objects.fetch_sub(dev->live.size() + 1U);
        delete dev;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkDeviceWaitIdle(VkDevice device) {
    return (device != VK_NULL_HANDLE) ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
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
VkHandle allocate_mock_handle(MockDevice* dev) {
    const std::uint64_t id = ++dev->next_id;
    dev->live.insert(id);
    g_mock_live_objects.fetch_add(1);
    VkHandle out{};
    static_assert(sizeof(out) <= sizeof(id), "handle exceeds uint64");
    std::memcpy(&out, &id, sizeof(out));
    return out;
}

template <typename VkHandle>
void release_mock_handle(VkDevice device, VkHandle handle) noexcept {
    if (device == VK_NULL_HANDLE || handle == VK_NULL_HANDLE) {
        return;  // destroying VK_NULL_HANDLE is a valid no-op
    }
    std::uint64_t id = 0;
    static_assert(sizeof(handle) <= sizeof(id), "handle exceeds uint64");
    std::memcpy(&id, &handle, sizeof(handle));
    auto* dev = reinterpret_cast<MockDevice*>(device);
    if (dev->live.erase(id) != 0) {
        g_mock_live_objects.fetch_sub(1);
    } else {
        g_mock_invalid_destroys.fetch_add(1);  // double destroy or foreign handle
    }
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
    (void)pAllocator;
    release_mock_handle(device, commandPool);
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
    (void)pAllocator;
    release_mock_handle(device, buffer);
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
    (void)pAllocator;
    release_mock_handle(device, image);
}

VKAPI_ATTR VkResult VKAPI_CALL mock_vkCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkShaderModule* pShaderModule) {
    (void)pAllocator;
    if (device == VK_NULL_HANDLE || pCreateInfo == nullptr || pShaderModule == nullptr ||
        pCreateInfo->pCode == nullptr || pCreateInfo->codeSize < 20U ||
        (pCreateInfo->codeSize % 4U) != 0U || pCreateInfo->pCode[0] != 0x07230203U) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *pShaderModule =
        allocate_mock_handle<VkShaderModule>(reinterpret_cast<MockDevice*>(device));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mock_vkDestroyShaderModule(
    VkDevice device,
    VkShaderModule shaderModule,
    const VkAllocationCallbacks* pAllocator) {
    (void)pAllocator;
    release_mock_handle(device, shaderModule);
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
    if (name == "vkDeviceWaitIdle") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkDeviceWaitIdle);
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
    if (name == "vkCreateShaderModule") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkCreateShaderModule);
    }
    if (name == "vkDestroyShaderModule") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkDestroyShaderModule);
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
    if (name == "vkGetPhysicalDeviceFeatures") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkGetPhysicalDeviceFeatures);
    }
    if (name == "vkGetPhysicalDeviceQueueFamilyProperties") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkGetPhysicalDeviceQueueFamilyProperties);
    }
    if (name == "vkCreateDevice") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkCreateDevice);
    }
    if (name == "vkEnumerateDeviceExtensionProperties") {
        return reinterpret_cast<PFN_vkVoidFunction>(&mock_vkEnumerateDeviceExtensionProperties);
    }
    return mock_vkGetDeviceProcAddr(VK_NULL_HANDLE, pName);
}

// -----------------------------------------------------------------------------
// Android hwvulkan HAL ABI (`hardware/hardware.h`, `hardware/hwvulkan.h`).
// This is the interface Android's own `libvulkan.so` (and adrenotools) use to
// enter a vendor/Mesa driver through its exported `HMI` symbol; drivers built
// only for that path may not export `vk_icd*`.
// -----------------------------------------------------------------------------

constexpr std::uint32_t make_hal_tag(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint32_t>(a) << 24U) | (static_cast<std::uint32_t>(b) << 16U) |
           (static_cast<std::uint32_t>(c) << 8U) | static_cast<std::uint32_t>(d);
}
constexpr std::uint32_t kHardwareModuleTag = make_hal_tag('H', 'W', 'M', 'T');
constexpr std::uint32_t kHardwareDeviceTag = make_hal_tag('H', 'W', 'D', 'T');
constexpr const char* kHwVulkanModuleId = "vulkan";
constexpr const char* kHwVulkanDevice0 = "vk0";

struct HalDevice;
struct HalModule;

struct HalModuleMethods {
    int (*open)(const HalModule* module, const char* id, HalDevice** device);
};

// `hw_module_t`: 32 pointer-sized words in total (`reserved[32 - 7]`).
struct HalModule {
    std::uint32_t tag;
    std::uint16_t module_api_version;
    std::uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    HalModuleMethods* methods;
    void* dso;
    std::uintptr_t reserved[32 - 7];
};

// `hw_device_t`.
struct HalDevice {
    std::uint32_t tag;
    std::uint32_t version;
    HalModule* module;
    std::uintptr_t reserved[12];
    int (*close)(HalDevice* device);
};

// `hwvulkan_device_t`.
struct HwVulkanDevice {
    HalDevice common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

int mock_hal_close(HalDevice* device) {
    delete reinterpret_cast<HwVulkanDevice*>(device);
    return 0;
}

int mock_hal_open(const HalModule* module, const char* id, HalDevice** device);

HalModuleMethods g_mock_hal_methods{&mock_hal_open};

HalModule g_mock_hal_module{
    kHardwareModuleTag, 1U, 0U, kHwVulkanModuleId, "WSE2 mock hwvulkan", "wse2-android",
    &g_mock_hal_methods, nullptr, {}};

int mock_hal_open(const HalModule* module, const char* id, HalDevice** device) {
    if (module != &g_mock_hal_module || id == nullptr || device == nullptr ||
        std::strcmp(id, kHwVulkanDevice0) != 0) {
        return -22;  // -EINVAL
    }
    auto* dev = new HwVulkanDevice{};
    dev->common.tag = kHardwareDeviceTag;
    dev->common.version = 1U;
    dev->common.module = &g_mock_hal_module;
    dev->common.close = &mock_hal_close;
    dev->EnumerateInstanceExtensionProperties = &mock_vkEnumerateInstanceExtensionProperties;
    dev->CreateInstance = &mock_vkCreateInstance;
    dev->GetInstanceProcAddr = &mock_vk_icdGetInstanceProcAddr;
    *device = &dev->common;
    return 0;
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
    bool used_hal_module = false;
    std::uint32_t negotiated_icd_version = 0;
    std::string loaded_library{};
    VulkanMockConfig mock_config{};

    PFN_vkGetInstanceProcAddr gipa = nullptr;
    HalDevice* hal_device = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::uint32_t max_image_dimension_2d = 0;

    PFN_vkDestroyInstance fn_destroy_instance = nullptr;
    PFN_vkDestroyDevice fn_destroy_device = nullptr;
    PFN_vkDeviceWaitIdle fn_device_wait_idle = nullptr;
    PFN_vkDestroyCommandPool fn_destroy_command_pool = nullptr;
    PFN_vkCreateBuffer fn_create_buffer = nullptr;
    PFN_vkDestroyBuffer fn_destroy_buffer = nullptr;
    PFN_vkCreateImage fn_create_image = nullptr;
    PFN_vkDestroyImage fn_destroy_image = nullptr;
    PFN_vkCreateShaderModule fn_create_shader_module = nullptr;
    PFN_vkDestroyShaderModule fn_destroy_shader_module = nullptr;

    std::unordered_set<std::uint64_t> live_buffers{};
    std::unordered_set<std::uint64_t> live_images{};
    std::unordered_set<std::uint64_t> live_shader_modules{};
    // Process-unique id of the current `VkDevice` (0 when there is none); see
    // `VulkanDriverLoader::device_generation`.
    std::uint64_t device_generation = 0;
    VulkanDriverProbeResult probe{};

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    // Releasing an `Impl` (destructor or move-assignment of the owning loader)
    // must tear down its Vulkan objects and library handles.
    ~Impl() { unload(); }

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
            // No GPU work may still reference an object we are about to destroy.
            if (fn_device_wait_idle != nullptr) {
                (void)fn_device_wait_idle(device);
            }
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
            for (const std::uint64_t h : live_shader_modules) {
                if (fn_destroy_shader_module != nullptr) {
                    fn_destroy_shader_module(
                        device, u64_to_handle<VkShaderModule>(h), nullptr);
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
        live_shader_modules.clear();
        command_pool = VK_NULL_HANDLE;
        graphics_queue = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        device_generation = 0;
        physical_device = VK_NULL_HANDLE;
        max_image_dimension_2d = 0;

        if (instance != VK_NULL_HANDLE && fn_destroy_instance != nullptr) {
            fn_destroy_instance(instance, nullptr);
        }
        instance = VK_NULL_HANDLE;

        fn_destroy_instance = nullptr;
        fn_destroy_device = nullptr;
        fn_device_wait_idle = nullptr;
        fn_destroy_command_pool = nullptr;
        fn_create_buffer = nullptr;
        fn_destroy_buffer = nullptr;
        fn_create_image = nullptr;
        fn_destroy_image = nullptr;
        fn_create_shader_module = nullptr;
        fn_destroy_shader_module = nullptr;
    }

    void unload() noexcept {
        destroy_vulkan_objects();
        if (hal_device != nullptr && hal_device->close != nullptr) {
            (void)hal_device->close(hal_device);
        }
        hal_device = nullptr;
        icd_lib.close();
        hw_stub_lib.close();
        loaded = false;
        is_mock = false;
        used_direct_icd = false;
        used_hal_module = false;
        negotiated_icd_version = 0;
        loaded_library.clear();
        gipa = nullptr;
        probe = VulkanDriverProbeResult{};
    }

    // Opens `HWVULKAN_DEVICE_0` on an `HMI` hwvulkan module and adopts its
    // `GetInstanceProcAddr` as the instance-level entry point.
    [[nodiscard]] bool enter_hal_module(const void* hmi, std::string& err) noexcept {
        if (hmi == nullptr) {
            err = "HAL module pointer is null";
            return false;
        }
        const auto* module = static_cast<const HalModule*>(hmi);
        if (module->tag != kHardwareModuleTag) {
            err = "HMI symbol is not a hw_module_t (bad tag)";
            return false;
        }
        if (module->id == nullptr || std::strcmp(module->id, kHwVulkanModuleId) != 0) {
            err = "HMI module id is not \"vulkan\"";
            return false;
        }
        if (module->methods == nullptr || module->methods->open == nullptr) {
            err = "HMI module has no open() method";
            return false;
        }
        HalDevice* dev = nullptr;
        const int rc = module->methods->open(module, kHwVulkanDevice0, &dev);
        if (rc != 0 || dev == nullptr) {
            err = "hwvulkan open(\"vk0\") failed with " + std::to_string(rc);
            return false;
        }
        const auto* vk_dev = reinterpret_cast<const HwVulkanDevice*>(dev);
        if (dev->tag != kHardwareDeviceTag || vk_dev->GetInstanceProcAddr == nullptr) {
            if (dev->close != nullptr) {
                (void)dev->close(dev);
            }
            err = "hwvulkan device is malformed (bad tag or no GetInstanceProcAddr)";
            return false;
        }
        hal_device = dev;
        gipa = vk_dev->GetInstanceProcAddr;
        used_hal_module = true;
        used_direct_icd = true;
        return true;
    }
};

VulkanDriverLoader::VulkanDriverLoader() : impl_(std::make_unique<Impl>()) {}
VulkanDriverLoader::~VulkanDriverLoader() = default;
VulkanDriverLoader::VulkanDriverLoader(VulkanDriverLoader&&) noexcept = default;
VulkanDriverLoader& VulkanDriverLoader::operator=(VulkanDriverLoader&&) noexcept = default;

namespace {

void set_err(std::string* error_out, std::string msg) {
    if (error_out != nullptr) {
        *error_out = std::move(msg);
    }
}

// Highest Vulkan version this backend asks for; newer drivers still work.
constexpr std::uint32_t kMaxRequestedApiVersion = VK_API_VERSION_1_3;

[[nodiscard]] std::uint32_t clamp_api_version(std::uint32_t available) noexcept {
    const std::uint32_t major = VK_API_VERSION_MAJOR(available);
    const std::uint32_t minor = VK_API_VERSION_MINOR(available);
    const std::uint32_t req_major = VK_API_VERSION_MAJOR(kMaxRequestedApiVersion);
    const std::uint32_t req_minor = VK_API_VERSION_MINOR(kMaxRequestedApiVersion);
    if (major == 0U) {
        return VK_API_VERSION_1_0;
    }
    if (major > req_major || (major == req_major && minor >= req_minor)) {
        return kMaxRequestedApiVersion;
    }
    return VK_MAKE_API_VERSION(0, major, minor, 0);
}

[[nodiscard]] bool set_env_if_unset(const char* name, const std::string& value) {
    if (value.empty()) {
        return false;
    }
#if defined(_WIN32)
    // GetEnvironmentVariableA avoids MSVC's C4996 on getenv under /W4 /WX.
    char probe[2]{};
    if (::GetEnvironmentVariableA(name, probe, sizeof(probe)) != 0) {
        return false;
    }
    return ::_putenv_s(name, value.c_str()) == 0;
#else
    const char* current = std::getenv(name);
    if (current != nullptr && current[0] != '\0') {
        return false;
    }
    return ::setenv(name, value.c_str(), 0) == 0;
#endif
}

}  // namespace

std::size_t apply_mesa_driver_environment(DriverMode mode, const std::string& shader_cache_dir) {
    std::size_t set = 0;
    if (mode == DriverMode::system) {
        return set;  // The system driver is not Mesa-controlled by us.
    }
    if (set_env_if_unset("MESA_SHADER_CACHE_DIR", shader_cache_dir)) {
        ++set;
    }
    if (mode == DriverMode::panvk && set_env_if_unset("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1")) {
        ++set;
    }
    return set;
}

const void* mock_vulkan_hal_module() noexcept {
    return &g_mock_hal_module;
}

MockVulkanObjectStats mock_vulkan_object_stats() noexcept {
    MockVulkanObjectStats stats{};
    stats.live_objects = g_mock_live_objects.load();
    stats.leaked_at_device_destroy = g_mock_leaked_at_device_destroy.load();
    stats.invalid_destroys = g_mock_invalid_destroys.load();
    return stats;
}

bool VulkanDriverLoader::load_system(std::string* error_out) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    unload();
    impl_->mode = DriverMode::system;

#if defined(_WIN32)
    static constexpr const char* kCandidates[] = {"vulkan-1.dll"};
#elif defined(__ANDROID__)
    // Android ships only the unversioned soname.
    static constexpr const char* kCandidates[] = {"libvulkan.so"};
#else
    static constexpr const char* kCandidates[] = {"libvulkan.so.1", "libvulkan.so"};
#endif

    std::string last_err;
    for (const char* name : kCandidates) {
        if (impl_->icd_lib.open_system(name)) {
            impl_->loaded_library = name;
            break;
        }
        last_err = LibHandle::last_error();
    }
    if (impl_->loaded_library.empty()) {
        set_err(error_out, "failed to load system Vulkan loader: " + last_err);
        return false;
    }

    impl_->icd_lib.load_sym("vkGetInstanceProcAddr", impl_->gipa);
    if (impl_->gipa == nullptr) {
        set_err(error_out, impl_->loaded_library + " does not export vkGetInstanceProcAddr");
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
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    unload();
    if (icd_so_path.empty()) {
        set_err(error_out, "custom ICD path is empty");
        return false;
    }

    impl_->mode = mode;

    // Preload `libpanvk_hw.so` into the global namespace when supplied so a Mesa
    // driver whose `libhardware.so` dependency was rewritten resolves it cleanly.
    if (!hw_stub_so_path.empty()) {
        if (!impl_->hw_stub_lib.open(hw_stub_so_path.c_str(), true)) {
            set_err(error_out, "failed to preload " + hw_stub_so_path + ": " +
                                   LibHandle::last_error());
            unload();
            return false;
        }
    }

    if (!impl_->icd_lib.open(icd_so_path.c_str(), false)) {
        // dlerror() names the missing DT_NEEDED library or the ELF class/machine
        // mismatch, which is the actionable part for the user.
        set_err(error_out, "failed to open Vulkan ICD " + icd_so_path + ": " +
                               LibHandle::last_error());
        unload();
        return false;
    }
    impl_->loaded_library = icd_so_path;

    // 1. Loader-ICD interface (`vk_icd*`).
    PFN_vk_icdGetInstanceProcAddr icd_gipa = nullptr;
    PFN_vk_icdNegotiateLoaderICDInterfaceVersion icd_negotiate = nullptr;
    impl_->icd_lib.load_sym("vk_icdGetInstanceProcAddr", icd_gipa);
    impl_->icd_lib.load_sym("vk_icdNegotiateLoaderICDInterfaceVersion", icd_negotiate);

    if (icd_gipa != nullptr) {
        if (icd_negotiate != nullptr) {
            std::uint32_t ver = kMaxSupportedIcdVersion;
            const VkResult neg_res = icd_negotiate(&ver);
            if (neg_res != VK_SUCCESS) {
                set_err(error_out, "ICD refused loader interface version 5 (VkResult " +
                                       std::to_string(static_cast<int>(neg_res)) + ")");
                unload();
                return false;
            }
            // The loader-ICD contract: the ICD may only lower the offered
            // version, and 0 means no usable interface.
            if (ver == 0U || ver > kMaxSupportedIcdVersion) {
                set_err(error_out, "ICD negotiated unsupported loader interface version " +
                                       std::to_string(ver) + " (expected 1.." +
                                       std::to_string(kMaxSupportedIcdVersion) + ")");
                unload();
                return false;
            }
            impl_->negotiated_icd_version = ver;
        } else {
            impl_->negotiated_icd_version = 1U;
        }
        // Physical-device entry points come from `vk_icdGetInstanceProcAddr` on
        // the instance, which is valid for every interface version; the loader-only
        // `vk_icdGetPhysicalDeviceProcAddr` (unknown-extension dispatch) is unused.
        impl_->gipa = icd_gipa;
        impl_->used_direct_icd = true;
        impl_->loaded = true;
        return true;
    }

    // 2. Android hwvulkan HAL module (`HMI`), as Android's libvulkan enters it.
    if (const void* hmi = impl_->icd_lib.sym("HMI"); hmi != nullptr) {
        std::string hal_err;
        if (!impl_->enter_hal_module(hmi, hal_err)) {
            set_err(error_out, icd_so_path + ": " + hal_err);
            unload();
            return false;
        }
        impl_->loaded = true;
        return true;
    }

    // 3. A loader-style library exporting `vkGetInstanceProcAddr`.
    impl_->icd_lib.load_sym("vkGetInstanceProcAddr", impl_->gipa);
    if (impl_->gipa == nullptr) {
        set_err(error_out, icd_so_path +
                               " exports none of vk_icdGetInstanceProcAddr, HMI, or vkGetInstanceProcAddr");
        unload();
        return false;
    }

    impl_->used_direct_icd = false;
    impl_->loaded = true;
    return true;
}

bool VulkanDriverLoader::attach_hal_module(DriverMode mode,
                                           const void* hw_module,
                                           std::string* error_out) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    unload();
    impl_->mode = mode;
    std::string err;
    if (!impl_->enter_hal_module(hw_module, err)) {
        set_err(error_out, std::move(err));
        unload();
        return false;
    }
    impl_->loaded_library = "hal://vulkan";
    impl_->loaded = true;
    return true;
}

bool VulkanDriverLoader::load_mock_icd(const VulkanMockConfig& config) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
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
    if (!impl_) {
        out.error = "VulkanDriverLoader was moved from";
        return false;
    }
    out.mode = impl_->mode;
    out.used_direct_icd = impl_->used_direct_icd;
    out.used_hal_module = impl_->used_hal_module;
    out.is_mock = impl_->is_mock;
    out.negotiated_icd_version = impl_->negotiated_icd_version;
    out.loaded_library = impl_->loaded_library;

    // Clears the thread-local mock config on every exit path.
    struct MockConfigScope {
        ~MockConfigScope() { g_tls_mock_config = nullptr; }
    } mock_scope;

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
        if (impl_->used_direct_icd) {
            g_tls_mock_icd_version = impl_->negotiated_icd_version;
        }
    }

    // Check instance WSI extensions to select Android Surface WSI vs offscreen
    // AHardwareBuffer bridge for custom direct-dlopen ICDs (`docs/PLATFORM_PLAN.md` §P5).
    bool has_khr_surface = false;
    bool has_android_surface = false;
    auto enum_inst_exts = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        impl_->gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    if (enum_inst_exts != nullptr) {
        std::uint32_t ext_count = 0U;
        if (enum_inst_exts(nullptr, &ext_count, nullptr) == VK_SUCCESS && ext_count > 0U) {
            std::vector<VkExtensionProperties> exts(ext_count);
            const VkResult ext_res = enum_inst_exts(nullptr, &ext_count, exts.data());
            if (ext_res == VK_SUCCESS || ext_res == VK_INCOMPLETE) {
                exts.resize(std::min<std::size_t>(exts.size(), ext_count));
                for (const VkExtensionProperties& ext : exts) {
                    const std::string_view name(ext.extensionName);
                    if (name == "VK_KHR_surface") {
                        has_khr_surface = true;
                    } else if (name == "VK_KHR_android_surface") {
                        has_android_surface = true;
                    }
                }
            }
        }
    }
    out.supports_android_surface_wsi = has_khr_surface && has_android_surface;
    out.presentation_path = (!out.used_direct_icd && out.supports_android_surface_wsi)
                                ? PresentationPath::android_surface_wsi
                                : PresentationPath::offscreen_hardware_buffer_bridge;

    auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(impl_->gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (create_instance == nullptr) {
        return fail("missing vkCreateInstance entry point");
    }

    // Ask for the newest version the instance offers (capped at 1.3): Turnip and
    // recent PanVK expose 1.3/1.4 features (dynamic rendering, synchronization2,
    // timeline semaphores) that a 1.0 instance cannot use.
    std::uint32_t instance_version = VK_API_VERSION_1_0;
    auto enum_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        impl_->gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enum_version != nullptr) {
        std::uint32_t v = 0;
        if (enum_version(&v) == VK_SUCCESS) {
            instance_version = v;
        }
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "WSE2-Android";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 1, 1, 5);
    app_info.pEngineName = "WSE2";
    app_info.engineVersion = VK_MAKE_API_VERSION(0, 1, 1, 5);
    app_info.apiVersion = clamp_api_version(instance_version);

    VkInstanceCreateInfo inst_info{};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pApplicationInfo = &app_info;
    // The surface path needs the instance WSI extensions enabled, not just offered.
    static constexpr std::array<const char*, 2> kWsiInstanceExts{"VK_KHR_surface",
                                                                 "VK_KHR_android_surface"};
    if (out.presentation_path == PresentationPath::android_surface_wsi) {
        inst_info.enabledExtensionCount = static_cast<std::uint32_t>(kWsiInstanceExts.size());
        inst_info.ppEnabledExtensionNames = kWsiInstanceExts.data();
    }

    // Both fallbacks compose regardless of which error triggers first:
    // a 1.0-only ICD that also refuses WSI returns INCOMPATIBLE_DRIVER on 1.3,
    // then EXTENSION_NOT_PRESENT on 1.0 + WSI, then SUCCEEDS on 1.0 without WSI.
    VkResult inst_res = VK_ERROR_INITIALIZATION_FAILED;
    for (int attempt = 0; attempt < 3; ++attempt) {
        impl_->instance = VK_NULL_HANDLE;
        inst_res = create_instance(&inst_info, nullptr, &impl_->instance);
        if (inst_res == VK_ERROR_EXTENSION_NOT_PRESENT && inst_info.enabledExtensionCount != 0U) {
            // Offered but refused: present through the offscreen bridge instead.
            inst_info.enabledExtensionCount = 0U;
            inst_info.ppEnabledExtensionNames = nullptr;
            out.presentation_path = PresentationPath::offscreen_hardware_buffer_bridge;
            out.presentation_note = "instance refused WSI extensions; using offscreen bridge";
            continue;
        }
        if (inst_res == VK_ERROR_INCOMPATIBLE_DRIVER && app_info.apiVersion != VK_API_VERSION_1_0) {
            // Pre-v5 ICDs may reject apiVersion > 1.0 outright; retry as 1.0.
            app_info.apiVersion = VK_API_VERSION_1_0;
            continue;
        }
        break;
    }
    if (inst_res != VK_SUCCESS || impl_->instance == VK_NULL_HANDLE) {
        impl_->instance = VK_NULL_HANDLE;
        return fail("vkCreateInstance failed with VkResult " +
                    std::to_string(static_cast<int>(inst_res)));
    }
    out.instance_api_version = app_info.apiVersion;
    out.wsi_instance_extensions_enabled =
        out.presentation_path == PresentationPath::android_surface_wsi;

    impl_->resolve_inst("vkDestroyInstance", impl_->fn_destroy_instance);
    PFN_vkEnumeratePhysicalDevices enum_phys = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_phys_props = nullptr;
    PFN_vkGetPhysicalDeviceFeatures get_phys_features = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_q_props = nullptr;
    PFN_vkCreateDevice create_dev = nullptr;
    impl_->resolve_inst("vkEnumeratePhysicalDevices", enum_phys);
    impl_->resolve_inst("vkGetPhysicalDeviceProperties", get_phys_props);
    impl_->resolve_inst("vkGetPhysicalDeviceFeatures", get_phys_features);
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
    if ((phys_res != VK_SUCCESS && phys_res != VK_INCOMPLETE) || phys_count == 0U) {
        return fail("vkEnumeratePhysicalDevices returned no usable handles");
    }
    phys_list.resize(std::min<std::size_t>(phys_list.size(), phys_count));

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

        for (std::uint32_t qi = 0; qi < q_count && qi < q_props.size(); ++qi) {
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

    VkPhysicalDeviceFeatures features{};
    if (get_phys_features != nullptr) {
        get_phys_features(impl_->physical_device, &features);
    }
    // Enable BC sampling when present so P5 can upload Warband `.dds` blocks as-is.
    VkPhysicalDeviceFeatures enabled{};
    enabled.textureCompressionBC = features.textureCompressionBC;

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
    dev_info.pEnabledFeatures = &enabled;

    // Presenting to a surface also needs VK_KHR_swapchain on the device; without it
    // the surface path is unusable, so fall back to the offscreen bridge.
    static constexpr const char* kSwapchainExt = "VK_KHR_swapchain";
    if (out.presentation_path == PresentationPath::android_surface_wsi) {
        bool has_swapchain = false;
        PFN_vkEnumerateDeviceExtensionProperties enum_dev_exts = nullptr;
        impl_->resolve_inst("vkEnumerateDeviceExtensionProperties", enum_dev_exts);
        if (enum_dev_exts != nullptr) {
            std::uint32_t n = 0U;
            if (enum_dev_exts(impl_->physical_device, nullptr, &n, nullptr) == VK_SUCCESS && n > 0U) {
                std::vector<VkExtensionProperties> dev_exts(n);
                const VkResult r = enum_dev_exts(impl_->physical_device, nullptr, &n, dev_exts.data());
                if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
                    dev_exts.resize(std::min<std::size_t>(dev_exts.size(), n));
                    for (const VkExtensionProperties& e : dev_exts) {
                        has_swapchain = has_swapchain ||
                                        std::string_view(e.extensionName) == kSwapchainExt;
                    }
                }
            }
        }
        if (has_swapchain) {
            dev_info.enabledExtensionCount = 1U;
            dev_info.ppEnabledExtensionNames = &kSwapchainExt;
            out.swapchain_extension_enabled = true;
        } else {
            out.presentation_path = PresentationPath::offscreen_hardware_buffer_bridge;
            out.presentation_note = "device lacks VK_KHR_swapchain; using offscreen bridge";
        }
    }

    const VkResult dev_res =
        create_dev(impl_->physical_device, &dev_info, nullptr, &impl_->device);
    if (dev_res != VK_SUCCESS || impl_->device == VK_NULL_HANDLE) {
        impl_->device = VK_NULL_HANDLE;
        return fail("vkCreateDevice failed with VkResult " +
                    std::to_string(static_cast<int>(dev_res)));
    }
    impl_->device_generation = g_next_device_generation.fetch_add(1);

    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    impl_->resolve_inst("vkGetDeviceProcAddr", gdpa);

    impl_->resolve_dev(gdpa, "vkDestroyDevice", impl_->fn_destroy_device);
    impl_->resolve_dev(gdpa, "vkDeviceWaitIdle", impl_->fn_device_wait_idle);
    impl_->resolve_dev(gdpa, "vkDestroyCommandPool", impl_->fn_destroy_command_pool);
    impl_->resolve_dev(gdpa, "vkCreateBuffer", impl_->fn_create_buffer);
    impl_->resolve_dev(gdpa, "vkDestroyBuffer", impl_->fn_destroy_buffer);
    impl_->resolve_dev(gdpa, "vkCreateImage", impl_->fn_create_image);
    impl_->resolve_dev(gdpa, "vkDestroyImage", impl_->fn_destroy_image);
    impl_->resolve_dev(gdpa, "vkCreateShaderModule", impl_->fn_create_shader_module);
    impl_->resolve_dev(gdpa, "vkDestroyShaderModule", impl_->fn_destroy_shader_module);

    if (impl_->fn_destroy_device == nullptr) {
        // Without vkDestroyDevice the device would leak on every unload.
        return fail("Vulkan device is missing vkDestroyDevice");
    }

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
            impl_->command_pool = VK_NULL_HANDLE;
            return fail("vkCreateCommandPool failed with VkResult " +
                        std::to_string(static_cast<int>(pool_res)));
        }
    }

    impl_->max_image_dimension_2d = chosen_props.limits.maxImageDimension2D;

    out.ok = true;
    out.api_version = chosen_props.apiVersion;
    out.driver_version = chosen_props.driverVersion;
    out.vendor_id = chosen_props.vendorID;
    out.device_id = chosen_props.deviceID;
    out.graphics_queue_family = chosen_q_family;
    out.max_image_dimension_2d = chosen_props.limits.maxImageDimension2D;
    out.texture_compression_bc = features.textureCompressionBC == VK_TRUE;
    const char* name_begin = chosen_props.deviceName;
    const char* name_end = std::find(name_begin,
                                     name_begin + VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, '\0');
    out.device_name.assign(name_begin, name_end);
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

std::uint64_t VulkanDriverLoader::device_generation() const noexcept {
    return has_device() ? impl_->device_generation : 0U;
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
    if (size_bytes == 0U) {
        // VkBufferCreateInfo::size must be > 0 (VUID-VkBufferCreateInfo-size-00912).
        set_err(error_out, "cannot create a zero-sized VkBuffer");
        return false;
    }

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = static_cast<VkDeviceSize>(size_bytes);
    // TRANSFER_DST: contents arrive through a staging copy (or vkCmdUpdateBuffer).
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    switch (usage) {
        case BufferUsage::vertex:
            info.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            break;
        case BufferUsage::index:
            info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            break;
        case BufferUsage::uniform:
            info.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            break;
    }
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buf = VK_NULL_HANDLE;
    const VkResult res = impl_->fn_create_buffer(impl_->device, &info, nullptr, &buf);
    if (res != VK_SUCCESS || buf == VK_NULL_HANDLE) {
        set_err(error_out, "vkCreateBuffer failed with VkResult " +
                               std::to_string(static_cast<int>(res)));
        return false;
    }

    out_vk_handle = handle_to_u64(buf);
    impl_->live_buffers.insert(out_vk_handle);
    return true;
}

void VulkanDriverLoader::destroy_vk_buffer(std::uint64_t vk_handle,
                                           std::uint64_t generation) noexcept {
    // A handle from an earlier device died with it; its value may since have
    // been reused by the current device for someone else's object.
    if (vk_handle == 0 || !has_device() || generation != impl_->device_generation) {
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
    if (width == 0U || height == 0U) {
        set_err(error_out, "VkImage dimensions must be non-zero");
        return false;
    }
    const std::uint32_t limit = impl_->max_image_dimension_2d;
    if (limit != 0U && (width > limit || height > limit)) {
        set_err(error_out, "texture " + std::to_string(width) + "x" + std::to_string(height) +
                               " exceeds device maxImageDimension2D " + std::to_string(limit));
        return false;
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
        set_err(error_out, "vkCreateImage failed with VkResult " +
                               std::to_string(static_cast<int>(res)));
        return false;
    }

    out_vk_handle = handle_to_u64(img);
    impl_->live_images.insert(out_vk_handle);
    return true;
}

void VulkanDriverLoader::destroy_vk_image(std::uint64_t vk_handle,
                                          std::uint64_t generation) noexcept {
    if (vk_handle == 0 || !has_device() || generation != impl_->device_generation) {
        return;
    }
    if (impl_->live_images.erase(vk_handle) > 0 && impl_->fn_destroy_image != nullptr) {
        impl_->fn_destroy_image(impl_->device, u64_to_handle<VkImage>(vk_handle), nullptr);
    }
}

bool VulkanDriverLoader::create_vk_shader_module(std::span<const std::uint32_t> spirv_words,
                                                 std::uint64_t& out_vk_handle,
                                                 std::string* error_out) {
    out_vk_handle = 0;
    if (!has_device()) {
        return true;
    }
    if (impl_->fn_create_shader_module == nullptr) {
        // Unlike buffers, a shader module cannot degrade to a no-op: a pipeline
        // would later be built without it.
        set_err(error_out, "Vulkan device does not expose vkCreateShaderModule");
        return false;
    }
    if (!is_valid_spirv_module(spirv_words)) {
        set_err(error_out, "invalid SPIR-V shader module payload");
        return false;
    }

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv_words.size() * sizeof(std::uint32_t);
    info.pCode = spirv_words.data();

    VkShaderModule mod = VK_NULL_HANDLE;
    const VkResult res =
        impl_->fn_create_shader_module(impl_->device, &info, nullptr, &mod);
    if (res != VK_SUCCESS || mod == VK_NULL_HANDLE) {
        set_err(error_out, "vkCreateShaderModule failed with VkResult " +
                               std::to_string(static_cast<int>(res)));
        return false;
    }

    out_vk_handle = handle_to_u64(mod);
    impl_->live_shader_modules.insert(out_vk_handle);
    return true;
}

void VulkanDriverLoader::destroy_vk_shader_module(std::uint64_t vk_handle,
                                                  std::uint64_t generation) noexcept {
    if (vk_handle == 0 || !has_device() || generation != impl_->device_generation) {
        return;
    }
    if (impl_->live_shader_modules.erase(vk_handle) > 0 &&
        impl_->fn_destroy_shader_module != nullptr) {
        impl_->fn_destroy_shader_module(
            impl_->device, u64_to_handle<VkShaderModule>(vk_handle), nullptr);
    }
}

std::size_t VulkanDriverLoader::live_vk_buffers() const noexcept {
    return impl_ ? impl_->live_buffers.size() : 0U;
}

std::size_t VulkanDriverLoader::live_vk_images() const noexcept {
    return impl_ ? impl_->live_images.size() : 0U;
}

std::size_t VulkanDriverLoader::live_vk_shader_modules() const noexcept {
    return impl_ ? impl_->live_shader_modules.size() : 0U;
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
        for (const auto& [h, ref] : vk_buffers_) {
            (void)h;
            loader_->destroy_vk_buffer(ref.handle, ref.generation);
        }
        for (const auto& [h, ref] : vk_textures_) {
            (void)h;
            loader_->destroy_vk_image(ref.handle, ref.generation);
        }
        for (const auto& [h, ref] : vk_shader_modules_) {
            (void)h;
            loader_->destroy_vk_shader_module(ref.handle, ref.generation);
        }
    }
    vk_buffers_.clear();
    vk_textures_.clear();
    vk_shader_modules_.clear();
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
        vk_buffers_.emplace(h, VkRef{vk_h, loader_->device_generation(), data.size(), usage});
    }
    return h;
}

bool VulkanRhiDevice::update_buffer(BufferHandle handle,
                                    std::span<const std::uint8_t> data,
                                    std::string* error_out) {
    if (!loader_ || !loader_->has_device()) {
        if (error_out != nullptr) {
            *error_out = "VulkanRhiDevice requires an initialized VulkanDriverLoader device";
        }
        return false;
    }
    // Grow the mirrored VkBuffer first so a failure leaves the buffer untouched.
    const auto it = vk_buffers_.find(handle);
    if (it != vk_buffers_.end() && data.size() > it->second.bytes) {
        std::uint64_t vk_h = 0;
        if (!loader_->create_vk_buffer(data.size(), it->second.usage, vk_h, error_out)) {
            return false;
        }
        loader_->destroy_vk_buffer(it->second.handle, it->second.generation);
        if (vk_h == 0) {
            vk_buffers_.erase(it);
        } else {
            it->second = VkRef{vk_h, loader_->device_generation(), data.size(), it->second.usage};
        }
    }
    return rasterizer_.update_buffer(handle, data, error_out);
}

void VulkanRhiDevice::destroy_buffer(BufferHandle handle) {
    const auto it = vk_buffers_.find(handle);
    if (it != vk_buffers_.end()) {
        if (loader_) {
            loader_->destroy_vk_buffer(it->second.handle, it->second.generation);
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
        vk_textures_.emplace(h, VkRef{vk_h, loader_->device_generation(), 0U, BufferUsage::vertex});
    }
    return h;
}

void VulkanRhiDevice::destroy_texture(TextureHandle handle) {
    const auto it = vk_textures_.find(handle);
    if (it != vk_textures_.end()) {
        if (loader_) {
            loader_->destroy_vk_image(it->second.handle, it->second.generation);
        }
        vk_textures_.erase(it);
    }
    rasterizer_.destroy_texture(handle);
}

ShaderModuleHandle VulkanRhiDevice::create_shader_module(const ShaderModuleDesc& desc,
                                                           std::string* error_out) {
    if (!loader_ || !loader_->has_device()) {
        if (error_out != nullptr) {
            *error_out = "VulkanRhiDevice requires an initialized VulkanDriverLoader device";
        }
        return 0;
    }
    const ShaderModuleHandle h = rasterizer_.create_shader_module(desc, error_out);
    if (h == 0) {
        return 0;
    }
    std::uint64_t vk_h = 0;
    if (!loader_->create_vk_shader_module(desc.spirv_words, vk_h, error_out)) {
        rasterizer_.destroy_shader_module(h);
        return 0;
    }
    if (vk_h != 0) {
        vk_shader_modules_.emplace(h, VkRef{vk_h, loader_->device_generation(), 0U, BufferUsage::vertex});
    }
    return h;
}

void VulkanRhiDevice::destroy_shader_module(ShaderModuleHandle handle) {
    const auto it = vk_shader_modules_.find(handle);
    if (it != vk_shader_modules_.end()) {
        if (loader_) {
            loader_->destroy_vk_shader_module(it->second.handle, it->second.generation);
        }
        vk_shader_modules_.erase(it);
    }
    rasterizer_.destroy_shader_module(handle);
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

std::size_t VulkanRhiDevice::live_vk_shader_modules() const noexcept {
    return loader_ ? loader_->live_vk_shader_modules() : 0U;
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
    std::string err;
    const auto finish_error = [&](std::string msg) -> VulkanDriverProbeResult {
        probe.ok = false;
        probe.mode = mode;
        probe.error = std::move(msg);
        set_err(error_out, probe.error);
        return probe;
    };

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

    if (!loaded) {
        if (!allow_mock_fallback) {
            return finish_error(load_err);
        }
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
        if (!loader->load_mock_icd(cfg) || !loader->probe_and_create_device(probe)) {
            return finish_error(load_err + " (mock fallback also failed: " + probe.error + ")");
        }
        probe.fell_back_to_mock = true;
        probe.fallback_reason = load_err;
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
    if (!wse::asset::parse_dds(dds_bytes, dds, &err)) {
        return finish_error("smoke-test DDS: " + err);
    }

    float mvp[16]{};
    make_orbit_mvp_matrix(1.0f, 1.8f, 0.0f, 0.0f, mvp);

    VulkanRhiDevice rhi(loader);
    std::vector<std::uint8_t> frame;
    if (!render_textured_brf_mesh_frame(rhi, mesh, dds, 64U, 64U, mvp, frame, &err,
                                        CullMode::none)) {
        return finish_error("smoke-test render: " + err);
    }
    out_frame_hash = hash_rgba8_frame(frame);
    return probe;
}

}  // namespace wse::platform
