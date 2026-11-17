// macOS glue for the Mirror's Edge VR mod (letsgosportsteam/mirrors-edge-vr-mod, MIT) running
// inside the RunnerRecompiled arm64 port.
//
// The mod's own source (vr-mod/src, branch `macos`, every change guarded by MEVR_MAC) is compiled
// unchanged in logic. This file supplies what it used to get from Windows:
//   - OpenXR on DXVK's Vulkan device (XR_KHR_vulkan_enable2): the instance and HMD system are
//     created at load, and DXVK creates its VkInstance/VkDevice through them (patched DXVK calls
//     mevr_vk_create_instance / mevr_vk_create_device);
//   - the D3D11 copy path, replaced by Vulkan copies from the back buffer through DXVK interop;
//   - vtable patching, replaced by the runtime's per-interface slot table;
//   - game memory (the runtime's guest address space), files, threads, MinHook (refused).
//
// Everything falls back: with no runtime or headset, the game runs flat.

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <windows.h>     // compat/: DXVK native windows_base.h + mevr_win32.h
#include <d3d9.h>
#include "mevr_gpu_mac.h"
#include "MinHook.h"

#undef xrAcquireSwapchainImage
#undef xrReleaseSwapchainImage
#undef xrBeginFrame
#undef xrEndFrame

#include <dlfcn.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <condition_variable>
#include <fcntl.h>
#include <sys/stat.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../../RunnerRecompiled/pc/runtime/rt_vr.h"

extern "C" void MevrModAttach(void* self);
extern "C" void MevrModDirect3D9Created(IDirect3D9* d3d);

// ------------------------------------------------------------------ host connection
static const MevrHostApi* g_host;
static std::string g_selfPath;     // .../libmevr.dylib

static void GlueLog(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g_host) g_host->log(buf); else fprintf(stderr, "mevr: %s\n", buf);
}

extern "C" void* mevr_guest_ptr(uintptr_t a)
{
    if (!g_host || a < 0x10000 || a >= 0x100000000ull) return nullptr;
    return g_host->guest_ok((uint32_t)a) ? g_host->guest_ptr((uint32_t)a) : nullptr;
}
extern "C" uintptr_t mevr_guest_image_base() { return 0x400000; }
extern "C" void mevr_debug_out(const char* s) { GlueLog("[dbg] %s", s); }

void MevrGuestCallRefused(const char* where)
{
    static std::mutex m; static std::map<std::string, int> seen;
    std::lock_guard<std::mutex> l(m);
    if (seen[where]++ == 0) GlueLog("guest call from %s refused (not available in the recompiled game yet)", where);
}

// ------------------------------------------------------------------ Win32: errors, threads, files, env
static thread_local DWORD t_lastError;
DWORD GetLastError() { return t_lastError; }
void SetLastError(DWORD e) { t_lastError = e; }

struct MacThread { pthread_t t; LPTHREAD_START_ROUTINE fn; LPVOID arg; std::atomic<bool> done{false}; };
static void* ThreadMain(void* p)
{
    auto* th = static_cast<MacThread*>(p);
    th->fn(th->arg);
    th->done = true;
    return nullptr;
}
struct MacFile { int fd; };
enum { KIND_THREAD = 0x54485244, KIND_FILE = 0x46494C45 };
struct MacHandle { uint32_t kind; MacThread* thread; MacFile* file; };

HANDLE CreateThread(void*, size_t, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD, DWORD* id)
{
    auto* th = new MacThread; th->fn = fn; th->arg = arg;
    if (pthread_create(&th->t, nullptr, ThreadMain, th) != 0) { delete th; return nullptr; }
    if (id) { uint64_t t; pthread_threadid_np(th->t, &t); *id = (DWORD)t; }
    return new MacHandle{ KIND_THREAD, th, nullptr };
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    auto* mh = static_cast<MacHandle*>(h);
    if (!mh || mh->kind != KIND_THREAD) return WAIT_OBJECT_0;
    if (ms == INFINITE) { pthread_join(mh->thread->t, nullptr); return WAIT_OBJECT_0; }
    for (DWORD waited = 0; !mh->thread->done; waited += 1) { if (waited >= ms) return WAIT_TIMEOUT; Sleep(1); }
    return WAIT_OBJECT_0;
}
BOOL CloseHandle(HANDLE h)
{
    auto* mh = static_cast<MacHandle*>(h);
    if (!mh || h == INVALID_HANDLE_VALUE) return FALSE;
    if (mh->kind == KIND_FILE) { close(mh->file->fd); delete mh->file; }
    else if (mh->kind == KIND_THREAD) pthread_detach(mh->thread->t);   // the thread object leaks by design
    mh->kind = 0;
    delete mh;
    return TRUE;
}

// Windows paths from the mod: backslashes become slashes (the folders it builds are its own).
static std::string MacPath(const wchar_t* w)
{
    std::string s = MevrUtf8(w);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}
static std::string MacPath(const char* a)
{
    std::string s(a ? a : "");
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

static HANDLE OpenPath(const std::string& path, DWORD access, DWORD disp, bool append)
{
    int flags = (access & GENERIC_WRITE) || append ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default: break;
    }
    if (append) flags |= O_APPEND;
    int fd = open(path.c_str(), flags, 0644);
    if (fd < 0) { SetLastError(errno == ENOENT ? ERROR_FILE_NOT_FOUND : 5); return INVALID_HANDLE_VALUE; }
    return new MacHandle{ KIND_FILE, nullptr, new MacFile{ fd } };
}
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD, void*, DWORD disp, DWORD, HANDLE)
{ return OpenPath(MacPath(path), access, disp, (access & FILE_APPEND_DATA) != 0); }
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD, void*, DWORD disp, DWORD, HANDLE)
{ return OpenPath(MacPath(path), access, disp, (access & FILE_APPEND_DATA) != 0); }
static int Fd(HANDLE h) { auto* mh = static_cast<MacHandle*>(h); return mh && h != INVALID_HANDLE_VALUE && mh->kind == KIND_FILE ? mh->file->fd : -1; }
BOOL ReadFile(HANDLE h, LPVOID out, DWORD n, DWORD* got, void*)
{ ssize_t r = read(Fd(h), out, n); if (got) *got = r > 0 ? (DWORD)r : 0; return r >= 0; }
BOOL WriteFile(HANDLE h, LPCVOID in, DWORD n, DWORD* put, void*)
{ ssize_t r = write(Fd(h), in, n); if (put) *put = r > 0 ? (DWORD)r : 0; return r == (ssize_t)n; }
BOOL FlushFileBuffers(HANDLE h) { return fsync(Fd(h)) == 0; }
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) { struct stat st; if (fstat(Fd(h), &st)) return FALSE; size->QuadPart = st.st_size; return TRUE; }
DWORD SetFilePointer(HANDLE h, LONG lo, LONG* hi, DWORD m)
{
    off_t off = hi ? (off_t)(((int64_t)*hi << 32) | (uint32_t)lo) : lo;
    off_t r = lseek(Fd(h), off, m == FILE_BEGIN ? SEEK_SET : m == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) return INVALID_SET_FILE_POINTER;
    if (hi) *hi = (LONG)(r >> 32);
    return (DWORD)r;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER d, LARGE_INTEGER* pos, DWORD m)
{
    off_t r = lseek(Fd(h), d.QuadPart, m == FILE_BEGIN ? SEEK_SET : m == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) return FALSE;
    if (pos) pos->QuadPart = r;
    return TRUE;
}
BOOL SetEndOfFile(HANDLE h) { off_t p = lseek(Fd(h), 0, SEEK_CUR); return p >= 0 && ftruncate(Fd(h), p) == 0; }
DWORD GetFileAttributesW(LPCWSTR path)
{
    struct stat st;
    if (stat(MacPath(path).c_str(), &st)) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL DeleteFileW(LPCWSTR path) { return unlink(MacPath(path).c_str()) == 0; }
BOOL MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD) { return rename(MacPath(a).c_str(), MacPath(b).c_str()) == 0; }
BOOL MoveFileW(LPCWSTR a, LPCWSTR b) { return rename(MacPath(a).c_str(), MacPath(b).c_str()) == 0; }
BOOL CreateDirectoryW(LPCWSTR path, void*)
{ if (mkdir(MacPath(path).c_str(), 0755) == 0) return TRUE; SetLastError(errno == EEXIST ? ERROR_ALREADY_EXISTS : 5); return FALSE; }

static DWORD PutWide(const std::string& s, wchar_t* out, DWORD n)
{
    std::vector<wchar_t> w(s.size() + 1);
    int k = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), (int)w.size());
    DWORD len = k > 0 ? (DWORD)k - 1 : 0;
    if (!out || n <= len) return len + 1;
    wmemcpy(out, w.data(), len + 1);
    return len;
}
// The mod's paths come from LOCALAPPDATA / USERPROFILE / Documents. MEVR_ENV_<NAME> overrides
// any of them (the launch script points them at the game's user folder).
DWORD GetEnvironmentVariableW(LPCWSTR name, wchar_t* out, DWORD n)
{
    std::string k = MevrUtf8(name);
    const char* v = getenv(("MEVR_ENV_" + k).c_str());
    if (!v) v = getenv(k.c_str());
    if (!v && (k == "LOCALAPPDATA" || k == "USERPROFILE")) v = getenv("HOME");
    if (!v) { SetLastError(203); return 0; }
    std::string w(v);
    for (char& c : w) if (c == '/') c = '\\';
    return PutWide(w, out, n);
}
DWORD ExpandEnvironmentStringsW(LPCWSTR src, wchar_t* out, DWORD n)
{
    std::string s = MevrUtf8(src), r;
    for (size_t i = 0; i < s.size(); ++i) {
        size_t j;
        if (s[i] == '%' && (j = s.find('%', i + 1)) != std::string::npos) {
            std::wstring wn; std::string var = s.substr(i + 1, j - i - 1);
            for (char c : var) wn += (wchar_t)c;
            wchar_t buf[1024];
            if (GetEnvironmentVariableW(wn.c_str(), buf, 1024)) { r += MevrUtf8(buf); i = j; continue; }
        }
        r += s[i];
    }
    DWORD len = PutWide(r, out, n);
    return out && n > len ? len + 1 : len;
}
DWORD GetModuleFileNameW(HMODULE m, wchar_t* out, DWORD n)
{
    // The mod's own module -> libmevr.dylib (mevr.ini and the log live beside it); the process
    // -> the game's MirrorsEdge.exe, as the runtime was given it (MEVR_GAME_EXE).
    // The mod finds mevr.ini by replacing its own file name: report the name it was built as.
    std::string p = g_selfPath.substr(0, g_selfPath.rfind('/') + 1) + "d3d9.dll";
    if (!m || m == (HMODULE)mevr_guest_image_base()) {
        const char* exe = getenv("MEVR_GAME_EXE");
        if (exe) p = exe;
    }
    // Windows separators: the mod splits paths at '\\'; file calls turn them back (MacPath).
    for (char& c : p) if (c == '/') c = '\\';
    DWORD len = PutWide(p, out, n);
    return len >= n ? n : len;
}
DWORD GetPrivateProfileStringW(LPCWSTR sec, LPCWSTR key, LPCWSTR def, wchar_t* out, DWORD n, LPCWSTR file)
{
    std::string want = MevrUtf8(sec), k = MevrUtf8(key), value = MevrUtf8(def), cur;
    if (FILE* f = fopen(MacPath(file).c_str(), "r")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (!l.empty() && l[0] == '[') { cur = l.substr(1, l.find(']') - 1); continue; }
            size_t eq = l.find('=');
            if (eq == std::string::npos || strcasecmp(cur.c_str(), want.c_str())) continue;
            std::string lk = l.substr(0, eq);
            while (!lk.empty() && lk.back() == ' ') lk.pop_back();
            if (!strcasecmp(lk.c_str(), k.c_str())) { value = l.substr(eq + 1); value.erase(0, value.find_first_not_of(' ')); }
        }
        fclose(f);
    }
    return PutWide(value, out, n);
}

// ------------------------------------------------------------------ game-function hooks
// MinHook's model on top of the runtime's entry hooks: MevrHookGuest registers a detour for one
// of lift.py's ENTRY_HOOKS addresses and hands back a host "original" that runs the lifted
// function for the call in progress. MH_Enable/Disable/RemoveHook act on those registrations.
struct GuestHook { uintptr_t addr; void* detour; int kind; bool enabled; };
static GuestHook g_guestHooks[8];
static int g_guestHookCount;
static std::mutex g_guestHookLock;

struct HookFrame { void* ctx; uintptr_t addr; bool originalRan; };
static thread_local HookFrame* t_hook;     // innermost hook in progress on this thread (nests)

typedef void (*PFN_ThisPP)(void*, void*, void*, void*);
typedef void (*PFN_ThisFI)(void*, void*, float, int);

static void RunOriginal()
{
    HookFrame* f = t_hook;
    if (!f || f->originalRan) return;
    f->originalRan = true;
    g_host->call_original(f->ctx, (uint32_t)f->addr);
}
// The "original" the mod calls: the arguments are the ones already on the guest stack (the mod
// passes them through unchanged), so only the call itself is replayed.
static void OrigThisPP(void*, void*, void*, void*) { RunOriginal(); }
static void OrigThisFI(void*, void*, float, int) { RunOriginal(); }

static uint32_t Ld32(uint32_t a) { uint32_t v = 0; if (void* p = mevr_guest_ptr(a)) memcpy(&v, p, 4); return v; }

struct X86CtxHead { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; };   // X86Ctx's first fields

static int EntryHook(void* ctx, uint32_t addr)
{
    GuestHook h{};
    {
        std::lock_guard<std::mutex> l(g_guestHookLock);
        for (int i = 0; i < g_guestHookCount; ++i)
            if (g_guestHooks[i].addr == addr && g_guestHooks[i].enabled) { h = g_guestHooks[i]; break; }
    }
    if (!h.detour) return 0;
    auto* regs = static_cast<X86CtxHead*>(ctx);
    const uint32_t self = regs->ecx, esp = regs->esp;
    const uint32_t a = Ld32(esp + 4), b = Ld32(esp + 8);
    HookFrame frame{ ctx, addr, false };
    HookFrame* outer = t_hook;
    t_hook = &frame;
    if (h.kind == MEVR_HOOK_THIS_PP)
        reinterpret_cast<PFN_ThisPP>(h.detour)((void*)(uintptr_t)self, nullptr, (void*)(uintptr_t)a, (void*)(uintptr_t)b);
    else {
        float f; memcpy(&f, &a, 4);
        reinterpret_cast<PFN_ThisFI>(h.detour)((void*)(uintptr_t)self, nullptr, f, (int)b);
    }
    t_hook = outer;
    // A detour that skipped the original still has to return like the function: ret 8.
    if (!frame.originalRan) regs->esp = esp + 4 + 8;
    return 1;
}

MH_STATUS MevrHookGuest(uintptr_t target, void* detour, void** original, int kind)
{
    if (original) *original = nullptr;
    if (!g_host || !g_host->enable_entry_hooks) return MH_ERROR_UNSUPPORTED_FUNCTION;
    std::lock_guard<std::mutex> l(g_guestHookLock);
    for (int i = 0; i < g_guestHookCount; ++i)
        if (g_guestHooks[i].addr == target) return MH_ERROR_ALREADY_CREATED;
    if (g_guestHookCount == 8) return MH_ERROR_MEMORY_ALLOC;
    g_guestHooks[g_guestHookCount++] = { target, detour, kind, false };
    if (original) *original = kind == MEVR_HOOK_THIS_PP ? reinterpret_cast<void*>(&OrigThisPP)
                                                        : reinterpret_cast<void*>(&OrigThisFI);
    GlueLog("game function %#lx hooked at entry (lift.py ENTRY_HOOKS must list it)", (unsigned long)target);
    return MH_OK;
}

static MH_STATUS SetEnabled(void* target, bool on, bool remove)
{
    std::lock_guard<std::mutex> l(g_guestHookLock);
    for (int i = 0; i < g_guestHookCount; ++i)
        if (g_guestHooks[i].addr == (uintptr_t)target) {
            if (remove) { g_guestHooks[i] = g_guestHooks[--g_guestHookCount]; }
            else g_guestHooks[i].enabled = on;
            bool any = false;
            for (int k = 0; k < g_guestHookCount; ++k) any |= g_guestHooks[k].enabled;
            g_host->enable_entry_hooks(any ? 1 : 0);
            return MH_OK;
        }
    return MH_ERROR_NOT_CREATED;
}

MH_STATUS MH_Initialize(void) { return MH_OK; }
MH_STATUS MH_Uninitialize(void) { return MH_OK; }
MH_STATUS MH_CreateHook(void* target, void*, void** original)
{
    // Only kernel32/XInput/loader diagnostics still come here: nothing on macOS to detour.
    if (original) *original = nullptr;
    GlueLog("MinHook target %p refused (not a game function this port hooks)", target);
    return MH_ERROR_UNSUPPORTED_FUNCTION;
}
MH_STATUS MH_RemoveHook(void* t) { return SetEnabled(t, false, true); }
MH_STATUS MH_EnableHook(void* t) { return SetEnabled(t, true, false); }
MH_STATUS MH_DisableHook(void* t) { return SetEnabled(t, false, false); }
const char* MH_StatusToString(MH_STATUS s) { return s == MH_OK ? "MH_OK" : "MH_ERROR"; }

// ------------------------------------------------------------------ calls into game code
// Parameters are copied into guest memory, the call runs on this (guest) thread, and only the
// bytes the game changed are copied back: a caller may pass a struct smaller than `size`, and
// bytes beyond it are never written by the callee, so they are never written back either.
static uint32_t GuestCopyIn(const void* host, size_t size, std::vector<uint8_t>& snapshot)
{
    uint32_t g = g_host->guest_alloc((uint32_t)size);
    if (!g) return 0;
    snapshot.assign(static_cast<const uint8_t*>(host), static_cast<const uint8_t*>(host) + size);
    memcpy(mevr_guest_ptr(g), host, size);
    return g;
}
static void GuestCopyBack(uint32_t g, void* host, const std::vector<uint8_t>& snapshot)
{
    const uint8_t* src = static_cast<const uint8_t*>(mevr_guest_ptr(g));
    uint8_t* dst = static_cast<uint8_t*>(host);
    for (size_t i = 0; i < snapshot.size(); ++i) if (src[i] != snapshot[i]) dst[i] = src[i];
    g_host->guest_free(g);
}

bool MevrGuestProcessEvent(uintptr_t pe, uintptr_t object, uintptr_t function, void* parms, size_t size)
{
    if (!g_host || !g_host->thread_ctx() || !pe || !object) return false;
    std::vector<uint8_t> snap;
    uint32_t g = parms && size ? GuestCopyIn(parms, size, snap) : 0;
    if (parms && size && !g) return false;
    const uint32_t args[3] = { (uint32_t)function, g, 0 };
    g_host->call_guest(nullptr, (uint32_t)pe, (uint32_t)object, 3, args);
    if (g) GuestCopyBack(g, parms, snap);
    return true;
}

bool MevrGuestNativeExec(uintptr_t exec, uintptr_t object, uintptr_t function, const unsigned char* code,
                         unsigned codeLen, void* result, unsigned resultLen)
{
    if (!g_host || !g_host->thread_ctx() || !exec || !object) return false;
    // FFrame (32-bit): vtable, Node, Object, Code, Locals, PreviousFrame, OutParms = 28 bytes.
    const uint32_t total = 28 + codeLen + resultLen;
    uint32_t g = g_host->guest_alloc(total);
    if (!g) return false;
    uint8_t* base = static_cast<uint8_t*>(mevr_guest_ptr(g));
    const uint32_t frame[7] = { 0, (uint32_t)function, (uint32_t)object, g + 28, 0, 0, 0 };
    memcpy(base, frame, 28);
    memcpy(base + 28, code, codeLen);
    memcpy(base + 28 + codeLen, result, resultLen);
    const uint32_t args[2] = { g, g + 28 + codeLen };
    g_host->call_guest(nullptr, (uint32_t)exec, (uint32_t)object, 2, args);
    memcpy(result, base + 28 + codeLen, resultLen);
    g_host->guest_free(g);
    return true;
}

// ------------------------------------------------------------------ OpenXR instance (at load)
static XrInstance g_xr = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;
static bool g_xrReady;
static bool g_vkFromXr;
static PFN_vkGetInstanceProcAddr g_gipa;
static VkInstance g_vkInstance;

XrInstance MevrXrInstance() { return g_vkFromXr ? g_xr : XR_NULL_HANDLE; }
XrSystemId MevrXrSystem() { return g_vkFromXr ? g_system : XR_NULL_SYSTEM_ID; }

static bool XrOk(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (g_xr) xrResultToString(g_xr, r, name);
    GlueLog("%s failed: %d %s", what, (int)r, name);
    return false;
}
template <class T> static T XrFn(const char* name)
{
    PFN_xrVoidFunction f = nullptr;
    xrGetInstanceProcAddr(g_xr, name, &f);
    return reinterpret_cast<T>(f);
}

static void XrInitEarly()
{
    if (getenv("MEVR_FLAT") && *getenv("MEVR_FLAT") == '1') { GlueLog("MEVR_FLAT=1: VR off"); return; }
    const char* exts[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ci.applicationInfo.applicationName, "MirrorsEdgeVR");
    strcpy(ci.applicationInfo.engineName, "RunnerRecompiled");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    if (!XrOk(xrCreateInstance(&ci, &g_xr), "xrCreateInstance (is the WiVRn server running with the headset connected?)")) {
        g_xr = XR_NULL_HANDLE; return;
    }
    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(g_xr, &ip);
    GlueLog("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
            XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    XrSystemGetInfo si{ XR_TYPE_SYSTEM_GET_INFO };
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrOk(xrGetSystem(g_xr, &si, &g_system), "xrGetSystem")) return;
    XrGraphicsRequirementsVulkan2KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    auto getReq = XrFn<PFN_xrGetVulkanGraphicsRequirements2KHR>("xrGetVulkanGraphicsRequirements2KHR");
    if (!getReq || !XrOk(getReq(g_xr, g_system, &req), "xrGetVulkanGraphicsRequirements2KHR")) return;
    g_xrReady = true;
}

extern "C" __attribute__((visibility("default")))
VkResult mevr_vk_create_instance(PFN_vkGetInstanceProcAddr gipa, const VkInstanceCreateInfo* info, VkInstance* out)
{
    g_gipa = gipa;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (g_xrReady) {
        auto create = XrFn<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR");
        XrVulkanInstanceCreateInfoKHR xi{ XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
        xi.systemId = g_system; xi.pfnGetInstanceProcAddr = gipa; xi.vulkanCreateInfo = info;
        if (create && XrOk(create(g_xr, &xi, out, &vr), "xrCreateVulkanInstanceKHR") && vr == VK_SUCCESS) {
            GlueLog("Vulkan instance created through OpenXR");
            g_vkInstance = *out;
            return vr;
        }
        GlueLog("instance through OpenXR failed (VkResult %d): VR off", (int)vr);
        g_xrReady = false;
    }
    auto plain = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    vr = plain(info, nullptr, out);
    if (vr == VK_SUCCESS) g_vkInstance = *out;
    return vr;
}

extern "C" __attribute__((visibility("default")))
VkResult mevr_vk_create_device(PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice phys, const VkDeviceCreateInfo* info, VkDevice* out)
{
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if (g_xrReady && g_vkInstance) {
        VkPhysicalDevice want = VK_NULL_HANDLE;
        XrVulkanGraphicsDeviceGetInfoKHR gi{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
        gi.systemId = g_system; gi.vulkanInstance = g_vkInstance;
        auto getDev = XrFn<PFN_xrGetVulkanGraphicsDevice2KHR>("xrGetVulkanGraphicsDevice2KHR");
        if (getDev && XrOk(getDev(g_xr, &gi, &want), "xrGetVulkanGraphicsDevice2KHR") && want != phys)
            GlueLog("warning: OpenXR wants physical device %p, DXVK picked %p", (void*)want, (void*)phys);
        auto create = XrFn<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR");
        XrVulkanDeviceCreateInfoKHR xi{ XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
        xi.systemId = g_system; xi.pfnGetInstanceProcAddr = gipa; xi.vulkanPhysicalDevice = phys; xi.vulkanCreateInfo = info;
        if (create && XrOk(create(g_xr, &xi, out, &vr), "xrCreateVulkanDeviceKHR") && vr == VK_SUCCESS) {
            GlueLog("Vulkan device created through OpenXR");
            g_vkFromXr = true;
            return vr;
        }
        GlueLog("device through OpenXR failed (VkResult %d): VR off", (int)vr);
        g_xrReady = false;
    }
    auto plain = reinterpret_cast<PFN_vkCreateDevice>(gipa(g_vkInstance, "vkCreateDevice"));
    return plain(phys, info, nullptr, out);
}

// ------------------------------------------------------------------ DXVK interop (by ABI)
struct MGuid { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; };
struct MUnknown {
    virtual int32_t QueryInterface(const MGuid& riid, void** out) = 0;
    virtual uint32_t AddRef() = 0;
    virtual uint32_t Release() = 0;
};
struct MInteropTexture : MUnknown {
    virtual int32_t GetVulkanImageInfo(VkImage* image, VkImageLayout* layout, VkImageCreateInfo* info) = 0;
};
struct MInteropDevice : MUnknown {
    virtual void GetVulkanHandles(VkInstance*, VkPhysicalDevice*, VkDevice*) = 0;
    virtual void GetSubmissionQueue(VkQueue*, uint32_t* queueIndex, uint32_t* family) = 0;
    virtual void TransitionTextureLayout(MInteropTexture*, const VkImageSubresourceRange*, VkImageLayout, VkImageLayout) = 0;
    virtual void FlushRenderingCommands() = 0;
    virtual void LockSubmissionQueue() = 0;
    virtual void ReleaseSubmissionQueue() = 0;
};
static const MGuid IID_InteropDevice  = {0x2eaa4b89, 0x0107, 0x4bdb, {0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0}};
static const MGuid IID_InteropTexture = {0xd56344f5, 0x8d35, 0x46fd, {0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1}};
static const MGuid IID_D3D9   = {0x81BDCBCA, 0x64D4, 0x426D, {0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C}};
static const MGuid IID_Device = {0xD0223B96, 0xBF7A, 0x43FD, {0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB}};
static const MGuid IID_Query  = {0xD9771460, 0xA695, 0x4F26, {0xBB, 0xD3, 0x27, 0xB8, 0x40, 0xB5, 0x41, 0xCC}};

static MInteropDevice* g_interop;
static void* g_d3dDevice;
static VkDevice g_vkDevice;
static VkQueue g_queue;
static uint32_t g_family, g_queueIndex;
static VkPhysicalDevice g_vkPhys;

#define VKFN(name) static PFN_##name p_##name
VKFN(vkGetDeviceProcAddr); VKFN(vkCreateCommandPool); VKFN(vkAllocateCommandBuffers);
VKFN(vkCreateFence); VKFN(vkWaitForFences); VKFN(vkResetFences); VKFN(vkResetCommandBuffer);
VKFN(vkBeginCommandBuffer); VKFN(vkEndCommandBuffer); VKFN(vkCmdPipelineBarrier);
VKFN(vkCmdCopyImage); VKFN(vkCmdBlitImage); VKFN(vkCmdClearColorImage); VKFN(vkQueueSubmit);
VKFN(vkCreateBuffer); VKFN(vkGetBufferMemoryRequirements); VKFN(vkAllocateMemory); VKFN(vkBindBufferMemory);
VKFN(vkMapMemory); VKFN(vkCmdCopyBufferToImage); VKFN(vkGetPhysicalDeviceMemoryProperties);

static VkCommandPool g_pool;
static const int kRing = 16;
static VkCommandBuffer g_cmd[kRing];
static VkFence g_fence[kRing];
static uint32_t g_ringNext;
static VkBuffer g_staging; static VkDeviceMemory g_stagingMem; static void* g_stagingPtr; static VkDeviceSize g_stagingSize;

static bool GpuInit()
{
    if (g_pool) return true;
    if (!g_d3dDevice) return false;
    if (!g_interop && static_cast<MUnknown*>(g_d3dDevice)->QueryInterface(IID_InteropDevice, reinterpret_cast<void**>(&g_interop)) < 0) {
        GlueLog("DXVK has no ID3D9VkInteropDevice"); return false;
    }
    VkInstance inst;
    g_interop->GetVulkanHandles(&inst, &g_vkPhys, &g_vkDevice);
    g_interop->GetSubmissionQueue(&g_queue, &g_queueIndex, &g_family);
    GlueLog("DXVK Vulkan: device %p queue family %u index %u", (void*)g_vkDevice, g_family, g_queueIndex);
    auto gdpa = (PFN_vkGetDeviceProcAddr)g_gipa(g_vkInstance, "vkGetDeviceProcAddr");
    p_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(g_vkInstance, "vkGetPhysicalDeviceMemoryProperties");
    if (!gdpa) return false;
#define L(name) if (!(p_##name = (PFN_##name)gdpa(g_vkDevice, #name))) { GlueLog("no %s", #name); return false; }
    L(vkCreateCommandPool) L(vkAllocateCommandBuffers) L(vkCreateFence) L(vkWaitForFences) L(vkResetFences)
    L(vkResetCommandBuffer) L(vkBeginCommandBuffer) L(vkEndCommandBuffer) L(vkCmdPipelineBarrier) L(vkCmdCopyImage)
    L(vkCmdBlitImage) L(vkCmdClearColorImage) L(vkQueueSubmit) L(vkCreateBuffer) L(vkGetBufferMemoryRequirements)
    L(vkAllocateMemory) L(vkBindBufferMemory) L(vkMapMemory) L(vkCmdCopyBufferToImage)
#undef L
    VkCommandPoolCreateInfo pi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = g_family;
    if (p_vkCreateCommandPool(g_vkDevice, &pi, nullptr, &g_pool)) return false;
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = g_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = kRing;
    if (p_vkAllocateCommandBuffers(g_vkDevice, &ai, g_cmd)) return false;
    for (auto& f : g_fence) {
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (p_vkCreateFence(g_vkDevice, &fi, nullptr, &f)) return false;
    }
    return true;
}

bool MevrGpuSessionBinding(XrGraphicsBindingVulkan2KHR* bind)
{
    if (!GpuInit()) return false;
    bind->instance = g_vkInstance; bind->physicalDevice = g_vkPhys; bind->device = g_vkDevice;
    bind->queueFamilyIndex = g_family; bind->queueIndex = g_queueIndex;
    return true;
}

void MevrGpuSwapchainImages(XrSwapchain sc, std::vector<MevrImage>& out)
{
    out.clear();
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(sc, 0, &n, nullptr)) || !n) return;
    std::vector<XrSwapchainImageVulkan2KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
    if (XR_FAILED(xrEnumerateSwapchainImages(sc, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())))) return;
    for (auto& i : imgs) out.push_back(i.image);
}

static void Barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags s, VkAccessFlags d)
{
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = s; b.dstAccessMask = d; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
static VkCommandBuffer CmdBegin(uint32_t* slot)
{
    const uint32_t k = g_ringNext++ % kRing;
    p_vkWaitForFences(g_vkDevice, 1, &g_fence[k], VK_TRUE, UINT64_MAX);
    p_vkResetFences(g_vkDevice, 1, &g_fence[k]);
    p_vkResetCommandBuffer(g_cmd[k], 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(g_cmd[k], &bi);
    *slot = k;
    return g_cmd[k];
}
static void CmdSubmit(uint32_t k)
{
    p_vkEndCommandBuffer(g_cmd[k]);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd[k];
    g_interop->LockSubmissionQueue();          // also waits until DXVK's own work is queued
    p_vkQueueSubmit(g_queue, 1, &si, g_fence[k]);
    g_interop->ReleaseSubmissionQueue();
}

// ---- the captured back buffer ----
static MInteropTexture* g_capTex;
static MUnknown* g_capSurf;
static VkImage g_capImage;
static VkImageLayout g_capLayout;
static VkFormat g_capFormat;
static const VkImageSubresourceRange kColor0{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

bool MevrGpuBeginCapture(IDirect3DDevice9* dev, unsigned* w, unsigned* h)
{
    MevrGpuEndCapture();
    if (!GpuInit()) return false;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    MInteropTexture* tex = nullptr;
    if (reinterpret_cast<MUnknown*>(bb)->QueryInterface(IID_InteropTexture, reinterpret_cast<void**>(&tex)) < 0 || !tex) {
        bb->Release(); return false;
    }
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    tex->GetVulkanImageInfo(&g_capImage, &g_capLayout, &info);
    g_capFormat = info.format;
    g_interop->TransitionTextureLayout(tex, &kColor0, g_capLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    g_interop->FlushRenderingCommands();
    g_capTex = tex; g_capSurf = reinterpret_cast<MUnknown*>(bb);
    *w = info.extent.width; *h = info.extent.height;
    return true;
}
bool MevrGpuHaveCapture() { return g_capTex != nullptr; }
void MevrGpuEndCapture()
{
    if (!g_capTex) return;
    g_interop->TransitionTextureLayout(g_capTex, &kColor0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_capLayout);
    g_capTex->Release(); g_capSurf->Release();
    g_capTex = nullptr; g_capSurf = nullptr; g_capImage = VK_NULL_HANDLE;
}

void MevrGpuCopyFromCapture(MevrImage dst, unsigned x, unsigned y, unsigned w, unsigned h)
{
    if (!g_capTex || !dst) return;
    uint32_t k; VkCommandBuffer cb = CmdBegin(&k);
    Barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    Barrier(cb, g_capImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    // The game's pixels are sRGB-encoded UNORM; the swapchain is the _SRGB twin of the same
    // channel order, so a raw copy keeps them right (a blit would re-encode them).
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.srcOffset = { (int32_t)x, (int32_t)y, 0 };
    c.extent = { w, h, 1 };
    p_vkCmdCopyImage(cb, g_capImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    CmdSubmit(k);
}

void MevrGpuClear(MevrImage dst, float r, float g, float b, float a)
{
    if (!GpuInit() || !dst) return;
    uint32_t k; VkCommandBuffer cb = CmdBegin(&k);
    Barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue v{}; v.float32[0] = r; v.float32[1] = g; v.float32[2] = b; v.float32[3] = a;
    p_vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &kColor0);
    Barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    CmdSubmit(k);
}

void MevrGpuUpload(MevrImage dst, const uint32_t* pixels, unsigned w, unsigned h)
{
    if (!GpuInit() || !dst) return;
    const VkDeviceSize size = (VkDeviceSize)w * h * 4;
    if (g_stagingSize < size) {
        // One host-visible buffer, kept: the panel is the only upload (1200x1000).
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (p_vkCreateBuffer(g_vkDevice, &bi, nullptr, &g_staging)) return;
        VkMemoryRequirements mr; p_vkGetBufferMemoryRequirements(g_vkDevice, g_staging, &mr);
        VkPhysicalDeviceMemoryProperties mp; p_vkGetPhysicalDeviceMemoryProperties(g_vkPhys, &mp);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags &
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size; ai.memoryTypeIndex = type;
        if (type == UINT32_MAX || p_vkAllocateMemory(g_vkDevice, &ai, nullptr, &g_stagingMem)) return;
        p_vkBindBufferMemory(g_vkDevice, g_staging, g_stagingMem, 0);
        p_vkMapMemory(g_vkDevice, g_stagingMem, 0, VK_WHOLE_SIZE, 0, &g_stagingPtr);
        g_stagingSize = size;
    }
    // Every ring slot must be idle before the buffer is rewritten (the last upload may still run).
    p_vkWaitForFences(g_vkDevice, kRing, g_fence, VK_TRUE, UINT64_MAX);
    memcpy(g_stagingPtr, pixels, (size_t)size);
    uint32_t k; VkCommandBuffer cb = CmdBegin(&k);
    Barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = { w, h, 1 };
    p_vkCmdCopyBufferToImage(cb, g_staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    CmdSubmit(k);
}

// ---- OpenXR calls that can touch DXVK's queue ----
struct QueueLock {
    QueueLock() { if (g_interop) g_interop->LockSubmissionQueue(); }
    ~QueueLock() { if (g_interop) g_interop->ReleaseSubmissionQueue(); }
};
XrResult MevrXrAcquireSwapchainImage(XrSwapchain s, const XrSwapchainImageAcquireInfo* i, uint32_t* idx)
{ QueueLock l; return xrAcquireSwapchainImage(s, i, idx); }
XrResult MevrXrReleaseSwapchainImage(XrSwapchain s, const XrSwapchainImageReleaseInfo* i)
{ QueueLock l; return xrReleaseSwapchainImage(s, i); }
XrResult MevrXrBeginFrame(XrSession s, const XrFrameBeginInfo* i) { QueueLock l; return xrBeginFrame(s, i); }
// Runtimes that stream only when a projection layer is present (OXRSys) drop a frame that is
// nothing but the mod's head-locked menu quad. Such frames get a black projection layer behind
// the quad: a tiny swapchain cleared once, with this frame's located views.
static XrSwapchain g_blankSwap = XR_NULL_HANDLE;
static uint32_t g_blankW = 2064, g_blankH = 2208;
static std::vector<MevrImage> g_blankImages;
// The runtime snapshots an image when it is released while a client is connected, so the blank
// image is produced again every frame (a 16x16 clear) rather than once.
static void BlankProduce()
{
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (XR_FAILED(MevrXrAcquireSwapchainImage(g_blankSwap, &ai, &idx))) return;
    xrWaitSwapchainImage(g_blankSwap, &wi);
    if (idx < g_blankImages.size()) MevrGpuClear(g_blankImages[idx], 0, 0, 0, 1);
    MevrXrReleaseSwapchainImage(g_blankSwap, &ri);
}
static bool BlankSwapchain(XrSession s)
{
    if (g_blankSwap) { BlankProduce(); return true; }
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    // Full eye size: the runtime composites the menu quad into this image, so its size is the
    // menu's resolution in the headset.
    uint32_t vc = 0; XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_xr, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &vc, vcv)) && vc >= 1)
    { g_blankW = vcv[0].recommendedImageRectWidth; g_blankH = vcv[0].recommendedImageRectHeight; }
    ci.format = VK_FORMAT_B8G8R8A8_SRGB; ci.sampleCount = 1; ci.width = g_blankW; ci.height = g_blankH;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(s, &ci, &g_blankSwap))) { g_blankSwap = XR_NULL_HANDLE; return false; }
    MevrGpuSwapchainImages(g_blankSwap, g_blankImages);
    BlankProduce();
    GlueLog("blank projection layer ready (menus on a projection-only runtime)");
    return true;
}

XrResult MevrXrEndFrame(XrSession s, const XrFrameEndInfo* i)
{
    bool hasProjection = false;
    XrSpace quadSpace = XR_NULL_HANDLE;
    for (uint32_t k = 0; i && k < i->layerCount; ++k) {
        if (i->layers[k]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) hasProjection = true;
        if (i->layers[k]->type == XR_TYPE_COMPOSITION_LAYER_QUAD && !quadSpace)
            quadSpace = reinterpret_cast<const XrCompositionLayerQuad*>(i->layers[k])->space;
    }
    const bool oxrsys = getenv("XR_RUNTIME_JSON") && strstr(getenv("XR_RUNTIME_JSON"), "oxrsys");
    // OXRSys streams each projection view's image rectangle as if it covered the headset's own
    // FOV and ignores the FOV the app submits. The mod renders wider than the lenses (the game's
    // symmetric frustum), so on OXRSys each view is cropped to the headset FOV: for a rectilinear
    // image the crop is linear in tangent space, so the rect is exact. MEVR_FOV_CROP=0 disables.
    const char* cropEnv = getenv("MEVR_FOV_CROP");
    const bool crop = cropEnv ? *cropEnv == '1' : oxrsys;
    if (hasProjection && crop) {
        std::vector<const XrCompositionLayerBaseHeader*> layers(i->layers, i->layers + i->layerCount);
        std::vector<XrCompositionLayerProjection> projs;
        std::vector<std::vector<XrCompositionLayerProjectionView>> views;
        projs.reserve(i->layerCount); views.reserve(i->layerCount);
        for (auto& L : layers) {
            if (L->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(L);
            if (p->viewCount != 2) continue;
            XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = i->displayTime; li.space = p->space;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            XrView hv[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
            uint32_t n = 0;
            if (XR_FAILED(xrLocateViews(s, &li, &vs, 2, &n, hv)) || n != 2) continue;
            views.emplace_back(p->views, p->views + 2);
            for (int e = 0; e < 2; ++e) {
                auto& v = views.back()[e];
                const XrFovf sf = v.fov, hf = hv[e].fov;
                const double tl = tan(sf.angleLeft), tr = tan(sf.angleRight), tu = tan(sf.angleUp), td = tan(sf.angleDown);
                if (!(tr > tl) || !(tu > td)) continue;
                const double W = v.subImage.imageRect.extent.width, H = v.subImage.imageRect.extent.height;
                double x0 = (tan(hf.angleLeft) - tl) / (tr - tl) * W, x1 = (tan(hf.angleRight) - tl) / (tr - tl) * W;
                double y0 = (tu - tan(hf.angleUp)) / (tu - td) * H, y1 = (tu - tan(hf.angleDown)) / (tu - td) * H;
                // The game frustum must cover the lens; where it does not, keep what there is.
                XrFovf used = hf;
                if (x0 < 0) { x0 = 0; used.angleLeft = sf.angleLeft; }
                if (x1 > W) { x1 = W; used.angleRight = sf.angleRight; }
                if (y0 < 0) { y0 = 0; used.angleUp = sf.angleUp; }
                if (y1 > H) { y1 = H; used.angleDown = sf.angleDown; }
                v.subImage.imageRect.offset.x += (int32_t)lround(x0);
                v.subImage.imageRect.offset.y += (int32_t)lround(y0);
                v.subImage.imageRect.extent.width = (int32_t)lround(x1 - x0);
                v.subImage.imageRect.extent.height = (int32_t)lround(y1 - y0);
                v.fov = used;
            }
            static int logged;
            if (logged < 3) {
                ++logged;
                for (int e = 0; e < 2; ++e) {
                    const auto& v = views.back()[e];
                    GlueLog("OXRSys FOV crop: eye %d rect %d,%d %dx%d; headset fov L%.1f R%.1f U%.1f D%.1f, game half %.1fx%.1f",
                            e, v.subImage.imageRect.offset.x, v.subImage.imageRect.offset.y,
                            v.subImage.imageRect.extent.width, v.subImage.imageRect.extent.height,
                            hv[e].fov.angleLeft * 57.2958f, hv[e].fov.angleRight * 57.2958f,
                            hv[e].fov.angleUp * 57.2958f, hv[e].fov.angleDown * 57.2958f,
                            p->views[e].fov.angleRight * 57.2958f, p->views[e].fov.angleUp * 57.2958f);
                }
            }
            projs.push_back(*p);
            projs.back().views = views.back().data();
            L = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projs.back());
        }
        XrFrameEndInfo ei = *i;
        ei.layers = layers.data();
        QueueLock l; return xrEndFrame(s, &ei);
    }
    if (hasProjection || !quadSpace || !oxrsys || !BlankSwapchain(s)) {
        QueueLock l; return xrEndFrame(s, i);
    }
    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = i->displayTime; li.space = quadSpace;
    XrViewState vs{ XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(s, &li, &vs, 2, &n, views)) || n != 2) { QueueLock l; return xrEndFrame(s, i); }
    XrCompositionLayerProjectionView pv[2];
    for (int e = 0; e < 2; ++e) {
        pv[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        pv[e].pose = views[e].pose; pv[e].fov = views[e].fov;
        pv[e].subImage.swapchain = g_blankSwap;
        pv[e].subImage.imageRect = { { 0, 0 }, { (int32_t)g_blankW, (int32_t)g_blankH } };
    }
    XrCompositionLayerProjection proj{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    proj.space = quadSpace; proj.viewCount = 2; proj.views = pv;
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj));
    for (uint32_t k = 0; k < i->layerCount; ++k) layers.push_back(i->layers[k]);
    XrFrameEndInfo ei = *i;
    ei.layerCount = (uint32_t)layers.size(); ei.layers = layers.data();
    QueueLock l; return xrEndFrame(s, &ei);
}

// ------------------------------------------------------------------ slot patching
enum { IF_D3D9 = 0, IF_DEVICE = 1, IF_QUERY = 16 };   // rt_d3d9.h interface enum
enum { DEV_Present = 17 };

typedef HRESULT (*PFN_PresentHost)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static PFN_PresentHost g_realPresent;
// What the mod calls as "the original Present": the capture's layout goes back first, so DXVK
// presents the back buffer in the layout it expects.
static HRESULT MacOrigPresent(IDirect3DDevice9* d, const RECT* s, const RECT* t, HWND w, const RGNDATA* r)
{
    MevrGpuEndCapture();
    return g_realPresent(d, s, t, w, r);
}

void* MevrPatchSlot(void* obj, int index, void* repl)
{
    if (!obj || !g_host || index < 0 || (uint32_t)index >= g_host->slot_max) return nullptr;
    auto* u = static_cast<MUnknown*>(obj);
    void* probe = nullptr;
    int iface = -1;
    if (u->QueryInterface(IID_Device, &probe) >= 0 && probe) { iface = IF_DEVICE; g_d3dDevice = obj; }
    else if (u->QueryInterface(IID_D3D9, &probe) >= 0 && probe) iface = IF_D3D9;
    else if (u->QueryInterface(IID_Query, &probe) >= 0 && probe) iface = IF_QUERY;
    if (probe) static_cast<MUnknown*>(probe)->Release();
    if (iface < 0) { GlueLog("PatchVTable on an unknown interface (%p slot %d) refused", obj, index); return nullptr; }
    void* orig = (*reinterpret_cast<void***>(obj))[index];
    g_host->slot_override[iface * g_host->slot_max + index] = repl;
    if (iface == IF_DEVICE && index == DEV_Present) {
        g_realPresent = reinterpret_cast<PFN_PresentHost>(orig);
        return reinterpret_cast<void*>(&MacOrigPresent);
    }
    return orig;
}

// ------------------------------------------------------------------ XInput
typedef DWORD (*PFN_PadHook)(DWORD, void*);
static PFN_PadHook g_padHook;
DWORD MevrRealXInputGetState(DWORD index, void* state)
{ return g_host ? g_host->pad_real(index, static_cast<uint8_t*>(state)) : ERROR_DEVICE_NOT_CONNECTED; }
void MevrSetPadHook(void* hook) { g_padHook = reinterpret_cast<PFN_PadHook>(hook); }
static uint32_t PadGet(uint32_t index, uint8_t state[16])
{ return g_padHook ? g_padHook(index, state) : MevrRealXInputGetState(index, state); }

// ------------------------------------------------------------------ duplication diagnostics
// The vertex-shader hash is the mod's own (FNV-1a over the bytecode words, EffectShaderHash).
static uint32_t VsHash(IDirect3DVertexShader9* vs)
{
    static std::map<void*, uint32_t> cache;
    auto it = cache.find(vs);
    if (it != cache.end()) return it->second;
    UINT size = 0; std::vector<DWORD> code;
    if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size >= 4 && !(size % 4)) {
        code.resize(size / 4);
        if (FAILED(vs->GetFunction(code.data(), &size))) code.clear();
    }
    uint32_t h = 2166136261u; for (DWORD w : code) { h ^= w; h *= 16777619u; }
    cache[vs] = h;
    return h;
}
bool MevrDupVeto()
{
    static int mode = -1; static std::vector<uint32_t> skip;
    if (mode < 0) {
        mode = getenv("MEVR_NO_DUP") && *getenv("MEVR_NO_DUP") == '1' ? 1 : 0;
        if (const char* l = getenv("MEVR_DUP_SKIP_VS"))
            for (const char* p = l; *p; ) { char* e; uint32_t h = (uint32_t)strtoul(p, &e, 16); if (e == p) break; skip.push_back(h); p = *e ? e + 1 : e; }
        if (mode || !skip.empty()) GlueLog("duplication veto: all=%d, %zu vertex shaders", mode, skip.size());
    }
    if (mode) return true;
    if (skip.empty() || !g_d3dDevice) return false;
    IDirect3DVertexShader9* vs = nullptr;
    if (FAILED(static_cast<IDirect3DDevice9*>(g_d3dDevice)->GetVertexShader(&vs)) || !vs) return false;
    const uint32_t h = VsHash(vs);
    vs->Release();
    for (uint32_t k : skip) if (k == h) return true;
    return false;
}

// MEVR_LOG_VS=1: the vertex shaders of scene draws, by count (logged every 200000 draws).
// MEVR_REVERT_VS=hash,...: those draws use the engine's unforced scene matrix (diagnostic).
extern int g_vmReg;
extern float g_sceneMat[16];
extern bool g_sceneMatValid;
extern bool g_c0IsScene;
typedef HRESULT (*PFN_SetVSConstF_)(IDirect3DDevice9*, UINT, const float*, UINT);
extern PFN_SetVSConstF_ g_origSetVSConstF;
static uint32_t CurrentVsHash(IDirect3DDevice9* dev)
{
    IDirect3DVertexShader9* vs = nullptr;
    if (FAILED(dev->GetVertexShader(&vs)) || !vs) return 0;
    const uint32_t h = VsHash(vs);
    vs->Release();
    return h;
}
bool MevrRevertBegin(IDirect3DDevice9* dev, float saved[16])
{
    static int init = 0; static bool logVs = false; static std::vector<uint32_t> revert;
    static std::map<uint32_t, long> counts; static long n = 0;
    if (!init) {
        init = 1;
        logVs = getenv("MEVR_LOG_VS") && *getenv("MEVR_LOG_VS") == '1';
        if (const char* l = getenv("MEVR_REVERT_VS"))
            for (const char* p = l; *p; ) { char* e; uint32_t h = (uint32_t)strtoul(p, &e, 16); if (e == p) break; revert.push_back(h); p = *e ? e + 1 : e; }
        if (logVs || !revert.empty()) GlueLog("revert diagnostics: log=%d, %zu shaders", logVs ? 1 : 0, revert.size());
    }
    if ((!logVs && revert.empty()) || !g_c0IsScene || !g_sceneMatValid || g_vmReg < 0 || !g_origSetVSConstF) return false;
    const uint32_t h = CurrentVsHash(dev);
    if (logVs) {
        counts[h]++;
        if (++n % 200000 == 0) {
            std::string line;
            for (auto& kv : counts) { char b[32]; snprintf(b, sizeof b, "%08X:%ld ", kv.first, kv.second); line += b; }
            GlueLog("scene VS: %s", line.c_str());
            counts.clear();
        }
    }
    bool hit = false;
    for (uint32_t k : revert) if (k == h) hit = true;
    if (!hit) return false;
    dev->GetVertexShaderConstantF((UINT)g_vmReg, saved, 4);
    g_origSetVSConstF(dev, (UINT)g_vmReg, g_sceneMat, 4);
    return true;
}
void MevrRevertEnd(IDirect3DDevice9* dev, const float saved[16]) { g_origSetVSConstF(dev, (UINT)g_vmReg, saved, 4); }

// ------------------------------------------------------------------ audio to the headset
// The runtime hands over its final stereo mix (rt_vr.h audio_mixed). A TCP listener on
// 127.0.0.1:MEVR_AUDIO_PORT (default 9950; the Quest reaches it through `adb reverse`) sends it to
// one client: a 16-byte header {'MEAU', version 1, rate, channels 2}, then raw s16 interleaved
// PCM. A ring buffer of ~250 ms sits between the audio callback and the socket; on overflow the
// oldest audio is dropped, so a stalled client never blocks the game. While a client is connected
// the Mac's own output is silenced (MEVR_AUDIO_MAC=1 keeps both).
namespace {
struct AudioOut {
    std::mutex m; std::condition_variable cv;
    std::vector<int16_t> ring; size_t head = 0, size = 0;   // in samples (2 per frame)
    uint32_t rate = 0;
    std::atomic<bool> client{false};
    bool started = false;
} g_audio;

void AudioServer()
{
    int port = 9950;
    if (const char* e = getenv("MEVR_AUDIO_PORT")) port = atoi(e);
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ls < 0 || bind(ls, (sockaddr*)&a, sizeof a) || listen(ls, 1)) { GlueLog("audio: cannot listen on 127.0.0.1:%d", port); return; }
    GlueLog("audio: listening on 127.0.0.1:%d for the headset", port);
    for (;;) {
        int c = accept(ls, nullptr, nullptr);
        if (c < 0) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        uint32_t rate;
        { std::lock_guard<std::mutex> l(g_audio.m); rate = g_audio.rate; g_audio.size = 0; }
        const uint32_t hdr[4] = { 0x5541454Du /* 'MEAU' */, 1, rate, 2 };
        bool ok = send(c, hdr, sizeof hdr, 0) == (ssize_t)sizeof hdr;
        if (ok) { g_audio.client = true; GlueLog("audio: headset connected (%u Hz stereo)", rate); }
        std::vector<int16_t> chunk;
        while (ok) {
            {
                std::unique_lock<std::mutex> l(g_audio.m);
                g_audio.cv.wait_for(l, std::chrono::milliseconds(100), [] { return g_audio.size > 0; });
                chunk.resize(g_audio.size);
                const size_t cap = g_audio.ring.size();
                for (size_t k = 0; k < g_audio.size; ++k) chunk[k] = g_audio.ring[(g_audio.head + k) % cap];
                g_audio.head = (g_audio.head + g_audio.size) % (cap ? cap : 1); g_audio.size = 0;
            }
            if (chunk.empty()) continue;
            const char* p = reinterpret_cast<const char*>(chunk.data());
            size_t left = chunk.size() * sizeof(int16_t);
            while (left && ok) { ssize_t n = send(c, p, left, 0); if (n <= 0) ok = false; else { p += n; left -= (size_t)n; } }
        }
        g_audio.client = false;
        close(c);
        GlueLog("audio: headset disconnected");
    }
}

int AudioMixed(const int16_t* lr, uint32_t frames, uint32_t rate)
{
    {
        std::lock_guard<std::mutex> l(g_audio.m);
        if (!g_audio.started) {
            g_audio.started = true;
            g_audio.rate = rate;
            g_audio.ring.assign((size_t)rate / 4 * 2, 0);   // 250 ms
            std::thread(AudioServer).detach();
        }
        if (g_audio.client) {
            const size_t cap = g_audio.ring.size(), n = (size_t)frames * 2;
            for (size_t k = 0; k < n; ++k) {
                if (g_audio.size == cap) { g_audio.head = (g_audio.head + 2) % cap; g_audio.size -= 2; }  // drop oldest frame
                g_audio.ring[(g_audio.head + g_audio.size) % cap] = lr[k];
                ++g_audio.size;
            }
        }
    }
    if (g_audio.client) g_audio.cv.notify_one();
    static const bool keepMac = getenv("MEVR_AUDIO_MAC") && *getenv("MEVR_AUDIO_MAC") == '1';
    return g_audio.client && !keepMac ? 1 : 0;
}
} // namespace

// ------------------------------------------------------------------ module entry
static void OnD3D9Created(void* d3d)
{
    MevrModDirect3D9Created(static_cast<IDirect3D9*>(d3d));
}

static const MevrModule g_module = { MEVR_API_VERSION, nullptr, OnD3D9Created, PadGet, EntryHook, AudioMixed };

extern "C" __attribute__((visibility("default")))
const MevrModule* mevr_attach(const MevrHostApi* api)
{
    if (!api || api->version != MEVR_API_VERSION) return nullptr;
    g_host = api;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&mevr_attach), &info) && info.dli_fname) g_selfPath = info.dli_fname;
    GlueLog("Mirror's Edge VR (macOS port) at %s", g_selfPath.c_str());
    XrInitEarly();
    MevrModAttach(reinterpret_cast<void*>(&g_selfPath));   // the mod's DllMain(PROCESS_ATTACH)
    return &g_module;
}
