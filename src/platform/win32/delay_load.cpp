// SPDX-License-Identifier: Apache-2.0
// System DLLs the window needs only now and then (common dialogs, the shell, DWM, the IME),
// loaded from System32 on first use instead of being imported: the executable starts with
// kernel32, user32, gdi32 and advapi32 only. The linker sends every call of these functions here
// (-Wl,--wrap=<name>, set by rayomd_gui_win32), and delay_import_pointers.cpp points the
// __imp_ entries of the headers that declare them dllimport at the same wrappers.
// LoadSystemProcedure and RAYOMD_DELAY_PROC are copied from rayomd's src/win32/delay_load.cpp.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <cderr.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>

namespace {

template <typename Function>
Function LoadSystemProcedure(const wchar_t* moduleName, const char* procedureName) {
    HMODULE module = LoadLibraryExW(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return nullptr;
    // Through void (*)(), which -Wcast-function-type accepts as any function type.
    return reinterpret_cast<Function>(reinterpret_cast<void (*)()>(GetProcAddress(module, procedureName)));
}

#define RAYOMD_DELAY_PROC(moduleName, procedureName) \
    static const auto procedure = \
        LoadSystemProcedure<decltype(&procedureName)>(moduleName, #procedureName)

} // namespace

// comdlg32. When it cannot be loaded the dialogs fail and CommDlgExtendedError reports
// CDERR_INITIALIZATION, so the caller treats the dialog as unavailable.
extern "C" BOOL WINAPI __wrap_GetOpenFileNameW(LPOPENFILENAMEW dialog) {
    RAYOMD_DELAY_PROC(L"comdlg32.dll", GetOpenFileNameW);
    return procedure ? procedure(dialog) : FALSE;
}

extern "C" BOOL WINAPI __wrap_GetSaveFileNameW(LPOPENFILENAMEW dialog) {
    RAYOMD_DELAY_PROC(L"comdlg32.dll", GetSaveFileNameW);
    return procedure ? procedure(dialog) : FALSE;
}

extern "C" DWORD WINAPI __wrap_CommDlgExtendedError() {
    RAYOMD_DELAY_PROC(L"comdlg32.dll", CommDlgExtendedError);
    return procedure ? procedure() : CDERR_INITIALIZATION;
}

// shell32.
extern "C" void WINAPI __wrap_DragAcceptFiles(HWND window, BOOL accept) {
    RAYOMD_DELAY_PROC(L"shell32.dll", DragAcceptFiles);
    if (procedure) procedure(window, accept);
}

extern "C" UINT WINAPI __wrap_DragQueryFileW(HDROP drop, UINT index, LPWSTR file, UINT size) {
    RAYOMD_DELAY_PROC(L"shell32.dll", DragQueryFileW);
    return procedure ? procedure(drop, index, file, size) : 0;
}

extern "C" void WINAPI __wrap_DragFinish(HDROP drop) {
    RAYOMD_DELAY_PROC(L"shell32.dll", DragFinish);
    if (procedure) procedure(drop);
}

extern "C" BOOL WINAPI __wrap_ShellExecuteExW(SHELLEXECUTEINFOW* info) {
    RAYOMD_DELAY_PROC(L"shell32.dll", ShellExecuteExW);
    if (procedure) return procedure(info);
    SetLastError(ERROR_MOD_NOT_FOUND);
    return FALSE;
}

extern "C" HRESULT WINAPI __wrap_SHGetFolderPathW(HWND window, int folder, HANDLE token, DWORD flags,
                                                  LPWSTR path) {
    RAYOMD_DELAY_PROC(L"shell32.dll", SHGetFolderPathW);
    return procedure ? procedure(window, folder, token, flags, path)
                     : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}

// dwmapi.
extern "C" HRESULT WINAPI __wrap_DwmSetWindowAttribute(HWND window, DWORD attribute, LPCVOID value,
                                                       DWORD size) {
    RAYOMD_DELAY_PROC(L"dwmapi.dll", DwmSetWindowAttribute);
    return procedure ? procedure(window, attribute, value, size)
                     : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
}

// imm32. Without it there is no IME: no context, no composition.
extern "C" HIMC WINAPI __wrap_ImmGetContext(HWND window) {
    RAYOMD_DELAY_PROC(L"imm32.dll", ImmGetContext);
    return procedure ? procedure(window) : nullptr;
}

extern "C" BOOL WINAPI __wrap_ImmReleaseContext(HWND window, HIMC context) {
    RAYOMD_DELAY_PROC(L"imm32.dll", ImmReleaseContext);
    return procedure ? procedure(window, context) : FALSE;
}

extern "C" LONG WINAPI __wrap_ImmGetCompositionStringW(HIMC context, DWORD index, LPVOID buffer,
                                                       DWORD size) {
    RAYOMD_DELAY_PROC(L"imm32.dll", ImmGetCompositionStringW);
    return procedure ? procedure(context, index, buffer, size) : IMM_ERROR_GENERAL;
}

extern "C" BOOL WINAPI __wrap_ImmSetCompositionWindow(HIMC context, LPCOMPOSITIONFORM form) {
    RAYOMD_DELAY_PROC(L"imm32.dll", ImmSetCompositionWindow);
    return procedure ? procedure(context, form) : FALSE;
}

extern "C" BOOL WINAPI __wrap_ImmSetCandidateWindow(HIMC context, LPCANDIDATEFORM form) {
    RAYOMD_DELAY_PROC(L"imm32.dll", ImmSetCandidateWindow);
    return procedure ? procedure(context, form) : FALSE;
}

extern "C" BOOL WINAPI __wrap_ImmAssociateContextEx(HWND window, HIMC context, DWORD flags) {
    RAYOMD_DELAY_PROC(L"imm32.dll", ImmAssociateContextEx);
    return procedure ? procedure(window, context, flags) : FALSE;
}
