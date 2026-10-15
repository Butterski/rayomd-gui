/* SPDX-License-Identifier: Apache-2.0 */
/* xdg-open (see open_external.h). */
#include "platform/linux/open_external.h"

#include <signal.h>
#include <spawn.h>
#include <stdbool.h>

extern char** environ;

pid_t rgui_open_externally(const char* path) {
    if (!path || !*path) return -1;
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0) return -1;
    sigset_t none;
    sigset_t defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGCHLD);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    bool ok = posix_spawnattr_setsigmask(&attr, &none) == 0 && posix_spawnattr_setsigdefault(&attr, &defaults) == 0 &&
              posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF) == 0;
    pid_t pid = -1;
    char* argv[] = {(char*)"xdg-open", (char*)path, NULL};
    if (ok && posix_spawnp(&pid, "xdg-open", NULL, &attr, argv, environ) != 0) pid = -1;
    posix_spawnattr_destroy(&attr);
    return pid;
}
