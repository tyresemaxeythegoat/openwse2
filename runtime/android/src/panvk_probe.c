/*
 * Off-device/on-device probe for the native PanVK loading path.
 *
 * It loads the ICD through vk_panvk_loader.c, enumerates the instance and the
 * physical device, and creates a logical device with a graphics queue. It does
 * not create a window or a swapchain; that needs an ANativeWindow from the
 * NativeActivity shell.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "vk_panvk_loader.h"

static int fail(const char *what, VkResult result) {
    fprintf(stderr, "FAIL: %s (VkResult %d)\n", what, (int)result);
    return 1;
}

static int has_extension(const VkExtensionProperties *properties,
                         uint32_t count, const char *name) {
    uint32_t index;

    for (index = 0; index < count; ++index) {
        if (strcmp(properties[index].extensionName, name) == 0)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *icd_path = argc > 1 ? argv[1] : NULL;
    VkApplicationInfo application_info;
    VkInstanceCreateInfo instance_info;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice *physical_devices = NULL;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkQueueFamilyProperties *queue_families = NULL;
    VkDeviceCreateInfo device_info;
    VkDeviceQueueCreateInfo queue_info;
    VkDevice device = VK_NULL_HANDLE;
    VkExtensionProperties *extensions = NULL;
    const char *enabled_extensions[2];
    uint32_t api_version = VK_API_VERSION_1_0;
    uint32_t extension_count = 0;
    uint32_t physical_device_count = 0;
    uint32_t queue_family_count = 0;
    uint32_t enabled_extension_count = 0;
    uint32_t graphics_family = UINT32_MAX;
    uint32_t index;
    float queue_priority = 1.0f;
    VkResult result;
    int status = 1;

    PFN_vkEnumerateInstanceVersion enumerate_instance_version;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties;
    PFN_vkCreateDevice create_device;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkDestroyInstance destroy_instance;

    if (wse2_panvk_load(icd_path) != 0) {
        fprintf(stderr, "FAIL: %s\n", wse2_panvk_last_error());
        return 1;
    }

    printf("ICD            %s\n", wse2_panvk_icd_path());
    printf("loader/ICD     interface version %u\n",
           wse2_panvk_interface_version());

    enumerate_instance_version =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
            NULL, "vkEnumerateInstanceVersion");
    if (enumerate_instance_version) {
        result = enumerate_instance_version(&api_version);
        if (result != VK_SUCCESS)
            return fail("vkEnumerateInstanceVersion", result);
    }
    printf("instance API   %u.%u.%u\n", VK_API_VERSION_MAJOR(api_version),
           VK_API_VERSION_MINOR(api_version), VK_API_VERSION_PATCH(api_version));

    result = vkEnumerateInstanceExtensionProperties(NULL, &extension_count,
                                                    NULL);
    if (result != VK_SUCCESS)
        return fail("vkEnumerateInstanceExtensionProperties(count)", result);

    extensions = calloc(extension_count ? extension_count : 1,
                        sizeof(*extensions));
    if (!extensions) {
        fprintf(stderr, "FAIL: out of memory\n");
        goto cleanup;
    }

    result = vkEnumerateInstanceExtensionProperties(NULL, &extension_count,
                                                    extensions);
    if (result != VK_SUCCESS) {
        fail("vkEnumerateInstanceExtensionProperties(list)", result);
        goto cleanup;
    }

    printf("instance ext   %u\n", extension_count);
    for (index = 0; index < extension_count; ++index)
        printf("  %s\n", extensions[index].extensionName);

    if (has_extension(extensions, extension_count, VK_KHR_SURFACE_EXTENSION_NAME))
        enabled_extensions[enabled_extension_count++] =
            VK_KHR_SURFACE_EXTENSION_NAME;
    if (has_extension(extensions, extension_count, "VK_KHR_android_surface"))
        enabled_extensions[enabled_extension_count++] =
            "VK_KHR_android_surface";

    memset(&application_info, 0, sizeof(application_info));
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "WSE2 PanVK probe";
    application_info.applicationVersion = 1;
    application_info.pEngineName = "wse2-native";
    application_info.engineVersion = 1;
    application_info.apiVersion = api_version;

    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;
    instance_info.enabledExtensionCount = enabled_extension_count;
    instance_info.ppEnabledExtensionNames = enabled_extensions;

    result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS)
        return fail("vkCreateInstance", result);

    enumerate_physical_devices =
        (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(
            instance, "vkEnumeratePhysicalDevices");
    get_physical_device_properties =
        (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceProperties");
    get_queue_family_properties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    create_device = (PFN_vkCreateDevice)vkGetInstanceProcAddr(
        instance, "vkCreateDevice");
    destroy_device = (PFN_vkDestroyDevice)vkGetInstanceProcAddr(
        instance, "vkDestroyDevice");
    destroy_instance = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(
        instance, "vkDestroyInstance");

    if (!enumerate_physical_devices || !get_physical_device_properties ||
        !get_queue_family_properties || !create_device || !destroy_device ||
        !destroy_instance) {
        fprintf(stderr, "FAIL: ICD did not expose the core device entry "
                        "points\n");
        goto cleanup;
    }

    result = enumerate_physical_devices(instance, &physical_device_count, NULL);
    if (result != VK_SUCCESS) {
        fail("vkEnumeratePhysicalDevices(count)", result);
        goto cleanup;
    }

    printf("physical devs  %u\n", physical_device_count);
    if (physical_device_count == 0) {
        fprintf(stderr, "FAIL: ICD reported no physical devices\n");
        goto cleanup;
    }

    physical_devices = calloc(physical_device_count, sizeof(*physical_devices));
    if (!physical_devices) {
        fprintf(stderr, "FAIL: out of memory\n");
        goto cleanup;
    }

    result = enumerate_physical_devices(instance, &physical_device_count,
                                        physical_devices);
    if (result != VK_SUCCESS) {
        fail("vkEnumeratePhysicalDevices(list)", result);
        goto cleanup;
    }

    for (index = 0; index < physical_device_count; ++index) {
        uint32_t family;

        get_physical_device_properties(physical_devices[index], &properties);
        printf("device[%u]      %s\n", index, properties.deviceName);
        printf("  type         %u\n", (unsigned)properties.deviceType);
        printf("  vulkan       %u.%u.%u\n",
               VK_API_VERSION_MAJOR(properties.apiVersion),
               VK_API_VERSION_MINOR(properties.apiVersion),
               VK_API_VERSION_PATCH(properties.apiVersion));
        printf("  driver       %u\n", properties.driverVersion);
        printf("  vendor/device 0x%04x/0x%04x\n", properties.vendorID,
               properties.deviceID);

        get_queue_family_properties(physical_devices[index],
                                    &queue_family_count, NULL);
        queue_families = calloc(queue_family_count ? queue_family_count : 1,
                                sizeof(*queue_families));
        if (!queue_families) {
            fprintf(stderr, "FAIL: out of memory\n");
            goto cleanup;
        }
        get_queue_family_properties(physical_devices[index],
                                    &queue_family_count, queue_families);

        for (family = 0; family < queue_family_count; ++family) {
            printf("  queue[%u]     flags 0x%x count %u\n", family,
                   queue_families[family].queueFlags,
                   queue_families[family].queueCount);
            if ((queue_families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                graphics_family == UINT32_MAX &&
                queue_families[family].queueCount > 0)
                graphics_family = family;
        }

        free(queue_families);
        queue_families = NULL;

        if (graphics_family != UINT32_MAX && physical_device == VK_NULL_HANDLE)
            physical_device = physical_devices[index];
    }

    if (physical_device == VK_NULL_HANDLE) {
        fprintf(stderr, "FAIL: no graphics queue family found\n");
        goto cleanup;
    }

    memset(&queue_info, 0, sizeof(queue_info));
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = graphics_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    memset(&device_info, 0, sizeof(device_info));
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    result = create_device(physical_device, &device_info, NULL, &device);
    if (result != VK_SUCCESS) {
        fail("vkCreateDevice", result);
        goto cleanup;
    }

    printf("logical device created on queue family %u\n", graphics_family);
    printf("PASS\n");
    status = 0;

cleanup:
    if (device != VK_NULL_HANDLE && destroy_device)
        destroy_device(device, NULL);
    if (instance != VK_NULL_HANDLE && destroy_instance)
        destroy_instance(instance, NULL);
    free(queue_families);
    free(physical_devices);
    free(extensions);
    return status;
}
