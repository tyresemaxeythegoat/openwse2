/*
 * Mock Vulkan ICD used to exercise vk_panvk_loader.c on a desktop host.
 *
 * It implements only the ICD interface and the handful of Vulkan entry points
 * that panvk_probe.c calls. All objects are static dummies; nothing here
 * touches a GPU. The point is to prove that the loader:
 *
 *   - resolves vk_icdNegotiateLoaderICDInterfaceVersion /
 *     vk_icdGetInstanceProcAddr / vk_icdGetPhysicalDeviceProcAddr,
 *   - negotiates a loader interface version,
 *   - forwards global, instance, physical-device and device commands.
 *
 * Build:
 *   zig cc -target x86_64-windows-gnu -shared -I third_party \
 *       -o mock_icd.dll tests/mock_icd.c
 */

#include <stdint.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define MOCK_INSTANCE_MAGIC 0x494E5354u
#define MOCK_PHYSICAL_MAGIC 0x50485953u
#define MOCK_DEVICE_MAGIC 0x44455649u

typedef struct MockInstance {
    uint32_t magic;
} MockInstance;

typedef struct MockPhysicalDevice {
    uint32_t magic;
} MockPhysicalDevice;

typedef struct MockDevice {
    uint32_t magic;
} MockDevice;

static MockInstance g_instance = {MOCK_INSTANCE_MAGIC};
static MockPhysicalDevice g_physical_device = {MOCK_PHYSICAL_MAGIC};
static MockDevice g_device = {MOCK_DEVICE_MAGIC};

static VKAPI_ATTR VkResult VKAPI_CALL mock_EnumerateInstanceVersion(
    uint32_t *pApiVersion) {
    if (!pApiVersion)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pApiVersion = VK_API_VERSION_1_3;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
mock_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                          uint32_t *pPropertyCount,
                                          VkExtensionProperties *pProperties) {
    static const char *const names[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                        "VK_KHR_android_surface"};

    if (pLayerName)
        return VK_ERROR_LAYER_NOT_PRESENT;
    if (!pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (!pProperties) {
        *pPropertyCount = (uint32_t)(sizeof(names) / sizeof(names[0]));
        return VK_SUCCESS;
    }

    if (*pPropertyCount < (uint32_t)(sizeof(names) / sizeof(names[0]))) {
        *pPropertyCount = (uint32_t)(sizeof(names) / sizeof(names[0]));
        return VK_INCOMPLETE;
    }

    for (uint32_t i = 0; i < (uint32_t)(sizeof(names) / sizeof(names[0])); ++i) {
        memset(&pProperties[i], 0, sizeof(pProperties[i]));
        strncpy(pProperties[i].extensionName, names[i],
                sizeof(pProperties[i].extensionName) - 1);
        pProperties[i].specVersion = 1;
    }
    *pPropertyCount = (uint32_t)(sizeof(names) / sizeof(names[0]));
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL mock_CreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
    (void)pAllocator;
    if (!pCreateInfo || !pInstance)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pInstance = (VkInstance)&g_instance;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mock_DestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    (void)instance;
    (void)pAllocator;
}

static VKAPI_ATTR VkResult VKAPI_CALL mock_EnumeratePhysicalDevices(
    VkInstance instance, uint32_t *pPhysicalDeviceCount,
    VkPhysicalDevice *pPhysicalDevices) {
    MockInstance *mock = (MockInstance *)instance;

    if (!mock || mock->magic != MOCK_INSTANCE_MAGIC)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!pPhysicalDeviceCount)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = 1;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceCount == 0) {
        *pPhysicalDeviceCount = 1;
        return VK_INCOMPLETE;
    }
    pPhysicalDevices[0] = (VkPhysicalDevice)&g_physical_device;
    *pPhysicalDeviceCount = 1;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mock_GetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties) {
    MockPhysicalDevice *mock = (MockPhysicalDevice *)physicalDevice;

    if (!mock || mock->magic != MOCK_PHYSICAL_MAGIC || !pProperties)
        return;

    memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion = VK_API_VERSION_1_3;
    pProperties->driverVersion = 0x00030007u;
    pProperties->vendorID = 0x13B5u;
    pProperties->deviceID = 0x7093u;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strncpy(pProperties->deviceName, "Mock Mali-G68 (PanVK ICD)",
            sizeof(pProperties->deviceName) - 1);
    pProperties->limits.maxImageDimension2D = 8192;
}

static VKAPI_ATTR void VKAPI_CALL mock_GetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount,
    VkQueueFamilyProperties *pQueueFamilyProperties) {
    MockPhysicalDevice *mock = (MockPhysicalDevice *)physicalDevice;

    if (!mock || mock->magic != MOCK_PHYSICAL_MAGIC ||
        !pQueueFamilyPropertyCount)
        return;

    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    if (*pQueueFamilyPropertyCount == 0) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }

    memset(&pQueueFamilyProperties[0], 0,
           sizeof(pQueueFamilyProperties[0]));
    pQueueFamilyProperties[0].queueFlags = VK_QUEUE_GRAPHICS_BIT |
                                          VK_QUEUE_COMPUTE_BIT |
                                          VK_QUEUE_TRANSFER_BIT;
    pQueueFamilyProperties[0].queueCount = 1;
    pQueueFamilyProperties[0].timestampValidBits = 64;
    pQueueFamilyProperties[0].minImageTransferGranularity.width = 1;
    pQueueFamilyProperties[0].minImageTransferGranularity.height = 1;
    pQueueFamilyProperties[0].minImageTransferGranularity.depth = 1;
    *pQueueFamilyPropertyCount = 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL mock_CreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    MockPhysicalDevice *mock = (MockPhysicalDevice *)physicalDevice;

    (void)pAllocator;
    if (!mock || mock->magic != MOCK_PHYSICAL_MAGIC || !pCreateInfo ||
        !pDevice)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pCreateInfo->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO)
        return VK_ERROR_INITIALIZATION_FAILED;
    *pDevice = (VkDevice)&g_device;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL mock_DestroyDevice(
    VkDevice device, const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
}

/* ------------------------------------------------------------------------- */
/* ICD interface                                                             */
/* ------------------------------------------------------------------------- */

__declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    if (!pVersion)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (*pVersion > 5)
        *pVersion = 5;
    return VK_SUCCESS;
}

__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    (void)instance;

    if (!pName)
        return NULL;
    if (strcmp(pName, "vkEnumerateInstanceVersion") == 0)
        return (PFN_vkVoidFunction)mock_EnumerateInstanceVersion;
    if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)mock_EnumerateInstanceExtensionProperties;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)mock_CreateInstance;
    if (strcmp(pName, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)mock_DestroyInstance;
    if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0)
        return (PFN_vkVoidFunction)mock_EnumeratePhysicalDevices;
    if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0)
        return (PFN_vkVoidFunction)mock_GetPhysicalDeviceProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
        return (PFN_vkVoidFunction)mock_GetPhysicalDeviceQueueFamilyProperties;
    if (strcmp(pName, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)mock_CreateDevice;
    if (strcmp(pName, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)mock_DestroyDevice;

    return NULL;
}

__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName) {
    (void)instance;
    (void)pName;
    return NULL;
}
