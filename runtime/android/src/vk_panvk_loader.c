/*
 * Minimal Vulkan loader for the FristOneRR PanVK ICD.
 *
 * Android's system libvulkan.so discovers drivers through
 * hw_get_module("vulkan") and does not honour VK_ICD_FILENAMES on a retail
 * device. This shim bypasses that loader and talks to the ICD through the
 * documented Vulkan ICD interface:
 *
 *   vk_icdNegotiateLoaderICDInterfaceVersion
 *   vk_icdGetInstanceProcAddr
 *   vk_icdGetPhysicalDeviceProcAddr
 *
 * This is a loader, not an emulator. The ICD submits native jobs to the Mali
 * kbase kernel driver; no x86 code, Wine, or translation layer is involved.
 */

#define _GNU_SOURCE
#include "vk_panvk_loader.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#define PANVK_LOADER_INTERFACE_VERSION 5u
#define PANVK_DEFAULT_ICD "libvulkan_panfrost.so"
#define PANVK_MAX_PATH 1024

typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vk_icdGetInstanceProcAddr)(
    VkInstance instance, const char *pName);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vk_icdGetPhysicalDeviceProcAddr)(
    VkInstance instance, const char *pName);
typedef VkResult (VKAPI_PTR *PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(
    uint32_t *pVersion);

/* ------------------------------------------------------------------------- */
/* Platform glue                                                             */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32)
#include <windows.h>

static void *os_open(const char *path) {
    return (void *)LoadLibraryA(path);
}

static void *os_sym(void *handle, const char *name) {
    return (void *)GetProcAddress((HMODULE)handle, name);
}

static void os_close(void *handle) {
    if (handle)
        FreeLibrary((HMODULE)handle);
}

static const char *os_error(void) {
    static char buffer[256];
    DWORD code = GetLastError();
    snprintf(buffer, sizeof(buffer), "Win32 error %lu", (unsigned long)code);
    return buffer;
}
#else
#include <dlfcn.h>

static void *os_open(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static void *os_sym(void *handle, const char *name) {
    return dlsym(handle, name);
}

static void os_close(void *handle) {
    if (handle)
        dlclose(handle);
}

static const char *os_error(void) {
    const char *error = dlerror();
    return error ? error : "unknown dynamic-loader error";
}
#endif

#if defined(WSE2_PANVK_SINGLE_THREADED)
#define panvk_lock() ((void)0)
#define panvk_unlock() ((void)0)
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define panvk_lock() ((void)pthread_mutex_lock(&g_lock))
#define panvk_unlock() ((void)pthread_mutex_unlock(&g_lock))
#endif

#define LOAD_SYM(handle, name, target)                    \
    do {                                                  \
        void *symbol_ = os_sym((handle), (name));         \
        memcpy(&(target), &symbol_, sizeof(symbol_));     \
    } while (0)

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static void *g_icd;
static PFN_vk_icdGetInstanceProcAddr g_icd_gipa;
static PFN_vk_icdGetPhysicalDeviceProcAddr g_icd_gpdpa;
static PFN_vk_icdNegotiateLoaderICDInterfaceVersion g_icd_negotiate;
static uint32_t g_icd_interface_version;
static int g_load_status = -1;
static char g_icd_path[PANVK_MAX_PATH];
static char g_error[512];

static void set_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_error, sizeof(g_error), format, args);
    va_end(args);

#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "panvk-loader", "%s", g_error);
#elif !defined(WSE2_PANVK_SILENT)
    fprintf(stderr, "panvk-loader: %s\n", g_error);
#endif
}

static void copy_path(char *destination, size_t capacity, const char *source) {
    size_t length;

    if (!source) {
        destination[0] = '\0';
        return;
    }

    length = strlen(source);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static int load_icd(const char *requested_path) {
    const char *candidates[2];
    unsigned candidate_count = 0;
    unsigned index;

    if (requested_path && requested_path[0])
        candidates[candidate_count++] = requested_path;
    candidates[candidate_count++] = PANVK_DEFAULT_ICD;

    for (index = 0; index < candidate_count; ++index) {
        g_icd = os_open(candidates[index]);
        if (g_icd) {
            copy_path(g_icd_path, sizeof(g_icd_path), candidates[index]);
            break;
        }
        set_error("cannot load %s: %s", candidates[index], os_error());
    }

    if (!g_icd)
        return -ENOENT;

    LOAD_SYM(g_icd, "vk_icdGetInstanceProcAddr", g_icd_gipa);
    LOAD_SYM(g_icd, "vk_icdNegotiateLoaderICDInterfaceVersion",
             g_icd_negotiate);

    if (!g_icd_gipa || !g_icd_negotiate) {
        set_error("%s does not export the Vulkan ICD entry points",
                  g_icd_path);
        os_close(g_icd);
        g_icd = NULL;
        return -ENOSYS;
    }

    {
        uint32_t version = PANVK_LOADER_INTERFACE_VERSION;
        VkResult result = g_icd_negotiate(&version);

        if (result != VK_SUCCESS) {
            set_error("ICD refused loader interface version %u (VkResult %d)",
                      PANVK_LOADER_INTERFACE_VERSION, (int)result);
            os_close(g_icd);
            g_icd = NULL;
            return -EPROTONOSUPPORT;
        }

        g_icd_interface_version = version;
    }

    if (g_icd_interface_version >= 2)
        LOAD_SYM(g_icd, "vk_icdGetPhysicalDeviceProcAddr", g_icd_gpdpa);

    {
        PFN_vkCreateInstance create_instance =
            (PFN_vkCreateInstance)g_icd_gipa(NULL, "vkCreateInstance");
        PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions =
            (PFN_vkEnumerateInstanceExtensionProperties)g_icd_gipa(
                NULL, "vkEnumerateInstanceExtensionProperties");

        if (!create_instance || !enumerate_extensions) {
            set_error("%s is missing vkCreateInstance or instance extension "
                      "enumeration",
                      g_icd_path);
            os_close(g_icd);
            g_icd = NULL;
            return -ENOSYS;
        }
    }

    return 0;
}

int wse2_panvk_load(const char *icd_path) {
    int status;

    panvk_lock();
    if (g_load_status == -1)
        g_load_status = load_icd(icd_path);
    status = g_load_status;
    panvk_unlock();

    return status;
}

const char *wse2_panvk_icd_path(void) {
    return g_icd_path;
}

const char *wse2_panvk_last_error(void) {
    return g_error;
}

unsigned wse2_panvk_interface_version(void) {
    return g_icd_interface_version;
}

static int ensure_loaded(void) {
    if (g_load_status == 0)
        return 0;
    return wse2_panvk_load(NULL);
}

/* ------------------------------------------------------------------------- */
/* Vulkan entry points                                                       */
/* ------------------------------------------------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(
    uint32_t *pApiVersion) {
    PFN_vkEnumerateInstanceVersion enumerate_version;

    if (!pApiVersion)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (ensure_loaded() != 0)
        return VK_ERROR_INCOMPATIBLE_DRIVER;

    enumerate_version = (PFN_vkEnumerateInstanceVersion)g_icd_gipa(
        NULL, "vkEnumerateInstanceVersion");
    if (!enumerate_version) {
        *pApiVersion = VK_API_VERSION_1_0;
        return VK_SUCCESS;
    }

    return enumerate_version(pApiVersion);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount,
    VkExtensionProperties *pProperties) {
    PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions;

    if (pLayerName)
        return VK_ERROR_LAYER_NOT_PRESENT;
    if (!pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (ensure_loaded() != 0)
        return VK_ERROR_INCOMPATIBLE_DRIVER;

    enumerate_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)g_icd_gipa(
            NULL, "vkEnumerateInstanceExtensionProperties");
    if (!enumerate_extensions)
        return VK_ERROR_INITIALIZATION_FAILED;

    return enumerate_extensions(NULL, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t *pPropertyCount, VkLayerProperties *pProperties) {
    (void)pProperties;

    if (!pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;

    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
    PFN_vkCreateInstance create_instance;

    if (!pCreateInfo || !pInstance)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (ensure_loaded() != 0)
        return VK_ERROR_INCOMPATIBLE_DRIVER;

    create_instance = (PFN_vkCreateInstance)g_icd_gipa(
        NULL, "vkCreateInstance");
    if (!create_instance)
        return VK_ERROR_INCOMPATIBLE_DRIVER;

    return create_instance(pCreateInfo, pAllocator, pInstance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char *pName) {
    PFN_vkVoidFunction function;

    if (!pName || !pName[0])
        return NULL;

    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(pName, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)vkCreateInstance;
    if (strcmp(pName, "vkEnumerateInstanceVersion") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;
    if (strcmp(pName, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    if (strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0)
        return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;

    if (ensure_loaded() != 0)
        return NULL;

    function = g_icd_gipa(instance, pName);
    if (function)
        return function;

    if (instance && g_icd_gpdpa) {
        function = g_icd_gpdpa(instance, pName);
        if (function)
            return function;
    }

    return NULL;
}
