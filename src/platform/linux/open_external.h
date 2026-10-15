/* SPDX-License-Identifier: Apache-2.0 */
/* Opening a file in the user's default application (C11): xdg-open, not waited for. */
#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The file:// URI of `path`: made absolute against the working directory when it is relative,
 * every byte other than ASCII letters, digits, `-._~` and `/` percent-encoded. So xdg-open never
 * reads it as an option (`-x.pdf`) or as another scheme (`mailto:x.pdf`). A malloc'd string the
 * caller frees, or NULL. */
char* rgui_file_uri(const char* path);

/* Starts `xdg-open <file URI of path>` through $PATH and returns at once: the child's pid, or
 * -1 when it could not be started. The child gets the default dispositions of SIGPIPE, SIGCHLD,
 * SIGINT and SIGTERM, no blocked signals whatever the app set for its own loop, and no file
 * descriptor beyond 0-2, close-on-exec or not. The app reaps nothing (its SIGCHLD has
 * SA_NOCLDWAIT). */
pid_t rgui_open_externally(const char* path);

#ifdef __cplusplus
}
#endif
