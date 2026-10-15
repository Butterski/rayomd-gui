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

static const char kRequest[] = "j/getoption decoration:rounding";

enum { kConnecting, kSending, kReading };

struct rgui_hypr_query {
    int fd;
    int phase;
    int64_t deadline;
    size_t sent;
    size_t size;
    char reply[RGUI_HYPR_MAX_REPLY];
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool rgui_hypr_parse_rounding(const char* reply, size_t length, int* rounding) {
    static const char kKey[] = "\"int\":";
    const size_t key_length = sizeof kKey - 1;
    for (size_t i = 0; i + key_length <= length; ++i) {
        if (memcmp(reply + i, kKey, key_length) != 0) continue;
        size_t p = i + key_length;
        while (p < length && (reply[p] == ' ' || reply[p] == '\t' || reply[p] == '\n' || reply[p] == '\r')) ++p;
        if (p >= length || reply[p] < '0' || reply[p] > '9') return false;
        int value = 0;
        while (p < length && reply[p] >= '0' && reply[p] <= '9') {
            if (value <= RGUI_HYPR_MAX_ROUNDING) value = value * 10 + (reply[p] - '0');
            ++p;
        }
        /* A number goes on with a fraction or exponent: not an integer. */
        if (p < length && (reply[p] == '.' || reply[p] == 'e' || reply[p] == 'E')) return false;
        *rounding = value > RGUI_HYPR_MAX_ROUNDING ? RGUI_HYPR_MAX_ROUNDING : value;
        return true;
    }
    return false;
}

bool rgui_hypr_socket_path(char* out, size_t capacity) {
    const char* signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (!signature || !*signature || strchr(signature, '/') || !runtime || runtime[0] != '/') return false;
    int n = snprintf(out, capacity, "%s/hypr/%s/.socket.sock", runtime, signature);
    return n >= 0 && (size_t)n < capacity && (size_t)n < sizeof(((struct sockaddr_un*)0)->sun_path);
}

rgui_hypr_query* rgui_hypr_query_start(const char* socket_path, int timeout_ms) {
    struct sockaddr_un sa;
    size_t path_length = strlen(socket_path);
    if (path_length == 0 || path_length >= sizeof sa.sun_path) return NULL;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, socket_path, path_length);

    rgui_hypr_query* q = (rgui_hypr_query*)malloc(sizeof *q);
    if (!q) return NULL;
    q->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    q->phase = kSending;
    q->deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 1);
    q->sent = 0;
    q->size = 0;
    if (q->fd < 0) {
        free(q);
        return NULL;
    }
    if (connect(q->fd, (const struct sockaddr*)&sa, sizeof sa) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) {
            rgui_hypr_query_free(q);
            return NULL;
        }
        q->phase = kConnecting;
    }
    return q;
}

int rgui_hypr_query_fd(const rgui_hypr_query* q) { return q->fd; }

short rgui_hypr_query_events(const rgui_hypr_query* q) { return q->phase == kReading ? POLLIN : POLLOUT; }

int rgui_hypr_query_timeout_ms(const rgui_hypr_query* q) {
    int64_t left = q->deadline - now_ms();
    if (left < 0) return 0;
    return left > 0x7FFFFFFF ? 0x7FFFFFFF : (int)left;
}

int rgui_hypr_query_step(rgui_hypr_query* q, int* rounding) {
    if (q->phase == kConnecting) {
        int error = 0;
        socklen_t error_length = sizeof error;
        struct pollfd p = {q->fd, POLLOUT, 0};
        if (poll(&p, 1, 0) > 0) {
            if (getsockopt(q->fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 || error != 0) {
                return RGUI_HYPR_FAILED;
            }
            q->phase = kSending;
        }
    }
    while (q->phase == kSending) {
        ssize_t n = send(q->fd, kRequest + q->sent, sizeof kRequest - 1 - q->sent, MSG_NOSIGNAL);
        if (n > 0) {
            q->sent += (size_t)n;
            if (q->sent == sizeof kRequest - 1) q->phase = kReading;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            return RGUI_HYPR_FAILED;
        }
    }
    while (q->phase == kReading) {
        if (q->size == RGUI_HYPR_MAX_REPLY) return RGUI_HYPR_FAILED; /* longer than any answer */
        ssize_t n = recv(q->fd, q->reply + q->size, RGUI_HYPR_MAX_REPLY - q->size, 0);
        if (n > 0) {
            q->size += (size_t)n;
        } else if (n == 0) {
            return rgui_hypr_parse_rounding(q->reply, q->size, rounding) ? RGUI_HYPR_DONE : RGUI_HYPR_FAILED;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            return RGUI_HYPR_FAILED;
        }
    }
    return now_ms() >= q->deadline ? RGUI_HYPR_FAILED : RGUI_HYPR_PENDING;
}

void rgui_hypr_query_free(rgui_hypr_query* q) {
    if (!q) return;
    if (q->fd >= 0) close(q->fd);
    free(q);
}
