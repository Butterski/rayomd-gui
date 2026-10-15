/* SPDX-License-Identifier: Apache-2.0 */
/* The D-Bus client (see dbus_wire.h). */
#include "platform/linux/dbus_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* The most a queue (bytes not yet sent, or received and not yet routed) may hold. */
#define RGUI_DBUS_MAX_QUEUE ((size_t)2 * RGUI_DBUS_MAX_MESSAGE)
#define RGUI_DBUS_READ_CHUNK ((size_t)65536)

/* ---- Marshalling ----------------------------------------------------------------------------- */

void rgui_dbus_buf_free(rgui_dbus_buf* buf) {
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
    buf->failed = false;
}

static bool buf_reserve(rgui_dbus_buf* buf, size_t extra) {
    if (buf->failed) return false;
    if (extra > RGUI_DBUS_MAX_QUEUE || buf->size > RGUI_DBUS_MAX_QUEUE - extra) {
        buf->failed = true;
        return false;
    }
    size_t need = buf->size + extra;
    if (need <= buf->capacity) return true;
    size_t capacity = buf->capacity ? buf->capacity : 256;
    while (capacity < need) capacity *= 2;
    uint8_t* data = (uint8_t*)realloc(buf->data, capacity);
    if (!data) {
        buf->failed = true;
        return false;
    }
    buf->data = data;
    buf->capacity = capacity;
    return true;
}

static void buf_append(rgui_dbus_buf* buf, const void* bytes, size_t size) {
    if (!buf_reserve(buf, size)) return;
    if (size) memcpy(buf->data + buf->size, bytes, size);
    buf->size += size;
}

static void buf_align(rgui_dbus_buf* buf, size_t alignment) {
    size_t pad = (alignment - buf->size % alignment) % alignment;
    if (pad == 0 || !buf_reserve(buf, pad)) return;
    memset(buf->data + buf->size, 0, pad);
    buf->size += pad;
}

static void put_le(rgui_dbus_buf* buf, uint64_t value, size_t size) {
    uint8_t bytes[8];
    for (size_t i = 0; i < size; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
    buf_align(buf, size);
    buf_append(buf, bytes, size);
}

void rgui_dbus_put_byte(rgui_dbus_buf* buf, uint8_t value) { buf_append(buf, &value, 1); }
void rgui_dbus_put_bool(rgui_dbus_buf* buf, bool value) { put_le(buf, value ? 1 : 0, 4); }
void rgui_dbus_put_int16(rgui_dbus_buf* buf, int16_t value) { put_le(buf, (uint16_t)value, 2); }
void rgui_dbus_put_uint16(rgui_dbus_buf* buf, uint16_t value) { put_le(buf, value, 2); }
void rgui_dbus_put_int32(rgui_dbus_buf* buf, int32_t value) { put_le(buf, (uint32_t)value, 4); }
void rgui_dbus_put_uint32(rgui_dbus_buf* buf, uint32_t value) { put_le(buf, value, 4); }
void rgui_dbus_put_int64(rgui_dbus_buf* buf, int64_t value) { put_le(buf, (uint64_t)value, 8); }
void rgui_dbus_put_uint64(rgui_dbus_buf* buf, uint64_t value) { put_le(buf, value, 8); }

void rgui_dbus_put_double(rgui_dbus_buf* buf, double value) {
    uint64_t bits;
    memcpy(&bits, &value, 8);
    put_le(buf, bits, 8);
}

/* Strict UTF-8 without NUL: no overlong forms, surrogates or code points over U+10FFFF. */
static bool utf8_valid(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            if (c == 0) return false;
            ++i;
            continue;
        }
        size_t length;
        uint32_t cp;
        uint32_t min;
        if ((c & 0xE0) == 0xC0) {
            length = 2, cp = c & 0x1Fu, min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3, cp = c & 0x0Fu, min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4, cp = c & 0x07u, min = 0x10000;
        } else {
            return false;
        }
        if (length > n - i) return false;
        for (size_t k = 1; k < length; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += length;
    }
    return true;
}

void rgui_dbus_put_string_n(rgui_dbus_buf* buf, const char* value, size_t length) {
    if (length > RGUI_DBUS_MAX_MESSAGE || !utf8_valid((const uint8_t*)value, length)) {
        buf->failed = true;
        return;
    }
    put_le(buf, (uint32_t)length, 4);
    buf_append(buf, value, length);
    rgui_dbus_put_byte(buf, 0);
}

void rgui_dbus_put_string(rgui_dbus_buf* buf, const char* value) {
    rgui_dbus_put_string_n(buf, value, strlen(value));
}

void rgui_dbus_put_signature(rgui_dbus_buf* buf, const char* value) {
    size_t length = strlen(value);
    if (!rgui_dbus_signature_valid(value, length, false)) {
        buf->failed = true;
        return;
    }
    rgui_dbus_put_byte(buf, (uint8_t)length);
    buf_append(buf, value, length + 1);
}

static size_t align_of(char type) {
    switch (type) {
    case 'y': case 'g': case 'v': return 1;
    case 'n': case 'q': return 2;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    default: return 4;
    }
}

rgui_dbus_array rgui_dbus_open_array(rgui_dbus_buf* buf, char element) {
    rgui_dbus_array array;
    put_le(buf, 0, 4);
    array.length_at = buf->size - 4;
    buf_align(buf, align_of(element));
    array.start = buf->size;
    return array;
}

void rgui_dbus_close_array(rgui_dbus_buf* buf, rgui_dbus_array array) {
    if (buf->failed) return;
    size_t length = buf->size - array.start;
    if (length > RGUI_DBUS_MAX_ARRAY) {
        buf->failed = true;
        return;
    }
    for (int i = 0; i < 4; ++i) buf->data[array.length_at + (size_t)i] = (uint8_t)(length >> (8 * i));
}

void rgui_dbus_open_struct(rgui_dbus_buf* buf) { buf_align(buf, 8); }

/* ---- Signatures and object paths --------------------------------------------------------- */

static bool is_basic(char c) { return c && strchr("ybnqiuxtdsogh", c) != NULL; }

/* Parses one complete type at *p; false when invalid. */
static bool parse_type(const char** p, const char* end, int arrays, int structs) {
    if (*p >= end) return false;
    char c = **p;
    ++*p;
    if (is_basic(c) || c == 'v') return true;
    if (c == 'a') {
        if (arrays + 1 > RGUI_DBUS_MAX_NESTING) return false;
        if (*p < end && **p == '{') {
            ++*p;
            if (structs + 1 > RGUI_DBUS_MAX_NESTING) return false;
            if (*p >= end || !is_basic(**p)) return false;
            ++*p;
            if (!parse_type(p, end, arrays + 1, structs + 1)) return false;
            if (*p >= end || **p != '}') return false;
            ++*p;
            return true;
        }
        return parse_type(p, end, arrays + 1, structs);
    }
    if (c == '(') {
        if (structs + 1 > RGUI_DBUS_MAX_NESTING) return false;
        if (*p < end && **p == ')') return false;
        while (*p < end && **p != ')') {
            if (!parse_type(p, end, arrays, structs + 1)) return false;
        }
        if (*p >= end) return false;
        ++*p;
        return true;
    }
    return false;
}

bool rgui_dbus_signature_valid(const char* sig, size_t length, bool single) {
    if (length > RGUI_DBUS_MAX_SIGNATURE) return false;
    if (memchr(sig, 0, length)) return false;
    const char* p = sig;
    const char* end = sig + length;
    if (single) return parse_type(&p, end, 0, 0) && p == end;
    while (p < end) {
        if (!parse_type(&p, end, 0, 0)) return false;
    }
    return true;
}

bool rgui_dbus_object_path_valid(const char* path, size_t length) {
    if (length == 0 || path[0] != '/') return false;
    if (length == 1) return true;
    if (path[length - 1] == '/') return false;
    for (size_t i = 1; i < length; ++i) {
        char c = path[i];
        if (c == '/') {
            if (path[i - 1] == '/') return false;
        } else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/* The length of the complete type at s (a valid signature). */
static size_t type_length(const char* s, const char* end) {
    const char* p = s;
    while (p < end && *p == 'a') ++p;
    if (p >= end) return 0;
    if (*p == '(' || *p == '{') {
        int depth = 0;
        for (; p < end; ++p) {
            if (*p == '(' || *p == '{') {
                ++depth;
            } else if (*p == ')' || *p == '}') {
                if (--depth == 0) return (size_t)(p - s) + 1;
            }
        }
        return 0;
    }
    return (size_t)(p - s) + 1;
}

/* ---- Unmarshalling --------------------------------------------------------------------------- */

static bool it_fail(rgui_dbus_iter* it) {
    it->bad = true;
    return false;
}

static void iter_setup(rgui_dbus_iter* it, const uint8_t* base, size_t pos, size_t end, const char* sig,
                       const char* sig_end, bool swap) {
    it->base = base;
    it->pos = pos;
    it->end = end;
    it->sig = sig;
    it->sig_end = sig_end;
    it->array = false;
    it->swap = swap;
    it->bad = false;
    it->depth = 0;
}

void rgui_dbus_iter_init(rgui_dbus_iter* it, const uint8_t* base, size_t size, const char* signature,
                         bool big_endian) {
    size_t length = strlen(signature);
    iter_setup(it, base, 0, size, signature, signature + length, big_endian);
    if (!rgui_dbus_signature_valid(signature, length, false)) it->bad = true;
}

char rgui_dbus_iter_type(const rgui_dbus_iter* it) {
    if (it->bad) return 0;
    if (it->array) return it->pos < it->end ? it->sig[0] : 0;
    return it->sig < it->sig_end ? it->sig[0] : 0;
}

/* Skips the padding to `alignment`, which must be zero bytes inside the level. */
static bool it_align(rgui_dbus_iter* it, size_t alignment) {
    size_t pad = (alignment - it->pos % alignment) % alignment;
    if (pad > it->end - it->pos) return it_fail(it);
    for (size_t i = 0; i < pad; ++i) {
        if (it->base[it->pos + i] != 0) return it_fail(it);
    }
    it->pos += pad;
    return true;
}

static void it_advance_sig(rgui_dbus_iter* it) {
    if (!it->array) it->sig += type_length(it->sig, it->sig_end);
}

static uint64_t load(const rgui_dbus_iter* it, size_t size) {
    uint64_t value = 0;
    const uint8_t* p = it->base + it->pos;
    for (size_t i = 0; i < size; ++i) {
        size_t index = it->swap ? size - 1 - i : i;
        value |= (uint64_t)p[index] << (8 * i);
    }
    return value;
}

/* Reads a fixed-size value of one of the `types`. */
static bool get_fixed(rgui_dbus_iter* it, const char* types, size_t size, uint64_t* value) {
    char type = rgui_dbus_iter_type(it);
    if (!type || !strchr(types, type)) return it_fail(it);
    if (!it_align(it, size)) return false;
    if (size > it->end - it->pos) return it_fail(it);
    *value = load(it, size);
    it->pos += size;
    it_advance_sig(it);
    return true;
}

bool rgui_dbus_get_byte(rgui_dbus_iter* it, uint8_t* value) {
    uint64_t v;
    if (!get_fixed(it, "y", 1, &v)) return false;
    *value = (uint8_t)v;
    return true;
}

bool rgui_dbus_get_bool(rgui_dbus_iter* it, bool* value) {
    uint64_t v;
    if (!get_fixed(it, "b", 4, &v)) return false;
    if (v > 1) return it_fail(it);
    *value = v != 0;
    return true;
}

bool rgui_dbus_get_int16(rgui_dbus_iter* it, int16_t* value) {
    uint64_t v;
    if (!get_fixed(it, "n", 2, &v)) return false;
    *value = (int16_t)(uint16_t)v;
    return true;
}

bool rgui_dbus_get_uint16(rgui_dbus_iter* it, uint16_t* value) {
    uint64_t v;
    if (!get_fixed(it, "q", 2, &v)) return false;
    *value = (uint16_t)v;
    return true;
}

bool rgui_dbus_get_int32(rgui_dbus_iter* it, int32_t* value) {
    uint64_t v;
    if (!get_fixed(it, "i", 4, &v)) return false;
    *value = (int32_t)(uint32_t)v;
    return true;
}

bool rgui_dbus_get_uint32(rgui_dbus_iter* it, uint32_t* value) {
    uint64_t v;
    if (!get_fixed(it, "uh", 4, &v)) return false;
    *value = (uint32_t)v;
    return true;
}

bool rgui_dbus_get_int64(rgui_dbus_iter* it, int64_t* value) {
    uint64_t v;
    if (!get_fixed(it, "x", 8, &v)) return false;
    *value = (int64_t)v;
    return true;
}

bool rgui_dbus_get_uint64(rgui_dbus_iter* it, uint64_t* value) {
    uint64_t v;
    if (!get_fixed(it, "t", 8, &v)) return false;
    *value = v;
    return true;
}

bool rgui_dbus_get_double(rgui_dbus_iter* it, double* value) {
    uint64_t v;
    if (!get_fixed(it, "d", 8, &v)) return false;
    memcpy(value, &v, 8);
    return true;
}

/* Reads a signature's bytes (byte length, bytes, NUL) without consuming the iterator's type. */
static bool read_signature(rgui_dbus_iter* it, const char** value, size_t* length, bool single) {
    if (it->pos >= it->end) return it_fail(it);
    size_t n = it->base[it->pos];
    if (n + 2 > it->end - it->pos) return it_fail(it);
    const char* s = (const char*)it->base + it->pos + 1;
    if (s[n] != 0 || !rgui_dbus_signature_valid(s, n, single)) return it_fail(it);
    it->pos += n + 2;
    *value = s;
    *length = n;
    return true;
}

bool rgui_dbus_get_string(rgui_dbus_iter* it, const char** value, uint32_t* length) {
    char type = rgui_dbus_iter_type(it);
    if (type == 'g') {
        size_t n;
        if (!read_signature(it, value, &n, false)) return false;
        if (length) *length = (uint32_t)n;
        it_advance_sig(it);
        return true;
    }
    if (type != 's' && type != 'o') return it_fail(it);
    if (!it_align(it, 4)) return false;
    if (4 > it->end - it->pos) return it_fail(it);
    uint64_t n = load(it, 4);
    it->pos += 4;
    if (n >= it->end - it->pos) return it_fail(it); /* the bytes and the NUL */
    const uint8_t* s = it->base + it->pos;
    if (s[n] != 0 || !utf8_valid(s, (size_t)n)) return it_fail(it);
    if (type == 'o' && !rgui_dbus_object_path_valid((const char*)s, (size_t)n)) return it_fail(it);
    it->pos += (size_t)n + 1;
    *value = (const char*)s;
    if (length) *length = (uint32_t)n;
    it_advance_sig(it);
    return true;
}

bool rgui_dbus_recurse(rgui_dbus_iter* it, rgui_dbus_iter* sub) {
    char type = rgui_dbus_iter_type(it);
    if (it->depth + 1 > RGUI_DBUS_MAX_TOTAL_NESTING) return it_fail(it);
    if (type == 'a') {
        if (!it_align(it, 4)) return false;
        if (4 > it->end - it->pos) return it_fail(it);
        uint64_t length = load(it, 4);
        if (length > RGUI_DBUS_MAX_ARRAY) return it_fail(it);
        rgui_dbus_iter probe = *it;
        probe.pos += 4;
        const char* element = it->sig + 1;
        size_t element_length = type_length(element, it->sig_end);
        if (!it_align(&probe, align_of(element[0]))) return it_fail(it);
        if (length > probe.end - probe.pos) return it_fail(it);
        iter_setup(sub, it->base, probe.pos, probe.pos + (size_t)length, element, element + element_length, it->swap);
        sub->array = true;
    } else if (type == '(' || type == '{') {
        if (!it_align(it, 8)) return false;
        size_t length = type_length(it->sig, it->sig_end);
        iter_setup(sub, it->base, it->pos, it->end, it->sig + 1, it->sig + length - 1, it->swap);
    } else if (type == 'v') {
        size_t pos = it->pos;
        const char* sig;
        size_t length;
        if (!read_signature(it, &sig, &length, true)) return false;
        iter_setup(sub, it->base, it->pos, it->end, sig, sig + length, it->swap);
        it->pos = pos; /* the parent moves on at rgui_dbus_leave */
    } else {
        return it_fail(it);
    }
    sub->depth = it->depth + 1;
    return true;
}

bool rgui_dbus_leave(rgui_dbus_iter* it, rgui_dbus_iter* sub) {
    if (it->bad) return false;
    while (!sub->bad && rgui_dbus_iter_type(sub)) rgui_dbus_skip(sub);
    if (sub->bad) return it_fail(it);
    if (sub->array && sub->pos != sub->end) return it_fail(it);
    it->pos = sub->array ? sub->end : sub->pos;
    it_advance_sig(it);
    return true;
}

bool rgui_dbus_skip(rgui_dbus_iter* it) {
    char type = rgui_dbus_iter_type(it);
    uint64_t v;
    const char* s;
    switch (type) {
    case 0: return it_fail(it);
    case 'y': return get_fixed(it, "y", 1, &v);
    case 'b': {
        bool b;
        return rgui_dbus_get_bool(it, &b);
    }
    case 'n': case 'q': return get_fixed(it, "nq", 2, &v);
    case 'i': case 'u': case 'h': return get_fixed(it, "iuh", 4, &v);
    case 'x': case 't': case 'd': return get_fixed(it, "xtd", 8, &v);
    case 's': case 'o': case 'g': return rgui_dbus_get_string(it, &s, NULL);
    default: {
        rgui_dbus_iter sub;
        if (!rgui_dbus_recurse(it, &sub)) return false;
        return rgui_dbus_leave(it, &sub);
    }
    }
}

/* ---- Messages -------------------------------------------------------------------------------- */

enum {
    FIELD_PATH = 1,
    FIELD_INTERFACE = 2,
    FIELD_MEMBER = 3,
    FIELD_ERROR_NAME = 4,
    FIELD_REPLY_SERIAL = 5,
    FIELD_DESTINATION = 6,
    FIELD_SENDER = 7,
    FIELD_SIGNATURE = 8,
    FIELD_UNIX_FDS = 9,
};

static uint32_t read_u32(const uint8_t* p, bool big_endian) {
    if (big_endian) return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

size_t rgui_dbus_message_size(const uint8_t head[16]) {
    bool big_endian;
    if (head[0] == 'l') {
        big_endian = false;
    } else if (head[0] == 'B') {
        big_endian = true;
    } else {
        return 0;
    }
    if (head[1] == 0 || head[3] != 1) return 0;
    uint64_t body = read_u32(head + 4, big_endian);
    uint64_t fields = read_u32(head + 12, big_endian);
    if (fields > RGUI_DBUS_MAX_ARRAY) return 0;
    uint64_t header = (16 + fields + 7) & ~(uint64_t)7;
    uint64_t total = header + body;
    if (total > RGUI_DBUS_MAX_MESSAGE) return 0;
    return (size_t)total;
}

/* The header of a complete message of exactly `size` bytes; the body is not looked at. */
static bool parse_header(const uint8_t* data, size_t size, rgui_dbus_message* out) {
    if (size < 16 || rgui_dbus_message_size(data) != size) return false;
    memset(out, 0, sizeof *out);
    out->big_endian = data[0] == 'B';
    out->type = data[1];
    out->flags = data[2];
    out->body_size = read_u32(data + 4, out->big_endian);
    out->serial = read_u32(data + 8, out->big_endian);
    if (out->type < RGUI_DBUS_METHOD_CALL || out->type > RGUI_DBUS_SIGNAL || out->serial == 0) return false;
    size_t header_end = size - out->body_size;

    static const char kFieldsSig[] = "a(yv)";
    rgui_dbus_iter it;
    iter_setup(&it, data, 12, header_end, kFieldsSig, kFieldsSig + 5, out->big_endian);
    rgui_dbus_iter fields;
    if (!rgui_dbus_recurse(&it, &fields)) return false;
    unsigned seen = 0;
    while (rgui_dbus_iter_type(&fields)) {
        rgui_dbus_iter field;
        uint8_t code;
        if (!rgui_dbus_recurse(&fields, &field) || !rgui_dbus_get_byte(&field, &code)) return false;
        if (code >= FIELD_PATH && code <= FIELD_UNIX_FDS) {
            static const char kTypes[] = "?osssussgu"; /* by field code 1-9 */
            if (seen & (1u << code)) return false;
            seen |= 1u << code;
            rgui_dbus_iter value;
            if (!rgui_dbus_recurse(&field, &value)) return false;
            if (value.sig_end - value.sig != 1 || value.sig[0] != kTypes[code]) return false;
            const char* s = NULL;
            uint32_t u = 0;
            bool ok = kTypes[code] == 'u' ? rgui_dbus_get_uint32(&value, &u) : rgui_dbus_get_string(&value, &s, NULL);
            if (!ok || !rgui_dbus_leave(&field, &value)) return false;
            switch (code) {
            case FIELD_PATH: out->path = s; break;
            case FIELD_INTERFACE: out->interface = s; break;
            case FIELD_MEMBER: out->member = s; break;
            case FIELD_ERROR_NAME: out->error_name = s; break;
            case FIELD_REPLY_SERIAL: out->reply_serial = u; break;
            case FIELD_DESTINATION: out->destination = s; break;
            case FIELD_SENDER: out->sender = s; break;
            case FIELD_SIGNATURE: out->signature = s; break;
            default: out->unix_fds = u; break;
            }
        }
        if (!rgui_dbus_leave(&fields, &field)) return false;
    }
    if (!rgui_dbus_leave(&it, &fields)) return false;
    /* The padding up to the body. */
    if (!it_align(&it, 8) || it.pos != header_end) return false;

    /* We never negotiate file descriptor passing. */
    if (out->unix_fds != 0) return false;
    switch (out->type) {
    case RGUI_DBUS_METHOD_CALL:
        if (!out->path || !out->member) return false;
        break;
    case RGUI_DBUS_METHOD_RETURN:
        if (!(seen & (1u << FIELD_REPLY_SERIAL)) || out->reply_serial == 0) return false;
        break;
    case RGUI_DBUS_ERROR:
        if (!out->error_name || !(seen & (1u << FIELD_REPLY_SERIAL)) || out->reply_serial == 0) return false;
        break;
    default:
        if (!out->path || !out->interface || !out->member) return false;
        break;
    }
    if (!out->signature) out->signature = "";
    out->body = data + header_end;
    return true;
}

/* Every value of the body against its signature. */
static bool body_valid(const rgui_dbus_message* m) {
    rgui_dbus_iter body;
    rgui_dbus_body_iter(m, &body);
    while (rgui_dbus_iter_type(&body)) {
        if (!rgui_dbus_skip(&body)) return false;
    }
    return !body.bad && body.pos == body.end;
}

bool rgui_dbus_parse_message(const uint8_t* data, size_t size, rgui_dbus_message* out) {
    return parse_header(data, size, out) && body_valid(out);
}

void rgui_dbus_body_iter(const rgui_dbus_message* message, rgui_dbus_iter* it) {
    rgui_dbus_iter_init(it, message->body, message->body_size, message->signature, message->big_endian);
}

static void put_field_string(rgui_dbus_buf* buf, uint8_t code, const char* type, const char* value) {
    if (!value) return;
    rgui_dbus_open_struct(buf);
    rgui_dbus_put_byte(buf, code);
    rgui_dbus_put_signature(buf, type);
    if (type[0] == 'g') {
        rgui_dbus_put_signature(buf, value);
    } else {
        rgui_dbus_put_string(buf, value);
    }
}

bool rgui_dbus_encode_message(rgui_dbus_buf* out, const rgui_dbus_message* m) {
    if (m->serial == 0 || m->type < RGUI_DBUS_METHOD_CALL || m->type > RGUI_DBUS_SIGNAL) return false;
    if (m->path && !rgui_dbus_object_path_valid(m->path, strlen(m->path))) return false;
    rgui_dbus_buf buf = {0};
    rgui_dbus_put_byte(&buf, 'l');
    rgui_dbus_put_byte(&buf, m->type);
    rgui_dbus_put_byte(&buf, m->flags);
    rgui_dbus_put_byte(&buf, 1);
    rgui_dbus_put_uint32(&buf, m->body_size);
    rgui_dbus_put_uint32(&buf, m->serial);
    rgui_dbus_array fields = rgui_dbus_open_array(&buf, '(');
    put_field_string(&buf, FIELD_PATH, "o", m->path);
    put_field_string(&buf, FIELD_INTERFACE, "s", m->interface);
    put_field_string(&buf, FIELD_MEMBER, "s", m->member);
    put_field_string(&buf, FIELD_ERROR_NAME, "s", m->error_name);
    if (m->reply_serial) {
        rgui_dbus_open_struct(&buf);
        rgui_dbus_put_byte(&buf, FIELD_REPLY_SERIAL);
        rgui_dbus_put_signature(&buf, "u");
        rgui_dbus_put_uint32(&buf, m->reply_serial);
    }
    put_field_string(&buf, FIELD_DESTINATION, "s", m->destination);
    put_field_string(&buf, FIELD_SENDER, "s", m->sender);
    if (m->signature && m->signature[0]) put_field_string(&buf, FIELD_SIGNATURE, "g", m->signature);
    rgui_dbus_close_array(&buf, fields);
    buf_align(&buf, 8);
    buf_append(&buf, m->body, m->body_size);
    bool ok = !buf.failed && buf.size <= RGUI_DBUS_MAX_MESSAGE;
    if (ok) {
        buf_append(out, buf.data, buf.size);
        ok = !out->failed;
    }
    rgui_dbus_buf_free(&buf);
    return ok;
}

/* ---- Addresses --------------------------------------------------------------------------------- */

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool rgui_dbus_parse_address(const char* address, struct sockaddr_un* out, unsigned* length) {
    const char* entry = address;
    while (*entry) {
        const char* entry_end = strchr(entry, ';');
        if (!entry_end) entry_end = entry + strlen(entry);
        if ((size_t)(entry_end - entry) > 5 && memcmp(entry, "unix:", 5) == 0) {
            const char* kv = entry + 5;
            while (kv < entry_end) {
                const char* kv_end = memchr(kv, ',', (size_t)(entry_end - kv));
                if (!kv_end) kv_end = entry_end;
                const char* eq = memchr(kv, '=', (size_t)(kv_end - kv));
                bool abstract = false;
                bool match = false;
                if (eq && eq - kv == 4 && memcmp(kv, "path", 4) == 0) {
                    match = true;
                } else if (eq && eq - kv == 8 && memcmp(kv, "abstract", 8) == 0) {
                    match = abstract = true;
                }
                if (match) {
                    memset(out, 0, sizeof *out);
                    out->sun_family = AF_UNIX;
                    size_t n = abstract ? 1 : 0;
                    for (const char* p = eq + 1; p < kv_end; ++p) {
                        int c = (unsigned char)*p;
                        if (c == '%') {
                            if (kv_end - p < 3 || hex_value(p[1]) < 0 || hex_value(p[2]) < 0) return false;
                            c = hex_value(p[1]) * 16 + hex_value(p[2]);
                            p += 2;
                        }
                        if (c == 0 || n + 1 >= sizeof out->sun_path) return false;
                        out->sun_path[n++] = (char)c;
                    }
                    if (n == (abstract ? 1u : 0u)) return false;
                    *length = (unsigned)(offsetof(struct sockaddr_un, sun_path) + n + (abstract ? 0 : 1));
                    return true;
                }
                kv = kv_end < entry_end ? kv_end + 1 : entry_end;
            }
        }
        entry = *entry_end ? entry_end + 1 : entry_end;
    }
    return false;
}

bool rgui_dbus_session_address(char* out, size_t capacity) {
    const char* env = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (env && *env) {
        size_t n = strlen(env);
        if (n + 1 > capacity) return false;
        memcpy(out, env, n + 1);
        return true;
    }
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') return false;
    static const char kPrefix[] = "unix:path=";
    size_t n = sizeof kPrefix - 1;
    if (n + 1 > capacity) return false;
    memcpy(out, kPrefix, n);
    static const char kHex[] = "0123456789abcdef";
    for (const char* p = runtime;; ++p) {
        const char* s = *p ? NULL : "/bus";
        if (s) {
            size_t m = strlen(s);
            if (n + m + 1 > capacity) return false;
            memcpy(out + n, s, m + 1);
            return true;
        }
        unsigned char c = (unsigned char)*p;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                     strchr("-_/.\\*", c);
        if (n + (plain ? 1 : 3) + 1 > capacity) return false;
        if (plain) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = kHex[c >> 4];
            out[n++] = kHex[c & 15];
        }
    }
}

/* ---- Connection -------------------------------------------------------------------------------- */

typedef struct pending_call {
    uint32_t serial;
    int64_t deadline;
    rgui_dbus_reply_fn callback;
    void* user;
} pending_call;

typedef struct match_entry {
    uint32_t id; /* 0: removed */
    char* interface;
    char* member;
    char* path;
    char* sender;
    char* arg0;
    char* rule;
    rgui_dbus_signal_fn callback;
    void* user;
} match_entry;

enum { CONN_CONNECTING, CONN_AUTH, CONN_OPEN };

struct rgui_dbus_conn {
    int fd;
    int state;    /* CONN_*: the socket's connect, the authentication, then messages */
    bool ready;   /* Hello answered */
    bool dead;
    bool dispatching;
    char auth_line[512];
    size_t auth_length;
    size_t skip; /* bytes still to drop of a message over RGUI_DBUS_MAX_INCOMING */
    uint32_t next_serial;
    uint32_t next_match_id;
    char unique_name[256];
    uint8_t* in;
    size_t in_start;
    size_t in_size;
    size_t in_capacity;
    rgui_dbus_buf out;
    size_t out_start;
    pending_call* pending;
    size_t pending_count;
    size_t pending_capacity;
    match_entry* matches;
    size_t match_count;
    size_t match_capacity;
    rgui_dbus_method_fn method;
    void* method_user;
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Writes queued output until it is all sent or the socket would block. */
static bool conn_flush(rgui_dbus_conn* conn) {
    if (conn->state != CONN_OPEN) return !conn->dead; /* queued until after BEGIN */
    while (!conn->dead && conn->out_start < conn->out.size) {
        ssize_t n = send(conn->fd, conn->out.data + conn->out_start, conn->out.size - conn->out_start, MSG_NOSIGNAL);
        if (n > 0) {
            conn->out_start += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            conn->dead = true;
        }
    }
    if (conn->out_start == conn->out.size) {
        conn->out.size = 0;
        conn->out_start = 0;
    } else if (conn->out_start > conn->out.size / 2) {
        memmove(conn->out.data, conn->out.data + conn->out_start, conn->out.size - conn->out_start);
        conn->out.size -= conn->out_start;
        conn->out_start = 0;
    }
    return !conn->dead;
}

uint32_t rgui_dbus_send(rgui_dbus_conn* conn, const rgui_dbus_message* message) {
    if (!conn || conn->dead) return 0;
    rgui_dbus_message m = *message;
    m.serial = conn->next_serial++;
    if (conn->next_serial == 0) conn->next_serial = 1;
    size_t before = conn->out.size;
    if (!rgui_dbus_encode_message(&conn->out, &m)) {
        conn->out.size = before;
        conn->out.failed = false;
        return 0;
    }
    if (!conn_flush(conn)) return 0;
    return m.serial;
}

uint32_t rgui_dbus_call(rgui_dbus_conn* conn, const rgui_dbus_message* call, int timeout_ms,
                        rgui_dbus_reply_fn callback, void* user) {
    if (!conn || conn->dead) return 0;
    rgui_dbus_message m = *call;
    m.type = RGUI_DBUS_METHOD_CALL;
    if (!callback) m.flags |= RGUI_DBUS_NO_REPLY_EXPECTED;
    if (callback && conn->pending_count == conn->pending_capacity) {
        size_t capacity = conn->pending_capacity ? conn->pending_capacity * 2 : 8;
        pending_call* p = (pending_call*)realloc(conn->pending, capacity * sizeof *p);
        if (!p) return 0;
        conn->pending = p;
        conn->pending_capacity = capacity;
    }
    uint32_t serial = rgui_dbus_send(conn, &m);
    if (serial && callback) {
        pending_call* p = &conn->pending[conn->pending_count++];
        p->serial = serial;
        p->deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 25000);
        p->callback = callback;
        p->user = user;
    }
    return serial;
}

/* Removes the pending call at `index`, returning it. */
static pending_call take_pending(rgui_dbus_conn* conn, size_t index) {
    pending_call p = conn->pending[index];
    conn->pending[index] = conn->pending[--conn->pending_count];
    return p;
}

void rgui_dbus_cancel(rgui_dbus_conn* conn, uint32_t serial) {
    if (!conn) return;
    for (size_t i = 0; i < conn->pending_count; ++i) {
        if (conn->pending[i].serial == serial) {
            take_pending(conn, i);
            return;
        }
    }
}

bool rgui_dbus_reply(rgui_dbus_conn* conn, const rgui_dbus_message* call, const char* signature,
                     const uint8_t* body, uint32_t body_size) {
    if (call->flags & RGUI_DBUS_NO_REPLY_EXPECTED) return true;
    rgui_dbus_message m = {0};
    m.type = RGUI_DBUS_METHOD_RETURN;
    m.reply_serial = call->serial;
    m.destination = call->sender;
    m.signature = signature;
    m.body = body;
    m.body_size = body_size;
    return rgui_dbus_send(conn, &m) != 0;
}

bool rgui_dbus_reply_error(rgui_dbus_conn* conn, const rgui_dbus_message* call, const char* name,
                           const char* text) {
    if (call->flags & RGUI_DBUS_NO_REPLY_EXPECTED) return true;
    rgui_dbus_buf body = {0};
    rgui_dbus_put_string(&body, text);
    rgui_dbus_message m = {0};
    m.type = RGUI_DBUS_ERROR;
    m.reply_serial = call->serial;
    m.destination = call->sender;
    m.error_name = name;
    m.signature = "s";
    m.body = body.data;
    m.body_size = (uint32_t)body.size;
    bool ok = !body.failed && rgui_dbus_send(conn, &m) != 0;
    rgui_dbus_buf_free(&body);
    return ok;
}

static bool bus_call_string(rgui_dbus_conn* conn, const char* member, const char* argument) {
    rgui_dbus_buf body = {0};
    rgui_dbus_put_string(&body, argument);
    rgui_dbus_message m = {0};
    m.destination = "org.freedesktop.DBus";
    m.path = "/org/freedesktop/DBus";
    m.interface = "org.freedesktop.DBus";
    m.member = member;
    m.signature = "s";
    m.body = body.data;
    m.body_size = (uint32_t)body.size;
    bool ok = !body.failed && rgui_dbus_call(conn, &m, 0, NULL, NULL) != 0;
    rgui_dbus_buf_free(&body);
    return ok;
}

static bool rule_value_ok(const char* value) {
    return !value || (strlen(value) < 1024 && !strchr(value, '\'') && !strchr(value, '\\'));
}

static char* dup_or_null(const char* s, bool* ok) {
    if (!s) return NULL;
    char* d = strdup(s);
    if (!d) *ok = false;
    return d;
}

static void free_match(match_entry* m) {
    free(m->interface);
    free(m->member);
    free(m->path);
    free(m->sender);
    free(m->arg0);
    free(m->rule);
    memset(m, 0, sizeof *m);
}

uint32_t rgui_dbus_add_match(rgui_dbus_conn* conn, const rgui_dbus_match* match, rgui_dbus_signal_fn callback,
                             void* user) {
    if (!conn || conn->dead || !callback) return 0;
    if (!rule_value_ok(match->interface) || !rule_value_ok(match->member) || !rule_value_ok(match->path) ||
        !rule_value_ok(match->sender) || !rule_value_ok(match->arg0)) {
        return 0;
    }
    if (conn->match_count == conn->match_capacity) {
        size_t capacity = conn->match_capacity ? conn->match_capacity * 2 : 8;
        match_entry* m = (match_entry*)realloc(conn->matches, capacity * sizeof *m);
        if (!m) return 0;
        conn->matches = m;
        conn->match_capacity = capacity;
    }
    match_entry entry = {0};
    bool ok = true;
    entry.interface = dup_or_null(match->interface, &ok);
    entry.member = dup_or_null(match->member, &ok);
    entry.path = dup_or_null(match->path, &ok);
    entry.sender = dup_or_null(match->sender, &ok);
    entry.arg0 = dup_or_null(match->arg0, &ok);
    size_t size = 5400;
    entry.rule = (char*)malloc(size);
    if (!ok || !entry.rule) {
        free_match(&entry);
        return 0;
    }
    int n = snprintf(entry.rule, size, "type='signal'%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                     match->sender ? ",sender='" : "", match->sender ? match->sender : "",
                     match->sender ? "'" : "", match->interface ? ",interface='" : "",
                     match->interface ? match->interface : "", match->interface ? "'" : "",
                     match->member ? ",member='" : "", match->member ? match->member : "",
                     match->member ? "'" : "", match->path ? ",path='" : "", match->path ? match->path : "",
                     match->path ? "'" : "", match->arg0 ? ",arg0='" : "", match->arg0 ? match->arg0 : "",
                     match->arg0 ? "'" : "");
    if (n < 0 || (size_t)n >= size || !bus_call_string(conn, "AddMatch", entry.rule)) {
        free_match(&entry);
        return 0;
    }
    entry.id = conn->next_match_id++;
    if (conn->next_match_id == 0) conn->next_match_id = 1;
    entry.callback = callback;
    entry.user = user;
    conn->matches[conn->match_count++] = entry;
    return entry.id;
}

void rgui_dbus_remove_match(rgui_dbus_conn* conn, uint32_t id) {
    if (!conn || id == 0) return;
    for (size_t i = 0; i < conn->match_count; ++i) {
        match_entry* m = &conn->matches[i];
        if (m->id != id) continue;
        bus_call_string(conn, "RemoveMatch", m->rule);
        if (conn->dispatching) {
            m->id = 0; /* freed after routing */
        } else {
            free_match(m);
            conn->matches[i] = conn->matches[--conn->match_count];
        }
        return;
    }
}

static void compact_matches(rgui_dbus_conn* conn) {
    size_t kept = 0;
    for (size_t i = 0; i < conn->match_count; ++i) {
        if (conn->matches[i].id == 0) {
            free_match(&conn->matches[i]);
        } else {
            conn->matches[kept++] = conn->matches[i];
        }
    }
    conn->match_count = kept;
}

void rgui_dbus_set_method_handler(rgui_dbus_conn* conn, rgui_dbus_method_fn handler, void* user) {
    conn->method = handler;
    conn->method_user = user;
}

static bool field_matches(const char* want, const char* have) {
    return !want || (have && strcmp(want, have) == 0);
}

/* A sender the client itself can compare: a unique name, or the bus. A well-known name's owner
 * is known to the caller only. */
static bool sender_checkable(const char* sender) {
    return sender[0] == ':' || strcmp(sender, "org.freedesktop.DBus") == 0;
}

/* The header part of a match: everything but arg0, which needs the body. */
static bool header_matches(const match_entry* e, const rgui_dbus_message* m) {
    return e->id && field_matches(e->interface, m->interface) && field_matches(e->member, m->member) &&
           field_matches(e->path, m->path) && (!e->sender || !sender_checkable(e->sender) || field_matches(e->sender, m->sender));
}

static bool arg0_matches(const match_entry* e, const rgui_dbus_message* m) {
    if (!e->arg0) return true;
    if (m->signature[0] != 's') return false;
    rgui_dbus_iter it;
    rgui_dbus_body_iter(m, &it);
    const char* value;
    return rgui_dbus_get_string(&it, &value, NULL) && strcmp(value, e->arg0) == 0;
}

/* Whether a message whose header is parsed is worth validating and routing. */
static bool wanted(const rgui_dbus_conn* conn, const rgui_dbus_message* m) {
    if (m->type == RGUI_DBUS_METHOD_RETURN || m->type == RGUI_DBUS_ERROR) {
        for (size_t i = 0; i < conn->pending_count; ++i) {
            if (conn->pending[i].serial == m->reply_serial) return true;
        }
        return false;
    }
    if (m->type == RGUI_DBUS_SIGNAL) {
        for (size_t i = 0; i < conn->match_count; ++i) {
            if (header_matches(&conn->matches[i], m)) return true;
        }
        return false;
    }
    return true; /* a method call: answered (Ping, the handler, or UnknownMethod) */
}

static void route(rgui_dbus_conn* conn, const rgui_dbus_message* m) {
    if (m->type == RGUI_DBUS_METHOD_RETURN || m->type == RGUI_DBUS_ERROR) {
        for (size_t i = 0; i < conn->pending_count; ++i) {
            if (conn->pending[i].serial == m->reply_serial) {
                pending_call p = take_pending(conn, i);
                p.callback(p.user, m);
                return;
            }
        }
    } else if (m->type == RGUI_DBUS_SIGNAL) {
        size_t count = conn->match_count;
        for (size_t i = 0; i < count; ++i) {
            match_entry e = conn->matches[i]; /* the array may grow in the callback */
            if (header_matches(&e, m) && arg0_matches(&e, m)) e.callback(e.user, m);
        }
    } else if (m->type == RGUI_DBUS_METHOD_CALL) {
        if (m->interface && strcmp(m->interface, "org.freedesktop.DBus.Peer") == 0 && strcmp(m->member, "Ping") == 0) {
            rgui_dbus_reply(conn, m, "", NULL, 0);
            return;
        }
        if (conn->method && conn->method(conn->method_user, conn, m)) return;
        rgui_dbus_reply_error(conn, m, "org.freedesktop.DBus.Error.UnknownMethod", "No such method");
    }
}

/* Routes every complete message in the input; false when the stream is not D-Bus. */
static bool process_input(rgui_dbus_conn* conn) {
    for (;;) {
        const size_t available = conn->in_size - conn->in_start;
        if (conn->skip) {
            const size_t drop = conn->skip < available ? conn->skip : available;
            conn->in_start += drop;
            conn->skip -= drop;
            if (conn->skip) break;
            continue;
        }
        if (available < 16) break;
        const uint8_t* head = conn->in + conn->in_start;
        size_t size = rgui_dbus_message_size(head);
        if (size == 0) return false;
        if (size > RGUI_DBUS_MAX_INCOMING) {
            conn->skip = size; /* dropped as it arrives */
            continue;
        }
        if (available < size) break;
        /* An invalid or unwanted message of a valid size is dropped; the body of a wanted one is
         * checked only now. */
        rgui_dbus_message m;
        if (parse_header(head, size, &m) && wanted(conn, &m) && body_valid(&m)) route(conn, &m);
        conn->in_start += size;
    }
    if (conn->in_start) {
        memmove(conn->in, conn->in + conn->in_start, conn->in_size - conn->in_start);
        conn->in_size -= conn->in_start;
        conn->in_start = 0;
    }
    return true;
}

/* Makes room for the next read: the whole of the message being received, or a chunk. */
static bool reserve_input(rgui_dbus_conn* conn) {
    size_t need = conn->in_size + RGUI_DBUS_READ_CHUNK;
    if (conn->in_size >= 16 && !conn->skip) {
        size_t size = rgui_dbus_message_size(conn->in);
        if (size == 0) return false;
        if (size <= RGUI_DBUS_MAX_INCOMING && size > need) need = size;
    }
    if (need <= conn->in_capacity) return true;
    size_t capacity = conn->in_capacity ? conn->in_capacity : RGUI_DBUS_READ_CHUNK;
    while (capacity < need) capacity *= 2;
    uint8_t* in = (uint8_t*)realloc(conn->in, capacity);
    if (!in) return false;
    conn->in = in;
    conn->in_capacity = capacity;
    return true;
}

static void expire_calls(rgui_dbus_conn* conn, bool all) {
    for (;;) {
        int64_t now = now_ms();
        size_t i = 0;
        while (i < conn->pending_count && !all && conn->pending[i].deadline > now) ++i;
        if (i >= conn->pending_count) return;
        pending_call p = take_pending(conn, i);
        p.callback(p.user, NULL);
    }
}

/* Sends a short handshake line at once; the socket is fresh, so it never fills. */
static bool send_line(rgui_dbus_conn* conn, const char* data, size_t size) {
    while (size > 0) {
        ssize_t n = send(conn->fd, data, size, MSG_NOSIGNAL);
        if (n > 0) {
            data += n;
            size -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

/* AUTH EXTERNAL with our uid's decimal digits in hex. */
static bool send_auth(rgui_dbus_conn* conn) {
    char uid[24];
    snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    char auth[96];
    size_t n = 0;
    auth[n++] = 0;
    n += (size_t)snprintf(auth + n, sizeof auth - n, "AUTH EXTERNAL ");
    for (const char* p = uid; *p; ++p) n += (size_t)snprintf(auth + n, sizeof auth - n, "%02x", (unsigned char)*p);
    n += (size_t)snprintf(auth + n, sizeof auth - n, "\r\n");
    return send_line(conn, auth, n);
}

/* Moves the handshake on as far as the socket allows: connect, AUTH, OK, BEGIN. */
static void advance_handshake(rgui_dbus_conn* conn) {
    if (conn->state == CONN_CONNECTING) {
        struct pollfd p = {conn->fd, POLLOUT, 0};
        if (poll(&p, 1, 0) <= 0) return;
        int error = 0;
        socklen_t error_length = sizeof error;
        if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 || error != 0 || !send_auth(conn)) {
            conn->dead = true;
            return;
        }
        conn->state = CONN_AUTH;
    }
    /* One line, read a byte at a time: a server sends nothing after OK before our BEGIN. */
    while (conn->state == CONN_AUTH) {
        if (conn->auth_length + 1 >= sizeof conn->auth_line) {
            conn->dead = true;
            return;
        }
        ssize_t r = recv(conn->fd, conn->auth_line + conn->auth_length, 1, 0);
        if (r == 1) {
            size_t n = ++conn->auth_length;
            if (n < 2 || conn->auth_line[n - 2] != '\r' || conn->auth_line[n - 1] != '\n') continue;
            conn->auth_line[n - 2] = 0;
            if (strncmp(conn->auth_line, "OK ", 3) != 0 || !send_line(conn, "BEGIN\r\n", 7)) {
                conn->dead = true;
                return;
            }
            conn->state = CONN_OPEN; /* Hello and whatever was queued go out now */
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            conn->dead = true;
            return;
        }
    }
}

bool rgui_dbus_dispatch(rgui_dbus_conn* conn) {
    if (!conn) return false;
    if (conn->dispatching) return !conn->dead;
    conn->dispatching = true;
    if (!conn->dead && conn->state != CONN_OPEN) advance_handshake(conn);
    /* Read at most 16 MiB per call, so other sources get their turn. */
    size_t budget = 16u << 20;
    while (!conn->dead && conn->state == CONN_OPEN && budget > 0) {
        if (!reserve_input(conn)) {
            conn->dead = true;
            break;
        }
        ssize_t n = recv(conn->fd, conn->in + conn->in_size, conn->in_capacity - conn->in_size, 0);
        if (n > 0) {
            conn->in_size += (size_t)n;
            budget = (size_t)n >= budget ? 0 : budget - (size_t)n;
            if (!process_input(conn)) conn->dead = true;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            conn->dead = true;
        }
    }
    conn_flush(conn);
    expire_calls(conn, conn->dead);
    if (conn->dead) expire_calls(conn, true); /* a callback (Hello's) may have closed it */
    compact_matches(conn);
    /* A big message made the buffer grow: give the memory back once it is drained. */
    if (conn->in_size == 0 && conn->in_capacity > RGUI_DBUS_READ_CHUNK) {
        free(conn->in);
        conn->in = NULL;
        conn->in_capacity = 0;
    }
    conn->dispatching = false;
    return !conn->dead;
}

int rgui_dbus_timeout_ms(const rgui_dbus_conn* conn) {
    if (!conn || conn->pending_count == 0) return -1;
    int64_t first = conn->pending[0].deadline;
    for (size_t i = 1; i < conn->pending_count; ++i) {
        if (conn->pending[i].deadline < first) first = conn->pending[i].deadline;
    }
    int64_t left = first - now_ms();
    if (left < 0) return 0;
    return left > 0x7FFFFFFF ? 0x7FFFFFFF : (int)left;
}

bool rgui_dbus_wait(rgui_dbus_conn* conn, int timeout_ms) {
    if (!conn || conn->dead) return rgui_dbus_dispatch(conn);
    int call_timeout = rgui_dbus_timeout_ms(conn);
    if (timeout_ms < 0 || (call_timeout >= 0 && call_timeout < timeout_ms)) timeout_ms = call_timeout;
    struct pollfd p = {conn->fd, rgui_dbus_poll_events(conn), 0};
    int r = poll(&p, 1, timeout_ms);
    if (r < 0 && errno != EINTR) conn->dead = true;
    return rgui_dbus_dispatch(conn);
}

const char* rgui_dbus_unique_name(const rgui_dbus_conn* conn) { return conn->unique_name; }
bool rgui_dbus_connected(const rgui_dbus_conn* conn) { return conn && !conn->dead; }
bool rgui_dbus_ready(const rgui_dbus_conn* conn) { return conn && conn->ready && !conn->dead; }
int rgui_dbus_fd(const rgui_dbus_conn* conn) { return conn->fd; }
size_t rgui_dbus_input_capacity(const rgui_dbus_conn* conn) { return conn->in_capacity; }

short rgui_dbus_poll_events(const rgui_dbus_conn* conn) {
    if (conn->state == CONN_CONNECTING) return POLLOUT;
    if (conn->state == CONN_AUTH) return POLLIN;
    return (short)(POLLIN | (conn->out_start < conn->out.size ? POLLOUT : 0));
}

void rgui_dbus_close(rgui_dbus_conn* conn) {
    if (!conn) return;
    if (conn->fd >= 0) close(conn->fd);
    for (size_t i = 0; i < conn->match_count; ++i) free_match(&conn->matches[i]);
    free(conn->matches);
    free(conn->pending);
    free(conn->in);
    rgui_dbus_buf_free(&conn->out);
    free(conn);
}

static void on_hello(void* user, const rgui_dbus_message* reply) {
    rgui_dbus_conn* conn = (rgui_dbus_conn*)user;
    const char* name = NULL;
    uint32_t length = 0;
    if (reply && reply->type == RGUI_DBUS_METHOD_RETURN && strcmp(reply->signature, "s") == 0) {
        rgui_dbus_iter it;
        rgui_dbus_body_iter(reply, &it);
        if (!rgui_dbus_get_string(&it, &name, &length) || length == 0 || length >= sizeof conn->unique_name ||
            name[0] != ':') {
            name = NULL;
        }
    }
    if (!name) {
        conn->dead = true; /* no Hello, no bus: the pending calls get their NULL replies */
        return;
    }
    memcpy(conn->unique_name, name, (size_t)length + 1);
    conn->ready = true;
}

rgui_dbus_conn* rgui_dbus_open_async(const char* address, int timeout_ms) {
    struct sockaddr_un sa;
    unsigned sa_length;
    if (!address || !rgui_dbus_parse_address(address, &sa, &sa_length)) return NULL;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return NULL;
    rgui_dbus_conn* conn = (rgui_dbus_conn*)calloc(1, sizeof *conn);
    if (!conn) {
        close(fd);
        return NULL;
    }
    conn->fd = fd;
    conn->next_serial = 1;
    conn->next_match_id = 1;
    conn->state = CONN_AUTH;
    if (connect(fd, (const struct sockaddr*)&sa, (socklen_t)sa_length) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) {
            rgui_dbus_close(conn);
            return NULL;
        }
        conn->state = CONN_CONNECTING;
    }
    if (conn->state == CONN_AUTH && !send_auth(conn)) {
        rgui_dbus_close(conn);
        return NULL;
    }
    /* Hello first; its timeout bounds the whole handshake. */
    rgui_dbus_message m = {0};
    m.destination = "org.freedesktop.DBus";
    m.path = "/org/freedesktop/DBus";
    m.interface = "org.freedesktop.DBus";
    m.member = "Hello";
    if (!rgui_dbus_call(conn, &m, timeout_ms > 0 ? timeout_ms : 1, on_hello, conn)) {
        rgui_dbus_close(conn);
        return NULL;
    }
    return conn;
}

bool rgui_dbus_wait_ready(rgui_dbus_conn* conn, int timeout_ms) {
    if (!conn) return false;
    int64_t deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    while (!conn->ready && !conn->dead) {
        int64_t left = deadline - now_ms();
        if (left <= 0) break;
        rgui_dbus_wait(conn, (int)(left > 0x7FFFFFFF ? 0x7FFFFFFF : left));
    }
    return rgui_dbus_ready(conn);
}

rgui_dbus_conn* rgui_dbus_open(const char* address, int timeout_ms) {
    rgui_dbus_conn* conn = rgui_dbus_open_async(address, timeout_ms);
    if (conn && !rgui_dbus_wait_ready(conn, timeout_ms)) {
        rgui_dbus_close(conn); /* the Hello callback, if still pending, is dropped */
        return NULL;
    }
    return conn;
}

rgui_dbus_conn* rgui_dbus_open_session_async(int timeout_ms) {
    char address[4096];
    if (!rgui_dbus_session_address(address, sizeof address)) return NULL;
    return rgui_dbus_open_async(address, timeout_ms);
}

rgui_dbus_conn* rgui_dbus_open_session(int timeout_ms) {
    char address[4096];
    if (!rgui_dbus_session_address(address, sizeof address)) return NULL;
    return rgui_dbus_open(address, timeout_ms);
}
