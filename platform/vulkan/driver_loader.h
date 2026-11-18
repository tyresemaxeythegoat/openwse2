// Platform seam: Runtime Vulkan driver loader (System / Turnip / PanVK / Mock ICD)
// and minimal Vulkan RHI backend (`docs/PLATFORM_PLAN.md`, slice P4).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.6 layout
// and carries no PE/PDB citations. `<vulkan/vulkan.h>` is intentionally isolated
// inside `driver_loader.cpp` so callers only depend on `wse/platform/rhi.h`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wse/platform/rhi.h"

namespace wse::platform {

struct VulkanDriverProbeResult {
    bool ok = false;
    DriverMode mode = DriverMode::system;
    bool used_direct_icd = false;
    // Driver entered through its Android `HMI` hwvulkan HAL module instead of the
    // `vk_icd*` loader-ICD exports.
    bool used_hal_module = false;
    bool is_mock = false;
    // `run_vulkan_mesh_smoke_test` replaced an unavailable real driver with the
    // mock ICD; `fallback_reason` says why. `error` stays empty on success.
    bool fell_back_to_mock = false;
    std::uint32_t negotiated_icd_version = 0;
    // `VkApplicationInfo::apiVersion` the instance was created with.
    std::uint32_t instance_api_version = 0;
    // Physical-device `apiVersion` (the usable version is the lower of the two).
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t graphics_queue_family = 0;
    std::uint32_t max_image_dimension_2d = 0;
    // `VkPhysicalDeviceFeatures::textureCompressionBC` (Warband `.dds` are BC1-3;
    // without it the RHI must decode or transcode on the CPU).
    bool texture_compression_bc = false;
    // Instance WSI check (`VK_KHR_surface` + `VK_KHR_android_surface`) and
    // presentation path (`android_surface_wsi` for Android loader vs
    // `offscreen_hardware_buffer_bridge` for direct custom ICD `dlopen`).
    bool supports_android_surface_wsi = false;
    PresentationPath presentation_path = PresentationPath::offscreen_hardware_buffer_bridge;
    // On `android_surface_wsi` the instance is created with `VK_KHR_surface` +
    // `VK_KHR_android_surface` enabled and the device with `VK_KHR_swapchain`; a
    // device without `VK_KHR_swapchain` drops back to the offscreen bridge
    // (`presentation_note` says so).
    bool wsi_instance_extensions_enabled = false;
    bool swapchain_extension_enabled = false;
    std::string presentation_note{};
    std::string device_name{};
    std::string loaded_library{};
    std::string fallback_reason{};
    std::string error{};
};

struct VulkanMockConfig {
    DriverMode mode = DriverMode::panvk;
    std::string device_name = "Mali-G610 (PanVK Mock ICD)";
    std::uint32_t vendor_id = 0x13B5U;
    std::uint32_t device_id = 0x0610U;
    std::uint32_t api_version = 0x00403000U;     // Vulkan 1.3.0
    std::uint32_t driver_version = 0x06C00000U;  // Mesa 27
    std::uint32_t max_image_dimension_2d = 4096U;
    bool texture_compression_bc = true;
    bool direct_icd = true;
    bool expose_android_surface_wsi = false;
    // Device-level `VK_KHR_swapchain`, offered only on a WSI-enabled instance.
    bool expose_swapchain = true;
    // Reject `vkCreateInstance` with `VK_ERROR_INCOMPATIBLE_DRIVER` unless `apiVersion == 1.0`.
    bool require_api_version_1_0 = false;
    // Reject `vkCreateInstance` with `VK_ERROR_EXTENSION_NOT_PRESENT` when any extension is enabled.
    bool refuse_wsi_instance_extensions = false;
};

[[nodiscard]] std::string format_vulkan_version(std::uint32_t vk_version);

// Sets the Mesa environment a custom Turnip/PanVK driver reads at load time.
// Call before `load_custom_driver`. Never overrides a variable that is already
// set, so a user/debug value wins.
//   - `MESA_SHADER_CACHE_DIR=<shader_cache_dir>`: Android apps have no `$HOME` /
//     `$XDG_CACHE_HOME`, so without it Mesa's on-disk shader cache is off and
//     every launch recompiles every pipeline.
//   - PanVK only: `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`. Mesa refuses to
//     enumerate Mali generations it does not yet call conformant (e.g. Bifrost
//     G52/G57) unless it is set; the user opted into PanVK explicitly.
// Returns the number of variables set.
std::size_t apply_mesa_driver_environment(DriverMode mode, const std::string& shader_cache_dir);

// In-process mock Android hwvulkan HAL module (`hw_module_t` with id "vulkan")
// backed by the mock ICD, for `VulkanDriverLoader::attach_hal_module` tests.
[[nodiscard]] const void* mock_vulkan_hal_module() noexcept;

// Process-wide accounting of objects created through the mock ICD (instances,
// devices, command pools, buffers, images). Tests compare snapshots to prove
// every object is destroyed exactly once and children die before their device.
struct MockVulkanObjectStats {
    std::size_t live_objects = 0;              // created and not yet destroyed
    std::size_t leaked_at_device_destroy = 0;  // children alive at vkDestroyDevice
    std::size_t invalid_destroys = 0;          // destroys of unknown/already-destroyed handles
};
[[nodiscard]] MockVulkanObjectStats mock_vulkan_object_stats() noexcept;

// Dynamically loads a Vulkan system loader or direct Vulkan ICD (`.so` / `.dll`),
// negotiates ICD v5 entry points when loading PanVK/Turnip directly, creates a
// `VkInstance` + graphics `VkDevice` + `VkCommandPool`, and manages resource handles.
class VulkanDriverLoader {
public:
    VulkanDriverLoader();
    ~VulkanDriverLoader();

    VulkanDriverLoader(const VulkanDriverLoader&) = delete;
    VulkanDriverLoader& operator=(const VulkanDriverLoader&) = delete;
    VulkanDriverLoader(VulkanDriverLoader&&) noexcept;
    VulkanDriverLoader& operator=(VulkanDriverLoader&&) noexcept;

    [[nodiscard]] bool load_system(std::string* error_out = nullptr);

    // Loads a custom ICD `.so`/`.dll`. Entry points are tried in order:
    // `vk_icdGetInstanceProcAddr` (loader-ICD interface, negotiated up to v5),
    // the Android `HMI` hwvulkan HAL module (`HWVULKAN_DEVICE_0`), then a plain
    // `vkGetInstanceProcAddr` export. For Mesa drivers patched by
    // `stage_driver_package_to_vfs`, pass the bundled `libpanvk_hw.so` in
    // `hw_stub_so_path` so it is opened RTLD_GLOBAL before the driver. On
    // Linux/Android both libraries are opened RTLD_NODELETE: Mesa drivers start
    // worker threads and register exit handlers, so they are never unmapped.
    [[nodiscard]] bool load_custom_driver(DriverMode mode,
                                          const std::string& icd_so_path,
                                          const std::string& hw_stub_so_path = {},
                                          std::string* error_out = nullptr);

    // Attaches an in-process mock ICD implementing the Vulkan ICD v5 ABI for
    // deterministic CI/host testing without physical Mali/Adreno hardware.
    [[nodiscard]] bool load_mock_icd(const VulkanMockConfig& config);

    // Enters an already-resolved Android hwvulkan HAL module (`hw_module_t*`,
    // the driver's `HMI` symbol). `load_custom_driver` uses this when an ICD has
    // no `vk_icd*` exports; exposed so the HAL path is testable without a `.so`.
    [[nodiscard]] bool attach_hal_module(DriverMode mode,
                                         const void* hw_module,
                                         std::string* error_out = nullptr);

    // Creates `VkInstance`, selects a graphics physical device, and opens `VkDevice`.
    [[nodiscard]] bool probe_and_create_device(VulkanDriverProbeResult& out);

    void unload() noexcept;

    [[nodiscard]] bool is_loaded() const noexcept;
    [[nodiscard]] bool has_device() const noexcept;
    [[nodiscard]] bool can_execute_gpu() const noexcept;
    [[nodiscard]] const VulkanDriverProbeResult& probe_result() const noexcept;

    // Process-unique id of the current `VkDevice`; 0 without one. Changes every
    // time `probe_and_create_device` opens a new device, so a caller holding a
    // handle from an earlier device can tell it is gone.
    [[nodiscard]] std::uint64_t device_generation() const noexcept;

    // Vulkan object handle tracking (non-zero opaque uint64 handles). Record
    // `device_generation()` next to each handle and pass it back to destroy:
    // Vulkan handle values are reused, so a stale handle from a destroyed device
    // can equal a live handle of the current one; a mismatched generation makes
    // the destroy a no-op.
    [[nodiscard]] bool create_vk_buffer(std::size_t size_bytes,
                                        BufferUsage usage,
                                        std::uint64_t& out_vk_handle,
                                        std::string* error_out = nullptr);
    void destroy_vk_buffer(std::uint64_t vk_handle, std::uint64_t generation) noexcept;

    [[nodiscard]] bool create_vk_image_rgba8(std::uint32_t width,
                                             std::uint32_t height,
                                             std::uint64_t& out_vk_handle,
                                             std::string* error_out = nullptr);
    void destroy_vk_image(std::uint64_t vk_handle, std::uint64_t generation) noexcept;

    [[nodiscard]] bool create_vk_shader_module(std::span<const std::uint32_t> spirv_words,
                                               std::uint64_t& out_vk_handle,
                                               std::string* error_out = nullptr);
    void destroy_vk_shader_module(std::uint64_t vk_handle, std::uint64_t generation) noexcept;

    [[nodiscard]] std::size_t live_vk_buffers() const noexcept;
    [[nodiscard]] std::size_t live_vk_images() const noexcept;
    [[nodiscard]] std::size_t live_vk_shader_modules() const noexcept;

    // Opaque state; public only so `driver_loader.cpp`'s GPU helpers can name it.
    struct Impl;

private:
    friend class VulkanRhiDevice;
    std::unique_ptr<Impl> impl_;
};

// Minimal RHI implementation backed by `VulkanDriverLoader`.
// Creates and tracks genuine Vulkan `VkBuffer`, `VkImage`, and `VkShaderModule`
// objects on the active `VkDevice`. Devices that support queue submission and
// fence completion render on the GPU and read back the offscreen RGBA8 target;
// mock or non-executing devices fall back to `SoftwareRhiDevice`.
class VulkanRhiDevice final : public RhiDevice {
public:
    explicit VulkanRhiDevice(std::shared_ptr<VulkanDriverLoader> loader);
    ~VulkanRhiDevice() override;

    VulkanRhiDevice(const VulkanRhiDevice&) = delete;
    VulkanRhiDevice& operator=(const VulkanRhiDevice&) = delete;

    [[nodiscard]] std::string_view backend_name() const noexcept override { return "vulkan"; }
    [[nodiscard]] std::string device_name() const override;
    [[nodiscard]] DriverMode driver_mode() const noexcept override;
    [[nodiscard]] std::uint32_t api_version() const noexcept override;
    [[nodiscard]] const VulkanDriverProbeResult& probe_result() const noexcept;
    [[nodiscard]] bool last_frame_used_gpu() const noexcept;

    [[nodiscard]] BufferHandle create_buffer(BufferUsage usage,
                                             std::span<const std::uint8_t> data,
                                             std::string* error_out = nullptr) override;
    void destroy_buffer(BufferHandle handle) override;
    [[nodiscard]] bool update_buffer(BufferHandle handle,
                                     std::span<const std::uint8_t> data,
                                     std::string* error_out = nullptr) override;

    [[nodiscard]] TextureHandle create_texture_rgba8(
        std::uint32_t width,
        std::uint32_t height,
        std::span<const std::uint8_t> rgba8,
        std::string* error_out = nullptr) override;
    void destroy_texture(TextureHandle handle) override;

    [[nodiscard]] ShaderModuleHandle create_shader_module(
        const ShaderModuleDesc& desc,
        std::string* error_out = nullptr) override;
    void destroy_shader_module(ShaderModuleHandle handle) override;

    [[nodiscard]] PipelineHandle create_pipeline(const PipelineDesc& desc,
                                                 std::string* error_out = nullptr) override;
    void destroy_pipeline(PipelineHandle handle) override;

    [[nodiscard]] bool render_pass_and_readback(
        const RenderPassDesc& pass,
        std::span<const MeshDrawCall> draws,
        std::vector<std::uint8_t>& out_rgba8,
        std::string* error_out = nullptr) override;

    [[nodiscard]] std::size_t live_vk_buffers() const noexcept;
    [[nodiscard]] std::size_t live_vk_images() const noexcept;
    [[nodiscard]] std::size_t live_vk_shader_modules() const noexcept;

private:
    std::shared_ptr<VulkanDriverLoader> loader_{};
    SoftwareRhiDevice rasterizer_{};
    struct VkRef {
        std::uint64_t handle = 0;
        std::uint64_t generation = 0;  // loader device generation at creation
        std::size_t bytes = 0;         // buffers: VkBuffer size
        BufferUsage usage = BufferUsage::vertex;  // buffers only
    };
    std::unordered_map<BufferHandle, VkRef> vk_buffers_{};
    std::unordered_map<TextureHandle, VkRef> vk_textures_{};
    std::unordered_map<ShaderModuleHandle, VkRef> vk_shader_modules_{};
    struct GpuState;
    std::unique_ptr<GpuState> gpu_{};
    bool last_frame_used_gpu_ = false;
    void detach_and_release_gpu_resources() noexcept;
    friend struct VulkanDriverLoader::Impl;  // detaches attached devices on teardown
};

// Convenience helper: initializes a `VulkanDriverLoader` for the requested `mode`
// (or the mock ICD when `allow_mock_fallback == true` and the system/custom binary
// is unavailable, reported through `fell_back_to_mock` / `fallback_reason`),
// creates `VulkanRhiDevice`, and renders the synthetic textured static mesh test
// frame (on the GPU when supported, or via the CPU reference rasterizer on mock).
[[nodiscard]] VulkanDriverProbeResult run_vulkan_mesh_smoke_test(
    DriverMode mode,
    const std::string& custom_icd_path,
    const std::string& hw_stub_path,
    bool allow_mock_fallback,
    std::uint64_t& out_frame_hash,
    std::string* error_out = nullptr);

}  // namespace wse::platform
