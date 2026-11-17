// MinHook API on macOS: lifted game code cannot be detoured at run time. Hooks on game functions
// are routed to the runtime's lifter hook table by mevr_mac.cpp (MH_CreateHook looks the target up
// there); anything else is refused, which every caller in the mod already handles.
#pragma once
typedef enum MH_STATUS {
    MH_UNKNOWN = -1, MH_OK = 0, MH_ERROR_ALREADY_INITIALIZED, MH_ERROR_NOT_INITIALIZED,
    MH_ERROR_ALREADY_CREATED, MH_ERROR_NOT_CREATED, MH_ERROR_ENABLED, MH_ERROR_DISABLED,
    MH_ERROR_NOT_EXECUTABLE, MH_ERROR_UNSUPPORTED_FUNCTION, MH_ERROR_MEMORY_ALLOC,
    MH_ERROR_MEMORY_PROTECT, MH_ERROR_MODULE_NOT_FOUND, MH_ERROR_FUNCTION_NOT_FOUND
} MH_STATUS;
#define MH_ALL_HOOKS NULL
MH_STATUS MH_Initialize(void);
MH_STATUS MH_Uninitialize(void);
MH_STATUS MH_CreateHook(void* target, void* detour, void** original);
MH_STATUS MH_RemoveHook(void* target);
MH_STATUS MH_EnableHook(void* target);
MH_STATUS MH_DisableHook(void* target);
const char* MH_StatusToString(MH_STATUS status);
