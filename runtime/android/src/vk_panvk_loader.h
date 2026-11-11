#ifndef WSE2_VK_PANVK_LOADER_H
#define WSE2_VK_PANVK_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load the FristOneRR PanVK ICD directly, without Android's system
 * libvulkan.so loader.
 *
 * icd_path may be NULL, in which case the loader tries
 * "libvulkan_panfrost.so" from the app's native library directory.
 *
 * Returns 0 on success, a negative errno value on failure. The loader is
 * process-wide and is initialised at most once.
 */
int wse2_panvk_load(const char *icd_path);

/* Path of the ICD that was actually loaded, or "" before a successful load. */
const char *wse2_panvk_icd_path(void);

/* Human-readable reason for the last load failure, or "" on success. */
const char *wse2_panvk_last_error(void);

/* Negotiated Vulkan loader/ICD interface version, or 0 before load. */
unsigned wse2_panvk_interface_version(void);

#ifdef __cplusplus
}
#endif

#endif /* WSE2_VK_PANVK_LOADER_H */
