# vk-ngx-metalfx

Companion Windows NGX stand-in (`ngxcore`) and host Vulkan layer (`nvlayer`) for running Vulkan DLSS games through MetalFX temporal upscaling on KosmicKrisp (macOS / Wine).

## How it works

1. **`nvlayer/` (`VK_LAYER_METALFX_nvidia_id`):**
   - Reports an NVIDIA PCI vendor/device ID (`0x10DE` / `0x2704`) on `vkGetPhysicalDeviceProperties(2)`.
   - With `NVLAYER_NVX=1`, advertises `VK_NVX_binary_import` and `VK_NVX_image_view_handle` to the application, strips them before `vkCreateDevice` reaches the driver, and stubs their entry points.
   - Optionally overrides the device LUID (`NVLAYER_LUID=<low>:<high>`).
2. **`ngxcore/` (`nvngx.dll` / `_nvngx.dll`):**
   - Minimal Windows NGX core implementing the Vulkan NGX entry points (`NVSDK_NGX_VULKAN_*`) and `NVSDK_NGX_Parameter` MSVC vtable using the public NVIDIA DLSS SDK parameter names.
   - Because `VkImage` handles pass through `winevulkan` 1:1, `EvaluateFeature` reads the real host `VkImage` handles and (with `NGXSTUB_MFX=1`) records `vkCmdPushConstants` with a reserved pipeline-layout value (`0x4D564B4D46583100`) and an 88-byte `mfx_info` payload.
3. **Host Vulkan driver (KosmicKrisp with `MESA_KK_MFX=1`):**
   - Intercepts the magic `vkCmdPushConstants` / `vkCmdPushConstants2KHR` call and encodes `MTL4FXTemporalScaler` into the active Metal command buffer, bracketed by fenced compute encoders.
   - See the KosmicKrisp Draft / RFC MR: https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/46529

## Certificate / Authenticode requirement

Tested on Enshrouded (Vulkan NGX `rel_310_9`):
- An **unsigned** `nvngx.dll` is rejected by `WinVerifyTrust` (`0x80092009`).
- A **`nvngx.dll` signed by a non-NVIDIA certificate** (trusted in the Wine prefix's root store) **loads and initializes DLSS cleanly** — the `"NVIDIA "` signer-name check is not enforced on the `nvngx.dll` fallback when `_nvngx.dll` is absent.

So a Wine/runtime distribution (e.g. Mach9) can sign `nvngx.dll` with its own trusted Authenticode certificate rather than needing an NVIDIA-named cert. For local testing, generate a self-signed code-signing CA, trust it in the Wine prefix (`certutil` / `wine` cert store), and sign `nvngx.dll` with `osslsigncode`.

## Build

### Host Vulkan layer (macOS)
```bash
./nvlayer/build.sh
```
Requires Vulkan headers (`VULKAN_INC=/path/to/include` if not in `/opt/homebrew/include`).

### Windows NGX DLL (mingw-w64)
```bash
./ngxcore/build.sh
```
Set `MINGW_CC=/path/to/x86_64-w64-mingw32-clang` if not on `PATH`.

## Runtime environment

```bash
export VK_LAYER_PATH=/path/to/vk-ngx-metalfx/nvlayer
export VK_INSTANCE_LAYERS=VK_LAYER_METALFX_nvidia_id
export NVLAYER_NVX=1
export NGXSTUB_MFX=1
export NGXSTUB_RATIO=1.7   # optional render-scale override (1.7 ≈ Balanced)
export MESA_KK_MFX=1
```
Place the signed `nvngx.dll` next to the game executable (or point `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath` to it).

## Known limits

- Private `vkCmdPushConstants` ABI (magic layout + raw `VkImage` handles), which is why the driver side is currently a Draft / RFC rather than an upstream Vulkan extension.
- Primary command buffers only; resubmitted reusable command buffers do not re-scale.
- Tested on Enshrouded under Wine/Mach9 on Apple Silicon (M5 Pro, macOS 27.2). Other Vulkan DLSS games may query additional NGX capability parameters or Vulkan device properties.
