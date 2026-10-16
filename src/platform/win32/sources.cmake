# SPDX-License-Identifier: Apache-2.0
# The Win32 platform layer, except main_win32.cpp and the resources.
list(APPEND RGUI_WIN32_SOURCES
    platform/win32/delay_import_pointers.cpp
    platform/win32/delay_load.cpp
    platform/win32/frame_pacer.cpp
    platform/win32/win32_window.cpp)

# System DLLs loaded on first use (delay_load.cpp): every call of these goes to its __wrap_.
set(RGUI_WIN32_DELAY_LOADED
    GetOpenFileNameW GetSaveFileNameW CommDlgExtendedError
    DragAcceptFiles DragQueryFileW DragFinish ShellExecuteExW SHGetFolderPathW
    DwmSetWindowAttribute DwmGetCompositionTimingInfo
    ImmGetContext ImmReleaseContext ImmGetCompositionStringW ImmSetCompositionWindow
    ImmSetCandidateWindow ImmAssociateContextEx)
