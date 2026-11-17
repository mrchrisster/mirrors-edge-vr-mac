"""Applies the macOS port edits to the mod's source (vr-mod, branch `macos`).

Every edit is guarded by MEVR_MAC, so the Windows build is unchanged. Each anchor must match
exactly once; the script refuses to run on a tree it has already patched.
Run: python3 -I port_patch.py
"""
import os
import sys

SRC = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "vr-mod", "src"))


def load(name):
    with open(os.path.join(SRC, name), encoding="utf-8", newline="") as f:
        return f.read()


def save(name, text):
    with open(os.path.join(SRC, name), "w", encoding="utf-8", newline="") as f:
        f.write(text)


def nl(text):
    return "\r\n" if "\r\n" in text else "\n"


def sub(text, old, new, name):
    e = nl(text)
    old, new = old.replace("\n", e), new.replace("\n", e)
    n = text.count(old)
    if n != 1:
        sys.exit(f"{name}: anchor found {n} times:\n{old[:200]}")
    return text.replace(old, new)


def wrap(text, start, end, mac, name):
    """Wraps text from the line starting with `start` up to (not including) the line starting
    with `end` in #ifndef MEVR_MAC, with `mac` as the #else branch."""
    e = nl(text)
    s_at = text.find(start)
    if s_at < 0:
        sys.exit(f"{name}: start anchor missing: {start[:80]}")
    if text.find(start, s_at + 1) >= 0:
        sys.exit(f"{name}: start anchor not unique: {start[:80]}")
    e_at = text.find(end, s_at)
    if e_at < 0:
        sys.exit(f"{name}: end anchor missing: {end[:80]}")
    block = text[s_at:e_at]
    repl = "#ifndef MEVR_MAC" + e + block + "#else" + e + mac.replace("\n", e) + "#endif" + e
    return text[:s_at] + repl + text[e_at:]


d = load("d3d9.cpp")
if "MEVR_MAC" in d:
    sys.exit("d3d9.cpp is already patched")

# ---- headers ----
d = sub(d, """#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
""", """#ifdef MEVR_MAC
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#else
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <windows.h>
#include <d3d9.h>
#ifndef MEVR_MAC
#include <d3d11.h>
#include <dxgi1_2.h>
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#ifdef MEVR_MAC
#include "mevr_gpu_mac.h"   // macOS: Vulkan output through DXVK interop, game memory, hooks
#else
typedef ID3D11Texture2D* MevrImage;
#define MEVR_FMT_BGRA8_SRGB  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
#define MEVR_FMT_RGBA8_SRGB  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
#define MEVR_FMT_BGRA8_UNORM DXGI_FORMAT_B8G8R8A8_UNORM
#endif
""", "headers")

# ---- OpenXR / D3D11 state ----
d = sub(d, """static ID3D11Device*        g_dev11    = nullptr;
static ID3D11DeviceContext* g_ctx11    = nullptr;
""", """#ifdef MEVR_MAC
static bool                 g_dev11    = false;  // macOS: true once the Vulkan session exists
#else
static ID3D11Device*        g_dev11    = nullptr;
static ID3D11DeviceContext* g_ctx11    = nullptr;
#endif
""", "state dev11")
d = sub(d, "static ID3D11Texture2D**    g_scImages   = nullptr;\n",
        "static MevrImage*           g_scImages   = nullptr;\n", "state scImages")
d = sub(d, "static ID3D11Texture2D*     g_upload   = nullptr;   // DYNAMIC D3D11 texture the frame lands in\n",
        "#ifndef MEVR_MAC\nstatic ID3D11Texture2D*     g_upload   = nullptr;   // DYNAMIC D3D11 texture the frame lands in\n#endif\n",
        "state upload")

# ---- vtable patching -> the runtime's slot table ----
d = sub(d, """static void* PatchVTable(void* obj, int index, void* repl)
{
    if (!obj) return nullptr;
""", """static void* PatchVTable(void* obj, int index, void* repl)
{
#ifdef MEVR_MAC
    return MevrPatchSlot(obj, index, repl);
#endif
    if (!obj) return nullptr;
""", "PatchVTable")

# ---- InitXR: the instance and system are created by the host glue before DXVK's Vulkan ----
d = sub(d, """    XrStartupDiagnostics diagnostics;
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
""", """#ifdef MEVR_MAC
    g_xrInstance = MevrXrInstance();
    g_xrSystem = MevrXrSystem();
    if (!g_xrInstance || g_xrSystem == XR_NULL_SYSTEM_ID) {
        Log("[xr] macOS: no OpenXR instance/system from the host (runtime or headset missing)");
        return false;
    }
    XrResult r = XR_SUCCESS;
    const char* exts[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    if (false) {
#else
    XrStartupDiagnostics diagnostics;
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
#endif
""", "InitXR head")
# close the `if (false) {` around the instance + system creation, just before the view query
d = sub(d, """    if (!XrLogResult("xrGetSystem (HMD)", xrGetSystem(g_xrInstance, &sgi, &g_xrSystem))) return false;
""", """    if (!XrLogResult("xrGetSystem (HMD)", xrGetSystem(g_xrInstance, &sgi, &g_xrSystem))) return false;
#ifdef MEVR_MAC
    }
    (void)r;
#endif
""", "InitXR system")
d = wrap(d, "    PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;",
         "    XrResult sr = xrCreateSession(",
         """    // macOS: DXVK's own Vulkan device, created through the runtime (XR_KHR_vulkan_enable2).
    XrGraphicsBindingVulkan2KHR bind{ XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
    if (!MevrGpuSessionBinding(&bind)) { Log("[xr] macOS: no Vulkan binding from DXVK"); return false; }
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &bind; sci.systemId = g_xrSystem;
""", "InitXR binding")
d = sub(d, """    XrResult sr = xrCreateSession(g_xrInstance, &sci, &g_xrSession);
    if (!XrLogResult("xrCreateSession", sr)) return false;
""", """    XrResult sr = xrCreateSession(g_xrInstance, &sci, &g_xrSession);
    if (!XrLogResult("xrCreateSession", sr)) return false;
#ifdef MEVR_MAC
    g_dev11 = true;
#endif
""", "InitXR session")

# ---- swapchain formats and images ----
d = sub(d, """    const int64_t prefer[] = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                               DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                               DXGI_FORMAT_B8G8R8A8_UNORM };
""", """    const int64_t prefer[] = { MEVR_FMT_BGRA8_SRGB, MEVR_FMT_RGBA8_SRGB, MEVR_FMT_BGRA8_UNORM };
""", "formats prefer")
d = sub(d, """        (chosen == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || chosen == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
""", """        (chosen == MEVR_FMT_BGRA8_SRGB || chosen == MEVR_FMT_RGBA8_SRGB)
""", "formats log")
d = wrap(d, "    xrEnumerateSwapchainImages(g_swapchain, 0, &g_scImageCount, nullptr);",
         "    g_scW = w; g_scH = h; g_scFormat = chosen;",
         """    {
        std::vector<MevrImage> imgs;
        MevrGpuSwapchainImages(g_swapchain, imgs);
        g_scImageCount = (uint32_t)imgs.size();
        g_scImages = new MevrImage[g_scImageCount];
        for (uint32_t i = 0; i < g_scImageCount; ++i) g_scImages[i] = imgs[i];
    }
""", "EnsureSwapchain images")
d = sub(d, """    if (g_scImageCount > 0 && g_scImages[0]) {
        D3D11_TEXTURE2D_DESC td{};
""", """#ifndef MEVR_MAC
    if (g_scImageCount > 0 && g_scImages[0]) {
        D3D11_TEXTURE2D_DESC td{};
""", "EnsureSwapchain desc")
d = sub(d, """            Log("[xr] note: resource format differs from the requested one - typeless, as expected");
    }
    return true;
}
""", """            Log("[xr] note: resource format differs from the requested one - typeless, as expected");
    }
#endif
    return true;
}
""", "EnsureSwapchain desc end")

# ---- frame capture: the back buffer itself, through DXVK interop (no CPU copy) ----
d = wrap(d, "static void ReleaseFrameCapture()\n",
         "static void ApplyHeadTracking(XrTime when);",
         """static void ReleaseFrameCapture()
{
    PkGhostReleaseState();      // PK.43: a state block dies with the device, like everything here
    MevrGpuEndCapture();
    g_capW = g_capH = 0;
    g_capFmt = D3DFMT_UNKNOWN;
    g_haveFrame = false;
}
static bool  g_fastCapture     = true;      // kept for the menu and the ini; macOS always shares
static bool  g_fastCaptureOK   = true;
typedef HRESULT (STDMETHODCALLTYPE *PFN_StretchRect)(IDirect3DDevice9*, IDirect3DSurface9*,
                                                     const RECT*, IDirect3DSurface9*,
                                                     const RECT*, D3DTEXTUREFILTERTYPE);
extern PFN_StretchRect g_origStretchRect;
static bool CaptureFrame(IDirect3DDevice9* dev)
{
    if (!g_dev11) return false;
    const double t0 = NowMs();
    UINT w = 0, h = 0;
    if (!MevrGpuBeginCapture(dev, &w, &h)) return false;
    g_capW = w; g_capH = h;
    g_capMsTotal += NowMs() - t0;
    g_capSamples++;
    return true;
}

""", "capture")

d = sub(d, "static ID3D11Texture2D** g_eyeImages[2] = { nullptr, nullptr };\n",
        "static MevrImage*        g_eyeImages[2] = { nullptr, nullptr };\n", "eye images decl")
d = wrap(d, "        xrEnumerateSwapchainImages(g_eyeSwap[e], 0, &g_eyeImageCount[e], nullptr);",
         "    }\n    g_eyeW = w; g_eyeH = h;",
         """        std::vector<MevrImage> imgs;
        MevrGpuSwapchainImages(g_eyeSwap[e], imgs);
        g_eyeImageCount[e] = (uint32_t)imgs.size();
        g_eyeImages[e] = new MevrImage[g_eyeImageCount[e]];
        for (uint32_t i = 0; i < g_eyeImageCount[e]; ++i) g_eyeImages[e][i] = imgs[i];
""", "EnsureEyeSwapchains images")

d = sub(d, """    ID3D11Resource* src = FrameSource();
    if (eye < 0 || eye > 1 || g_eyeSwap[eye] == XR_NULL_HANDLE || !src) return false;
""", """#ifdef MEVR_MAC
    const bool src = MevrGpuHaveCapture();
#else
    ID3D11Resource* src = FrameSource();
#endif
    if (eye < 0 || eye > 1 || g_eyeSwap[eye] == XR_NULL_HANDLE || !src) return false;
""", "FillEye source")
d = wrap(d, "        if (g_simulStereo) {\n            // Both eyes live in one frame side by side",
         "        ok = true;\n    }\n    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };\n    xrReleaseSwapchainImage(g_eyeSwap[eye], &ri);\n    if (ok) g_eyeFilled[eye] = true;",
         """        if (g_simulStereo)
            MevrGpuCopyFromCapture(g_eyeImages[eye][idx], SceneX() + (eye == 0 ? 0 : SceneW() / 2),
                                   SceneY(), SceneW() / 2, SceneH());
        else
            MevrGpuCopyFromCapture(g_eyeImages[eye][idx], 0, 0, g_capW, g_capH);
""", "FillEye copy")

d = wrap(d, "              ID3D11Resource* qsrc = FrameSource();",
         "            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };\n            xrReleaseSwapchainImage(g_swapchain, &ri);",
         """              if (g_haveFrame && MevrGpuHaveCapture()) {
                MevrGpuCopyFromCapture(g_scImages[idx], 0, 0, g_capW, g_capH);
                submitted = true;
              } else {
                float t = (float)(n % 180) / 180.0f * 6.2831853f;
                MevrGpuClear(g_scImages[idx], 0.5f + 0.5f * sinf(t), 0.5f + 0.5f * sinf(t + 2.0944f),
                             0.5f + 0.5f * sinf(t + 4.1888f), 1.0f);
                submitted = true;
              }
            }
""", "SubmitTestQuad copy")

# ---- MSVC-only spellings ----
d = sub(d, """    Log("[host] source build %s with MSC %d", kMevrBuildId, (int)_MSC_VER);
""", """#ifdef MEVR_MAC
    Log("[host] source build %s with clang %s (macOS arm64 port)", kMevrBuildId, __clang_version__);
#else
    Log("[host] source build %s with MSC %d", kMevrBuildId, (int)_MSC_VER);
#endif
""", "MSC_VER")
# Clang rejects `static` on a definition that an earlier `extern`/plain declaration made
# non-static (MSVC accepts it). Dropping `static` from these definitions is harmless everywhere.
for decl in ["static volatile LONG g_vmScanArmed = 0;", "static int  g_vmBestReg = -1;",
             "static bool g_vmBestRow = false;", "static int  g_vmCandidates = 0;",
             "static bool GetCameraPose(float* loc, float* fwd)\n", "static bool g_overlay = true;"]:
    d = sub(d, decl, decl[len("static "):], "static " + decl[:30])

# ---- entry points: the runtime made the IDirect3D9 (DXVK); the mod only has to patch it ----
d = sub(d, """    if (!d3d) {
        pfn_Create9 fn = (pfn_Create9)Real("Direct3DCreate9");
        d3d = fn ? fn(SDKVersion) : nullptr;
    }
""", """#ifdef MEVR_MAC
    if (!d3d) d3d = g_macCreatedD3D9;
#else
    if (!d3d) {
        pfn_Create9 fn = (pfn_Create9)Real("Direct3DCreate9");
        d3d = fn ? fn(SDKVersion) : nullptr;
    }
#endif
""", "Direct3DCreate9 body")
d = sub(d, """extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
""", """#ifdef MEVR_MAC
static IDirect3D9* g_macCreatedD3D9 = nullptr;
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion);
// macOS: called by the glue with the IDirect3D9 the runtime created through DXVK.
extern "C" void MevrModDirect3D9Created(IDirect3D9* d3d)
{
    g_macCreatedD3D9 = d3d;
    Direct3DCreate9(32);
    g_macCreatedD3D9 = nullptr;
}
BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID);
extern "C" void MevrModAttach(void* self)
{
    DllMain((HMODULE)self, DLL_PROCESS_ATTACH, nullptr);
}
#endif
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
""", "Direct3DCreate9 head")

# DXVK's MEMORYSTATUS has only the fields DXVK uses; this diagnostic never runs on macOS anyway
# (kernel32 cannot be hooked), so it logs the total only.
d = sub(d, """            (long)n, g_frames, (unsigned long)ms->dwMemoryLoad,
            (unsigned long)(ms->dwAvailPhys >> 20), (unsigned long)(ms->dwTotalPhys >> 20),
            (unsigned long)(ms->dwAvailVirtual >> 20),
            (unsigned long)(ms->dwTotalVirtual >> 20));
""", """#ifdef MEVR_MAC
            (long)n, g_frames, 0ul, 0ul, (unsigned long)(ms->dwTotalPhys >> 20), 0ul, 0ul);
#else
            (long)n, g_frames, (unsigned long)ms->dwMemoryLoad,
            (unsigned long)(ms->dwAvailPhys >> 20), (unsigned long)(ms->dwTotalPhys >> 20),
            (unsigned long)(ms->dwAvailVirtual >> 20),
            (unsigned long)(ms->dwTotalVirtual >> 20));
#endif
""", "GlobalMemoryStatus log")

# Calls INTO game code (ProcessEvent, native execs) go through the runtime (rt_vr.h call_guest),
# with their parameter blocks copied into guest memory and changed bytes copied back.
d = sub(d, """        __try {
            ((PkProcessEventFn)pe)((void*)obj, (void*)fn, parms, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) { crashed = true; }
""", """#ifdef MEVR_MAC
        crashed = !MevrGuestProcessEvent(pe, (uintptr_t)obj, (uintptr_t)fn, parms, sizeof(parms));
#else
        __try {
            ((PkProcessEventFn)pe)((void*)obj, (void*)fn, parms, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) { crashed = true; }
#endif
""", "guest call 1")
d = sub(d, """    bool crashed = false;
    __try {
        ((PkProcessEventFn)pe)((void*)obj, (void*)fn, parms, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
""", """    bool crashed = false;
#ifdef MEVR_MAC
    if (!MevrGuestProcessEvent(pe, (uintptr_t)obj, (uintptr_t)fn, parms, sizeof(parms))) {
#else
    __try {
        ((PkProcessEventFn)pe)((void*)obj, (void*)fn, parms, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
#endif
""", "guest call 2")

# The game image lives in guest memory: its headers are read through the guest mapping.
d = sub(d, """    g_imgBase = (uintptr_t)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)g_imgBase;
    auto* nt  = (IMAGE_NT_HEADERS*)(g_imgBase + dos->e_lfanew);
""", """    g_imgBase = (uintptr_t)GetModuleHandleW(nullptr);
#ifdef MEVR_MAC
    auto* dos = (IMAGE_DOS_HEADER*)mevr_guest_ptr(g_imgBase);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) { Log("[obj] game image headers not mapped at %p", (void*)g_imgBase); return; }
    auto* nt  = (IMAGE_NT_HEADERS*)mevr_guest_ptr(g_imgBase + dos->e_lfanew);
    if (!nt) return;
#else
    auto* dos = (IMAGE_DOS_HEADER*)g_imgBase;
    auto* nt  = (IMAGE_NT_HEADERS*)(g_imgBase + dos->e_lfanew);
#endif
""", "FindSections")
d = sub(d, """        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)GetModuleHandleW(nullptr);
        if (dos) {""", """#ifdef MEVR_MAC
        const IMAGE_DOS_HEADER* dos = nullptr;   // a 4 GB guest space: nothing to report
#else
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)GetModuleHandleW(nullptr);
#endif
        if (dos) {""", "LogAddressSpace")

# XInput: the runtime asks the module for the game's pad (rt_vr.h pad_get) instead of a detour.
d = sub(d, """    HMODULE xi = GetModuleHandleA("xinput1_3.dll");
    if (!xi) xi = GetModuleHandleA("XINPUT1_3.dll");""", """#ifdef MEVR_MAC
    g_origXiGetState = (PFN_XInputGetState)MevrRealXInputGetState;
    MevrSetPadHook((void*)&Hook_XInputGetState);
    g_xiHooked = true;
    Log("*** [pad] XInputGetState answered for the game by the runtime's pad hook - motion controllers act as a pad");
    return;
#endif
    HMODULE xi = GetModuleHandleA("xinput1_3.dll");
    if (!xi) xi = GetModuleHandleA("XINPUT1_3.dll");""", "XInput hook")

# Game-function detours: MinHook on Windows, the runtime's entry hooks here (lift.py ENTRY_HOOKS).
d = sub(d, """    const MH_STATUS create = MH_CreateHook(reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(&Hook_Update1pArms),
        reinterpret_cast<void**>(&g_origUpdate1pArms));""", """#ifdef MEVR_MAC
    const MH_STATUS create = MevrHookGuest(target, reinterpret_cast<void*>(&Hook_Update1pArms),
        reinterpret_cast<void**>(&g_origUpdate1pArms), MEVR_HOOK_THIS_PP);
#else
    const MH_STATUS create = MH_CreateHook(reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(&Hook_Update1pArms),
        reinterpret_cast<void**>(&g_origUpdate1pArms));
#endif""", "hook Update1pArms")
d = sub(d, """    if (MH_CreateHook(reinterpret_cast<void*>(updateSkelPose),
                      reinterpret_cast<void*>(&Hook_UpdateSkelPose),
                      reinterpret_cast<void**>(&g_origUpdateSkelPose)) != MH_OK) {""", """#ifdef MEVR_MAC
    if (MevrHookGuest(updateSkelPose, reinterpret_cast<void*>(&Hook_UpdateSkelPose),
                      reinterpret_cast<void**>(&g_origUpdateSkelPose), MEVR_HOOK_THIS_FI) != MH_OK) {
#else
    if (MH_CreateHook(reinterpret_cast<void*>(updateSkelPose),
                      reinterpret_cast<void*>(&Hook_UpdateSkelPose),
                      reinterpret_cast<void**>(&g_origUpdateSkelPose)) != MH_OK) {
#endif""", "hook UpdateSkelPose")

# Diagnostics (macOS): veto draw duplication globally or for listed vertex shaders (MEVR_NO_DUP,
# MEVR_DUP_SKIP_VS) to bisect stereo rendering faults.
d = sub(d, """static bool ShouldDuplicate()
{
""", """static bool ShouldDuplicate()
{
#ifdef MEVR_MAC
    if (MevrDupVeto()) return false;
#endif
""", "dup veto")

d = sub(d, """        LoadSettings();
        VrMenuInitializeSettings();""", """        LoadSettings();
        VrMenuInitializeSettings();
#ifdef MEVR_MAC
        // Diagnostics: MEVR_FOV_FORCE=0 leaves the engine's own projection scale in place.
        if (getenv("MEVR_FOV_FORCE") && *getenv("MEVR_FOV_FORCE") == '0') { g_fovForce = false; Log("[mac] FOV forcing off (MEVR_FOV_FORCE=0)"); }
#endif""", "fov force env")

# Diagnostics (macOS): draw listed vertex shaders with the engine's unforced scene matrix
# (MEVR_REVERT_VS) to find draws that FOV forcing breaks.
d = sub(d, """    if(DrawStereoDistantSun(dev,[&]{return g_origDrawIndexed(dev,type,baseVertex,minIndex,numVerts,startIndex,primCount);},&uiResult))return uiResult;
    if (!ShouldDuplicate())""", """    if(DrawStereoDistantSun(dev,[&]{return g_origDrawIndexed(dev,type,baseVertex,minIndex,numVerts,startIndex,primCount);},&uiResult))return uiResult;
#ifdef MEVR_MAC
    { float mevrSaved[16]; if (MevrRevertBegin(dev, mevrSaved)) {
        const HRESULT r = g_origDrawIndexed(dev, type, baseVertex, minIndex, numVerts, startIndex, primCount);
        MevrRevertEnd(dev, mevrSaved); return r; } }
#endif
    if (!ShouldDuplicate())""", "revert indexed")
d = sub(d, """    if(DrawStereoDistantSun(dev,[&]{return g_origDrawPrim(dev,type,start,count);},&uiResult))return uiResult;
    if (!ShouldDuplicate()) return g_origDrawPrim(dev, type, start, count);""", """    if(DrawStereoDistantSun(dev,[&]{return g_origDrawPrim(dev,type,start,count);},&uiResult))return uiResult;
#ifdef MEVR_MAC
    { float mevrSaved[16]; if (MevrRevertBegin(dev, mevrSaved)) {
        const HRESULT r = g_origDrawPrim(dev, type, start, count);
        MevrRevertEnd(dev, mevrSaved); return r; } }
#endif
    if (!ShouldDuplicate()) return g_origDrawPrim(dev, type, start, count);""", "revert prim")

# macOS: cap the FOV written into the engine for culling (MEVR_CULL_FOV_MAX, default 135).
d = sub(d, """            const float want = atanf(t) * 2.0f * 57.29578f;
""", """#ifdef MEVR_MAC
            float want = atanf(t) * 2.0f * 57.29578f;
            // Above ~140 degrees the engine's own sky breaks (measured: blue up to 138, white at
            // 142 - the uncapped value for a 110-degree eye). 135 keeps a margin; culling then
            // covers ~127 of the frame's 142, i.e. the outer few degrees of each eye may cull early.
            { const char* cap = getenv("MEVR_CULL_FOV_MAX"); const float c = cap ? (float)atof(cap) : 135.0f;
              if (c > 10.0f && want > c) want = c; }
#else
            const float want = atanf(t) * 2.0f * 57.29578f;
#endif
""", "cull fov cap")

# macOS (OXRSys streams each eye as covering the headset FOV, so the rendered frustum must cover
# it on every side): the horizontal target also has to reach the outer edge of each eye, not just
# follow from the vertical and the eye aspect. If it does not, widen both (square pixels kept).
d = sub(d, """                g_targetHalfFovX = atanf(tanf(halfY) * aspect);
""", """                g_targetHalfFovX = atanf(tanf(halfY) * aspect);
#ifdef MEVR_MAC
                {   float lr = 0.0f;
                    for (int e = 0; e < 2; ++e) {
                        if (-g_views[e].fov.angleLeft > lr) lr = -g_views[e].fov.angleLeft;
                        if (g_views[e].fov.angleRight > lr) lr = g_views[e].fov.angleRight;
                    }
                    lr *= 1.01f;
                    if (lr > g_targetHalfFovX && lr < 1.45f) {
                        g_targetHalfFovX = lr;
                        g_targetHalfFovY = atanf(tanf(lr) / aspect);
                    }
                }
#endif
""", "fov cover horizontal")

save("d3d9.cpp", d)
print("d3d9.cpp patched")

# ---- vr_menu.inl: the settings panel is CPU-rasterized; upload it through a staging buffer ----
m = load("vr_menu.inl")
if "MEVR_MAC" in m:
    sys.exit("vr_menu.inl is already patched")
m = sub(m, "static std::vector<ID3D11Texture2D*> g_menuImages;\nstatic ID3D11Texture2D* g_menuUpload=nullptr;\n",
        "static std::vector<MevrImage> g_menuImages;\n#ifdef MEVR_MAC\nstatic bool g_menuUpload=false;\n#else\n"
        "static ID3D11Texture2D* g_menuUpload=nullptr;\n#endif\n", "menu decl")
m = wrap(m, "    std::vector<XrSwapchainImageD3D11KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});",
         "    g_menuPixels.resize(kMenuW*kMenuH);",
         """    g_menuImages.clear();MevrGpuSwapchainImages(g_menuSwap,g_menuImages);
    if(g_menuImages.empty())return false;
    g_menuUpload=true;
""", "menu images")
m = sub(m, "        const bool bgra=g_scFormat==DXGI_FORMAT_B8G8R8A8_UNORM || g_scFormat==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;\n",
        "        const bool bgra=g_scFormat==MEVR_FMT_BGRA8_UNORM || g_scFormat==MEVR_FMT_BGRA8_SRGB;\n", "menu bgra")
m = sub(m, "        g_ctx11->UpdateSubresource(g_menuUpload,0,nullptr,g_menuPixels.data(),kMenuW*4,0);\n",
        "#ifndef MEVR_MAC\n        g_ctx11->UpdateSubresource(g_menuUpload,0,nullptr,g_menuPixels.data(),kMenuW*4,0);\n#endif\n",
        "menu update")
m = sub(m, "    if(ready && index<g_menuImages.size())g_ctx11->CopyResource(g_menuImages[index],g_menuUpload);\n",
        "#ifdef MEVR_MAC\n    if(ready && index<g_menuImages.size())MevrGpuUpload(g_menuImages[index],g_menuPixels.data(),kMenuW,kMenuH);\n"
        "#else\n    if(ready && index<g_menuImages.size())g_ctx11->CopyResource(g_menuImages[index],g_menuUpload);\n#endif\n",
        "menu copy")
save("vr_menu.inl", m)
for name, old, new in [
    ("combat.inl", """    __try {
        ((PkProcessEventFn)pe)((void*)object, (void*)fn, parms, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
""", """#ifdef MEVR_MAC
    if (!MevrGuestProcessEvent(pe, (uintptr_t)object, (uintptr_t)fn, parms, 256)) {
#else
    __try {
        ((PkProcessEventFn)pe)((void*)object, (void*)fn, parms, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
#endif
"""),
    ("combat_native.inl", """    __try { ((PFN_Update1pArms)exec)((void*)object,nullptr,&frame,result); }
    __except(EXCEPTION_EXECUTE_HANDLER){""", """#ifdef MEVR_MAC
    if(!MevrGuestNativeExec(exec,(uintptr_t)object,(uintptr_t)function,code,used,result,fn==CState?8:4)){
#else
    __try { ((PFN_Update1pArms)exec)((void*)object,nullptr,&frame,result); }
    __except(EXCEPTION_EXECUTE_HANDLER){
#endif
"""),
    ("pipe_exit.inl", """    __try {((PkProcessEventFn)pe)((void*)object,(void*)g_pipeFunctions[function].fn,parameters,nullptr);}
    __except(EXCEPTION_EXECUTE_HANDLER){""", """#ifdef MEVR_MAC
    if(!MevrGuestProcessEvent(pe,(uintptr_t)object,(uintptr_t)g_pipeFunctions[function].fn,parameters,256)){
#else
    __try {((PkProcessEventFn)pe)((void*)object,(void*)g_pipeFunctions[function].fn,parameters,nullptr);}
    __except(EXCEPTION_EXECUTE_HANDLER){
#endif
""")]:
    t = load(name)
    if "MEVR_MAC" in t:
        sys.exit(name + " is already patched")
    save(name, sub(t, old, new, name))
    print(name, "patched")
# Game pop-ups/HUD: the mod centres them in each eye, i.e. at infinity, so with near geometry
# behind them the eyes cannot fuse the text. On macOS they converge at MEVR_UI_DISTANCE metres
# (default 2.0): each eye's copy moves toward the nose by the matching disparity.
t = load("stereo_effects.inl")
t = sub(t, """        vp.X=original.X+eye*eyeWidth+(eyeWidth-vp.Width)/2;
""", """        vp.X=original.X+eye*eyeWidth+(eyeWidth-vp.Width)/2;
#ifdef MEVR_MAC
        {   static float uiDist=-1.f;
            if(uiDist<0.f){const char* e=getenv("MEVR_UI_DISTANCE");uiDist=e?(float)atof(e):2.0f;}
            if(uiDist>0.05f&&g_worldScale>1.f&&g_halfIpdUU>0.f&&g_targetHalfFovX>0.1f){
                const float halfIpdM=g_halfIpdUU/g_worldScale;
                const float focal=(eyeWidth*.5f)/tanf(g_targetHalfFovX);
                const int shift=(int)lroundf(focal*halfIpdM/uiDist);
                const int x=(int)vp.X+(eye==0?shift:-shift);
                const int lo=(int)(original.X+eye*eyeWidth),hi=(int)(original.X+(eye+1)*eyeWidth-vp.Width);
                vp.X=(DWORD)(std::max)(lo,(std::min)(hi,x));
            }
        }
#endif
""", "ui convergence")
save("stereo_effects.inl", t)
t = load("combat.inl")
t = sub(t, """    const MH_STATUS create = MH_CreateHook((void*)script, (void*)&CombatProcessScript,
        (void**)&g_combatOriginalScript);""", """#ifdef MEVR_MAC
    const MH_STATUS create = MevrHookGuest(script, (void*)&CombatProcessScript,
        (void**)&g_combatOriginalScript, MEVR_HOOK_THIS_PP);
#else
    const MH_STATUS create = MH_CreateHook((void*)script, (void*)&CombatProcessScript,
        (void**)&g_combatOriginalScript);
#endif""", "hook combat script")
save("combat.inl", t)

print("vr_menu.inl patched")
