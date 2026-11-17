// Win32 surface the Mirror's Edge VR mod uses, on macOS arm64, for the RunnerRecompiled port.
//
// Types and COM come from DXVK native's windows_base.h. This file adds the functions:
// - game memory: ReadProcessMemory / WriteProcessMemory translate the game's 32-bit addresses
//   through the runtime (mevr_guest_ptr); the lifted game keeps every address it had on Windows;
// - CRT "_s" functions, Interlocked*, SRWLOCK, files, time, threads;
// - things with no meaning here (registry, LoadLibrary of Windows DLLs, debug output) fail cleanly,
//   which the mod already handles because they can fail on Windows too.
#pragma once

#include <windows_base.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- annotations and calling conventions
#define _Printf_format_string_
#define _In_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Inout_
#ifndef WINAPI
#define WINAPI
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef __cdecl
#define __cdecl
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#define __declspec(x)
#define __forceinline inline __attribute__((always_inline))

// SEH: the guarded blocks only call into game memory through checked reads here.
#define __try if (1)
#define __except(x) else if (0)
#define EXCEPTION_EXECUTE_HANDLER 1
#define GetExceptionCode() 0

// ---------------------------------------------------------------- the runtime connection (mevr_mac.cpp)
extern "C" void* mevr_guest_ptr(uintptr_t guest);      // NULL when not committed guest memory
extern "C" uintptr_t mevr_guest_image_base();          // MirrorsEdge.exe, 0x400000
extern "C" void mevr_debug_out(const char* s);

// ---------------------------------------------------------------- constants
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define _TRUNCATE ((size_t)-1)
#define ERROR_SUCCESS 0L
#define ERROR_DEVICE_NOT_CONNECTED 1167L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_ALREADY_EXISTS 183L
#define CP_UTF8 65001
#define CP_ACP 0
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define KEY_QUERY_VALUE 1
#define KEY_READ 0x20019
#define HKEY_LOCAL_MACHINE ((HKEY)(uintptr_t)0x80000002)
#define HKEY_CURRENT_USER ((HKEY)(uintptr_t)0x80000001)
#define RRF_RT_REG_SZ 2
#define RRF_RT_REG_DWORD 0x10
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 4
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_NOACCESS 1
#define PAGE_GUARD 0x100
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_F1 0x70

typedef intptr_t (*FARPROC)();
typedef BYTE BOOLEAN;
typedef int64_t LONG64;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef void* HKEY;
typedef HKEY* PHKEY;
typedef LONG LSTATUS;
typedef DWORD (WINAPI* LPTHREAD_START_ROUTINE)(LPVOID);
typedef const wchar_t* LPCWSTR_;

// ---------------------------------------------------------------- strings
static inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
static inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
static inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
static inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) { return wcsncasecmp(a, b, n); }
static inline int lstrcmpiA(const char* a, const char* b) { return strcasecmp(a, b); }

static inline int strcpy_s(char* d, size_t n, const char* s)
{ if (!d || !n) return EINVAL; size_t l = strlen(s); if (l >= n) { d[0] = 0; return ERANGE; } memcpy(d, s, l + 1); return 0; }
template <size_t N> static inline int strcpy_s(char (&d)[N], const char* s) { return strcpy_s(d, N, s); }
static inline int strcat_s(char* d, size_t n, const char* s)
{ size_t a = strnlen(d, n); if (a == n) return EINVAL; return strcpy_s(d + a, n - a, s); }
template <size_t N> static inline int strcat_s(char (&d)[N], const char* s) { return strcat_s(d, N, s); }
static inline int strncpy_s(char* d, size_t n, const char* s, size_t count)
{
    if (!d || !n) return EINVAL;
    size_t l = strnlen(s, count == _TRUNCATE ? n - 1 : count);
    if (l >= n) l = n - 1;
    memcpy(d, s, l); d[l] = 0; return 0;
}
template <size_t N> static inline int strncpy_s(char (&d)[N], const char* s, size_t c) { return strncpy_s(d, N, s, c); }
static inline int vsprintf_s(char* d, size_t n, const char* f, va_list ap) { return vsnprintf(d, n, f, ap); }
static inline int sprintf_s(char* d, size_t n, const char* f, ...)
{ va_list ap; va_start(ap, f); int r = vsnprintf(d, n, f, ap); va_end(ap); return r; }
template <size_t N> static inline int sprintf_s(char (&d)[N], const char* f, ...)
{ va_list ap; va_start(ap, f); int r = vsnprintf(d, N, f, ap); va_end(ap); return r; }
static inline int _snprintf_s(char* d, size_t n, size_t count, const char* f, ...)
{ (void)count; va_list ap; va_start(ap, f); int r = vsnprintf(d, n, f, ap); va_end(ap); return r < 0 || (size_t)r >= n ? -1 : r; }
template <size_t N> static inline int _snprintf_s(char (&d)[N], size_t count, const char* f, ...)
{ (void)count; va_list ap; va_start(ap, f); int r = vsnprintf(d, N, f, ap); va_end(ap); return r < 0 || (size_t)r >= N ? -1 : r; }
static inline int _vsnprintf_s(char* d, size_t n, size_t count, const char* f, va_list ap)
{ (void)count; int r = vsnprintf(d, n, f, ap); return r < 0 || (size_t)r >= n ? -1 : r; }
template <size_t N> static inline int _vsnprintf_s(char (&d)[N], size_t count, const char* f, va_list ap)
{ return _vsnprintf_s(d, N, count, f, ap); }
#define sscanf_s sscanf
#define _snprintf snprintf
#define _vsnprintf vsnprintf
static inline char* strtok_s(char* s, const char* d, char** ctx) { return strtok_r(s, d, ctx); }
static inline int _stricmp_l(const char* a, const char* b, void*) { return strcasecmp(a, b); }
static inline int fopen_s(FILE** f, const char* path, const char* mode) { *f = fopen(path, mode); return *f ? 0 : errno; }


// MSVC wide printf: %s / %c in a WIDE format take wide arguments; POSIX wants %ls / %lc.
static inline std::wstring MevrWideFormat(const wchar_t* f)
{
    std::wstring o;
    for (const wchar_t* p = f; *p; ++p) {
        o += *p;
        if (*p != L'%') continue;
        if (p[1] == L'%') { o += *++p; continue; }
        const wchar_t* q = p + 1;
        while (*q && wcschr(L"-+ #0123456789.*", *q)) o += *q++;
        bool sized = false;
        while (*q && wcschr(L"hlLzjtIw", *q)) { sized = true; if (*q == L'h') { ++q; continue; } o += *q++; }
        if ((*q == L's' || *q == L'c') && !sized) o += L'l';
        if (*q == L'S') { o += L's'; p = q; continue; }   // %S = narrow string in a wide format
        if (*q) o += *q;
        p = *q ? q : q - 1;
    }
    return o;
}
static inline int MevrVswprintf(wchar_t* d, size_t n, const wchar_t* f, va_list ap)
{ return vswprintf(d, n, MevrWideFormat(f).c_str(), ap); }

static inline int wcscpy_s(wchar_t* d, size_t n, const wchar_t* s)
{ if (!d || !n) return EINVAL; size_t l = wcslen(s); if (l >= n) { d[0] = 0; return ERANGE; } wmemcpy(d, s, l + 1); return 0; }
template <size_t N> static inline int wcscpy_s(wchar_t (&d)[N], const wchar_t* s) { return wcscpy_s(d, N, s); }
static inline int wcscat_s(wchar_t* d, size_t n, const wchar_t* s)
{ size_t a = wcsnlen(d, n); if (a == n) return EINVAL; return wcscpy_s(d + a, n - a, s); }
template <size_t N> static inline int wcscat_s(wchar_t (&d)[N], const wchar_t* s) { return wcscat_s(d, N, s); }
static inline int wcsncpy_s(wchar_t* d, size_t n, const wchar_t* s, size_t count)
{ size_t l = wcsnlen(s, count == _TRUNCATE ? n - 1 : count); if (l >= n) l = n - 1; wmemcpy(d, s, l); d[l] = 0; return 0; }
template <size_t N> static inline int wcsncpy_s(wchar_t (&d)[N], const wchar_t* s, size_t c) { return wcsncpy_s(d, N, s, c); }
static inline size_t wcsnlen_s(const wchar_t* s, size_t n) { return s ? wcsnlen(s, n) : 0; }
static inline size_t strnlen_s(const char* s, size_t n) { return s ? strnlen(s, n) : 0; }
static inline int swprintf_s(wchar_t* d, size_t n, const wchar_t* f, ...)
{ va_list ap; va_start(ap, f); int r = MevrVswprintf(d, n, f, ap); va_end(ap); return r; }
template <size_t N> static inline int swprintf_s(wchar_t (&d)[N], const wchar_t* f, ...)
{ va_list ap; va_start(ap, f); int r = MevrVswprintf(d, N, f, ap); va_end(ap); return r; }
static inline int _wtoi(const wchar_t* s) { return (int)wcstol(s, nullptr, 10); }
#define _wcsdup wcsdup

// UTF-8 <-> wchar_t (UTF-32 here). Paths and INI text in this mod are ASCII in practice.
static inline int WideCharToMultiByte(UINT, DWORD, const wchar_t* w, int wn, char* out, int outn, const char*, BOOL*)
{
    std::string s;
    for (int i = 0; wn < 0 ? w[i] != 0 : i < wn; ++i) {
        uint32_t c = (uint32_t)w[i];
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    if (wn < 0) s += '\0';
    if (!out || !outn) return (int)s.size();
    if ((int)s.size() > outn) return 0;
    memcpy(out, s.data(), s.size());
    return (int)s.size();
}
static inline int MultiByteToWideChar(UINT, DWORD, const char* a, int an, wchar_t* out, int outn)
{
    std::wstring w;
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* e = an < 0 ? nullptr : p + an;
    while (e ? p < e : *p) {
        uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
        while (extra-- > 0 && (e ? p < e : *p)) c = (c << 6) | (*p++ & 0x3F);
        w += (wchar_t)c;
    }
    if (an < 0) w += L'\0';
    if (!out || !outn) return (int)w.size();
    if ((int)w.size() > outn) return 0;
    wmemcpy(out, w.data(), w.size());
    return (int)w.size();
}
static inline std::string MevrUtf8(const wchar_t* w)
{
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// ---------------------------------------------------------------- atomics
static inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedIncrement(volatile LONG* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedAdd(volatile LONG* p, LONG v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchangeAdd(volatile LONG* p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG* p, LONG x, LONG c)
{ __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
static inline LONG64 InterlockedExchange64(volatile LONG64* p, LONG64 v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedIncrement64(volatile LONG64* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedCompareExchange64(volatile LONG64* p, LONG64 x, LONG64 c)
{ __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
static inline void* InterlockedExchangePointer(void* volatile* p, void* v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline void* InterlockedCompareExchangePointer(void* volatile* p, void* x, void* c)
{ __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor() __asm__ __volatile__("yield")

// ---------------------------------------------------------------- locks
typedef struct { pthread_rwlock_t l; } SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT { PTHREAD_RWLOCK_INITIALIZER }
static inline void InitializeSRWLock(SRWLOCK* s) { pthread_rwlock_init(&s->l, nullptr); }
static inline void AcquireSRWLockExclusive(SRWLOCK* s) { pthread_rwlock_wrlock(&s->l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK* s) { pthread_rwlock_unlock(&s->l); }
static inline void AcquireSRWLockShared(SRWLOCK* s) { pthread_rwlock_rdlock(&s->l); }
static inline void ReleaseSRWLockShared(SRWLOCK* s) { pthread_rwlock_unlock(&s->l); }
static inline BOOLEAN TryAcquireSRWLockExclusive(SRWLOCK* s) { return pthread_rwlock_trywrlock(&s->l) == 0; }
typedef struct { pthread_mutex_t m; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION* c)
{ pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&c->m, &a); }
static inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* c, DWORD) { InitializeCriticalSection(c); return TRUE; }
static inline void EnterCriticalSection(CRITICAL_SECTION* c) { pthread_mutex_lock(&c->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION* c) { pthread_mutex_unlock(&c->m); }
static inline void DeleteCriticalSection(CRITICAL_SECTION* c) { pthread_mutex_destroy(&c->m); }

// ---------------------------------------------------------------- errors, time, threads, processes
DWORD GetLastError();
void SetLastError(DWORD e);
static inline ULONGLONG GetTickCount64() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (ULONGLONG)t.tv_sec * 1000ull + (ULONGLONG)t.tv_nsec / 1000000ull; }
static inline DWORD GetTickCount() { return (DWORD)GetTickCount64(); }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); c->QuadPart = (LONGLONG)t.tv_sec * 1000000000ll + t.tv_nsec; return TRUE; }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000ll; return TRUE; }
static inline void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000u); }
static inline DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
static inline DWORD GetCurrentThreadId() { uint64_t t; pthread_threadid_np(nullptr, &t); return (DWORD)t; }
static inline HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)-1; }
static inline HANDLE GetCurrentThread() { return (HANDLE)(intptr_t)-2; }
HANDLE CreateThread(void* attrs, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD* id);
BOOL CloseHandle(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
static inline BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_ABOVE_NORMAL 1

// ---------------------------------------------------------------- game memory
static inline BOOL ReadProcessMemory(HANDLE, LPCVOID addr, LPVOID out, SIZE_T n, SIZE_T* got)
{
    // Every page the read touches must be guest memory; the runtime commits in 64 KB blocks,
    // so checking the first and the last byte covers every read this mod makes (<= 4 KB).
    const uintptr_t a = (uintptr_t)addr;
    void* p = mevr_guest_ptr(a);
    if (got) *got = 0;
    if (!p || !n || (n > 1 && !mevr_guest_ptr(a + n - 1)) || a + n < a || a + n > 0x100000000ull) return FALSE;
    memcpy(out, p, n);
    if (got) *got = n;
    return TRUE;
}
static inline BOOL WriteProcessMemory(HANDLE, LPVOID addr, LPCVOID in, SIZE_T n, SIZE_T* put)
{
    const uintptr_t a = (uintptr_t)addr;
    void* p = mevr_guest_ptr(a);
    if (put) *put = 0;
    if (!p || !n || (n > 1 && !mevr_guest_ptr(a + n - 1)) || a + n > 0x100000000ull) return FALSE;
    memcpy(p, in, n);
    if (put) *put = n;
    return TRUE;
}

// Modules: only the game image has a meaning (its guest base). Windows DLLs do not exist here.
static inline HMODULE GetModuleHandleW(LPCWSTR name) { return name ? nullptr : (HMODULE)mevr_guest_image_base(); }
static inline HMODULE GetModuleHandleA(LPCSTR name) { return name ? nullptr : (HMODULE)mevr_guest_image_base(); }
static inline BOOL GetModuleHandleExW(DWORD, LPCWSTR, HMODULE* out) { if (out) *out = nullptr; return FALSE; }
static inline HMODULE LoadLibraryW(LPCWSTR) { return nullptr; }
static inline HMODULE LoadLibraryA(LPCSTR) { return nullptr; }
static inline HMODULE LoadLibraryExW(LPCWSTR, HANDLE, DWORD) { return nullptr; }
static inline FARPROC GetProcAddress(HMODULE, LPCSTR) { return nullptr; }
static inline BOOL FreeLibrary(HMODULE) { return TRUE; }
DWORD GetModuleFileNameW(HMODULE m, wchar_t* out, DWORD n);
static inline UINT GetSystemDirectoryW(wchar_t* out, UINT n) { return (UINT)(wcscpy_s(out, n, L"/usr/lib") ? 0 : wcslen(out)); }
DWORD GetEnvironmentVariableW(LPCWSTR name, wchar_t* out, DWORD n);
static inline void OutputDebugStringA(const char* s) { mevr_debug_out(s); }
static inline void OutputDebugStringW(const wchar_t* s) { mevr_debug_out(MevrUtf8(s).c_str()); }
static inline BOOL IsDebuggerPresent() { return FALSE; }

// Registry: absent.
static inline LSTATUS RegOpenKeyExW(HKEY, LPCWSTR, DWORD, DWORD, PHKEY out) { if (out) *out = nullptr; return ERROR_FILE_NOT_FOUND; }
static inline LSTATUS RegOpenKeyExA(HKEY, LPCSTR, DWORD, DWORD, PHKEY out) { if (out) *out = nullptr; return ERROR_FILE_NOT_FOUND; }
static inline LSTATUS RegQueryValueExW(HKEY, LPCWSTR, DWORD*, DWORD*, BYTE*, DWORD*) { return ERROR_FILE_NOT_FOUND; }
static inline LSTATUS RegQueryValueExA(HKEY, LPCSTR, DWORD*, DWORD*, BYTE*, DWORD*) { return ERROR_FILE_NOT_FOUND; }
static inline LSTATUS RegGetValueW(HKEY, LPCWSTR, LPCWSTR, DWORD, DWORD*, void*, DWORD*) { return ERROR_FILE_NOT_FOUND; }
static inline LSTATUS RegCloseKey(HKEY) { return ERROR_SUCCESS; }

// ---------------------------------------------------------------- files (wide paths -> UTF-8)
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void* sa, DWORD disp, DWORD flags, HANDLE tmpl);
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, void* sa, DWORD disp, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, LPVOID out, DWORD n, DWORD* got, void* ov);
BOOL WriteFile(HANDLE h, LPCVOID in, DWORD n, DWORD* put, void* ov);
BOOL FlushFileBuffers(HANDLE h);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size);
DWORD SetFilePointer(HANDLE h, LONG lo, LONG* hi, DWORD method);
BOOL SetEndOfFile(HANDLE h);
DWORD GetFileAttributesW(LPCWSTR path);
BOOL DeleteFileW(LPCWSTR path);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL CreateDirectoryW(LPCWSTR path, void* sa);
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8
#define INVALID_SET_FILE_POINTER ((DWORD)-1)

// ---------------------------------------------------------------- input
static inline SHORT GetAsyncKeyState(int) { return 0; }
static inline SHORT GetKeyState(int) { return 0; }

// ---------------------------------------------------------------- batch 2: more types and constants
typedef int32_t INT32;
typedef int16_t INT16;
typedef BYTE* LPBYTE;
#ifndef BOOLEAN_DEFINED
#endif

#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#define FILE_APPEND_DATA 4u
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_DWORD 4
#define KEY_WOW64_32KEY 0x200
#define ERROR_NO_MORE_ITEMS 259L
#define MB_ERR_INVALID_CHARS 8
#define FORMAT_MESSAGE_FROM_SYSTEM 0x1000
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x200
#define PAGE_READONLY 2
#define PAGE_EXECUTE_READ 0x20
#define MEM_FREE 0x10000
#define WM_CLOSE 0x10
#define LOWORD(l) ((WORD)((uintptr_t)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((uintptr_t)(l) >> 16) & 0xffff))
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_FILE_MACHINE_I386 0x014c
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020

static inline int _snwprintf_s(wchar_t* d, size_t n, size_t, const wchar_t* f, ...)
{ va_list ap; va_start(ap, f); int r = MevrVswprintf(d, n, f, ap); va_end(ap); return r; }
template <size_t N> static inline int _snwprintf_s(wchar_t (&d)[N], size_t, const wchar_t* f, ...)
{ va_list ap; va_start(ap, f); int r = MevrVswprintf(d, N, f, ap); va_end(ap); return r; }
static inline long InterlockedIncrement(volatile long* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline long InterlockedDecrement(volatile long* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline long InterlockedExchange(volatile long* p, long v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline long InterlockedCompareExchange(volatile long* p, long x, long c)
{ __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
static inline long InterlockedExchangeAdd(volatile long* p, long v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

// PE structures (the game image is mapped in guest memory by the runtime's PE loader).
#pragma pack(push, 2)
typedef struct { WORD e_magic, e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc, e_ss, e_sp, e_csum, e_ip, e_cs,
                 e_lfarlc, e_ovno, e_res[4], e_oemid, e_oeminfo, e_res2[10]; LONG e_lfanew; } IMAGE_DOS_HEADER;
#pragma pack(pop)
typedef struct { WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
                 WORD SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct { WORD Magic; BYTE MajorLinkerVersion, MinorLinkerVersion; DWORD SizeOfCode, SizeOfInitializedData,
                 SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode, BaseOfData, ImageBase, SectionAlignment,
                 FileAlignment; WORD MajorOperatingSystemVersion, MinorOperatingSystemVersion, MajorImageVersion,
                 MinorImageVersion, MajorSubsystemVersion, MinorSubsystemVersion; DWORD Win32VersionValue,
                 SizeOfImage, SizeOfHeaders, CheckSum; WORD Subsystem, DllCharacteristics; DWORD SizeOfStackReserve,
                 SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit, LoaderFlags, NumberOfRvaAndSizes;
                 IMAGE_DATA_DIRECTORY DataDirectory[16]; } IMAGE_OPTIONAL_HEADER32;
typedef struct { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER32 OptionalHeader; } IMAGE_NT_HEADERS;
typedef struct { BYTE Name[8]; union { DWORD PhysicalAddress, VirtualSize; } Misc; DWORD VirtualAddress, SizeOfRawData,
                 PointerToRawData, PointerToRelocations, PointerToLinenumbers; WORD NumberOfRelocations,
                 NumberOfLinenumbers; DWORD Characteristics; } IMAGE_SECTION_HEADER;
#define IMAGE_FIRST_SECTION(nt) ((IMAGE_SECTION_HEADER*)((uintptr_t)&(nt)->OptionalHeader + (nt)->FileHeader.SizeOfOptionalHeader))
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_CNT_CODE 0x00000020
#define IMAGE_SCN_MEM_WRITE 0x80000000

typedef struct { void* BaseAddress; void* AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize; DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION;
static inline SIZE_T VirtualQuery(LPCVOID, MEMORY_BASIC_INFORMATION* m, SIZE_T) { memset(m, 0, sizeof *m); return 0; }
static inline BOOL VirtualProtect(LPVOID, SIZE_T, DWORD, DWORD* old) { if (old) *old = 0; return FALSE; }
typedef MEMORYSTATUS* LPMEMORYSTATUS; // MEMORYSTATUS itself is in DXVK's windows_base.h
typedef struct { WORD wProcessorArchitecture, wReserved; DWORD dwPageSize; void* lpMinimumApplicationAddress; void* lpMaximumApplicationAddress;
                 uintptr_t dwActiveProcessorMask; DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
                 WORD wProcessorLevel, wProcessorRevision; } SYSTEM_INFO;
static inline void GetSystemInfo(SYSTEM_INFO* s) { memset(s, 0, sizeof *s); s->dwPageSize = 4096; s->dwAllocationGranularity = 65536; s->dwNumberOfProcessors = (DWORD)sysconf(_SC_NPROCESSORS_ONLN); }
typedef struct { void* lpBaseOfDll; DWORD SizeOfImage; void* EntryPoint; } MODULEINFO;
static inline BOOL GetModuleInformation(HANDLE, HMODULE, MODULEINFO* m, DWORD) { memset(m, 0, sizeof *m); return FALSE; }
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
static inline void GetLocalTime(SYSTEMTIME* s)
{
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm t; localtime_r(&ts.tv_sec, &t);
    s->wYear = (WORD)(t.tm_year + 1900); s->wMonth = (WORD)(t.tm_mon + 1); s->wDayOfWeek = (WORD)t.tm_wday; s->wDay = (WORD)t.tm_mday;
    s->wHour = (WORD)t.tm_hour; s->wMinute = (WORD)t.tm_min; s->wSecond = (WORD)t.tm_sec; s->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}
static inline void GetSystemTime(SYSTEMTIME* s) { GetLocalTime(s); }
typedef struct { DWORD dwSignature, dwStrucVersion, dwFileVersionMS, dwFileVersionLS, dwProductVersionMS, dwProductVersionLS,
                 dwFileFlagsMask, dwFileFlags, dwFileOS, dwFileType, dwFileSubtype, dwFileDateMS, dwFileDateLS; } VS_FIXEDFILEINFO;
static inline DWORD GetFileVersionInfoSizeW(LPCWSTR, DWORD*) { return 0; }
static inline BOOL GetFileVersionInfoW(LPCWSTR, DWORD, DWORD, void*) { return FALSE; }
static inline BOOL VerQueryValueW(const void*, LPCWSTR, void**, UINT*) { return FALSE; }
static inline LSTATUS RegEnumValueW(HKEY, DWORD, wchar_t*, DWORD*, DWORD*, DWORD*, BYTE*, DWORD*) { return ERROR_NO_MORE_ITEMS; }
static inline DWORD FormatMessageA(DWORD, const void*, DWORD, DWORD, char* buf, DWORD n, va_list*) { if (buf && n) buf[0] = 0; return 0; }
static inline DWORD FormatMessageW(DWORD, const void*, DWORD, DWORD, wchar_t* buf, DWORD n, va_list*) { if (buf && n) buf[0] = 0; return 0; }
DWORD ExpandEnvironmentStringsW(LPCWSTR src, wchar_t* out, DWORD n);
DWORD GetPrivateProfileStringW(LPCWSTR sec, LPCWSTR key, LPCWSTR def, wchar_t* out, DWORD n, LPCWSTR file);
BOOL MoveFileW(LPCWSTR from, LPCWSTR to);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* pos, DWORD method);
static inline BOOL IsWindow(HWND w) { return w != nullptr; }
static inline BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM) { return FALSE; }

// Virtual keys (only used for the desktop debug keys; GetAsyncKeyState reports nothing here).
enum {
    VK_BACK = 0x08, VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24,
    VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_INSERT = 0x2D, VK_DELETE = 0x2E,
    VK_NUMPAD0 = 0x60, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9,
    VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F,
    VK_F2 = 0x71, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
    VK_SCROLL = 0x91, VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_MINUS = 0xBD, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB,
    VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE
};

static inline BOOL GetFileTime(HANDLE, FILETIME* c, FILETIME* a, FILETIME* w)
{ for (FILETIME* f : { c, a, w }) if (f) f->dwLowDateTime = f->dwHighDateTime = 0; return FALSE; }
static inline BOOL FileTimeToLocalFileTime(const FILETIME* in, FILETIME* out) { *out = *in; return TRUE; }
static inline BOOL FileTimeToSystemTime(const FILETIME*, SYSTEMTIME* s) { GetLocalTime(s); return TRUE; }
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3
#ifndef APIENTRY
#define APIENTRY
#endif
static inline BOOL DisableThreadLibraryCalls(HMODULE) { return TRUE; }
