/*
 * Minimal libhardware replacement for a native Android app.
 *
 * PanVK links libhardware.so and calls hw_get_module("gralloc") while setting
 * up its optional gralloc path. libhardware.so is a VNDK-SP library and is not
 * in an ordinary app's linker namespace, so the ICD cannot load it directly.
 *
 * Returning -ENOENT is a supported PanVK path: the driver logs that no gralloc
 * hwmodule was detected and uses Mesa's fallback gralloc implementation.
 * Video-buffer interop is the only feature that needs the vendor gralloc HAL;
 * ordinary swapchain rendering does not.
 */

#include <errno.h>
#include <stddef.h>

struct hw_module_t;

int hw_get_module(const char *id, const struct hw_module_t **module) {
    (void)id;
    if (module)
        *module = NULL;
    return -ENOENT;
}

int hw_get_module_by_class(const char *class_id, const char *inst,
                           const struct hw_module_t **module) {
    (void)class_id;
    (void)inst;
    if (module)
        *module = NULL;
    return -ENOENT;
}
