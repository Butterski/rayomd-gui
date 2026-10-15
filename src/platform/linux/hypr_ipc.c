/* SPDX-License-Identifier: Apache-2.0 */
/* Hyprland's rounding over its IPC socket (see hypr_ipc.h). */
#include "platform/linux/hypr_ipc.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Polls for `events` until `deadline`; false on timeout or error. */
static bool wait_for(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t left = deadline - now_ms();
        if (left <= 0) return false;
        struct pollfd p = {fd, events, 0};
        int r = poll(&p, 1, (int)left);
        if (r > 0) return true;
        if (r < 0 && errno != EINTR) return false;
    }
}

bool rgui_hypr_parse_rounding(const char* reply, size_t length, int* rounding) {
    static const char kKey[] = "\"int\":";
    const size_t key_length = sizeof kKey - 1;
    for (size_t i = 0; i + key_length <= length; ++i) {
        if (memcmp(reply + i, kKey, key_length) != 0) continue;
        size_t p = i + key_length;
        while (p < length && (reply[p] == ' ' || reply[p] == '\t' || reply[p] == '\n' || reply[p] == '\r')) ++p;
        if (p >= length || reply[p] < '0' || reply[p] > '9') return false;
        long value = 0;
        while (p < length && reply[p] >= '0' && reply[p] <= '9') {
            value = value * 10 + (reply[p] - '0');
            if (value > 100000) return false;
            ++p;
        }
        /* A number goes on with a fraction or exponent: not an integer. */
        if (p < length && (reply[p] == '.' || reply[p] == 'e' || reply[p] == 'E')) return false;
        *rounding = (int)value;
        return true;
    }
    return false;
}

bool rgui_hypr_query_rounding_at(const char* socket_path, int timeout_ms, int* rounding) {
    struct sockaddr_un sa;
    size_t path_length = strlen(socket_path);
    if (path_length == 0 || path_length >= sizeof sa.sun_path) return false;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, socket_path, path_length);
    int64_t deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    bool ok = false;
    char* reply = NULL;
    if (connect(fd, (const struct sockaddr*)&sa, sizeof sa) != 0) {
        int error = 0;
        socklen_t error_length = sizeof error;
        if (errno != EINPROGRESS || !wait_for(fd, POLLOUT, deadline) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 || error != 0) {
            goto done;
        }
    }

    static const char kRequest[] = "j/getoption decoration:rounding";
    size_t sent = 0;
    while (sent < sizeof kRequest - 1) {
        ssize_t n = send(fd, kRequest + sent, sizeof kRequest - 1 - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_for(fd, POLLOUT, deadline)) goto done;
        } else {
            goto done;
        }
    }

    reply = (char*)malloc(RGUI_HYPR_MAX_REPLY);
    if (!reply) goto done;
    size_t size = 0;
    for (;;) {
        if (size == RGUI_HYPR_MAX_REPLY) goto done; /* longer than any answer to this request */
        ssize_t n = recv(fd, reply + size, RGUI_HYPR_MAX_REPLY - size, 0);
        if (n > 0) {
            size += (size_t)n;
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_for(fd, POLLIN, deadline)) goto done;
        } else {
            goto done;
        }
    }
    ok = rgui_hypr_parse_rounding(reply, size, rounding);
done:
    free(reply);
    close(fd);
    return ok;
}

bool rgui_hypr_query_rounding(int timeout_ms, int* rounding) {
    const char* signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (!signature || !*signature || strchr(signature, '/') || !runtime || runtime[0] != '/') return false;
    char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
    int n = snprintf(path, sizeof path, "%s/hypr/%s/.socket.sock", runtime, signature);
    if (n < 0 || (size_t)n >= sizeof path) return false;
    return rgui_hypr_query_rounding_at(path, timeout_ms, rounding);
}
