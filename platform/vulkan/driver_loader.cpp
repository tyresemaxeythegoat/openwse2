// Platform seam: Runtime Vulkan driver loader (System / Turnip / PanVK / Mock ICD)
// and minimal Vulkan RHI backend (`docs/PLATFORM_PLAN.md`, slice P4).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.6 layout
// and carries no PE/PDB citations.

#include "driver_loader.h"

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/asset/shader.h"
#include "wse/asset/shader_compiler.h"
#include "wse/platform/rhi_brf.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
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
    VkDeviceSize non_coherent_atom_size = 1U;
    VkDeviceSize min_ubo_offset_alignment = 256U;
    bool can_execute_gpu = false;

    PFN_vkDestroyInstance fn_destroy_instance = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties fn_get_phys_mem_props = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties fn_get_phys_format_props = nullptr;
    PFN_vkDestroyDevice fn_destroy_device = nullptr;
    PFN_vkDeviceWaitIdle fn_device_wait_idle = nullptr;
    PFN_vkDestroyCommandPool fn_destroy_command_pool = nullptr;
    PFN_vkCreateBuffer fn_create_buffer = nullptr;
    PFN_vkDestroyBuffer fn_destroy_buffer = nullptr;
    PFN_vkCreateImage fn_create_image = nullptr;
    PFN_vkDestroyImage fn_destroy_image = nullptr;
    PFN_vkCreateShaderModule fn_create_shader_module = nullptr;
    PFN_vkDestroyShaderModule fn_destroy_shader_module = nullptr;

    // GPU execution procs (resolved on real devices; null on MockICD).
    PFN_vkGetBufferMemoryRequirements fn_get_buffer_mem_reqs = nullptr;
    PFN_vkGetImageMemoryRequirements fn_get_image_mem_reqs = nullptr;
    PFN_vkAllocateMemory fn_allocate_memory = nullptr;
    PFN_vkFreeMemory fn_free_memory = nullptr;
    PFN_vkBindBufferMemory fn_bind_buffer_memory = nullptr;
    PFN_vkBindImageMemory fn_bind_image_memory = nullptr;
    PFN_vkMapMemory fn_map_memory = nullptr;
    PFN_vkUnmapMemory fn_unmap_memory = nullptr;
    PFN_vkFlushMappedMemoryRanges fn_flush_mapped_memory_ranges = nullptr;
    PFN_vkInvalidateMappedMemoryRanges fn_invalidate_mapped_memory_ranges = nullptr;
    PFN_vkAllocateCommandBuffers fn_allocate_command_buffers = nullptr;
    PFN_vkFreeCommandBuffers fn_free_command_buffers = nullptr;
    PFN_vkBeginCommandBuffer fn_begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer fn_end_command_buffer = nullptr;
    PFN_vkResetCommandBuffer fn_reset_command_buffer = nullptr;
    PFN_vkQueueSubmit fn_queue_submit = nullptr;
    PFN_vkCreateFence fn_create_fence = nullptr;
    PFN_vkDestroyFence fn_destroy_fence = nullptr;
    PFN_vkWaitForFences fn_wait_for_fences = nullptr;
    PFN_vkResetFences fn_reset_fences = nullptr;
    PFN_vkCreateImageView fn_create_image_view = nullptr;
    PFN_vkDestroyImageView fn_destroy_image_view = nullptr;
    PFN_vkCreateSampler fn_create_sampler = nullptr;
    PFN_vkDestroySampler fn_destroy_sampler = nullptr;
    PFN_vkCreateRenderPass fn_create_render_pass = nullptr;
    PFN_vkDestroyRenderPass fn_destroy_render_pass = nullptr;
    PFN_vkCreateFramebuffer fn_create_framebuffer = nullptr;
    PFN_vkDestroyFramebuffer fn_destroy_framebuffer = nullptr;
    PFN_vkCreateDescriptorSetLayout fn_create_descriptor_set_layout = nullptr;
    PFN_vkDestroyDescriptorSetLayout fn_destroy_descriptor_set_layout = nullptr;
    PFN_vkCreateDescriptorPool fn_create_descriptor_pool = nullptr;
    PFN_vkDestroyDescriptorPool fn_destroy_descriptor_pool = nullptr;
    PFN_vkResetDescriptorPool fn_reset_descriptor_pool = nullptr;
    PFN_vkAllocateDescriptorSets fn_allocate_descriptor_sets = nullptr;
    PFN_vkUpdateDescriptorSets fn_update_descriptor_sets = nullptr;
    PFN_vkCreatePipelineLayout fn_create_pipeline_layout = nullptr;
    PFN_vkDestroyPipelineLayout fn_destroy_pipeline_layout = nullptr;
    PFN_vkCreateGraphicsPipelines fn_create_graphics_pipelines = nullptr;
    PFN_vkDestroyPipeline fn_destroy_pipeline = nullptr;
    PFN_vkCmdPipelineBarrier fn_cmd_pipeline_barrier = nullptr;
    PFN_vkCmdCopyBuffer fn_cmd_copy_buffer = nullptr;
    PFN_vkCmdCopyBufferToImage fn_cmd_copy_buffer_to_image = nullptr;
    PFN_vkCmdCopyImageToBuffer fn_cmd_copy_image_to_buffer = nullptr;
    PFN_vkCmdBeginRenderPass fn_cmd_begin_render_pass = nullptr;
    PFN_vkCmdEndRenderPass fn_cmd_end_render_pass = nullptr;
    PFN_vkCmdBindPipeline fn_cmd_bind_pipeline = nullptr;
    PFN_vkCmdSetViewport fn_cmd_set_viewport = nullptr;
    PFN_vkCmdSetScissor fn_cmd_set_scissor = nullptr;
    PFN_vkCmdBindDescriptorSets fn_cmd_bind_descriptor_sets = nullptr;
    PFN_vkCmdBindVertexBuffers fn_cmd_bind_vertex_buffers = nullptr;
    PFN_vkCmdBindIndexBuffer fn_cmd_bind_index_buffer = nullptr;
    PFN_vkCmdDrawIndexed fn_cmd_draw_indexed = nullptr;

    std::unordered_set<std::uint64_t> live_buffers{};
    std::unordered_set<std::uint64_t> live_images{};
    std::unordered_set<std::uint64_t> live_shader_modules{};
    std::vector<VulkanRhiDevice*> attached_rhi_devices{};
    // Process-unique id of the current `VkDevice` (0 when there is none); see
    // `VulkanDriverLoader::device_generation`.
    std::uint64_t device_generation = 0;
    VulkanDriverProbeResult probe{};

    void attach_rhi_device(VulkanRhiDevice* rhi) {
        if (rhi != nullptr &&
            std::find(attached_rhi_devices.begin(), attached_rhi_devices.end(), rhi) ==
                attached_rhi_devices.end()) {
            attached_rhi_devices.push_back(rhi);
        }
    }

    void detach_rhi_device(VulkanRhiDevice* rhi) noexcept {
        attached_rhi_devices.erase(
            std::remove(attached_rhi_devices.begin(), attached_rhi_devices.end(), rhi),
            attached_rhi_devices.end());
    }

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

    void clear_gpu_procs() noexcept {
        fn_get_phys_mem_props = nullptr;
        fn_get_phys_format_props = nullptr;
        fn_get_buffer_mem_reqs = nullptr;
        fn_get_image_mem_reqs = nullptr;
        fn_allocate_memory = nullptr;
        fn_free_memory = nullptr;
        fn_bind_buffer_memory = nullptr;
        fn_bind_image_memory = nullptr;
        fn_map_memory = nullptr;
        fn_unmap_memory = nullptr;
        fn_flush_mapped_memory_ranges = nullptr;
        fn_invalidate_mapped_memory_ranges = nullptr;
        fn_allocate_command_buffers = nullptr;
        fn_free_command_buffers = nullptr;
        fn_begin_command_buffer = nullptr;
        fn_end_command_buffer = nullptr;
        fn_reset_command_buffer = nullptr;
        fn_queue_submit = nullptr;
        fn_create_fence = nullptr;
        fn_destroy_fence = nullptr;
        fn_wait_for_fences = nullptr;
        fn_reset_fences = nullptr;
        fn_create_image_view = nullptr;
        fn_destroy_image_view = nullptr;
        fn_create_sampler = nullptr;
        fn_destroy_sampler = nullptr;
        fn_create_render_pass = nullptr;
        fn_destroy_render_pass = nullptr;
        fn_create_framebuffer = nullptr;
        fn_destroy_framebuffer = nullptr;
        fn_create_descriptor_set_layout = nullptr;
        fn_destroy_descriptor_set_layout = nullptr;
        fn_create_descriptor_pool = nullptr;
        fn_destroy_descriptor_pool = nullptr;
        fn_reset_descriptor_pool = nullptr;
        fn_allocate_descriptor_sets = nullptr;
        fn_update_descriptor_sets = nullptr;
        fn_create_pipeline_layout = nullptr;
        fn_destroy_pipeline_layout = nullptr;
        fn_create_graphics_pipelines = nullptr;
        fn_destroy_pipeline = nullptr;
        fn_cmd_pipeline_barrier = nullptr;
        fn_cmd_copy_buffer = nullptr;
        fn_cmd_copy_buffer_to_image = nullptr;
        fn_cmd_copy_image_to_buffer = nullptr;
        fn_cmd_begin_render_pass = nullptr;
        fn_cmd_end_render_pass = nullptr;
        fn_cmd_bind_pipeline = nullptr;
        fn_cmd_set_viewport = nullptr;
        fn_cmd_set_scissor = nullptr;
        fn_cmd_bind_descriptor_sets = nullptr;
        fn_cmd_bind_vertex_buffers = nullptr;
        fn_cmd_bind_index_buffer = nullptr;
        fn_cmd_draw_indexed = nullptr;
    }

    void resolve_gpu_procs(PFN_vkGetDeviceProcAddr gdpa) noexcept {
        resolve_inst("vkGetPhysicalDeviceMemoryProperties", fn_get_phys_mem_props);
        resolve_inst("vkGetPhysicalDeviceFormatProperties", fn_get_phys_format_props);
        resolve_dev(gdpa, "vkGetBufferMemoryRequirements", fn_get_buffer_mem_reqs);
        resolve_dev(gdpa, "vkGetImageMemoryRequirements", fn_get_image_mem_reqs);
        resolve_dev(gdpa, "vkAllocateMemory", fn_allocate_memory);
        resolve_dev(gdpa, "vkFreeMemory", fn_free_memory);
        resolve_dev(gdpa, "vkBindBufferMemory", fn_bind_buffer_memory);
        resolve_dev(gdpa, "vkBindImageMemory", fn_bind_image_memory);
        resolve_dev(gdpa, "vkMapMemory", fn_map_memory);
        resolve_dev(gdpa, "vkUnmapMemory", fn_unmap_memory);
        resolve_dev(gdpa, "vkFlushMappedMemoryRanges", fn_flush_mapped_memory_ranges);
        resolve_dev(gdpa, "vkInvalidateMappedMemoryRanges", fn_invalidate_mapped_memory_ranges);
        resolve_dev(gdpa, "vkAllocateCommandBuffers", fn_allocate_command_buffers);
        resolve_dev(gdpa, "vkFreeCommandBuffers", fn_free_command_buffers);
        resolve_dev(gdpa, "vkBeginCommandBuffer", fn_begin_command_buffer);
        resolve_dev(gdpa, "vkEndCommandBuffer", fn_end_command_buffer);
        resolve_dev(gdpa, "vkResetCommandBuffer", fn_reset_command_buffer);
        resolve_dev(gdpa, "vkQueueSubmit", fn_queue_submit);
        resolve_dev(gdpa, "vkCreateFence", fn_create_fence);
        resolve_dev(gdpa, "vkDestroyFence", fn_destroy_fence);
        resolve_dev(gdpa, "vkWaitForFences", fn_wait_for_fences);
        resolve_dev(gdpa, "vkResetFences", fn_reset_fences);
        resolve_dev(gdpa, "vkCreateImageView", fn_create_image_view);
        resolve_dev(gdpa, "vkDestroyImageView", fn_destroy_image_view);
        resolve_dev(gdpa, "vkCreateSampler", fn_create_sampler);
        resolve_dev(gdpa, "vkDestroySampler", fn_destroy_sampler);
        resolve_dev(gdpa, "vkCreateRenderPass", fn_create_render_pass);
        resolve_dev(gdpa, "vkDestroyRenderPass", fn_destroy_render_pass);
        resolve_dev(gdpa, "vkCreateFramebuffer", fn_create_framebuffer);
        resolve_dev(gdpa, "vkDestroyFramebuffer", fn_destroy_framebuffer);
        resolve_dev(gdpa, "vkCreateDescriptorSetLayout", fn_create_descriptor_set_layout);
        resolve_dev(gdpa, "vkDestroyDescriptorSetLayout", fn_destroy_descriptor_set_layout);
        resolve_dev(gdpa, "vkCreateDescriptorPool", fn_create_descriptor_pool);
        resolve_dev(gdpa, "vkDestroyDescriptorPool", fn_destroy_descriptor_pool);
        resolve_dev(gdpa, "vkResetDescriptorPool", fn_reset_descriptor_pool);
        resolve_dev(gdpa, "vkAllocateDescriptorSets", fn_allocate_descriptor_sets);
        resolve_dev(gdpa, "vkUpdateDescriptorSets", fn_update_descriptor_sets);
        resolve_dev(gdpa, "vkCreatePipelineLayout", fn_create_pipeline_layout);
        resolve_dev(gdpa, "vkDestroyPipelineLayout", fn_destroy_pipeline_layout);
        resolve_dev(gdpa, "vkCreateGraphicsPipelines", fn_create_graphics_pipelines);
        resolve_dev(gdpa, "vkDestroyPipeline", fn_destroy_pipeline);
        resolve_dev(gdpa, "vkCmdPipelineBarrier", fn_cmd_pipeline_barrier);
        resolve_dev(gdpa, "vkCmdCopyBuffer", fn_cmd_copy_buffer);
        resolve_dev(gdpa, "vkCmdCopyBufferToImage", fn_cmd_copy_buffer_to_image);
        resolve_dev(gdpa, "vkCmdCopyImageToBuffer", fn_cmd_copy_image_to_buffer);
        resolve_dev(gdpa, "vkCmdBeginRenderPass", fn_cmd_begin_render_pass);
        resolve_dev(gdpa, "vkCmdEndRenderPass", fn_cmd_end_render_pass);
        resolve_dev(gdpa, "vkCmdBindPipeline", fn_cmd_bind_pipeline);
        resolve_dev(gdpa, "vkCmdSetViewport", fn_cmd_set_viewport);
        resolve_dev(gdpa, "vkCmdSetScissor", fn_cmd_set_scissor);
        resolve_dev(gdpa, "vkCmdBindDescriptorSets", fn_cmd_bind_descriptor_sets);
        resolve_dev(gdpa, "vkCmdBindVertexBuffers", fn_cmd_bind_vertex_buffers);
        resolve_dev(gdpa, "vkCmdBindIndexBuffer", fn_cmd_bind_index_buffer);
        resolve_dev(gdpa, "vkCmdDrawIndexed", fn_cmd_draw_indexed);
    }

    [[nodiscard]] bool has_all_gpu_procs() const noexcept {
        return fn_get_phys_mem_props != nullptr && fn_get_phys_format_props != nullptr &&
               fn_create_buffer != nullptr && fn_destroy_buffer != nullptr &&
               fn_create_image != nullptr && fn_destroy_image != nullptr &&
               fn_create_shader_module != nullptr && fn_destroy_shader_module != nullptr &&
               fn_get_buffer_mem_reqs != nullptr && fn_get_image_mem_reqs != nullptr &&
               fn_allocate_memory != nullptr && fn_free_memory != nullptr &&
               fn_bind_buffer_memory != nullptr && fn_bind_image_memory != nullptr &&
               fn_map_memory != nullptr && fn_unmap_memory != nullptr &&
               fn_flush_mapped_memory_ranges != nullptr &&
               fn_invalidate_mapped_memory_ranges != nullptr &&
               fn_allocate_command_buffers != nullptr && fn_free_command_buffers != nullptr &&
               fn_begin_command_buffer != nullptr && fn_end_command_buffer != nullptr &&
               fn_reset_command_buffer != nullptr && fn_queue_submit != nullptr &&
               fn_create_fence != nullptr && fn_destroy_fence != nullptr &&
               fn_wait_for_fences != nullptr && fn_reset_fences != nullptr &&
               fn_create_image_view != nullptr && fn_destroy_image_view != nullptr &&
               fn_create_sampler != nullptr && fn_destroy_sampler != nullptr &&
               fn_create_render_pass != nullptr && fn_destroy_render_pass != nullptr &&
               fn_create_framebuffer != nullptr && fn_destroy_framebuffer != nullptr &&
               fn_create_descriptor_set_layout != nullptr &&
               fn_destroy_descriptor_set_layout != nullptr &&
               fn_create_descriptor_pool != nullptr && fn_destroy_descriptor_pool != nullptr &&
               fn_reset_descriptor_pool != nullptr && fn_allocate_descriptor_sets != nullptr &&
               fn_update_descriptor_sets != nullptr && fn_create_pipeline_layout != nullptr &&
               fn_destroy_pipeline_layout != nullptr && fn_create_graphics_pipelines != nullptr &&
               fn_destroy_pipeline != nullptr && fn_cmd_pipeline_barrier != nullptr &&
               fn_cmd_copy_buffer != nullptr && fn_cmd_copy_buffer_to_image != nullptr &&
               fn_cmd_copy_image_to_buffer != nullptr && fn_cmd_begin_render_pass != nullptr &&
               fn_cmd_end_render_pass != nullptr && fn_cmd_bind_pipeline != nullptr &&
               fn_cmd_set_viewport != nullptr && fn_cmd_set_scissor != nullptr &&
               fn_cmd_bind_descriptor_sets != nullptr && fn_cmd_bind_vertex_buffers != nullptr &&
               fn_cmd_bind_index_buffer != nullptr && fn_cmd_draw_indexed != nullptr;
    }

    [[nodiscard]] bool probe_gpu_execution() const noexcept {
        if (is_mock || device == VK_NULL_HANDLE || graphics_queue == VK_NULL_HANDLE ||
            command_pool == VK_NULL_HANDLE || !has_all_gpu_procs()) {
            return false;
        }
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1U;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (fn_allocate_command_buffers(device, &alloc_info, &cmd) != VK_SUCCESS ||
            cmd == VK_NULL_HANDLE) {
            return false;
        }
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        bool ok = false;
        if (fn_create_fence(device, &fence_info, nullptr, &fence) == VK_SUCCESS &&
            fence != VK_NULL_HANDLE) {
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (fn_begin_command_buffer(cmd, &begin_info) == VK_SUCCESS &&
                fn_end_command_buffer(cmd) == VK_SUCCESS) {
                VkSubmitInfo submit_info{};
                submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit_info.commandBufferCount = 1U;
                submit_info.pCommandBuffers = &cmd;
                if (fn_queue_submit(graphics_queue, 1U, &submit_info, fence) == VK_SUCCESS) {
                    ok = fn_wait_for_fences(device, 1U, &fence, VK_TRUE, 5'000'000'000ULL) ==
                         VK_SUCCESS;
                }
            }
            fn_destroy_fence(device, fence, nullptr);
        }
        fn_free_command_buffers(device, command_pool, 1U, &cmd);
        return ok;
    }

    void destroy_vulkan_objects() noexcept {
        if (!attached_rhi_devices.empty()) {
            const auto snapshot = attached_rhi_devices;
            for (VulkanRhiDevice* rhi : snapshot) {
                if (rhi != nullptr) {
                    rhi->detach_and_release_gpu_resources();
                }
            }
            attached_rhi_devices.clear();
        }
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
        non_coherent_atom_size = 1U;
        min_ubo_offset_alignment = 256U;
        can_execute_gpu = false;

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
        clear_gpu_procs();
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
    impl_->non_coherent_atom_size = std::max<VkDeviceSize>(
        1U, chosen_props.limits.nonCoherentAtomSize);
    impl_->min_ubo_offset_alignment = std::max<VkDeviceSize>(
        16U, chosen_props.limits.minUniformBufferOffsetAlignment);
    if (!impl_->is_mock) {
        impl_->resolve_gpu_procs(gdpa);
    }
    impl_->can_execute_gpu = impl_->probe_gpu_execution();

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

bool VulkanDriverLoader::can_execute_gpu() const noexcept {
    return has_device() && impl_->can_execute_gpu;
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
// VulkanRhiDevice GPU backend (memory allocator, shaders, pipelines, render pass)
// -----------------------------------------------------------------------------

namespace {

constexpr const char* kBuiltinRhiVertexGlsl = R"(#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;

layout(set = 0, binding = 0, std140) uniform DrawUniforms {
    mat4 mvp;
    mat4 model;
    vec4 normal_c0;
    vec4 normal_c1;
    vec4 normal_c2;
    vec4 tint;
    vec4 half_pixel_flags;
    vec4 lighting_params;
    vec4 spec_params;
} ubo;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_color;
layout(location = 3) out vec3 v_world_pos;

void main() {
    vec4 clip = ubo.mvp * vec4(in_pos, 1.0);
    clip.xy += ubo.half_pixel_flags.xy * clip.w;
    gl_Position = clip;
    v_uv = in_uv;
    mat3 normal_m = mat3(ubo.normal_c0.xyz, ubo.normal_c1.xyz, ubo.normal_c2.xyz);
    v_normal = normal_m * in_normal;
    v_color = in_color * ubo.tint;
    v_world_pos = (ubo.model * vec4(in_pos, 1.0)).xyz;
}
)";

constexpr const char* kBuiltinRhiFragmentGlsl = R"(#version 450

layout(constant_id = 0) const uint kAlphaTest = 0u;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_color;
layout(location = 3) in vec3 v_world_pos;

layout(set = 0, binding = 0, std140) uniform DrawUniforms {
    mat4 mvp;
    mat4 model;
    vec4 normal_c0;
    vec4 normal_c1;
    vec4 normal_c2;
    vec4 tint;
    vec4 half_pixel_flags;
    vec4 lighting_params;
    vec4 spec_params;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D u_diffuse_tex;
layout(set = 0, binding = 2) uniform sampler2D u_normal_tex;
layout(set = 0, binding = 3) uniform sampler2D u_specular_tex;

layout(location = 0) out vec4 out_color;

vec3 normalize_safe(vec3 v) {
    float len = length(v);
    return (len > 1.0e-6) ? (v / len) : vec3(0.0, 0.0, 1.0);
}

vec3 srgb_to_linear(vec3 c) {
    vec3 cl = clamp(c, 0.0, 1.0);
    vec3 lo = cl / 12.92;
    vec3 hi = pow((cl + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(cl, vec3(0.04045)));
}

void main() {
    // Evaluate screen-space derivatives before alpha discard so quad helper pixels stay valid.
    vec3 dpdx = dFdx(v_world_pos);
    vec3 dpdy = dFdy(v_world_pos);
    vec2 duvdx = dFdx(v_uv);
    vec2 duvdy = dFdy(v_uv);

    vec4 tex = texture(u_diffuse_tex, v_uv);
    vec4 vc = clamp(v_color, 0.0, 1.0);
    float src_a = tex.a * vc.a;

    if (kAlphaTest != 0u) {
        if (round(src_a * 255.0) < ubo.lighting_params.w) {
            discard;
        }
    }

    vec3 rgb = tex.rgb;
    if (ubo.half_pixel_flags.z > 0.5) {
        rgb = srgb_to_linear(rgb);
    }
    rgb *= vc.rgb;

    if (ubo.lighting_params.x > 0.5) {
        vec3 n = normalize_safe(v_normal);
        if (ubo.lighting_params.y > 0.5) {
            vec3 tn = texture(u_normal_tex, v_uv).xyz * 2.0 - 1.0;
            float det = duvdx.x * duvdy.y - duvdy.x * duvdx.y;
            vec3 t = vec3(0.0);
            float handed = 1.0;
            bool have_t = false;
            if (abs(det) > 1.0e-12) {
                float inv_det = 1.0 / det;
                vec3 raw_t = (dpdx * duvdy.y - dpdy * duvdx.y) * inv_det;
                vec3 raw_b = (dpdy * duvdx.x - dpdx * duvdy.x) * inv_det;
                vec3 ortho_t = raw_t - n * dot(n, raw_t);
                float len2 = dot(ortho_t, ortho_t);
                if (len2 > 1.0e-12) {
                    t = ortho_t * inversesqrt(len2);
                    handed = (dot(cross(n, t), raw_b) < 0.0) ? -1.0 : 1.0;
                    have_t = true;
                }
            }
            if (!have_t) {
                float ux = (abs(n.y) < 0.99) ? 0.0 : 1.0;
                float uy = (abs(n.y) < 0.99) ? 1.0 : 0.0;
                t = normalize_safe(vec3(uy * n.z, -ux * n.z, ux * n.y - uy * n.x));
            }
            vec3 b = handed * cross(n, t);
            n = normalize_safe(t * tn.x + b * tn.y + n * tn.z);
        }

        const vec3 kLightDir = vec3(0.451129, 0.701757, 0.551380);
        const vec3 kHalfVec = vec3(0.256110, 0.398394, 0.880733);
        float ndotl = max(0.0, dot(n, kLightDir));
        float diffuse = 0.28 + 0.72 * ndotl;

        float spec = 0.0;
        if (ubo.spec_params.x > 0.0) {
            float ndoth = max(0.0, dot(n, kHalfVec));
            float spec_mask = (ubo.lighting_params.z > 0.5) ? texture(u_specular_tex, v_uv).r : 1.0;
            if (ndoth > 0.0) {
                spec = ubo.spec_params.x * spec_mask * pow(ndoth, max(1.0, ubo.spec_params.y));
            }
        }
        rgb = rgb * diffuse + vec3(spec);
    }

    if (ubo.half_pixel_flags.w > 0.5) {
        rgb = srgb_to_linear(rgb);
    }
    out_color = vec4(clamp(rgb, 0.0, 1.0), src_a);
}
)";

struct GpuDrawUniforms {
    float mvp[16]{};
    float model[16]{};
    float normal_c0[4]{};
    float normal_c1[4]{};
    float normal_c2[4]{};
    float tint[4]{};
    float half_pixel_flags[4]{};
    float lighting_params[4]{};
    float spec_params[4]{};
};
static_assert(sizeof(GpuDrawUniforms) == 240U, "GpuDrawUniforms must match std140 block");
static_assert(std::is_standard_layout_v<MeshVertex>, "MeshVertex must be standard-layout");
static_assert(sizeof(MeshVertex) == 36U, "MeshVertex stride must be 36 bytes");

[[nodiscard]] VkDeviceSize align_up_vk(VkDeviceSize value, VkDeviceSize align) noexcept {
    if (align <= 1U) {
        return value;
    }
    const VkDeviceSize rem = value % align;
    return (rem == 0U) ? value : (value + (align - rem));
}

[[nodiscard]] std::array<float, 9> compute_normal_matrix(const float m[16]) noexcept {
    const float a = m[0], b = m[4], c = m[8];
    const float d = m[1], e = m[5], f = m[9];
    const float g = m[2], h = m[6], i = m[10];
    const float A = e * i - f * h;
    const float B = -(d * i - f * g);
    const float C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1.0e-12f) || !std::isfinite(det)) {
        return {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
    }
    const float inv = 1.0f / det;
    const float D = -(b * i - c * h);
    const float E = a * i - c * g;
    const float F = -(a * h - b * g);
    const float G = b * f - c * e;
    const float H = -(a * f - c * d);
    const float I = a * e - b * d;
    return {A * inv, D * inv, G * inv, B * inv, E * inv, H * inv, C * inv, F * inv, I * inv};
}

struct GpuMemoryAlloc {
    std::size_t block_index = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
    bool host_coherent = false;

    [[nodiscard]] bool valid() const noexcept { return memory != VK_NULL_HANDLE; }
};

class GpuAllocator {
public:
    static constexpr VkDeviceSize kSharedBlockSize = 4ULL * 1024ULL * 1024ULL;

    void init(VulkanDriverLoader::Impl* impl) noexcept {
        impl_ = impl;
        std::memset(&mem_props_, 0, sizeof(mem_props_));
        if (impl_ != nullptr && impl_->fn_get_phys_mem_props != nullptr &&
            impl_->physical_device != VK_NULL_HANDLE) {
            impl_->fn_get_phys_mem_props(impl_->physical_device, &mem_props_);
        }
    }

    [[nodiscard]] bool alloc(const VkMemoryRequirements& reqs,
                             bool is_image,
                             VkMemoryPropertyFlags required_flags,
                             VkMemoryPropertyFlags preferred_flags,
                             GpuMemoryAlloc& out,
                             std::string* error_out) {
        out = GpuMemoryAlloc{};
        if (impl_ == nullptr || impl_->device == VK_NULL_HANDLE || reqs.size == 0U) {
            set_err(error_out, "invalid Vulkan memory allocation request");
            return false;
        }
        const std::uint32_t type_idx =
            find_memory_type(reqs.memoryTypeBits, required_flags, preferred_flags);
        if (type_idx == UINT32_MAX) {
            set_err(error_out, "no compatible Vulkan memory type found");
            return false;
        }
        const VkMemoryPropertyFlags type_flags = mem_props_.memoryTypes[type_idx].propertyFlags;
        const bool host_visible = (type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
        const bool host_coherent = (type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;

        VkDeviceSize align = std::max<VkDeviceSize>(1U, reqs.alignment);
        VkDeviceSize alloc_size = reqs.size;
        if (host_visible && !host_coherent) {
            const VkDeviceSize atom = std::max<VkDeviceSize>(1U, impl_->non_coherent_atom_size);
            align = std::max(align, atom);
            alloc_size = align_up_vk(alloc_size, atom);
        }

        // Try suballocating from an existing compatible shared block first.
        const bool try_shared = alloc_size <= kSharedBlockSize / 2U;
        if (try_shared) {
            for (std::size_t bi = 0; bi < blocks_.size(); ++bi) {
                MemoryBlock& b = blocks_[bi];
                if (b.memory == VK_NULL_HANDLE || b.dedicated || b.type_index != type_idx ||
                    b.is_image != is_image) {
                    continue;
                }
                if (try_carve(bi, b, alloc_size, align, out)) {
                    return true;
                }
            }
        }

        // Allocate a new block (shared 4 MiB chunk or dedicated block).
        const bool dedicated = !try_shared;
        const VkDeviceSize block_size = dedicated ? alloc_size : kSharedBlockSize;
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = block_size;
        info.memoryTypeIndex = type_idx;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkResult res = impl_->fn_allocate_memory(impl_->device, &info, nullptr, &mem);
        bool actual_dedicated = dedicated;
        VkDeviceSize actual_block_size = block_size;
        if ((res != VK_SUCCESS || mem == VK_NULL_HANDLE) && !dedicated) {
            info.allocationSize = alloc_size;
            actual_dedicated = true;
            actual_block_size = alloc_size;
            res = impl_->fn_allocate_memory(impl_->device, &info, nullptr, &mem);
        }
        if (res != VK_SUCCESS || mem == VK_NULL_HANDLE) {
            set_err(error_out, "vkAllocateMemory failed with VkResult " +
                                   std::to_string(static_cast<int>(res)));
            return false;
        }

        void* mapped = nullptr;
        if (host_visible) {
            const VkResult map_res =
                impl_->fn_map_memory(impl_->device, mem, 0U, VK_WHOLE_SIZE, 0U, &mapped);
            if (map_res != VK_SUCCESS || mapped == nullptr) {
                impl_->fn_free_memory(impl_->device, mem, nullptr);
                set_err(error_out, "vkMapMemory failed with VkResult " +
                                       std::to_string(static_cast<int>(map_res)));
                return false;
            }
        }

        MemoryBlock blk{};
        blk.memory = mem;
        blk.size = actual_block_size;
        blk.type_index = type_idx;
        blk.is_image = is_image;
        blk.host_visible = host_visible;
        blk.host_coherent = host_coherent;
        blk.dedicated = actual_dedicated;
        blk.mapped = mapped;
        if (!actual_dedicated && actual_block_size > alloc_size) {
            blk.free_ranges.push_back({alloc_size, actual_block_size - alloc_size});
        }

        std::size_t bi = 0;
        if (!free_slots_.empty()) {
            bi = free_slots_.back();
            free_slots_.pop_back();
            blocks_[bi] = std::move(blk);
        } else {
            bi = blocks_.size();
            blocks_.push_back(std::move(blk));
        }

        out.block_index = bi;
        out.memory = mem;
        out.offset = 0U;
        out.size = alloc_size;
        out.mapped = mapped;
        out.host_coherent = host_coherent;
        return true;
    }

    void free(GpuMemoryAlloc& a) noexcept {
        if (!a.valid() || impl_ == nullptr || impl_->device == VK_NULL_HANDLE ||
            a.block_index >= blocks_.size()) {
            a = GpuMemoryAlloc{};
            return;
        }
        MemoryBlock& b = blocks_[a.block_index];
        if (b.memory != a.memory) {
            a = GpuMemoryAlloc{};
            return;
        }
        if (b.dedicated) {
            free_block(a.block_index);
            a = GpuMemoryAlloc{};
            return;
        }
        b.free_ranges.push_back({a.offset, a.size});
        std::sort(b.free_ranges.begin(), b.free_ranges.end(),
                  [](const Range& x, const Range& y) noexcept { return x.offset < y.offset; });
        std::size_t write = 0;
        for (std::size_t i = 0; i < b.free_ranges.size(); ++i) {
            if (write > 0 &&
                b.free_ranges[write - 1U].offset + b.free_ranges[write - 1U].size ==
                    b.free_ranges[i].offset) {
                b.free_ranges[write - 1U].size += b.free_ranges[i].size;
            } else {
                b.free_ranges[write++] = b.free_ranges[i];
            }
        }
        b.free_ranges.resize(write);

        // Keep at most one idle shared block per (type_index, is_image) to avoid churn.
        if (b.free_ranges.size() == 1U && b.free_ranges[0].offset == 0U &&
            b.free_ranges[0].size == b.size) {
            bool has_sibling = false;
            for (std::size_t j = 0; j < blocks_.size(); ++j) {
                if (j != a.block_index && blocks_[j].memory != VK_NULL_HANDLE &&
                    !blocks_[j].dedicated && blocks_[j].type_index == b.type_index &&
                    blocks_[j].is_image == b.is_image) {
                    has_sibling = true;
                    break;
                }
            }
            if (has_sibling) {
                free_block(a.block_index);
            }
        }
        a = GpuMemoryAlloc{};
    }

    void flush(const GpuMemoryAlloc& a, VkDeviceSize local_off, VkDeviceSize len) const noexcept {
        if (!a.valid() || a.host_coherent || len == 0U || impl_ == nullptr ||
            a.block_index >= blocks_.size()) {
            return;
        }
        const MemoryBlock& b = blocks_[a.block_index];
        const VkDeviceSize atom = std::max<VkDeviceSize>(1U, impl_->non_coherent_atom_size);
        const VkDeviceSize start = ((a.offset + local_off) / atom) * atom;
        const VkDeviceSize raw_end = a.offset + local_off + len;
        const VkDeviceSize end = std::min(b.size, align_up_vk(raw_end, atom));
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = b.memory;
        range.offset = start;
        range.size = (end >= b.size) ? VK_WHOLE_SIZE : (end - start);
        (void)impl_->fn_flush_mapped_memory_ranges(impl_->device, 1U, &range);
    }

    void invalidate(const GpuMemoryAlloc& a, VkDeviceSize local_off, VkDeviceSize len) const noexcept {
        if (!a.valid() || a.host_coherent || len == 0U || impl_ == nullptr ||
            a.block_index >= blocks_.size()) {
            return;
        }
        const MemoryBlock& b = blocks_[a.block_index];
        const VkDeviceSize atom = std::max<VkDeviceSize>(1U, impl_->non_coherent_atom_size);
        const VkDeviceSize start = ((a.offset + local_off) / atom) * atom;
        const VkDeviceSize raw_end = a.offset + local_off + len;
        const VkDeviceSize end = std::min(b.size, align_up_vk(raw_end, atom));
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = b.memory;
        range.offset = start;
        range.size = (end >= b.size) ? VK_WHOLE_SIZE : (end - start);
        (void)impl_->fn_invalidate_mapped_memory_ranges(impl_->device, 1U, &range);
    }

    void release_all() noexcept {
        if (impl_ != nullptr && impl_->device != VK_NULL_HANDLE) {
            for (std::size_t i = 0; i < blocks_.size(); ++i) {
                free_block(i);
            }
        }
        blocks_.clear();
        free_slots_.clear();
    }

private:
    struct Range {
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;
    };

    struct MemoryBlock {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        std::uint32_t type_index = 0;
        bool is_image = false;
        bool host_visible = false;
        bool host_coherent = false;
        bool dedicated = false;
        void* mapped = nullptr;
        std::vector<Range> free_ranges{};
    };

    [[nodiscard]] std::uint32_t find_memory_type(std::uint32_t type_filter,
                                                 VkMemoryPropertyFlags required,
                                                 VkMemoryPropertyFlags preferred) const noexcept {
        const VkMemoryPropertyFlags combined = required | preferred;
        for (std::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
            if ((type_filter & (1U << i)) != 0U &&
                (mem_props_.memoryTypes[i].propertyFlags & combined) == combined) {
                return i;
            }
        }
        for (std::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
            if ((type_filter & (1U << i)) != 0U &&
                (mem_props_.memoryTypes[i].propertyFlags & required) == required) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    [[nodiscard]] bool try_carve(std::size_t bi,
                                 MemoryBlock& b,
                                 VkDeviceSize size,
                                 VkDeviceSize align,
                                 GpuMemoryAlloc& out) {
        for (std::size_t ri = 0; ri < b.free_ranges.size(); ++ri) {
            const Range r = b.free_ranges[ri];
            const VkDeviceSize aligned = align_up_vk(r.offset, align);
            if (aligned < r.offset || aligned > r.offset + r.size ||
                size > (r.offset + r.size) - aligned) {
                continue;
            }
            const VkDeviceSize prefix = aligned - r.offset;
            const VkDeviceSize suffix_start = aligned + size;
            const VkDeviceSize suffix = (r.offset + r.size) - suffix_start;
            b.free_ranges.erase(b.free_ranges.begin() + static_cast<std::ptrdiff_t>(ri));
            if (prefix > 0U) {
                b.free_ranges.insert(b.free_ranges.begin() + static_cast<std::ptrdiff_t>(ri),
                                     Range{r.offset, prefix});
                ++ri;
            }
            if (suffix > 0U) {
                b.free_ranges.insert(b.free_ranges.begin() + static_cast<std::ptrdiff_t>(ri),
                                     Range{suffix_start, suffix});
            }
            out.block_index = bi;
            out.memory = b.memory;
            out.offset = aligned;
            out.size = size;
            out.mapped =
                (b.mapped != nullptr) ? (static_cast<std::uint8_t*>(b.mapped) + aligned) : nullptr;
            out.host_coherent = b.host_coherent;
            return true;
        }
        return false;
    }

    void free_block(std::size_t bi) noexcept {
        MemoryBlock& b = blocks_[bi];
        if (b.memory != VK_NULL_HANDLE && impl_ != nullptr && impl_->device != VK_NULL_HANDLE) {
            if (b.mapped != nullptr && impl_->fn_unmap_memory != nullptr) {
                impl_->fn_unmap_memory(impl_->device, b.memory);
            }
            if (impl_->fn_free_memory != nullptr) {
                impl_->fn_free_memory(impl_->device, b.memory, nullptr);
            }
        }
        b = MemoryBlock{};
        free_slots_.push_back(bi);
    }

    VulkanDriverLoader::Impl* impl_ = nullptr;
    VkPhysicalDeviceMemoryProperties mem_props_{};
    std::vector<MemoryBlock> blocks_{};
    std::vector<std::size_t> free_slots_{};
};

struct PipelineKey {
    bool depth_test = true;
    bool depth_write = true;
    CullMode cull_mode = CullMode::back;
    FrontFace front_face = FrontFace::counter_clockwise;
    BlendMode blend_mode = BlendMode::opaque;
    bool alpha_test = false;
    bool srgb_pass = false;

    bool operator==(const PipelineKey& o) const noexcept {
        return depth_test == o.depth_test && depth_write == o.depth_write &&
               cull_mode == o.cull_mode && front_face == o.front_face &&
               blend_mode == o.blend_mode && alpha_test == o.alpha_test &&
               srgb_pass == o.srgb_pass;
    }
};

struct PipelineKeyHash {
    std::size_t operator()(const PipelineKey& k) const noexcept {
        std::size_t h = k.depth_test ? 1U : 0U;
        h = (h << 1U) | (k.depth_write ? 1U : 0U);
        h = (h << 2U) | static_cast<std::size_t>(k.cull_mode);
        h = (h << 1U) | (k.front_face == FrontFace::clockwise ? 1U : 0U);
        h = (h << 3U) | static_cast<std::size_t>(k.blend_mode);
        h = (h << 1U) | (k.alpha_test ? 1U : 0U);
        h = (h << 1U) | (k.srgb_pass ? 1U : 0U);
        return h;
    }
};

struct DescriptorTriple {
    TextureHandle diffuse = 0;
    TextureHandle normal = 0;
    TextureHandle specular = 0;

    bool operator==(const DescriptorTriple& o) const noexcept {
        return diffuse == o.diffuse && normal == o.normal && specular == o.specular;
    }
};

struct DescriptorTripleHash {
    std::size_t operator()(const DescriptorTriple& t) const noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        const auto mix = [&](std::uint32_t v) noexcept {
            h ^= static_cast<std::uint64_t>(v);
            h *= 1099511628211ULL;
        };
        mix(t.diffuse);
        mix(t.normal);
        mix(t.specular);
        return static_cast<std::size_t>(h);
    }
};

}  // namespace

struct VulkanRhiDevice::GpuState {
    VulkanDriverLoader* loader = nullptr;
    VulkanDriverLoader::Impl* impl = nullptr;
    std::uint64_t generation = 0;
    bool initialized = false;
    std::string init_error{};

    GpuAllocator allocator{};
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    bool supports_srgb_rt = false;

    VkCommandBuffer upload_cmd = VK_NULL_HANDLE;
    VkCommandBuffer render_cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkShaderModule builtin_vs = VK_NULL_HANDLE;
    VkShaderModule builtin_fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    std::uint32_t desc_pool_capacity = 0;
    VkRenderPass render_pass_unorm = VK_NULL_HANDLE;
    VkRenderPass render_pass_srgb = VK_NULL_HANDLE;
    VkSampler nearest_sampler = VK_NULL_HANDLE;
    VkSampler linear_sampler = VK_NULL_HANDLE;

    VkImage default_tex_img = VK_NULL_HANDLE;
    GpuMemoryAlloc default_tex_mem{};
    VkImageView default_tex_view = VK_NULL_HANDLE;

    VkBuffer staging_upload_buf = VK_NULL_HANDLE;
    GpuMemoryAlloc staging_upload_mem{};
    VkDeviceSize staging_upload_cap = 0;

    VkBuffer readback_buf = VK_NULL_HANDLE;
    GpuMemoryAlloc readback_mem{};
    VkDeviceSize readback_cap = 0;

    VkBuffer ubo_buf = VK_NULL_HANDLE;
    GpuMemoryAlloc ubo_mem{};
    VkDeviceSize ubo_cap = 0;
    VkDeviceSize ubo_stride = 256U;

    std::uint32_t target_width = 0;
    std::uint32_t target_height = 0;
    VkImage color_img = VK_NULL_HANDLE;
    GpuMemoryAlloc color_mem{};
    VkImageView color_view_unorm = VK_NULL_HANDLE;
    VkImageView color_view_srgb = VK_NULL_HANDLE;
    VkImage depth_img = VK_NULL_HANDLE;
    GpuMemoryAlloc depth_mem{};
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFramebuffer fb_unorm = VK_NULL_HANDLE;
    VkFramebuffer fb_srgb = VK_NULL_HANDLE;

    struct UserBufferMeta {
        GpuMemoryAlloc mem{};
        std::size_t capacity = 0;
        std::size_t logical = 0;
    };
    struct UserTextureMeta {
        GpuMemoryAlloc mem{};
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    std::unordered_map<BufferHandle, UserBufferMeta> user_buffers{};
    std::unordered_map<TextureHandle, UserTextureMeta> user_textures{};
    std::unordered_map<PipelineHandle, PipelineDesc> user_pipelines{};
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipeline_cache{};

    [[nodiscard]] bool ready() const noexcept {
        return initialized && impl != nullptr && loader != nullptr &&
               loader->device_generation() == generation && generation != 0U;
    }

    [[nodiscard]] bool init(VulkanDriverLoader* ldr, std::string* error_out) {
        loader = ldr;
        impl = (ldr != nullptr) ? ldr->impl_.get() : nullptr;
        generation = (ldr != nullptr) ? ldr->device_generation() : 0U;
        if (impl == nullptr || generation == 0U || !impl->can_execute_gpu) {
            init_error = "Vulkan device cannot execute GPU work";
            set_err(error_out, init_error);
            return false;
        }

        allocator.init(impl);
        ubo_stride = align_up_vk(sizeof(GpuDrawUniforms), impl->min_ubo_offset_alignment);

        // Probe supported depth attachment format.
        static constexpr std::array<VkFormat, 4> kDepthCandidates{
            VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
        for (const VkFormat fmt : kDepthCandidates) {
            VkFormatProperties fp{};
            impl->fn_get_phys_format_props(impl->physical_device, fmt, &fp);
            if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U) {
                depth_format = fmt;
                break;
            }
        }
        if (depth_format == VK_FORMAT_UNDEFINED) {
            init_error = "no supported Vulkan depth attachment format";
            set_err(error_out, init_error);
            release_internal_objects();
            return false;
        }

        {
            VkFormatProperties srgb_fp{};
            impl->fn_get_phys_format_props(impl->physical_device, VK_FORMAT_R8G8B8A8_SRGB, &srgb_fp);
            constexpr VkFormatFeatureFlags kNeeded =
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
            supports_srgb_rt = (srgb_fp.optimalTilingFeatures & kNeeded) == kNeeded;
        }

        // Allocate reusable upload and render command buffers + fence.
        {
            VkCommandBufferAllocateInfo cb_info{};
            cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cb_info.commandPool = impl->command_pool;
            cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cb_info.commandBufferCount = 2U;
            VkCommandBuffer cbs[2]{};
            const VkResult res =
                impl->fn_allocate_command_buffers(impl->device, &cb_info, cbs);
            if (res != VK_SUCCESS) {
                init_error = "vkAllocateCommandBuffers failed";
                set_err(error_out, init_error);
                release_internal_objects();
                return false;
            }
            upload_cmd = cbs[0];
            render_cmd = cbs[1];
        }
        {
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (impl->fn_create_fence(impl->device, &fi, nullptr, &fence) != VK_SUCCESS ||
                fence == VK_NULL_HANDLE) {
                init_error = "vkCreateFence failed";
                set_err(error_out, init_error);
                release_internal_objects();
                return false;
            }
        }

        // Compile the built-in RHI material VS/FS once via wse_shader / glslang.
        {
            const auto compiler = wse::asset::create_glslang_shader_compiler();
            if (!compiler) {
                init_error = "failed to instantiate glslang SPIR-V compiler";
                set_err(error_out, init_error);
                release_internal_objects();
                return false;
            }
            std::vector<std::uint32_t> vs_spv;
            std::vector<std::uint32_t> fs_spv;
            std::string log;
            if (!compiler->compile_vulkan_glsl_to_spirv(
                    kBuiltinRhiVertexGlsl, wse::asset::SpirvStage::Vertex, "main", vs_spv,
                    nullptr, &log)) {
                init_error = "built-in RHI vertex shader compile failed: " + log;
                set_err(error_out, init_error);
                release_internal_objects();
                return false;
            }
            if (!compiler->compile_vulkan_glsl_to_spirv(
                    kBuiltinRhiFragmentGlsl, wse::asset::SpirvStage::Fragment, "main", fs_spv,
                    nullptr, &log)) {
                init_error = "built-in RHI fragment shader compile failed: " + log;
                set_err(error_out, init_error);
                release_internal_objects();
                return false;
            }
            if (!create_raw_shader_module(vs_spv, builtin_vs, &init_error) ||
                !create_raw_shader_module(fs_spv, builtin_fs, &init_error)) {
                set_err(error_out, init_error);
                release_internal_objects();
                return false;
            }
        }

        // Descriptor set layout + pipeline layout.
        {
            std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
            bindings[0].binding = 0U;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            bindings[0].descriptorCount = 1U;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            for (std::uint32_t bi = 1U; bi <= 3U; ++bi) {
                bindings[bi].binding = bi;
                bindings[bi].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[bi].descriptorCount = 1U;
                bindings[bi].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo dsl_info{};
            dsl_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dsl_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            dsl_info.pBindings = bindings.data();
            if (impl->fn_create_descriptor_set_layout(impl->device, &dsl_info, nullptr,
                                                      &desc_set_layout) != VK_SUCCESS) {
                init_error = "vkCreateDescriptorSetLayout failed";
                set_err(error_out, init_error);
                release_internal_objects();
                return false;
            }

            VkPipelineLayoutCreateInfo pl_info{};
            pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pl_info.setLayoutCount = 1U;
            pl_info.pSetLayouts = &desc_set_layout;
            if (impl->fn_create_pipeline_layout(impl->device, &pl_info, nullptr,
                                                &pipeline_layout) != VK_SUCCESS) {
                init_error = "vkCreatePipelineLayout failed";
                set_err(error_out, init_error);
                release_internal_objects();
                return false;
            }
        }

        if (!ensure_descriptor_pool(256U, &init_error)) {
            set_err(error_out, init_error);
            release_internal_objects();
            return false;
        }

        if (!create_render_pass(VK_FORMAT_R8G8B8A8_UNORM, render_pass_unorm, &init_error)) {
            set_err(error_out, init_error);
            release_internal_objects();
            return false;
        }
        if (supports_srgb_rt &&
            !create_render_pass(VK_FORMAT_R8G8B8A8_SRGB, render_pass_srgb, &init_error)) {
            set_err(error_out, init_error);
            release_internal_objects();
            return false;
        }

        if (!create_sampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, nearest_sampler,
                            &init_error) ||
            !create_sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, linear_sampler,
                            &init_error)) {
            set_err(error_out, init_error);
            release_internal_objects();
            return false;
        }

        // 1x1 white default texture for untextured / missing normal / specular slots.
        {
            const std::array<std::uint8_t, 4> white{255U, 255U, 255U, 255U};
            if (!create_raw_image(1U, 1U, VK_FORMAT_R8G8B8A8_UNORM, 0U,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  default_tex_img, default_tex_mem, &init_error) ||
                !create_image_view(default_tex_img, VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_ASPECT_COLOR_BIT, default_tex_view, &init_error) ||
                !upload_image_rgba8(default_tex_img, 1U, 1U, white, &init_error)) {
                set_err(error_out, init_error);
                release_internal_objects();
                return false;
            }
        }

        initialized = true;
        return true;
    }

    // Tears down GPU resources in strict Vulkan dependency order.
    void release_internal_objects() noexcept {
        if (impl == nullptr || impl->device == VK_NULL_HANDLE) {
            return;
        }
        const VkDevice dev = impl->device;
        if (impl->fn_device_wait_idle != nullptr) {
            (void)impl->fn_device_wait_idle(dev);
        }

        destroy_render_targets();

        for (auto& [k, pip] : pipeline_cache) {
            (void)k;
            if (pip != VK_NULL_HANDLE && impl->fn_destroy_pipeline != nullptr) {
                impl->fn_destroy_pipeline(dev, pip, nullptr);
            }
        }
        pipeline_cache.clear();
        user_pipelines.clear();

        if (pipeline_layout != VK_NULL_HANDLE && impl->fn_destroy_pipeline_layout != nullptr) {
            impl->fn_destroy_pipeline_layout(dev, pipeline_layout, nullptr);
            pipeline_layout = VK_NULL_HANDLE;
        }
        if (desc_pool != VK_NULL_HANDLE && impl->fn_destroy_descriptor_pool != nullptr) {
            impl->fn_destroy_descriptor_pool(dev, desc_pool, nullptr);
            desc_pool = VK_NULL_HANDLE;
            desc_pool_capacity = 0U;
        }
        if (desc_set_layout != VK_NULL_HANDLE &&
            impl->fn_destroy_descriptor_set_layout != nullptr) {
            impl->fn_destroy_descriptor_set_layout(dev, desc_set_layout, nullptr);
            desc_set_layout = VK_NULL_HANDLE;
        }
        if (render_pass_unorm != VK_NULL_HANDLE && impl->fn_destroy_render_pass != nullptr) {
            impl->fn_destroy_render_pass(dev, render_pass_unorm, nullptr);
            render_pass_unorm = VK_NULL_HANDLE;
        }
        if (render_pass_srgb != VK_NULL_HANDLE && impl->fn_destroy_render_pass != nullptr) {
            impl->fn_destroy_render_pass(dev, render_pass_srgb, nullptr);
            render_pass_srgb = VK_NULL_HANDLE;
        }
        if (nearest_sampler != VK_NULL_HANDLE && impl->fn_destroy_sampler != nullptr) {
            impl->fn_destroy_sampler(dev, nearest_sampler, nullptr);
            nearest_sampler = VK_NULL_HANDLE;
        }
        if (linear_sampler != VK_NULL_HANDLE && impl->fn_destroy_sampler != nullptr) {
            impl->fn_destroy_sampler(dev, linear_sampler, nullptr);
            linear_sampler = VK_NULL_HANDLE;
        }
        if (builtin_vs != VK_NULL_HANDLE && impl->fn_destroy_shader_module != nullptr) {
            impl->fn_destroy_shader_module(dev, builtin_vs, nullptr);
            builtin_vs = VK_NULL_HANDLE;
        }
        if (builtin_fs != VK_NULL_HANDLE && impl->fn_destroy_shader_module != nullptr) {
            impl->fn_destroy_shader_module(dev, builtin_fs, nullptr);
            builtin_fs = VK_NULL_HANDLE;
        }

        if (default_tex_view != VK_NULL_HANDLE && impl->fn_destroy_image_view != nullptr) {
            impl->fn_destroy_image_view(dev, default_tex_view, nullptr);
            default_tex_view = VK_NULL_HANDLE;
        }
        if (default_tex_img != VK_NULL_HANDLE && impl->fn_destroy_image != nullptr) {
            impl->fn_destroy_image(dev, default_tex_img, nullptr);
            default_tex_img = VK_NULL_HANDLE;
        }
        allocator.free(default_tex_mem);

        if (staging_upload_buf != VK_NULL_HANDLE && impl->fn_destroy_buffer != nullptr) {
            impl->fn_destroy_buffer(dev, staging_upload_buf, nullptr);
            staging_upload_buf = VK_NULL_HANDLE;
        }
        allocator.free(staging_upload_mem);
        staging_upload_cap = 0U;

        if (readback_buf != VK_NULL_HANDLE && impl->fn_destroy_buffer != nullptr) {
            impl->fn_destroy_buffer(dev, readback_buf, nullptr);
            readback_buf = VK_NULL_HANDLE;
        }
        allocator.free(readback_mem);
        readback_cap = 0U;

        if (ubo_buf != VK_NULL_HANDLE && impl->fn_destroy_buffer != nullptr) {
            impl->fn_destroy_buffer(dev, ubo_buf, nullptr);
            ubo_buf = VK_NULL_HANDLE;
        }
        allocator.free(ubo_mem);
        ubo_cap = 0U;

        for (auto& [th, meta] : user_textures) {
            (void)th;
            if (meta.view != VK_NULL_HANDLE && impl->fn_destroy_image_view != nullptr) {
                impl->fn_destroy_image_view(dev, meta.view, nullptr);
                meta.view = VK_NULL_HANDLE;
            }
        }

        if (fence != VK_NULL_HANDLE && impl->fn_destroy_fence != nullptr) {
            impl->fn_destroy_fence(dev, fence, nullptr);
            fence = VK_NULL_HANDLE;
        }
        if (impl->command_pool != VK_NULL_HANDLE && impl->fn_free_command_buffers != nullptr) {
            std::array<VkCommandBuffer, 2> cbs{};
            std::uint32_t count = 0U;
            if (upload_cmd != VK_NULL_HANDLE) {
                cbs[count++] = upload_cmd;
                upload_cmd = VK_NULL_HANDLE;
            }
            if (render_cmd != VK_NULL_HANDLE) {
                cbs[count++] = render_cmd;
                render_cmd = VK_NULL_HANDLE;
            }
            if (count > 0U) {
                impl->fn_free_command_buffers(dev, impl->command_pool, count, cbs.data());
            }
        }
        initialized = false;
    }

    void release_user_memory_and_allocator() noexcept {
        for (auto& [bh, meta] : user_buffers) {
            (void)bh;
            allocator.free(meta.mem);
        }
        user_buffers.clear();
        for (auto& [th, meta] : user_textures) {
            (void)th;
            allocator.free(meta.mem);
        }
        user_textures.clear();
        allocator.release_all();
        generation = 0U;
        impl = nullptr;
        loader = nullptr;
    }

    [[nodiscard]] bool bind_and_upload_buffer(BufferHandle handle,
                                              VkBuffer vk_buf,
                                              std::span<const std::uint8_t> data,
                                              std::string* error_out) {
        if (!ready() || vk_buf == VK_NULL_HANDLE) {
            set_err(error_out, init_error.empty() ? "GPU state is not ready" : init_error);
            return false;
        }
        VkMemoryRequirements reqs{};
        impl->fn_get_buffer_mem_reqs(impl->device, vk_buf, &reqs);
        GpuMemoryAlloc mem{};
        if (!allocator.alloc(reqs, false, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0U, mem,
                             error_out)) {
            return false;
        }
        const VkResult bind_res =
            impl->fn_bind_buffer_memory(impl->device, vk_buf, mem.memory, mem.offset);
        if (bind_res != VK_SUCCESS) {
            allocator.free(mem);
            set_err(error_out, "vkBindBufferMemory failed with VkResult " +
                                   std::to_string(static_cast<int>(bind_res)));
            return false;
        }
        if (!upload_buffer_bytes(vk_buf, data, error_out)) {
            allocator.free(mem);
            return false;
        }
        UserBufferMeta meta{};
        meta.mem = mem;
        meta.capacity = data.size();
        meta.logical = data.size();
        user_buffers.emplace(handle, meta);
        return true;
    }

    [[nodiscard]] bool upload_buffer_bytes(VkBuffer dst_buf,
                                           std::span<const std::uint8_t> data,
                                           std::string* error_out) {
        if (!ready() || dst_buf == VK_NULL_HANDLE || data.empty()) {
            set_err(error_out, init_error.empty() ? "invalid GPU buffer upload" : init_error);
            return false;
        }
        if (!ensure_staging_upload(data.size(), error_out)) {
            return false;
        }
        std::memcpy(staging_upload_mem.mapped, data.data(), data.size());
        allocator.flush(staging_upload_mem, 0U, data.size());

        if (impl->fn_reset_command_buffer(upload_cmd, 0U) != VK_SUCCESS) {
            set_err(error_out, "vkResetCommandBuffer failed");
            return false;
        }
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (impl->fn_begin_command_buffer(upload_cmd, &begin_info) != VK_SUCCESS) {
            set_err(error_out, "vkBeginCommandBuffer failed");
            return false;
        }
        VkBufferCopy region{};
        region.srcOffset = 0U;
        region.dstOffset = 0U;
        region.size = static_cast<VkDeviceSize>(data.size());
        impl->fn_cmd_copy_buffer(upload_cmd, staging_upload_buf, dst_buf, 1U, &region);
        if (impl->fn_end_command_buffer(upload_cmd) != VK_SUCCESS) {
            set_err(error_out, "vkEndCommandBuffer failed");
            return false;
        }
        return submit_and_wait(upload_cmd, error_out);
    }

    [[nodiscard]] bool bind_and_upload_texture(TextureHandle handle,
                                               VkImage vk_img,
                                               std::uint32_t width,
                                               std::uint32_t height,
                                               std::span<const std::uint8_t> rgba8,
                                               std::string* error_out) {
        if (!ready() || vk_img == VK_NULL_HANDLE) {
            set_err(error_out, init_error.empty() ? "GPU state is not ready" : init_error);
            return false;
        }
        VkMemoryRequirements reqs{};
        impl->fn_get_image_mem_reqs(impl->device, vk_img, &reqs);
        GpuMemoryAlloc mem{};
        if (!allocator.alloc(reqs, true, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0U, mem,
                             error_out)) {
            return false;
        }
        const VkResult bind_res =
            impl->fn_bind_image_memory(impl->device, vk_img, mem.memory, mem.offset);
        if (bind_res != VK_SUCCESS) {
            allocator.free(mem);
            set_err(error_out, "vkBindImageMemory failed with VkResult " +
                                   std::to_string(static_cast<int>(bind_res)));
            return false;
        }
        VkImageView view = VK_NULL_HANDLE;
        if (!create_image_view(vk_img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                               view, error_out)) {
            allocator.free(mem);
            return false;
        }
        if (!upload_image_rgba8(vk_img, width, height, rgba8, error_out)) {
            impl->fn_destroy_image_view(impl->device, view, nullptr);
            allocator.free(mem);
            return false;
        }
        UserTextureMeta meta{};
        meta.mem = mem;
        meta.view = view;
        meta.width = width;
        meta.height = height;
        user_textures.emplace(handle, meta);
        return true;
    }

    [[nodiscard]] bool upload_image_rgba8(VkImage dst_img,
                                          std::uint32_t width,
                                          std::uint32_t height,
                                          std::span<const std::uint8_t> rgba8,
                                          std::string* error_out) {
        if (dst_img == VK_NULL_HANDLE || rgba8.empty()) {
            set_err(error_out, "invalid GPU texture upload");
            return false;
        }
        if (!ensure_staging_upload(rgba8.size(), error_out)) {
            return false;
        }
        std::memcpy(staging_upload_mem.mapped, rgba8.data(), rgba8.size());
        allocator.flush(staging_upload_mem, 0U, rgba8.size());

        if (impl->fn_reset_command_buffer(upload_cmd, 0U) != VK_SUCCESS) {
            set_err(error_out, "vkResetCommandBuffer failed");
            return false;
        }
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (impl->fn_begin_command_buffer(upload_cmd, &begin_info) != VK_SUCCESS) {
            set_err(error_out, "vkBeginCommandBuffer failed");
            return false;
        }

        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = dst_img;
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
        to_dst.srcAccessMask = 0U;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        impl->fn_cmd_pipeline_barrier(upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U,
                                      nullptr, 1U, &to_dst);

        VkBufferImageCopy region{};
        region.bufferOffset = 0U;
        region.bufferRowLength = 0U;
        region.bufferImageHeight = 0U;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1U};
        impl->fn_cmd_copy_buffer_to_image(upload_cmd, staging_upload_buf, dst_img,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);

        VkImageMemoryBarrier to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.image = dst_img;
        to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
        to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        impl->fn_cmd_pipeline_barrier(upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, nullptr, 0U,
                                      nullptr, 1U, &to_read);

        if (impl->fn_end_command_buffer(upload_cmd) != VK_SUCCESS) {
            set_err(error_out, "vkEndCommandBuffer failed");
            return false;
        }
        return submit_and_wait(upload_cmd, error_out);
    }

    [[nodiscard]] bool render_pass(
        const RenderPassDesc& pass,
        std::span<const MeshDrawCall> draws,
        const std::unordered_map<BufferHandle, VkRef>& vk_bufs,
        std::vector<std::uint8_t>& out_rgba8,
        std::string* error_out) {
        out_rgba8.clear();
        const auto fail = [&](const std::string& msg) -> bool {
            out_rgba8.clear();
            set_err(error_out, msg);
            return false;
        };
        if (!ready()) {
            return fail(init_error.empty() ? "Vulkan GPU state is not initialized" : init_error);
        }
        if (pass.width == 0U || pass.height == 0U || pass.width > 4096U ||
            pass.height > 4096U) {
            return fail("invalid render pass dimensions");
        }
        if (impl->max_image_dimension_2d != 0U &&
            (pass.width > impl->max_image_dimension_2d ||
             pass.height > impl->max_image_dimension_2d)) {
            return fail("render pass exceeds device maxImageDimension2D");
        }
        if (!(pass.clear_depth >= 0.0f && pass.clear_depth <= 1.0f)) {
            return fail("render pass clear depth must be within [0, 1]");
        }

        // Validate every draw up front so failures match SoftwareRhiDevice cleanly.
        bool pass_srgb = false;
        for (const MeshDrawCall& draw : draws) {
            const auto pip_it = user_pipelines.find(draw.pipeline);
            const auto vb_it = vk_bufs.find(draw.vertex_buffer);
            const auto ib_it = vk_bufs.find(draw.index_buffer);
            const auto vb_meta = user_buffers.find(draw.vertex_buffer);
            const auto ib_meta = user_buffers.find(draw.index_buffer);
            if (pip_it == user_pipelines.end() || vb_it == vk_bufs.end() ||
                ib_it == vk_bufs.end() || vb_meta == user_buffers.end() ||
                ib_meta == user_buffers.end()) {
                return fail("draw call references invalid RHI handle");
            }
            if (vb_it->second.usage != BufferUsage::vertex ||
                ib_it->second.usage != BufferUsage::index) {
                return fail("draw call buffer usage mismatch");
            }
            if (draw.texture != 0U && user_textures.find(draw.texture) == user_textures.end()) {
                return fail("draw call references invalid texture");
            }
            if (draw.normal_texture != 0U &&
                user_textures.find(draw.normal_texture) == user_textures.end()) {
                return fail("draw call references invalid normal texture");
            }
            if (draw.specular_texture != 0U &&
                user_textures.find(draw.specular_texture) == user_textures.end()) {
                return fail("draw call references invalid specular texture");
            }
            const std::size_t num_indices = ib_meta->second.logical / sizeof(std::uint16_t);
            if (draw.first_index > num_indices ||
                draw.index_count > num_indices - draw.first_index ||
                (draw.index_count % 3U) != 0U) {
                return fail("invalid draw index count");
            }
            if (pip_it->second.srgb_framebuffer && supports_srgb_rt) {
                pass_srgb = true;
            }
        }

        std::string err;
        if (!ensure_render_targets(pass.width, pass.height, &err)) {
            return fail(err);
        }
        const std::size_t byte_count = static_cast<std::size_t>(pass.width) * pass.height * 4U;
        if (!ensure_readback_buffer(byte_count, &err)) {
            return fail(err);
        }

        // Fill dynamic UBO for all draws and allocate deduplicated descriptor sets.
        std::vector<VkDescriptorSet> draw_desc_sets;
        if (!draws.empty()) {
            const VkDeviceSize ubo_needed = static_cast<VkDeviceSize>(draws.size()) * ubo_stride;
            if (!ensure_ubo_buffer(ubo_needed, &err)) {
                return fail(err);
            }
            std::unordered_map<DescriptorTriple, std::size_t, DescriptorTripleHash> triple_map;
            std::vector<DescriptorTriple> unique_triples;
            draw_desc_sets.reserve(draws.size());
            std::vector<std::size_t> draw_triple_idx(draws.size());

            auto* ubo_base = static_cast<std::uint8_t*>(ubo_mem.mapped);
            for (std::size_t i = 0; i < draws.size(); ++i) {
                const MeshDrawCall& draw = draws[i];
                const PipelineDesc& pip = user_pipelines.at(draw.pipeline);
                GpuDrawUniforms u{};
                std::memcpy(u.mvp, draw.mvp, sizeof(u.mvp));
                std::memcpy(u.model, draw.model, sizeof(u.model));
                const std::array<float, 9> nm = compute_normal_matrix(draw.model);
                u.normal_c0[0] = nm[0];
                u.normal_c0[1] = nm[1];
                u.normal_c0[2] = nm[2];
                u.normal_c1[0] = nm[3];
                u.normal_c1[1] = nm[4];
                u.normal_c1[2] = nm[5];
                u.normal_c2[0] = nm[6];
                u.normal_c2[1] = nm[7];
                u.normal_c2[2] = nm[8];
                u.tint[0] = static_cast<float>(draw.tint_rgba & 0xFFU) / 255.0f;
                u.tint[1] = static_cast<float>((draw.tint_rgba >> 8U) & 0xFFU) / 255.0f;
                u.tint[2] = static_cast<float>((draw.tint_rgba >> 16U) & 0xFFU) / 255.0f;
                u.tint[3] = static_cast<float>((draw.tint_rgba >> 24U) & 0xFFU) / 255.0f;

                if (pip.compensate_d3d9_half_pixel) {
                    u.half_pixel_flags[0] = 1.0f / static_cast<float>(pass.width);
                    u.half_pixel_flags[1] = 1.0f / static_cast<float>(pass.height);
                }
                u.half_pixel_flags[2] = pip.srgb_texture ? 1.0f : 0.0f;
                u.half_pixel_flags[3] = (pass_srgb && !pip.srgb_framebuffer) ? 1.0f : 0.0f;

                u.lighting_params[0] = pip.enable_lighting ? 1.0f : 0.0f;
                u.lighting_params[1] =
                    (pip.enable_lighting && pip.use_normal_map && draw.normal_texture != 0U)
                        ? 1.0f
                        : 0.0f;
                u.lighting_params[2] =
                    (pip.enable_lighting && pip.use_specular_map && draw.specular_texture != 0U)
                        ? 1.0f
                        : 0.0f;
                u.lighting_params[3] = static_cast<float>(pip.alpha_ref);

                u.spec_params[0] = pip.specular_coefficient;
                u.spec_params[1] = std::max(1.0f, pip.specular_power);

                std::memcpy(ubo_base + static_cast<std::size_t>(i) * ubo_stride, &u, sizeof(u));

                const DescriptorTriple triple{draw.texture, draw.normal_texture,
                                              draw.specular_texture};
                const auto [it, inserted] = triple_map.emplace(triple, unique_triples.size());
                if (inserted) {
                    unique_triples.push_back(triple);
                }
                draw_triple_idx[i] = it->second;
            }
            allocator.flush(ubo_mem, 0U, ubo_needed);

            const std::uint32_t n_sets = static_cast<std::uint32_t>(unique_triples.size());
            if (desc_pool_capacity < n_sets) {
                if (!ensure_descriptor_pool(n_sets * 2U, &err)) {
                    return fail(err);
                }
            } else if (impl->fn_reset_descriptor_pool(impl->device, desc_pool, 0U) != VK_SUCCESS) {
                return fail("vkResetDescriptorPool failed");
            }

            std::vector<VkDescriptorSetLayout> set_layouts(n_sets, desc_set_layout);
            std::vector<VkDescriptorSet> allocated_sets(n_sets, VK_NULL_HANDLE);
            VkDescriptorSetAllocateInfo ds_alloc{};
            ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ds_alloc.descriptorPool = desc_pool;
            ds_alloc.descriptorSetCount = n_sets;
            ds_alloc.pSetLayouts = set_layouts.data();
            if (impl->fn_allocate_descriptor_sets(impl->device, &ds_alloc,
                                                  allocated_sets.data()) != VK_SUCCESS) {
                return fail("vkAllocateDescriptorSets failed");
            }

            const VkSampler active_sampler = select_sampler(false);
            VkDescriptorBufferInfo buf_info{};
            buf_info.buffer = ubo_buf;
            buf_info.offset = 0U;
            buf_info.range = sizeof(GpuDrawUniforms);

            std::vector<VkDescriptorImageInfo> img_infos(static_cast<std::size_t>(n_sets) * 3U);
            std::vector<VkWriteDescriptorSet> writes(static_cast<std::size_t>(n_sets) * 4U);
            const auto view_for = [&](TextureHandle th) noexcept -> VkImageView {
                if (th == 0U) {
                    return default_tex_view;
                }
                const auto it = user_textures.find(th);
                return (it != user_textures.end()) ? it->second.view : default_tex_view;
            };
            for (std::size_t si = 0; si < unique_triples.size(); ++si) {
                const DescriptorTriple& t = unique_triples[si];
                const std::size_t ibase = si * 3U;
                img_infos[ibase + 0U] = {active_sampler, view_for(t.diffuse),
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                img_infos[ibase + 1U] = {active_sampler, view_for(t.normal),
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                img_infos[ibase + 2U] = {active_sampler, view_for(t.specular),
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

                const std::size_t wbase = si * 4U;
                writes[wbase + 0U] = {};
                writes[wbase + 0U].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[wbase + 0U].dstSet = allocated_sets[si];
                writes[wbase + 0U].dstBinding = 0U;
                writes[wbase + 0U].descriptorCount = 1U;
                writes[wbase + 0U].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                writes[wbase + 0U].pBufferInfo = &buf_info;
                for (std::uint32_t k = 0U; k < 3U; ++k) {
                    writes[wbase + 1U + k] = {};
                    writes[wbase + 1U + k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[wbase + 1U + k].dstSet = allocated_sets[si];
                    writes[wbase + 1U + k].dstBinding = 1U + k;
                    writes[wbase + 1U + k].descriptorCount = 1U;
                    writes[wbase + 1U + k].descriptorType =
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[wbase + 1U + k].pImageInfo = &img_infos[ibase + k];
                }
            }
            impl->fn_update_descriptor_sets(impl->device,
                                            static_cast<std::uint32_t>(writes.size()),
                                            writes.data(), 0U, nullptr);
            for (std::size_t i = 0; i < draws.size(); ++i) {
                draw_desc_sets.push_back(allocated_sets[draw_triple_idx[i]]);
            }
        }

        // Record render pass + readback copy.
        if (impl->fn_reset_command_buffer(render_cmd, 0U) != VK_SUCCESS) {
            return fail("vkResetCommandBuffer failed");
        }
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (impl->fn_begin_command_buffer(render_cmd, &begin_info) != VK_SUCCESS) {
            return fail("vkBeginCommandBuffer failed");
        }

        VkClearValue clears[2]{};
        if (pass_srgb) {
            clears[0].color.float32[0] = srgb_u8_to_linear_f32(pass.clear_r);
            clears[0].color.float32[1] = srgb_u8_to_linear_f32(pass.clear_g);
            clears[0].color.float32[2] = srgb_u8_to_linear_f32(pass.clear_b);
        } else {
            clears[0].color.float32[0] = static_cast<float>(pass.clear_r) / 255.0f;
            clears[0].color.float32[1] = static_cast<float>(pass.clear_g) / 255.0f;
            clears[0].color.float32[2] = static_cast<float>(pass.clear_b) / 255.0f;
        }
        clears[0].color.float32[3] = static_cast<float>(pass.clear_a) / 255.0f;
        clears[1].depthStencil.depth = pass.clear_depth;
        clears[1].depthStencil.stencil = 0U;

        VkRenderPassBeginInfo rp_begin{};
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.renderPass = pass_srgb ? render_pass_srgb : render_pass_unorm;
        rp_begin.framebuffer = pass_srgb ? fb_srgb : fb_unorm;
        rp_begin.renderArea = {{0, 0}, {pass.width, pass.height}};
        rp_begin.clearValueCount = 2U;
        rp_begin.pClearValues = clears;

        impl->fn_cmd_begin_render_pass(render_cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(pass.width);
        viewport.height = static_cast<float>(pass.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, {pass.width, pass.height}};
        impl->fn_cmd_set_viewport(render_cmd, 0U, 1U, &viewport);
        impl->fn_cmd_set_scissor(render_cmd, 0U, 1U, &scissor);

        VkPipeline bound_pip = VK_NULL_HANDLE;
        for (std::size_t i = 0; i < draws.size(); ++i) {
            const MeshDrawCall& draw = draws[i];
            if (draw.index_count == 0U) {
                continue;
            }
            const auto vb_meta = user_buffers.find(draw.vertex_buffer);
            if (vb_meta == user_buffers.end() || vb_meta->second.logical < sizeof(MeshVertex)) {
                continue;
            }
            const PipelineDesc& pip_desc = user_pipelines.at(draw.pipeline);
            const PipelineKey pkey{
                pip_desc.depth_test, pip_desc.depth_write, pip_desc.cull_mode,
                pip_desc.front_face, pip_desc.blend_mode,  pip_desc.alpha_test,
                pass_srgb,
            };
            VkPipeline vk_pip = VK_NULL_HANDLE;
            if (!get_or_create_pipeline(pkey, vk_pip, &err)) {
                impl->fn_cmd_end_render_pass(render_cmd);
                (void)impl->fn_end_command_buffer(render_cmd);
                return fail(err);
            }
            if (vk_pip != bound_pip) {
                impl->fn_cmd_bind_pipeline(render_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pip);
                bound_pip = vk_pip;
            }

            const std::uint32_t dyn_off = static_cast<std::uint32_t>(i * ubo_stride);
            impl->fn_cmd_bind_descriptor_sets(render_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              pipeline_layout, 0U, 1U, &draw_desc_sets[i], 1U,
                                              &dyn_off);

            const VkBuffer vb = u64_to_handle<VkBuffer>(vk_bufs.at(draw.vertex_buffer).handle);
            const VkBuffer ib = u64_to_handle<VkBuffer>(vk_bufs.at(draw.index_buffer).handle);
            const VkDeviceSize vb_off = 0U;
            impl->fn_cmd_bind_vertex_buffers(render_cmd, 0U, 1U, &vb, &vb_off);
            impl->fn_cmd_bind_index_buffer(render_cmd, ib, 0U, VK_INDEX_TYPE_UINT16);
            impl->fn_cmd_draw_indexed(render_cmd, draw.index_count, 1U, draw.first_index, 0, 0U);
        }

        impl->fn_cmd_end_render_pass(render_cmd);

        VkBufferImageCopy copy_region{};
        copy_region.bufferOffset = 0U;
        copy_region.bufferRowLength = 0U;
        copy_region.bufferImageHeight = 0U;
        copy_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
        copy_region.imageOffset = {0, 0, 0};
        copy_region.imageExtent = {pass.width, pass.height, 1U};
        impl->fn_cmd_copy_image_to_buffer(render_cmd, color_img,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buf, 1U,
                                          &copy_region);

        VkBufferMemoryBarrier buf_barrier{};
        buf_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        buf_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        buf_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        buf_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buf_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buf_barrier.buffer = readback_buf;
        buf_barrier.offset = 0U;
        buf_barrier.size = static_cast<VkDeviceSize>(byte_count);
        impl->fn_cmd_pipeline_barrier(render_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U,
                                      &buf_barrier, 0U, nullptr);

        if (impl->fn_end_command_buffer(render_cmd) != VK_SUCCESS) {
            return fail("vkEndCommandBuffer failed");
        }
        if (!submit_and_wait(render_cmd, &err)) {
            return fail(err);
        }

        allocator.invalidate(readback_mem, 0U, byte_count);
        out_rgba8.resize(byte_count);
        std::memcpy(out_rgba8.data(), readback_mem.mapped, byte_count);
        return true;
    }

private:
    // Matches `SoftwareRhiDevice::sample_texture` (nearest-neighbor); linear is created
    // alongside it for callers once texture filter state is exposed on `RhiDevice`.
    [[nodiscard]] VkSampler select_sampler(bool linear) const noexcept {
        return linear ? linear_sampler : nearest_sampler;
    }

    [[nodiscard]] bool submit_and_wait(VkCommandBuffer cmd, std::string* error_out) const {
        if (impl->fn_reset_fences(impl->device, 1U, &fence) != VK_SUCCESS) {
            set_err(error_out, "vkResetFences failed");
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &cmd;
        const VkResult sub_res =
            impl->fn_queue_submit(impl->graphics_queue, 1U, &submit, fence);
        if (sub_res != VK_SUCCESS) {
            set_err(error_out, "vkQueueSubmit failed with VkResult " +
                                   std::to_string(static_cast<int>(sub_res)));
            return false;
        }
        const VkResult wait_res =
            impl->fn_wait_for_fences(impl->device, 1U, &fence, VK_TRUE, 10'000'000'000ULL);
        if (wait_res != VK_SUCCESS) {
            set_err(error_out, "vkWaitForFences failed with VkResult " +
                                   std::to_string(static_cast<int>(wait_res)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_raw_shader_module(std::span<const std::uint32_t> spv,
                                                VkShaderModule& out_mod,
                                                std::string* error_out) const {
        out_mod = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = spv.size() * sizeof(std::uint32_t);
        info.pCode = spv.data();
        const VkResult res =
            impl->fn_create_shader_module(impl->device, &info, nullptr, &out_mod);
        if (res != VK_SUCCESS || out_mod == VK_NULL_HANDLE) {
            out_mod = VK_NULL_HANDLE;
            set_err(error_out, "vkCreateShaderModule failed with VkResult " +
                                   std::to_string(static_cast<int>(res)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_raw_buffer(VkDeviceSize size,
                                         VkBufferUsageFlags usage,
                                         VkMemoryPropertyFlags req_mem,
                                         VkMemoryPropertyFlags pref_mem,
                                         VkBuffer& out_buf,
                                         GpuMemoryAlloc& out_mem,
                                         std::string* error_out) {
        out_buf = VK_NULL_HANDLE;
        out_mem = GpuMemoryAlloc{};
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const VkResult res = impl->fn_create_buffer(impl->device, &info, nullptr, &out_buf);
        if (res != VK_SUCCESS || out_buf == VK_NULL_HANDLE) {
            out_buf = VK_NULL_HANDLE;
            set_err(error_out, "vkCreateBuffer failed with VkResult " +
                                   std::to_string(static_cast<int>(res)));
            return false;
        }
        VkMemoryRequirements reqs{};
        impl->fn_get_buffer_mem_reqs(impl->device, out_buf, &reqs);
        if (!allocator.alloc(reqs, false, req_mem, pref_mem, out_mem, error_out)) {
            impl->fn_destroy_buffer(impl->device, out_buf, nullptr);
            out_buf = VK_NULL_HANDLE;
            return false;
        }
        const VkResult bind_res = impl->fn_bind_buffer_memory(impl->device, out_buf,
                                                              out_mem.memory, out_mem.offset);
        if (bind_res != VK_SUCCESS) {
            impl->fn_destroy_buffer(impl->device, out_buf, nullptr);
            out_buf = VK_NULL_HANDLE;
            allocator.free(out_mem);
            set_err(error_out, "vkBindBufferMemory failed with VkResult " +
                                   std::to_string(static_cast<int>(bind_res)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_raw_image(std::uint32_t width,
                                        std::uint32_t height,
                                        VkFormat format,
                                        VkImageCreateFlags flags,
                                        VkImageUsageFlags usage,
                                        VkImage& out_img,
                                        GpuMemoryAlloc& out_mem,
                                        std::string* error_out) {
        out_img = VK_NULL_HANDLE;
        out_mem = GpuMemoryAlloc{};
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.flags = flags;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1U};
        info.mipLevels = 1U;
        info.arrayLayers = 1U;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        const VkResult res = impl->fn_create_image(impl->device, &info, nullptr, &out_img);
        if (res != VK_SUCCESS || out_img == VK_NULL_HANDLE) {
            out_img = VK_NULL_HANDLE;
            set_err(error_out, "vkCreateImage failed with VkResult " +
                                   std::to_string(static_cast<int>(res)));
            return false;
        }
        VkMemoryRequirements reqs{};
        impl->fn_get_image_mem_reqs(impl->device, out_img, &reqs);
        if (!allocator.alloc(reqs, true, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0U, out_mem,
                             error_out)) {
            impl->fn_destroy_image(impl->device, out_img, nullptr);
            out_img = VK_NULL_HANDLE;
            return false;
        }
        const VkResult bind_res = impl->fn_bind_image_memory(impl->device, out_img,
                                                             out_mem.memory, out_mem.offset);
        if (bind_res != VK_SUCCESS) {
            impl->fn_destroy_image(impl->device, out_img, nullptr);
            out_img = VK_NULL_HANDLE;
            allocator.free(out_mem);
            set_err(error_out, "vkBindImageMemory failed with VkResult " +
                                   std::to_string(static_cast<int>(bind_res)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_image_view(VkImage img,
                                         VkFormat format,
                                         VkImageAspectFlags aspect,
                                         VkImageView& out_view,
                                         std::string* error_out) const {
        out_view = VK_NULL_HANDLE;
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = img;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        info.subresourceRange = {aspect, 0U, 1U, 0U, 1U};
        const VkResult res = impl->fn_create_image_view(impl->device, &info, nullptr, &out_view);
        if (res != VK_SUCCESS || out_view == VK_NULL_HANDLE) {
            out_view = VK_NULL_HANDLE;
            set_err(error_out, "vkCreateImageView failed with VkResult " +
                                   std::to_string(static_cast<int>(res)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_sampler(VkFilter filter,
                                      VkSamplerMipmapMode mip_mode,
                                      VkSampler& out_sampler,
                                      std::string* error_out) const {
        out_sampler = VK_NULL_HANDLE;
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = filter;
        info.minFilter = filter;
        info.mipmapMode = mip_mode;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.maxAnisotropy = 1.0f;
        info.compareOp = VK_COMPARE_OP_ALWAYS;
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        const VkResult res = impl->fn_create_sampler(impl->device, &info, nullptr, &out_sampler);
        if (res != VK_SUCCESS || out_sampler == VK_NULL_HANDLE) {
            out_sampler = VK_NULL_HANDLE;
            set_err(error_out, "vkCreateSampler failed with VkResult " +
                                   std::to_string(static_cast<int>(res)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_render_pass(VkFormat color_format,
                                          VkRenderPass& out_rp,
                                          std::string* error_out) const {
        out_rp = VK_NULL_HANDLE;
        std::array<VkAttachmentDescription, 2> atts{};
        atts[0].format = color_format;
        atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        atts[1].format = depth_format;
        atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref{0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1U, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1U;
        subpass.pColorAttachments = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;

        std::array<VkSubpassDependency, 2> deps{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0U;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = 0U;
        deps[0].dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        deps[1].srcSubpass = 0U;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = static_cast<std::uint32_t>(atts.size());
        info.pAttachments = atts.data();
        info.subpassCount = 1U;
        info.pSubpasses = &subpass;
        info.dependencyCount = static_cast<std::uint32_t>(deps.size());
        info.pDependencies = deps.data();

        const VkResult res = impl->fn_create_render_pass(impl->device, &info, nullptr, &out_rp);
        if (res != VK_SUCCESS || out_rp == VK_NULL_HANDLE) {
            out_rp = VK_NULL_HANDLE;
            set_err(error_out, "vkCreateRenderPass failed with VkResult " +
                                   std::to_string(static_cast<int>(res)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ensure_descriptor_pool(std::uint32_t min_sets, std::string* error_out) {
        if (desc_pool != VK_NULL_HANDLE && desc_pool_capacity >= min_sets) {
            return true;
        }
        if (desc_pool != VK_NULL_HANDLE) {
            impl->fn_destroy_descriptor_pool(impl->device, desc_pool, nullptr);
            desc_pool = VK_NULL_HANDLE;
            desc_pool_capacity = 0U;
        }
        std::array<VkDescriptorPoolSize, 2> pool_sizes{};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        pool_sizes[0].descriptorCount = min_sets;
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[1].descriptorCount = min_sets * 3U;

        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.maxSets = min_sets;
        info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        info.pPoolSizes = pool_sizes.data();
        const VkResult res =
            impl->fn_create_descriptor_pool(impl->device, &info, nullptr, &desc_pool);
        if (res != VK_SUCCESS || desc_pool == VK_NULL_HANDLE) {
            desc_pool = VK_NULL_HANDLE;
            set_err(error_out, "vkCreateDescriptorPool failed with VkResult " +
                                   std::to_string(static_cast<int>(res)));
            return false;
        }
        desc_pool_capacity = min_sets;
        return true;
    }

    [[nodiscard]] bool ensure_staging_upload(std::size_t needed, std::string* error_out) {
        const VkDeviceSize want = std::max<VkDeviceSize>(needed, 64ULL * 1024ULL);
        if (staging_upload_buf != VK_NULL_HANDLE && staging_upload_cap >= needed) {
            return true;
        }
        const VkDeviceSize cap = std::max(want, staging_upload_cap * 2U);
        if (staging_upload_buf != VK_NULL_HANDLE) {
            impl->fn_destroy_buffer(impl->device, staging_upload_buf, nullptr);
            staging_upload_buf = VK_NULL_HANDLE;
        }
        allocator.free(staging_upload_mem);
        staging_upload_cap = 0U;
        if (!create_raw_buffer(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_upload_buf,
                               staging_upload_mem, error_out)) {
            return false;
        }
        staging_upload_cap = cap;
        return true;
    }

    [[nodiscard]] bool ensure_readback_buffer(std::size_t needed, std::string* error_out) {
        const VkDeviceSize want = std::max<VkDeviceSize>(needed, 64ULL * 1024ULL);
        if (readback_buf != VK_NULL_HANDLE && readback_cap >= needed) {
            return true;
        }
        if (readback_buf != VK_NULL_HANDLE) {
            impl->fn_destroy_buffer(impl->device, readback_buf, nullptr);
            readback_buf = VK_NULL_HANDLE;
        }
        allocator.free(readback_mem);
        readback_cap = 0U;
        if (!create_raw_buffer(want, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                               readback_buf, readback_mem, error_out)) {
            return false;
        }
        readback_cap = want;
        return true;
    }

    [[nodiscard]] bool ensure_ubo_buffer(VkDeviceSize needed, std::string* error_out) {
        const VkDeviceSize want = std::max<VkDeviceSize>(needed, 256ULL * ubo_stride);
        if (ubo_buf != VK_NULL_HANDLE && ubo_cap >= needed) {
            return true;
        }
        if (ubo_buf != VK_NULL_HANDLE) {
            impl->fn_destroy_buffer(impl->device, ubo_buf, nullptr);
            ubo_buf = VK_NULL_HANDLE;
        }
        allocator.free(ubo_mem);
        ubo_cap = 0U;
        if (!create_raw_buffer(want, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ubo_buf, ubo_mem,
                               error_out)) {
            return false;
        }
        ubo_cap = want;
        return true;
    }

    void destroy_render_targets() noexcept {
        if (impl == nullptr || impl->device == VK_NULL_HANDLE) {
            return;
        }
        const VkDevice dev = impl->device;
        if (fb_unorm != VK_NULL_HANDLE && impl->fn_destroy_framebuffer != nullptr) {
            impl->fn_destroy_framebuffer(dev, fb_unorm, nullptr);
            fb_unorm = VK_NULL_HANDLE;
        }
        if (fb_srgb != VK_NULL_HANDLE && impl->fn_destroy_framebuffer != nullptr) {
            impl->fn_destroy_framebuffer(dev, fb_srgb, nullptr);
            fb_srgb = VK_NULL_HANDLE;
        }
        if (color_view_unorm != VK_NULL_HANDLE && impl->fn_destroy_image_view != nullptr) {
            impl->fn_destroy_image_view(dev, color_view_unorm, nullptr);
            color_view_unorm = VK_NULL_HANDLE;
        }
        if (color_view_srgb != VK_NULL_HANDLE && impl->fn_destroy_image_view != nullptr) {
            impl->fn_destroy_image_view(dev, color_view_srgb, nullptr);
            color_view_srgb = VK_NULL_HANDLE;
        }
        if (depth_view != VK_NULL_HANDLE && impl->fn_destroy_image_view != nullptr) {
            impl->fn_destroy_image_view(dev, depth_view, nullptr);
            depth_view = VK_NULL_HANDLE;
        }
        if (color_img != VK_NULL_HANDLE && impl->fn_destroy_image != nullptr) {
            impl->fn_destroy_image(dev, color_img, nullptr);
            color_img = VK_NULL_HANDLE;
        }
        if (depth_img != VK_NULL_HANDLE && impl->fn_destroy_image != nullptr) {
            impl->fn_destroy_image(dev, depth_img, nullptr);
            depth_img = VK_NULL_HANDLE;
        }
        allocator.free(color_mem);
        allocator.free(depth_mem);
        target_width = 0U;
        target_height = 0U;
    }

    [[nodiscard]] bool ensure_render_targets(std::uint32_t width,
                                             std::uint32_t height,
                                             std::string* error_out) {
        if (target_width == width && target_height == height && fb_unorm != VK_NULL_HANDLE) {
            return true;
        }
        destroy_render_targets();

        const VkImageCreateFlags color_flags =
            supports_srgb_rt ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0U;
        if (!create_raw_image(width, height, VK_FORMAT_R8G8B8A8_UNORM, color_flags,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              color_img, color_mem, error_out) ||
            !create_image_view(color_img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                               color_view_unorm, error_out)) {
            destroy_render_targets();
            return false;
        }
        if (supports_srgb_rt &&
            !create_image_view(color_img, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT,
                               color_view_srgb, error_out)) {
            destroy_render_targets();
            return false;
        }
        if (!create_raw_image(width, height, depth_format, 0U,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth_img, depth_mem,
                              error_out) ||
            !create_image_view(depth_img, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT, depth_view,
                               error_out)) {
            destroy_render_targets();
            return false;
        }

        const auto make_fb = [&](VkRenderPass rp, VkImageView cv, VkFramebuffer& out_fb) -> bool {
            const std::array<VkImageView, 2> views{cv, depth_view};
            VkFramebufferCreateInfo fb_info{};
            fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb_info.renderPass = rp;
            fb_info.attachmentCount = static_cast<std::uint32_t>(views.size());
            fb_info.pAttachments = views.data();
            fb_info.width = width;
            fb_info.height = height;
            fb_info.layers = 1U;
            const VkResult res =
                impl->fn_create_framebuffer(impl->device, &fb_info, nullptr, &out_fb);
            if (res != VK_SUCCESS || out_fb == VK_NULL_HANDLE) {
                out_fb = VK_NULL_HANDLE;
                set_err(error_out, "vkCreateFramebuffer failed with VkResult " +
                                       std::to_string(static_cast<int>(res)));
                return false;
            }
            return true;
        };

        if (!make_fb(render_pass_unorm, color_view_unorm, fb_unorm)) {
            destroy_render_targets();
            return false;
        }
        if (supports_srgb_rt && !make_fb(render_pass_srgb, color_view_srgb, fb_srgb)) {
            destroy_render_targets();
            return false;
        }
        target_width = width;
        target_height = height;
        return true;
    }

    [[nodiscard]] bool get_or_create_pipeline(const PipelineKey& key,
                                              VkPipeline& out_pip,
                                              std::string* error_out) {
        const auto it = pipeline_cache.find(key);
        if (it != pipeline_cache.end()) {
            out_pip = it->second;
            return true;
        }

        const std::uint32_t alpha_test_val = key.alpha_test ? 1U : 0U;
        const VkSpecializationMapEntry spec_entry{0U, 0U, sizeof(std::uint32_t)};
        const VkSpecializationInfo spec_info{1U, &spec_entry, sizeof(std::uint32_t),
                                             &alpha_test_val};

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = builtin_vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = builtin_fs;
        stages[1].pName = "main";
        stages[1].pSpecializationInfo = &spec_info;

        const VkVertexInputBindingDescription binding{0U, sizeof(MeshVertex),
                                                      VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 4> attrs{{
            {0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(MeshVertex, px))},
            {1U, 0U, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(MeshVertex, nx))},
            {2U, 0U, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(MeshVertex, u))},
            {3U, 0U, VK_FORMAT_R8G8B8A8_UNORM,
             static_cast<std::uint32_t>(offsetof(MeshVertex, color_rgba))},
        }};
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1U;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrs.size());
        vi.pVertexAttributeDescriptions = attrs.data();

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1U;
        vp.scissorCount = 1U;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        switch (key.cull_mode) {
            case CullMode::none:
                rs.cullMode = VK_CULL_MODE_NONE;
                break;
            case CullMode::back:
                rs.cullMode = VK_CULL_MODE_BACK_BIT;
                break;
            case CullMode::front:
                rs.cullMode = VK_CULL_MODE_FRONT_BIT;
                break;
        }
        rs.frontFace = (key.front_face == FrontFace::clockwise)
                           ? VK_FRONT_FACE_CLOCKWISE
                           : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = (key.depth_test || key.depth_write) ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = key.depth_test ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        switch (key.blend_mode) {
            case BlendMode::opaque:
                cba.blendEnable = VK_FALSE;
                break;
            case BlendMode::alpha:
                cba.blendEnable = VK_TRUE;
                cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba.colorBlendOp = VK_BLEND_OP_ADD;
                cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba.alphaBlendOp = VK_BLEND_OP_ADD;
                break;
            case BlendMode::additive_alpha:
                cba.blendEnable = VK_TRUE;
                cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.colorBlendOp = VK_BLEND_OP_ADD;
                cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.alphaBlendOp = VK_BLEND_OP_ADD;
                break;
            case BlendMode::multiply:
                cba.blendEnable = VK_TRUE;
                cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
                cba.colorBlendOp = VK_BLEND_OP_ADD;
                cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba.alphaBlendOp = VK_BLEND_OP_ADD;
                break;
            case BlendMode::additive:
                cba.blendEnable = VK_TRUE;
                cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.colorBlendOp = VK_BLEND_OP_ADD;
                cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba.alphaBlendOp = VK_BLEND_OP_ADD;
                break;
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1U;
        cb.pAttachments = &cba;

        static constexpr std::array<VkDynamicState, 2> kDynStates{
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = static_cast<std::uint32_t>(kDynStates.size());
        dyn.pDynamicStates = kDynStates.data();

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = static_cast<std::uint32_t>(stages.size());
        gp.pStages = stages.data();
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dyn;
        gp.layout = pipeline_layout;
        gp.renderPass = key.srgb_pass ? render_pass_srgb : render_pass_unorm;
        gp.subpass = 0U;

        VkPipeline pip = VK_NULL_HANDLE;
        const VkResult res = impl->fn_create_graphics_pipelines(
            impl->device, VK_NULL_HANDLE, 1U, &gp, nullptr, &pip);
        if (res != VK_SUCCESS || pip == VK_NULL_HANDLE) {
            set_err(error_out, "vkCreateGraphicsPipelines failed with VkResult " +
                                   std::to_string(static_cast<int>(res)));
            return false;
        }
        pipeline_cache.emplace(key, pip);
        out_pip = pip;
        return true;
    }
};

// -----------------------------------------------------------------------------
// VulkanRhiDevice
// -----------------------------------------------------------------------------

VulkanRhiDevice::VulkanRhiDevice(std::shared_ptr<VulkanDriverLoader> loader)
    : loader_(std::move(loader)) {
    if (loader_ && loader_->has_device()) {
        const auto& pr = loader_->probe_result();
        rasterizer_.set_identity(pr.device_name, pr.mode, pr.api_version);
        if (loader_->impl_) {
            loader_->impl_->attach_rhi_device(this);
        }
        if (loader_->can_execute_gpu()) {
            gpu_ = std::make_unique<GpuState>();
            std::string gpu_err;
            (void)gpu_->init(loader_.get(), &gpu_err);
        }
    } else {
        rasterizer_.set_identity("Vulkan Uninitialized Device", DriverMode::system, 0U);
    }
}

void VulkanRhiDevice::detach_and_release_gpu_resources() noexcept {
    if (gpu_ && loader_ && loader_->device_generation() == gpu_->generation &&
        gpu_->generation != 0U) {
        gpu_->release_internal_objects();
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
        gpu_->release_user_memory_and_allocator();
    }
    gpu_.reset();
}

VulkanRhiDevice::~VulkanRhiDevice() {
    if (loader_ && loader_->impl_) {
        loader_->impl_->detach_rhi_device(this);
    }
    if (gpu_) {
        detach_and_release_gpu_resources();
    }
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

bool VulkanRhiDevice::last_frame_used_gpu() const noexcept {
    return last_frame_used_gpu_;
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
    if (gpu_ && !gpu_->ready()) {
        set_err(error_out, gpu_->init_error);
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
    if (gpu_) {
        if (vk_h == 0 || !gpu_->bind_and_upload_buffer(h, u64_to_handle<VkBuffer>(vk_h), data,
                                                       error_out)) {
            loader_->destroy_vk_buffer(vk_h, loader_->device_generation());
            rasterizer_.destroy_buffer(h);
            return 0;
        }
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
    if (gpu_ && !gpu_->ready()) {
        set_err(error_out, gpu_->init_error);
        return false;
    }
    const auto it = vk_buffers_.find(handle);
    if (gpu_) {
        if (it == vk_buffers_.end() || data.empty()) {
            return rasterizer_.update_buffer(handle, data, error_out);
        }
        auto meta_it = gpu_->user_buffers.find(handle);
        if (meta_it == gpu_->user_buffers.end()) {
            return rasterizer_.update_buffer(handle, data, error_out);
        }
        if (data.size() > meta_it->second.capacity) {
            std::uint64_t new_vk_h = 0;
            if (!loader_->create_vk_buffer(data.size(), it->second.usage, new_vk_h, error_out)) {
                return false;
            }
            GpuState::UserBufferMeta saved = meta_it->second;
            gpu_->user_buffers.erase(meta_it);
            if (!gpu_->bind_and_upload_buffer(handle, u64_to_handle<VkBuffer>(new_vk_h), data,
                                              error_out)) {
                loader_->destroy_vk_buffer(new_vk_h, loader_->device_generation());
                gpu_->user_buffers.emplace(handle, saved);
                return false;
            }
            loader_->destroy_vk_buffer(it->second.handle, it->second.generation);
            gpu_->allocator.free(saved.mem);
            it->second = VkRef{new_vk_h, loader_->device_generation(), data.size(),
                               it->second.usage};
        } else {
            if (!gpu_->upload_buffer_bytes(u64_to_handle<VkBuffer>(it->second.handle), data,
                                           error_out)) {
                return false;
            }
            meta_it->second.logical = data.size();
            it->second.bytes = data.size();
        }
        return rasterizer_.update_buffer(handle, data, error_out);
    }

    // Grow the mirrored VkBuffer first so a failure leaves the buffer untouched.
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
        if (gpu_ && gpu_->ready() && gpu_->generation == it->second.generation) {
            const auto meta_it = gpu_->user_buffers.find(handle);
            if (meta_it != gpu_->user_buffers.end()) {
                gpu_->allocator.free(meta_it->second.mem);
                gpu_->user_buffers.erase(meta_it);
            }
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
    if (gpu_ && !gpu_->ready()) {
        set_err(error_out, gpu_->init_error);
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
    if (gpu_) {
        if (vk_h == 0 || !gpu_->bind_and_upload_texture(
                             h, u64_to_handle<VkImage>(vk_h), width, height, rgba8, error_out)) {
            loader_->destroy_vk_image(vk_h, loader_->device_generation());
            rasterizer_.destroy_texture(h);
            return 0;
        }
    }
    if (vk_h != 0) {
        vk_textures_.emplace(h, VkRef{vk_h, loader_->device_generation(), 0U, BufferUsage::vertex});
    }
    return h;
}

void VulkanRhiDevice::destroy_texture(TextureHandle handle) {
    const auto it = vk_textures_.find(handle);
    if (it != vk_textures_.end()) {
        if (gpu_ && gpu_->ready() && gpu_->generation == it->second.generation) {
            const auto meta_it = gpu_->user_textures.find(handle);
            if (meta_it != gpu_->user_textures.end()) {
                if (meta_it->second.view != VK_NULL_HANDLE &&
                    gpu_->impl->fn_destroy_image_view != nullptr) {
                    gpu_->impl->fn_destroy_image_view(gpu_->impl->device, meta_it->second.view,
                                                      nullptr);
                }
                if (loader_) {
                    loader_->destroy_vk_image(it->second.handle, it->second.generation);
                }
                gpu_->allocator.free(meta_it->second.mem);
                gpu_->user_textures.erase(meta_it);
            } else if (loader_) {
                loader_->destroy_vk_image(it->second.handle, it->second.generation);
            }
        } else if (loader_) {
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
    const PipelineHandle h = rasterizer_.create_pipeline(desc, error_out);
    if (h != 0 && gpu_) {
        gpu_->user_pipelines[h] = desc;
    }
    return h;
}

void VulkanRhiDevice::destroy_pipeline(PipelineHandle handle) {
    if (gpu_) {
        gpu_->user_pipelines.erase(handle);
    }
    rasterizer_.destroy_pipeline(handle);
}

bool VulkanRhiDevice::render_pass_and_readback(const RenderPassDesc& pass,
                                               std::span<const MeshDrawCall> draws,
                                               std::vector<std::uint8_t>& out_rgba8,
                                               std::string* error_out) {
    if (!loader_ || !loader_->has_device()) {
        last_frame_used_gpu_ = false;
        out_rgba8.clear();
        if (error_out != nullptr) {
            *error_out = "VulkanRhiDevice requires an initialized VulkanDriverLoader device";
        }
        return false;
    }
    if (gpu_) {
        if (!gpu_->ready()) {
            last_frame_used_gpu_ = false;
            out_rgba8.clear();
            set_err(error_out, gpu_->init_error.empty() ? "Vulkan device was lost or released"
                                                        : gpu_->init_error);
            return false;
        }
        const bool ok = gpu_->render_pass(pass, draws, vk_buffers_, out_rgba8, error_out);
        last_frame_used_gpu_ = ok;
        return ok;
    }
    last_frame_used_gpu_ = false;
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
