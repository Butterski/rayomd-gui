/* SPDX-License-Identifier: Apache-2.0 */
/* The Wayland wire protocol, client side: the connection, message encoding and decoding, file
 * descriptors over SCM_RIGHTS, the object map and dispatch. Every incoming byte is checked: a
 * malformed message fails the connection instead of being read past its end.
 *
 * A message is the object id (u32), then (size << 16 | opcode) (u32), then its arguments, each
 * a multiple of 4 bytes: i (int32), u (uint32), f (24.8 fixed), o (object id, 0 = null),
 * n (new object id), s (u32 length including the NUL, 0 = null, then the bytes padded to 4),
 * a (u32 length, then the bytes padded to 4), h (a file descriptor, carried out of band).
 * A signature is those letters, a '?' before s or o allowing null. */
#ifndef RGUI_PLATFORM_LINUX_WL_WIRE_H
#define RGUI_PLATFORM_LINUX_WL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RGUI_WL_BUFFER_SIZE = 65536,  /* the input and the output buffer */
    RGUI_WL_MAX_MESSAGE = 4096,   /* libwayland's limit: a server drops longer requests */
    RGUI_WL_MAX_ARGS = 20,
    RGUI_WL_MAX_FDS_OUT = 28,     /* per sendmsg, as libwayland */
    RGUI_WL_MAX_FDS_IN = 256,     /* received and not yet consumed */
    RGUI_WL_MAX_SERVER_OBJECTS = 65536,
};

#define RGUI_WL_DISPLAY_ID 1u
#define RGUI_WL_SERVER_ID_START 0xff000000u

/* A message's flags. */
enum { RGUI_WL_DESTRUCTOR = 1 };

typedef struct rgui_wl_interface rgui_wl_interface;

typedef struct rgui_wl_message {
    uint16_t opcode;     /* the message's position in its interface's XML */
    uint16_t flags;
    const char* name;
    const char* signature;
    const rgui_wl_interface* new_interface; /* the interface of an 'n' argument */
} rgui_wl_message;

/* Only the messages a program uses are listed; other incoming events are skipped by size. */
struct rgui_wl_interface {
    const char* name;
    uint32_t version;    /* the highest version these tables know */
    uint16_t request_count;
    uint16_t event_count;
    const rgui_wl_message* requests;
    const rgui_wl_message* events;
};

typedef struct rgui_wl_array {
    const void* data;
    uint32_t size;
} rgui_wl_array;

typedef union rgui_wl_arg {
    int32_t i;
    uint32_t u;
    int32_t f;          /* 24.8 fixed point */
    const char* s;      /* NUL-terminated, or NULL; incoming strings live until the handler returns */
    uint32_t o;         /* 0: null */
    uint32_t n;
    rgui_wl_array a;    /* incoming arrays live until the handler returns */
    int h;              /* incoming: the handler owns the fd; outgoing: duplicated, the caller keeps its own */
} rgui_wl_arg;

/* Receives an object's events. */
typedef void (*rgui_wl_dispatcher)(void* user, uint32_t object, uint16_t opcode, const rgui_wl_arg* args);

typedef struct rgui_wl_connection rgui_wl_connection;

/* Opens the compositor's socket: the fd in $WAYLAND_SOCKET when set (the variable is then
 * removed, as libwayland does), else $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY (default wayland-0; an
 * absolute WAYLAND_DISPLAY is used as is). The fd, or -1 with a message in `error`. */
int rgui_wl_connect_socket(char* error, size_t error_size);

/* A connection on a connected stream socket, which it takes over (set non-blocking, closed by
 * destroy). Object 1 is wl_display with `display_interface`, its error and delete_id events
 * handled here: a protocol error prints "Wayland protocol error: object <id> code <n>: <message>"
 * and exits with status 1. NULL when out of memory. */
rgui_wl_connection* rgui_wl_connection_create(int fd, const rgui_wl_interface* display_interface);
void rgui_wl_connection_destroy(rgui_wl_connection* c);
int rgui_wl_connection_fd(const rgui_wl_connection* c);
/* What failed last, for messages. */
const char* rgui_wl_connection_error(const rgui_wl_connection* c);

/* A new client object: the most recently freed id (ids return after wl_display.delete_id), else
 * the next unused one from 2 on.
 * 0 when the ids are exhausted. Send the request that creates it with the id as its 'n'. */
uint32_t rgui_wl_new_object(rgui_wl_connection* c, const rgui_wl_interface* iface, uint32_t version,
                            rgui_wl_dispatcher dispatcher, void* user);
/* Sets who receives an object's events (server-created objects start with none). -1 when the
 * object is unknown. */
int rgui_wl_set_dispatcher(rgui_wl_connection* c, uint32_t id, rgui_wl_dispatcher dispatcher, void* user);
/* The object's version, 0 when it is unknown or destroyed. */
uint32_t rgui_wl_object_version(const rgui_wl_connection* c, uint32_t id);

/* Queues a request; its arguments follow the message's signature. A destructor request retires
 * the object: client ids return after delete_id, server ids at once. A full output buffer is
 * flushed first, waiting for the socket when it must. 0, or -1 (unknown object or opcode, a
 * message over RGUI_WL_MAX_MESSAGE, null where the signature forbids it, a failed flush). */
int rgui_wl_send(rgui_wl_connection* c, uint32_t id, uint16_t opcode, const rgui_wl_arg* args);

/* Sends what is queued. 0 when everything went, 1 when the socket is full (call again when it is
 * writable), -1 on error. */
int rgui_wl_flush(rgui_wl_connection* c);
/* Like flush, waiting for the socket up to `timeout_ms` while it is full. 0 or -1. */
int rgui_wl_flush_blocking(rgui_wl_connection* c, int timeout_ms);

/* Reads what the socket has into the input buffer (one recvmsg). 1 when bytes came, 0 when there
 * was nothing (EAGAIN), -1 on error or when the compositor closed the connection. */
int rgui_wl_read(rgui_wl_connection* c);
/* Decodes and dispatches every complete message in the input buffer. The number dispatched, or
 * -1 when a message is malformed (the connection is unusable afterwards). Handlers may send
 * requests, create and destroy objects, and call rgui_wl_read / rgui_wl_dispatch again. */
int rgui_wl_dispatch(rgui_wl_connection* c);
/* Flushes, then reads and dispatches until the compositor has answered a wl_display.sync sent
 * now, waiting at most `timeout_ms` in total. 0, or -1 on error or timeout. */
int rgui_wl_roundtrip(rgui_wl_connection* c, const rgui_wl_interface* callback_interface, int timeout_ms);

/* 24.8 fixed point. */
static inline double rgui_wl_fixed_to_double(int32_t f) { return (double)f / 256.0; }
static inline int32_t rgui_wl_fixed_from_int(int v) { return (int32_t)((uint32_t)v << 8); }

#ifdef __cplusplus
}
#endif

#endif
