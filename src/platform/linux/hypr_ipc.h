/* SPDX-License-Identifier: Apache-2.0 */
/* Hyprland's window rounding through its IPC socket (C11): `j/getoption decoration:rounding`
 * on $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock, answered with JSON such
 * as {"option": "decoration:rounding", "int": 10, "set": true}. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RGUI_HYPR_MAX_REPLY = 65536 };

/* The rounding from a reply: the integer after the first `"int":`. False when there is none or
 * it is negative or over 100000. */
bool rgui_hypr_parse_rounding(const char* reply, size_t length, int* rounding);

/* Asks the Hyprland instance at `socket_path`, waiting at most `timeout_ms` for the whole reply
 * (read until EOF, at most RGUI_HYPR_MAX_REPLY bytes). On any failure returns false and leaves
 * `*rounding` unchanged. */
bool rgui_hypr_query_rounding_at(const char* socket_path, int timeout_ms, int* rounding);

/* The same for the Hyprland we run under; false at once when HYPRLAND_INSTANCE_SIGNATURE or
 * XDG_RUNTIME_DIR is not set. */
bool rgui_hypr_query_rounding(int timeout_ms, int* rounding);

#ifdef __cplusplus
}
#endif
