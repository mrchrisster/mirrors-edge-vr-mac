// macOS side of the Mirror's Edge VR mod: what d3d9.cpp calls in place of D3D11 and vtable
// patching. Implemented in mevr-mac/src/mevr_mac.cpp.
#pragma once

#include <vector>
#include "MinHook.h"
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

typedef VkImage MevrImage;
#define MEVR_FMT_BGRA8_SRGB  ((int64_t)VK_FORMAT_B8G8R8A8_SRGB)
#define MEVR_FMT_RGBA8_SRGB  ((int64_t)VK_FORMAT_R8G8B8A8_SRGB)
#define MEVR_FMT_BGRA8_UNORM ((int64_t)VK_FORMAT_B8G8R8A8_UNORM)

// OpenXR instance and HMD system, created by the glue at load time (DXVK's Vulkan instance and
// device have to be created through them, before the game creates its D3D9 device).
XrInstance MevrXrInstance();
XrSystemId MevrXrSystem();

// Fills the Vulkan binding with DXVK's instance, device and submission queue.
bool MevrGpuSessionBinding(XrGraphicsBindingVulkan2KHR* bind);
void MevrGpuSwapchainImages(XrSwapchain swapchain, std::vector<MevrImage>& out);

// This frame's back buffer as the copy source (transitioned for transfer until EndCapture).
bool MevrGpuBeginCapture(struct IDirect3DDevice9* dev, unsigned* w, unsigned* h);
bool MevrGpuHaveCapture();
void MevrGpuEndCapture();
// Copies a rectangle of the captured back buffer to (0,0) of a swapchain image; submitted at once.
void MevrGpuCopyFromCapture(MevrImage dst, unsigned x, unsigned y, unsigned w, unsigned h);
void MevrGpuClear(MevrImage dst, float r, float g, float b, float a);
void MevrGpuUpload(MevrImage dst, const uint32_t* pixels, unsigned w, unsigned h);

// IDirect3D9 / IDirect3DDevice9 / IDirect3DQuery9 slot replacement through the runtime.
void* MevrPatchSlot(void* obj, int index, void* repl);

// OpenXR calls that may submit to DXVK's queue go through the submission lock.
XrResult MevrXrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*);
XrResult MevrXrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*);
XrResult MevrXrBeginFrame(XrSession, const XrFrameBeginInfo*);
XrResult MevrXrEndFrame(XrSession, const XrFrameEndInfo*);
#define xrAcquireSwapchainImage MevrXrAcquireSwapchainImage
#define xrReleaseSwapchainImage MevrXrReleaseSwapchainImage
#define xrBeginFrame MevrXrBeginFrame
#define xrEndFrame MevrXrEndFrame
void MevrGuestCallRefused(const char* where);

// XInput through the runtime: the physical pad, and the mod's XInputGetState for the game.
DWORD MevrRealXInputGetState(DWORD index, void* state);
void MevrSetPadHook(void* hook);

// Game functions: detours through the runtime's entry hooks (lift.py ENTRY_HOOKS), and calls.
enum MevrHookKind { MEVR_HOOK_THIS_PP = 1,   // void __thiscall f(void*, void*), ret 8
                    MEVR_HOOK_THIS_FI = 2 }; // void __thiscall f(float, int), ret 8
MH_STATUS MevrHookGuest(uintptr_t target, void* detour, void** original, int kind);
// ProcessEvent(Function, Parms, nullptr) on object; parms (up to size bytes) copied in and back.
bool MevrGuestProcessEvent(uintptr_t pe, uintptr_t object, uintptr_t function, void* parms, size_t size);
// A native exec with a synthetic FFrame over `code` (bytecode); result copied back.
bool MevrGuestNativeExec(uintptr_t exec, uintptr_t object, uintptr_t function, const unsigned char* code,
                         unsigned codeLen, void* result, unsigned resultLen);

// Diagnostics: true when draw duplication is vetoed for the current draw (MEVR_NO_DUP / MEVR_DUP_SKIP_VS).
bool MevrDupVeto();
bool MevrRevertBegin(struct IDirect3DDevice9* dev, float saved[16]);
void MevrRevertEnd(struct IDirect3DDevice9* dev, const float saved[16]);
