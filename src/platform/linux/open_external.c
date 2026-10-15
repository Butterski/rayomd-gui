/* SPDX-License-Identifier: Apache-2.0 */
/* xdg-open (see open_external.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* posix_spawn_file_actions_addclosefrom_np (glibc 2.34) */
#endif
#include "platform/linux/open_external.h"

#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char** environ;

static bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~' || c == '/';
}

char* rgui_file_uri(const char* path) {
    if (!path || !*path) return NULL;
    char* cwd = NULL;
    if (path[0] != '/') {
        cwd = getcwd(NULL, 0);
        if (!cwd || cwd[0] != '/') {
            free(cwd);
            return NULL;
        }
    }
    static const char kHex[] = "0123456789ABCDEF";
    static const char kScheme[] = "file://";
    const size_t cwd_length = cwd ? strlen(cwd) : 0;
    const size_t path_length = strlen(path);
    /* Every byte may take three; one more for the '/' between cwd and path, one for the NUL. */
    if (path_length > ((size_t)-1 - 64) / 3 - cwd_length) {
        free(cwd);
        return NULL;
    }
    char* uri = (char*)malloc(sizeof kScheme + 3 * (cwd_length + path_length) + 2);
    if (!uri) {
        free(cwd);
        return NULL;
    }
    size_t n = 0;
    memcpy(uri, kScheme, sizeof kScheme - 1);
    n += sizeof kScheme - 1;
    for (int part = 0; part < 2; ++part) {
        const char* s = part == 0 ? cwd : path;
        if (!s) continue;
        if (part == 1 && cwd && (cwd_length == 0 || cwd[cwd_length - 1] != '/')) uri[n++] = '/';
        for (; *s; ++s) {
            unsigned char c = (unsigned char)*s;
            if (unreserved(c)) {
                uri[n++] = (char)c;
            } else {
                uri[n++] = '%';
                uri[n++] = kHex[c >> 4];
                uri[n++] = kHex[c & 15];
            }
        }
    }
    uri[n] = 0;
    free(cwd);
    return uri;
}

pid_t rgui_open_externally(const char* path) {
    char* uri = rgui_file_uri(path);
    if (!uri) return -1;
    pid_t pid = -1;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t actions;
    if (posix_spawnattr_init(&attr) != 0) {
        free(uri);
        return -1;
    }
    if (posix_spawn_file_actions_init(&actions) != 0) {
        posix_spawnattr_destroy(&attr);
        free(uri);
        return -1;
    }
    sigset_t none;
    sigset_t defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGCHLD);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    /* The engine opens files without O_CLOEXEC (fopen "rb"); none of them reaches the viewer. */
    bool ok = posix_spawnattr_setsigmask(&attr, &none) == 0 && posix_spawnattr_setsigdefault(&attr, &defaults) == 0 &&
              posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF) == 0 &&
              posix_spawn_file_actions_addclosefrom_np(&actions, 3) == 0;
    char* argv[] = {(char*)"xdg-open", uri, NULL};
    if (ok && posix_spawnp(&pid, "xdg-open", &actions, &attr, argv, environ) != 0) pid = -1;
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    free(uri);
    return pid;
}
