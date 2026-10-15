// SPDX-License-Identifier: Apache-2.0
// MinGW headers declare most of these APIs with dllimport, so callers reference the
// import-address pointer directly. Supplying that pointer here preserves the usual indirect call
// while resolving it to the lazy wrapper of delay_load.cpp, and keeps the import library's entry
// (and with it the DLL import) out of the executable. imm.h declares its functions without
// dllimport; those calls reach the wrappers through -Wl,--wrap alone.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>

extern "C" BOOL WINAPI __wrap_GetOpenFileNameW(LPOPENFILENAMEW);
extern "C" BOOL WINAPI __wrap_GetSaveFileNameW(LPOPENFILENAMEW);
extern "C" DWORD WINAPI __wrap_CommDlgExtendedError();
extern "C" void WINAPI __wrap_DragAcceptFiles(HWND, BOOL);
extern "C" UINT WINAPI __wrap_DragQueryFileW(HDROP, UINT, LPWSTR, UINT);
extern "C" void WINAPI __wrap_DragFinish(HDROP);
extern "C" HINSTANCE WINAPI __wrap_ShellExecuteW(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
extern "C" HRESULT WINAPI __wrap_SHGetFolderPathW(HWND, int, HANDLE, DWORD, LPWSTR);
extern "C" HRESULT WINAPI __wrap_DwmSetWindowAttribute(HWND, DWORD, LPCVOID, DWORD);
extern "C" HIMC WINAPI __wrap_ImmGetContext(HWND);
extern "C" BOOL WINAPI __wrap_ImmReleaseContext(HWND, HIMC);
extern "C" LONG WINAPI __wrap_ImmGetCompositionStringW(HIMC, DWORD, LPVOID, DWORD);
extern "C" BOOL WINAPI __wrap_ImmSetCompositionWindow(HIMC, LPCOMPOSITIONFORM);
extern "C" BOOL WINAPI __wrap_ImmSetCandidateWindow(HIMC, LPCANDIDATEFORM);
extern "C" BOOL WINAPI __wrap_ImmAssociateContextEx(HWND, HIMC, DWORD);

extern "C" {
decltype(&GetOpenFileNameW) __imp_GetOpenFileNameW = __wrap_GetOpenFileNameW;
decltype(&GetSaveFileNameW) __imp_GetSaveFileNameW = __wrap_GetSaveFileNameW;
decltype(&CommDlgExtendedError) __imp_CommDlgExtendedError = __wrap_CommDlgExtendedError;
decltype(&DragAcceptFiles) __imp_DragAcceptFiles = __wrap_DragAcceptFiles;
decltype(&DragQueryFileW) __imp_DragQueryFileW = __wrap_DragQueryFileW;
decltype(&DragFinish) __imp_DragFinish = __wrap_DragFinish;
decltype(&ShellExecuteW) __imp_ShellExecuteW = __wrap_ShellExecuteW;
decltype(&SHGetFolderPathW) __imp_SHGetFolderPathW = __wrap_SHGetFolderPathW;
decltype(&DwmSetWindowAttribute) __imp_DwmSetWindowAttribute = __wrap_DwmSetWindowAttribute;
decltype(&ImmGetContext) __imp_ImmGetContext = __wrap_ImmGetContext;
decltype(&ImmReleaseContext) __imp_ImmReleaseContext = __wrap_ImmReleaseContext;
decltype(&ImmGetCompositionStringW) __imp_ImmGetCompositionStringW = __wrap_ImmGetCompositionStringW;
decltype(&ImmSetCompositionWindow) __imp_ImmSetCompositionWindow = __wrap_ImmSetCompositionWindow;
decltype(&ImmSetCandidateWindow) __imp_ImmSetCandidateWindow = __wrap_ImmSetCandidateWindow;
decltype(&ImmAssociateContextEx) __imp_ImmAssociateContextEx = __wrap_ImmAssociateContextEx;
}
