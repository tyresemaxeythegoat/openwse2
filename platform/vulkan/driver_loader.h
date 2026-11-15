// Platform seam: Runtime Vulkan driver loader (System / Turnip / PanVK / Mock ICD)
// and minimal Vulkan RHI backend (`docs/PLATFORM_PLAN.md`, slice P4).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
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
    bool is_mock = false;
    std::uint32_t negotiated_icd_version = 0;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t graphics_queue_family = 0;
    std::string device_name{};
    std::string loaded_library{};
    std::string error{};
};

struct VulkanMockConfig {
    DriverMode mode = DriverMode::panvk;
    std::string device_name = "Mali-G610 (PanVK Mock ICD)";
    std::uint32_t vendor_id = 0x13B5U;
    std::uint32_t device_id = 0x0610U;
    std::uint32_t api_version = 0x00403000U;     // Vulkan 1.3.0
    std::uint32_t driver_version = 0x06C00000U;  // Mesa 27
    bool direct_icd = true;
};

[[nodiscard]] std::string format_vulkan_version(std::uint32_t vk_version);

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

    // Loads a custom ICD `.so`/`.dll`. For PanVK on Android, pass the path to
    // bundled `libpanvk_hw.so` in `hw_stub_so_path` so it is opened RTLD_GLOBAL
    // before the patched `libvulkan_panfrost.so`.
    [[nodiscard]] bool load_custom_driver(DriverMode mode,
                                          const std::string& icd_so_path,
                                          const std::string& hw_stub_so_path = {},
                                          std::string* error_out = nullptr);

    // Attaches an in-process mock ICD implementing the Vulkan ICD v5 ABI for
    // deterministic CI/host testing without physical Mali/Adreno hardware.
    [[nodiscard]] bool load_mock_icd(const VulkanMockConfig& config);

    // Creates `VkInstance`, selects a graphics physical device, and opens `VkDevice`.
    [[nodiscard]] bool probe_and_create_device(VulkanDriverProbeResult& out);

    void unload() noexcept;

    [[nodiscard]] bool is_loaded() const noexcept;
    [[nodiscard]] bool has_device() const noexcept;
    [[nodiscard]] const VulkanDriverProbeResult& probe_result() const noexcept;

    // Vulkan object handle tracking (non-zero opaque uint64 handles).
    [[nodiscard]] bool create_vk_buffer(std::size_t size_bytes,
                                        BufferUsage usage,
                                        std::uint64_t& out_vk_handle,
                                        std::string* error_out = nullptr);
    void destroy_vk_buffer(std::uint64_t vk_handle) noexcept;

    [[nodiscard]] bool create_vk_image_rgba8(std::uint32_t width,
                                             std::uint32_t height,
                                             std::uint64_t& out_vk_handle,
                                             std::string* error_out = nullptr);
    void destroy_vk_image(std::uint64_t vk_handle) noexcept;

    [[nodiscard]] std::size_t live_vk_buffers() const noexcept;
    [[nodiscard]] std::size_t live_vk_images() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Minimal RHI implementation backed by `VulkanDriverLoader`.
// Creates and tracks genuine Vulkan `VkBuffer` and `VkImage` objects on the active
// `VkDevice`, and executes deterministic offscreen readback through the P4
// reference rasterizer so host/CI verification stays driver-independent until P5.
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

    [[nodiscard]] BufferHandle create_buffer(BufferUsage usage,
                                             std::span<const std::uint8_t> data,
                                             std::string* error_out = nullptr) override;
    void destroy_buffer(BufferHandle handle) override;

    [[nodiscard]] TextureHandle create_texture_rgba8(
        std::uint32_t width,
        std::uint32_t height,
        std::span<const std::uint8_t> rgba8,
        std::string* error_out = nullptr) override;
    void destroy_texture(TextureHandle handle) override;

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

private:
    std::shared_ptr<VulkanDriverLoader> loader_{};
    SoftwareRhiDevice rasterizer_{};
    std::unordered_map<BufferHandle, std::uint64_t> vk_buffers_{};
    std::unordered_map<TextureHandle, std::uint64_t> vk_textures_{};
};

// Convenience helper: initializes a `VulkanDriverLoader` for the requested `mode`
// (or mock fallback when `allow_mock_fallback == true` if the system/custom binary
// is unavailable on headless/emulator devices), creates `VulkanRhiDevice`, and
// renders the synthetic textured static mesh test frame.
[[nodiscard]] VulkanDriverProbeResult run_vulkan_mesh_smoke_test(
    DriverMode mode,
    const std::string& custom_icd_path,
    const std::string& hw_stub_path,
    bool allow_mock_fallback,
    std::uint64_t& out_frame_hash,
    std::string* error_out = nullptr);

}  // namespace wse::platform
