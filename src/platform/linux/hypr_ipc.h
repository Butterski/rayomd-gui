/* SPDX-License-Identifier: Apache-2.0 */
/* Hyprland's window rounding through its IPC socket (C11): `j/getoption decoration:rounding`
 * on $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock, answered with JSON such
 * as {"option": "decoration:rounding", "int": 10, "set": true}.
 *
 * The query never blocks: the event loop polls rgui_hypr_query_fd() for
 * rgui_hypr_query_events() and calls rgui_hypr_query_step() when it is ready and when
 * rgui_hypr_query_timeout_ms() has passed. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RGUI_HYPR_MAX_REPLY = 65536,
    RGUI_HYPR_MAX_ROUNDING = 64, /* larger values are clamped to this */
};

/* The rounding from a reply: the integer after the first `"int":`, clamped to
 * 0..RGUI_HYPR_MAX_ROUNDING. False when there is none, it is negative or not an integer. */
bool rgui_hypr_parse_rounding(const char* reply, size_t length, int* rounding);

/* The socket of the Hyprland we run under, written into `out` (NUL-terminated). False when
 * HYPRLAND_INSTANCE_SIGNATURE or XDG_RUNTIME_DIR is not set or usable, or it does not fit. */
bool rgui_hypr_socket_path(char* out, size_t capacity);

typedef struct rgui_hypr_query rgui_hypr_query;

enum rgui_hypr_status {
    RGUI_HYPR_PENDING = 0,
    RGUI_HYPR_DONE = 1,
    RGUI_HYPR_FAILED = -1,
};

/* Starts asking the Hyprland instance at `socket_path`; the whole exchange must end within
 * `timeout_ms` (read until EOF, at most RGUI_HYPR_MAX_REPLY bytes). NULL when it cannot even
 * start (no such socket, out of memory). */
rgui_hypr_query* rgui_hypr_query_start(const char* socket_path, int timeout_ms);
int rgui_hypr_query_fd(const rgui_hypr_query* query);
/* POLLOUT while connecting and sending the request, then POLLIN. */
short rgui_hypr_query_events(const rgui_hypr_query* query);
/* Milliseconds left until the deadline (0 once it passed). */
int rgui_hypr_query_timeout_ms(const rgui_hypr_query* query);
/* Does what the socket allows without blocking. DONE sets `*rounding`; FAILED (an error, a reply
 * without a rounding, or the deadline passed) leaves it unchanged. After DONE or FAILED only
 * rgui_hypr_query_free may be called. */
int rgui_hypr_query_step(rgui_hypr_query* query, int* rounding);
void rgui_hypr_query_free(rgui_hypr_query* query);

#ifdef __cplusplus
}
#endif
