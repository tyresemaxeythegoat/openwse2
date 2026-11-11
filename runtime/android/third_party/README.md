# Vendored Vulkan headers

Source: [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers),
`main` branch, fetched 2026-11-11. `VK_HEADER_VERSION` 368.

Only the C headers are kept (`vulkan*.h`, `vk_icd.h`, `vk_layer.h`, and the
`vk_video/` codec headers); the C++ headers were removed to keep the tree
small. `vulkan_core.h` includes `vk_video/*` unconditionally, so those files are
required even though this project does not use video extensions.

License: Apache-2.0 OR MIT, see `vulkan/LICENSE.md`.
