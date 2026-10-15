/* SPDX-License-Identifier: Apache-2.0 */
/* Opening a file in the user's default application (C11): xdg-open, not waited for. */
#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts `xdg-open <path>` through $PATH and returns at once: the child's pid, or -1 when it
 * could not be started. The child gets the default dispositions of SIGPIPE, SIGCHLD, SIGINT and
 * SIGTERM and no blocked signals, whatever the app set for its own loop. The app reaps nothing
 * (its SIGCHLD has SA_NOCLDWAIT). */
pid_t rgui_open_externally(const char* path);

#ifdef __cplusplus
}
#endif
