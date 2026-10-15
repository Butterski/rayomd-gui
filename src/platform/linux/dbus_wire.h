/* SPDX-License-Identifier: Apache-2.0 */
/* A D-Bus client of our own (C11): the wire format (marshalling and strictly checked
 * unmarshalling), message headers, and a session-bus connection with method calls routed by
 * reply serial and signals routed by match rules.
 *
 * The connection is non-blocking and single-threaded. The event loop polls rgui_dbus_fd() for
 * rgui_dbus_poll_events(), calls rgui_dbus_dispatch() when it is readable or writable and when
 * rgui_dbus_timeout_ms() has passed. Callbacks run only from rgui_dbus_dispatch() (and
 * rgui_dbus_wait()); they may send, call, add and remove matches and cancel calls, but must not
 * dispatch, wait or close the connection.
 *
 * Reference: the D-Bus Specification 0.43, "Message Protocol" and "Authentication Protocol". */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The specification's limits. */
enum {
    RGUI_DBUS_MAX_MESSAGE = 134217728, /* 2^27 bytes, header and body */
    RGUI_DBUS_MAX_ARRAY = 67108864,    /* 2^26 bytes of array data */
    RGUI_DBUS_MAX_SIGNATURE = 255,
    RGUI_DBUS_MAX_NESTING = 32,       /* arrays, and separately structs, within a signature */
    RGUI_DBUS_MAX_TOTAL_NESTING = 64, /* containers of any kind, variants included, in a value */
};

/* The largest message the connection accepts. We only receive small replies and signals; a
 * larger message is skipped as it arrives, never buffered or parsed. */
enum { RGUI_DBUS_MAX_INCOMING = 1048576 };

enum rgui_dbus_message_type {
    RGUI_DBUS_METHOD_CALL = 1,
    RGUI_DBUS_METHOD_RETURN = 2,
    RGUI_DBUS_ERROR = 3,
    RGUI_DBUS_SIGNAL = 4,
};

enum rgui_dbus_flags {
    RGUI_DBUS_NO_REPLY_EXPECTED = 1,
    RGUI_DBUS_NO_AUTO_START = 2,
};

/* ---- Marshalling ---------------------------------------------------------------------------
 * A growable byte buffer that appends little-endian values aligned to their natural boundary,
 * relative to the buffer's start (a message body starts on an 8-byte boundary of its message,
 * so a body written into its own buffer is aligned as it will be in the message). A failed
 * allocation or a limit sets `failed`; later writes are ignored. */
typedef struct rgui_dbus_buf {
    uint8_t* data;
    size_t size;
    size_t capacity;
    bool failed;
} rgui_dbus_buf;

/* An array being written: where its length goes and where its elements start. */
typedef struct rgui_dbus_array {
    size_t length_at;
    size_t start;
} rgui_dbus_array;

void rgui_dbus_buf_free(rgui_dbus_buf* buf);
void rgui_dbus_put_byte(rgui_dbus_buf* buf, uint8_t value);
void rgui_dbus_put_bool(rgui_dbus_buf* buf, bool value);
void rgui_dbus_put_int16(rgui_dbus_buf* buf, int16_t value);
void rgui_dbus_put_uint16(rgui_dbus_buf* buf, uint16_t value);
void rgui_dbus_put_int32(rgui_dbus_buf* buf, int32_t value);
void rgui_dbus_put_uint32(rgui_dbus_buf* buf, uint32_t value); /* also 'h' (a fd index) */
void rgui_dbus_put_int64(rgui_dbus_buf* buf, int64_t value);
void rgui_dbus_put_uint64(rgui_dbus_buf* buf, uint64_t value);
void rgui_dbus_put_double(rgui_dbus_buf* buf, double value);
/* 's' and 'o': a uint32 length, the bytes and a NUL. */
void rgui_dbus_put_string(rgui_dbus_buf* buf, const char* value);
void rgui_dbus_put_string_n(rgui_dbus_buf* buf, const char* value, size_t length);
/* 'g', and the signature that starts a variant: a byte length, the bytes and a NUL. */
void rgui_dbus_put_signature(rgui_dbus_buf* buf, const char* value);
/* 'a': `element` is the first character of the element type, which decides its alignment. */
rgui_dbus_array rgui_dbus_open_array(rgui_dbus_buf* buf, char element);
void rgui_dbus_close_array(rgui_dbus_buf* buf, rgui_dbus_array array);
/* '(' and '{': aligns to 8; the fields follow. */
void rgui_dbus_open_struct(rgui_dbus_buf* buf);

/* ---- Unmarshalling -------------------------------------------------------------------------
 * An iterator over values of a signature, checking every length, alignment, padding byte,
 * string (UTF-8, no NUL inside, NUL-terminated), object path, signature, boolean and nesting
 * limit. A violation makes the iterator `bad`; every later read fails. */
typedef struct rgui_dbus_iter {
    const uint8_t* base; /* alignment origin: the message, or its body */
    size_t pos;
    size_t end;
    const char* sig; /* the types still to read at this level */
    const char* sig_end;
    bool array; /* sig is the element type of an array, read until pos == end */
    bool swap;  /* the message is big-endian */
    bool bad;
    int depth;
} rgui_dbus_iter;

/* Starts an iterator over `size` bytes at `base` holding the values of `signature`
 * (NUL-terminated). */
void rgui_dbus_iter_init(rgui_dbus_iter* it, const uint8_t* base, size_t size, const char* signature,
                         bool big_endian);
/* The type code of the next value, 0 at the end of this level or when the iterator is bad. */
char rgui_dbus_iter_type(const rgui_dbus_iter* it);
bool rgui_dbus_get_byte(rgui_dbus_iter* it, uint8_t* value);
bool rgui_dbus_get_bool(rgui_dbus_iter* it, bool* value);
bool rgui_dbus_get_int16(rgui_dbus_iter* it, int16_t* value);
bool rgui_dbus_get_uint16(rgui_dbus_iter* it, uint16_t* value);
bool rgui_dbus_get_int32(rgui_dbus_iter* it, int32_t* value);
bool rgui_dbus_get_uint32(rgui_dbus_iter* it, uint32_t* value); /* 'u' or 'h' */
bool rgui_dbus_get_int64(rgui_dbus_iter* it, int64_t* value);
bool rgui_dbus_get_uint64(rgui_dbus_iter* it, uint64_t* value);
bool rgui_dbus_get_double(rgui_dbus_iter* it, double* value);
/* 's', 'o' or 'g': points into the message; NUL-terminated; `length` may be NULL. */
bool rgui_dbus_get_string(rgui_dbus_iter* it, const char** value, uint32_t* length);
/* Enters the array, struct, dict entry or variant at the iterator. */
bool rgui_dbus_recurse(rgui_dbus_iter* it, rgui_dbus_iter* sub);
/* Returns from `sub` to its parent `it`, skipping (and checking) what `sub` did not read. */
bool rgui_dbus_leave(rgui_dbus_iter* it, rgui_dbus_iter* sub);
/* Skips one complete value, checking it. */
bool rgui_dbus_skip(rgui_dbus_iter* it);

/* True when `sig` (length bytes) is a valid signature: complete types only, within the length
 * and nesting limits. With `single`, exactly one complete type (a variant's signature). */
bool rgui_dbus_signature_valid(const char* sig, size_t length, bool single);
/* True when `path` is a valid object path. */
bool rgui_dbus_object_path_valid(const char* path, size_t length);

/* ---- Messages ------------------------------------------------------------------------------ */
typedef struct rgui_dbus_message {
    uint8_t type;  /* rgui_dbus_message_type */
    uint8_t flags; /* rgui_dbus_flags */
    bool big_endian;
    uint32_t serial;
    uint32_t reply_serial; /* 0 when absent */
    uint32_t unix_fds;
    /* NULL when absent; signature is "" when absent. */
    const char* path;
    const char* interface;
    const char* member;
    const char* error_name;
    const char* destination;
    const char* sender;
    const char* signature;
    const uint8_t* body;
    uint32_t body_size;
} rgui_dbus_message;

/* The total size of the message that starts with these 16 bytes, or 0 when they are not the
 * start of a valid message (endianness, version, type, or sizes over the limits). */
size_t rgui_dbus_message_size(const uint8_t head[16]);
/* Parses and checks a complete message of exactly `size` bytes: the header fields with their
 * types, the fields each message type requires, and the whole body against its signature.
 * The strings and body of `out` point into `data`. False when anything is invalid. */
bool rgui_dbus_parse_message(const uint8_t* data, size_t size, rgui_dbus_message* out);
/* An iterator over a parsed message's body. */
void rgui_dbus_body_iter(const rgui_dbus_message* message, rgui_dbus_iter* it);
/* Appends `message` (little-endian) to `out`: the header from its fields, then its body. */
bool rgui_dbus_encode_message(rgui_dbus_buf* out, const rgui_dbus_message* message);

/* ---- Addresses ----------------------------------------------------------------------------- */
struct sockaddr_un;
/* The socket of the first `unix:path=` or `unix:abstract=` entry of a D-Bus address (values
 * percent-decoded). `length` receives the socklen. False when there is none or it is too long. */
bool rgui_dbus_parse_address(const char* address, struct sockaddr_un* out, unsigned* length);
/* The session bus address: $DBUS_SESSION_BUS_ADDRESS, else unix:path=$XDG_RUNTIME_DIR/bus.
 * Writes into `out` (NUL-terminated); false when neither is available or it does not fit. */
bool rgui_dbus_session_address(char* out, size_t capacity);

/* ---- Connection ---------------------------------------------------------------------------- */
typedef struct rgui_dbus_conn rgui_dbus_conn;

/* A reply (method return or error) to a call; NULL when the call timed out or the connection
 * closed. */
typedef void (*rgui_dbus_reply_fn)(void* user, const rgui_dbus_message* reply);
typedef void (*rgui_dbus_signal_fn)(void* user, const rgui_dbus_message* signal);
/* An incoming method call; returns true when it replied (or the call wants no reply). Calls it
 * does not handle get org.freedesktop.DBus.Error.UnknownMethod. */
typedef bool (*rgui_dbus_method_fn)(void* user, rgui_dbus_conn* conn, const rgui_dbus_message* call);

/* What a signal match rule tests (type='signal' always); NULL fields match anything. Values
 * must not contain ' or \.
 *
 * `sender` goes into the rule, so the bus delivers only broadcasts from that sender. Signals
 * sent to us by name (unicast) skip match rules, so routing compares the message's sender too
 * when `sender` is a unique name (":1.42") or the bus itself ("org.freedesktop.DBus"); for a
 * well-known name the callback must compare the sender with the name's current owner.
 * `arg0` matches a signal whose first argument is that string. */
typedef struct rgui_dbus_match {
    const char* interface;
    const char* member;
    const char* path;
    const char* sender;
    const char* arg0;
} rgui_dbus_match;

/* Starts connecting without blocking: the socket's connect, the EXTERNAL authentication and
 * Hello go on in rgui_dbus_dispatch, and must end within `timeout_ms`, else the connection
 * closes (pending calls then get their NULL reply). Calls, sends and matches may be made at
 * once; they are queued after Hello. NULL when the address is unusable or the socket cannot be
 * created or connected (no such socket file). */
rgui_dbus_conn* rgui_dbus_open_async(const char* address, int timeout_ms);
rgui_dbus_conn* rgui_dbus_open_session_async(int timeout_ms);
/* The same, waiting for the handshake: at most `timeout_ms` in total. NULL on any failure. */
rgui_dbus_conn* rgui_dbus_open(const char* address, int timeout_ms);
rgui_dbus_conn* rgui_dbus_open_session(int timeout_ms);
/* True once the bus answered Hello: rgui_dbus_unique_name is known. */
bool rgui_dbus_ready(const rgui_dbus_conn* conn);
/* Waits (polling and dispatching) until the connection is ready or closed, or `timeout_ms`
 * passed; never from a callback. True when ready. */
bool rgui_dbus_wait_ready(rgui_dbus_conn* conn, int timeout_ms);
/* Closes the socket and frees everything; no callback runs. */
void rgui_dbus_close(rgui_dbus_conn* conn);

/* The unique name the bus gave us (":1.42"); "" before the connection is ready. */
const char* rgui_dbus_unique_name(const rgui_dbus_conn* conn);
bool rgui_dbus_connected(const rgui_dbus_conn* conn);
int rgui_dbus_fd(const rgui_dbus_conn* conn);
/* POLLIN, plus POLLOUT while output waits for the socket (POLLOUT alone while connecting). */
short rgui_dbus_poll_events(const rgui_dbus_conn* conn);
/* The bytes the input buffer holds now (diagnostics and tests): it shrinks back to one read
 * chunk once drained. */
size_t rgui_dbus_input_capacity(const rgui_dbus_conn* conn);
/* Milliseconds until the next call times out, -1 when none is pending. */
int rgui_dbus_timeout_ms(const rgui_dbus_conn* conn);
/* Reads what is available, routes every complete message, writes queued output and expires
 * calls whose time is up. A message's header is checked first: only a reply to a pending call,
 * a signal some match wants, or a method call has its body validated and routed. False once the connection is closed (the peer hung up, a read or
 * write failed, or the stream is not D-Bus); pending calls then got their NULL reply. */
bool rgui_dbus_dispatch(rgui_dbus_conn* conn);
/* Polls at most `timeout_ms` (-1: until the next call timeout), then dispatches. For start-up
 * and tests; never from a callback. */
bool rgui_dbus_wait(rgui_dbus_conn* conn, int timeout_ms);

/* Sends a message, giving it the next serial (returned; 0 on failure). */
uint32_t rgui_dbus_send(rgui_dbus_conn* conn, const rgui_dbus_message* message);
/* Sends a method call and routes its reply to `callback` (which may be NULL: the reply is
 * dropped). Returns the call's serial, 0 when it could not be sent (no callback then runs). */
uint32_t rgui_dbus_call(rgui_dbus_conn* conn, const rgui_dbus_message* call, int timeout_ms,
                        rgui_dbus_reply_fn callback, void* user);
/* Forgets a pending call: its callback never runs. */
void rgui_dbus_cancel(rgui_dbus_conn* conn, uint32_t serial);
/* Replies to `call` with a body (signature "" and body NULL for none). */
bool rgui_dbus_reply(rgui_dbus_conn* conn, const rgui_dbus_message* call, const char* signature,
                     const uint8_t* body, uint32_t body_size);
bool rgui_dbus_reply_error(rgui_dbus_conn* conn, const rgui_dbus_message* call, const char* name,
                           const char* text);
/* Asks the bus for the signals of `match` (AddMatch) and routes the matching ones to
 * `callback`. Returns an id for rgui_dbus_remove_match, 0 on failure. */
uint32_t rgui_dbus_add_match(rgui_dbus_conn* conn, const rgui_dbus_match* match,
                             rgui_dbus_signal_fn callback, void* user);
void rgui_dbus_remove_match(rgui_dbus_conn* conn, uint32_t id);
void rgui_dbus_set_method_handler(rgui_dbus_conn* conn, rgui_dbus_method_fn handler, void* user);

#ifdef __cplusplus
}
#endif
