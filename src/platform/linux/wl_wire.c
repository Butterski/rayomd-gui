/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/linux/wl_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { kFree = 0, kLive = 1, kZombie = 2 };

typedef struct {
    const rgui_wl_interface* iface;
    uint32_t version;
    rgui_wl_dispatcher dispatcher;
    void* user;
    uint8_t state;
} Object;

struct rgui_wl_connection {
    int fd;
    const rgui_wl_interface* display;
    uint8_t in[RGUI_WL_BUFFER_SIZE];
    size_t in_start, in_end;
    uint8_t out[RGUI_WL_BUFFER_SIZE];
    size_t out_len;
    int fds_in[RGUI_WL_MAX_FDS_IN]; /* a ring */
    size_t fds_in_head, fds_in_count;
    int fds_out[RGUI_WL_MAX_FDS_OUT];
    size_t fds_out_count;
    Object* client; /* indexed by id; 0 unused */
    size_t client_count, client_capacity;
    uint32_t* free_ids;
    size_t free_count, free_capacity;
    Object* server; /* indexed by id - RGUI_WL_SERVER_ID_START */
    size_t server_capacity;
    int depth; /* dispatch nesting: the input buffer is compacted only outside any dispatch */
    int debug; /* WAYLAND_DEBUG=1 or client: every message printed to stderr, as libwayland does */
    char error[256];
};

static const Object* LookupConst(const rgui_wl_connection* c, uint32_t id);

/* One message in libwayland's WAYLAND_DEBUG format: "[ms] -> wl_surface#3.attach(...)". */
static void Trace(const rgui_wl_connection* c, int outgoing, const char* interface_name, uint32_t id,
                  const rgui_wl_message* m, const rgui_wl_arg* args) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const unsigned long ms = (unsigned long)(ts.tv_sec % 100000) * 1000u + (unsigned long)(ts.tv_nsec / 1000000);
    char line[1024];
    size_t at = 0;
#define APPEND(...)                                                                     \
    do {                                                                                \
        if (at < sizeof line) {                                                         \
            const int w = snprintf(line + at, sizeof line - at, __VA_ARGS__);           \
            if (w > 0) at += (size_t)w;                                                 \
        }                                                                               \
    } while (0)
    APPEND("[%7lu.%03lu] %s%s#%u.%s(", ms / 1000, ms % 1000, outgoing ? " -> " : "", interface_name, id, m->name);
    size_t arg = 0;
    const char* bind_interface = NULL;
    for (const char* s = m->signature; *s; ++s) {
        if (*s == '?') continue;
        const rgui_wl_arg* a = &args[arg];
        if (arg) APPEND(", ");
        switch (*s) {
        case 'i': APPEND("%d", a->i); break;
        case 'u': APPEND("%u", a->u); break;
        case 'f': APPEND("%.3f", (double)a->f / 256.0); break;
        case 's':
            if (a->s) {
                APPEND("\"%.200s\"", a->s);
                bind_interface = a->s;
            } else {
                APPEND("nil");
            }
            break;
        case 'o': {
            const Object* o = LookupConst(c, a->o);
            if (!a->o) APPEND("nil");
            else APPEND("%s#%u", o && o->iface ? o->iface->name : "[unknown]", a->o);
            break;
        }
        case 'n':
            APPEND("new id %s#%u", m->new_interface ? m->new_interface->name : bind_interface ? bind_interface : "[unknown]",
                   a->n);
            break;
        case 'a': APPEND("array[%u]", a->a.size); break;
        case 'h': APPEND("fd %d", a->h); break;
        default: break;
        }
        ++arg;
    }
    APPEND(")\n");
#undef APPEND
    fputs(line, stderr);
}

static void SetError(rgui_wl_connection* c, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(c->error, sizeof c->error, format, args);
    va_end(args);
}

static uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void WriteU32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

/* --- connecting ------------------------------------------------------------------------------ */

int rgui_wl_connect_socket(char* error, size_t error_size) {
    const char* socket_env = getenv("WAYLAND_SOCKET");
    if (socket_env && *socket_env) {
        char* end = NULL;
        errno = 0;
        const long fd = strtol(socket_env, &end, 10);
        if (errno || !end || *end || fd < 0 || fd > 0x7fffffff) {
            snprintf(error, error_size, "WAYLAND_SOCKET is not a file descriptor: %s", socket_env);
            return -1;
        }
        const int flags = fcntl((int)fd, F_GETFD);
        if (flags < 0 || fcntl((int)fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
            snprintf(error, error_size, "WAYLAND_SOCKET %ld: %s", fd, strerror(errno));
            return -1;
        }
        unsetenv("WAYLAND_SOCKET");
        return (int)fd;
    }

    const char* display = getenv("WAYLAND_DISPLAY");
    if (!display || !*display) display = "wayland-0";
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    int length;
    if (display[0] == '/') {
        length = snprintf(address.sun_path, sizeof address.sun_path, "%s", display);
    } else {
        const char* runtime = getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) {
            snprintf(error, error_size, "XDG_RUNTIME_DIR is not set");
            return -1;
        }
        length = snprintf(address.sun_path, sizeof address.sun_path, "%s/%s", runtime, display);
    }
    if (length < 0 || (size_t)length >= sizeof address.sun_path) {
        snprintf(error, error_size, "the Wayland socket path is too long");
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(error, error_size, "socket: %s", strerror(errno));
        return -1;
    }
    if (connect(fd, (struct sockaddr*)&address, sizeof address) < 0) {
        snprintf(error, error_size, "cannot connect to %s: %s", address.sun_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* --- objects --------------------------------------------------------------------------------- */

static Object* Lookup(const rgui_wl_connection* c, uint32_t id) {
    if (id == 0) return NULL;
    if (id >= RGUI_WL_SERVER_ID_START) {
        const size_t index = id - RGUI_WL_SERVER_ID_START;
        return index < c->server_capacity ? &c->server[index] : NULL;
    }
    return id < c->client_count ? &c->client[id] : NULL;
}

static const Object* LookupConst(const rgui_wl_connection* c, uint32_t id) { return Lookup(c, id); }

static int EnsureClientSlot(rgui_wl_connection* c, size_t count) {
    if (count <= c->client_capacity) return 0;
    size_t next = c->client_capacity ? c->client_capacity * 2 : 64;
    while (next < count) next *= 2;
    Object* p = realloc(c->client, next * sizeof(Object));
    if (!p) return -1;
    memset(p + c->client_capacity, 0, (next - c->client_capacity) * sizeof(Object));
    c->client = p;
    c->client_capacity = next;
    return 0;
}

static Object* ServerSlot(rgui_wl_connection* c, uint32_t id) {
    if (id < RGUI_WL_SERVER_ID_START) return NULL;
    const size_t index = id - RGUI_WL_SERVER_ID_START;
    if (index >= RGUI_WL_MAX_SERVER_OBJECTS) return NULL;
    if (index >= c->server_capacity) {
        size_t next = c->server_capacity ? c->server_capacity * 2 : 64;
        while (next <= index) next *= 2;
        if (next > RGUI_WL_MAX_SERVER_OBJECTS) next = RGUI_WL_MAX_SERVER_OBJECTS;
        Object* p = realloc(c->server, next * sizeof(Object));
        if (!p) return NULL;
        memset(p + c->server_capacity, 0, (next - c->server_capacity) * sizeof(Object));
        c->server = p;
        c->server_capacity = next;
    }
    return &c->server[index];
}

static void ReleaseClientId(rgui_wl_connection* c, uint32_t id) {
    Object* o = Lookup(c, id);
    if (!o || o->state == kFree) return;
    memset(o, 0, sizeof *o);
    if (c->free_count == c->free_capacity) {
        const size_t next = c->free_capacity ? c->free_capacity * 2 : 64;
        uint32_t* p = realloc(c->free_ids, next * sizeof(uint32_t));
        if (!p) return; /* the id is lost, nothing worse */
        c->free_ids = p;
        c->free_capacity = next;
    }
    c->free_ids[c->free_count++] = id;
}

/* After a destructor: a client id waits for delete_id, a server id is free at once. */
static void Retire(rgui_wl_connection* c, uint32_t id) {
    Object* o = Lookup(c, id);
    if (!o || o->state != kLive) return;
    if (id >= RGUI_WL_SERVER_ID_START) {
        memset(o, 0, sizeof *o);
    } else {
        o->state = kZombie;
        o->dispatcher = NULL;
        o->user = NULL;
    }
}

uint32_t rgui_wl_new_object(rgui_wl_connection* c, const rgui_wl_interface* iface, uint32_t version,
                            rgui_wl_dispatcher dispatcher, void* user) {
    uint32_t id;
    if (c->free_count) {
        id = c->free_ids[--c->free_count];
    } else {
        if (c->client_count >= RGUI_WL_SERVER_ID_START) return 0;
        if (EnsureClientSlot(c, c->client_count + 1) != 0) return 0;
        id = (uint32_t)c->client_count++;
    }
    Object* o = &c->client[id];
    o->iface = iface;
    o->version = version;
    o->dispatcher = dispatcher;
    o->user = user;
    o->state = kLive;
    return id;
}

int rgui_wl_set_dispatcher(rgui_wl_connection* c, uint32_t id, rgui_wl_dispatcher dispatcher, void* user) {
    Object* o = Lookup(c, id);
    if (!o || o->state != kLive) return -1;
    o->dispatcher = dispatcher;
    o->user = user;
    return 0;
}

uint32_t rgui_wl_object_version(const rgui_wl_connection* c, uint32_t id) {
    const Object* o = Lookup(c, id);
    return o && o->state == kLive ? o->version : 0;
}

/* --- the connection -------------------------------------------------------------------------- */

rgui_wl_connection* rgui_wl_connection_create(int fd, const rgui_wl_interface* display_interface) {
    rgui_wl_connection* c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = fd;
    c->display = display_interface;
    const char* debug = getenv("WAYLAND_DEBUG");
    c->debug = debug && (strcmp(debug, "1") == 0 || strstr(debug, "client") != NULL);
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    c->client_count = 1; /* id 0 is null */
    if (EnsureClientSlot(c, 2) != 0) {
        free(c);
        return NULL;
    }
    if (rgui_wl_new_object(c, display_interface, 1, NULL, NULL) != RGUI_WL_DISPLAY_ID) {
        free(c->client);
        free(c);
        return NULL;
    }
    return c;
}

void rgui_wl_connection_destroy(rgui_wl_connection* c) {
    if (!c) return;
    while (c->fds_in_count) {
        close(c->fds_in[c->fds_in_head]);
        c->fds_in_head = (c->fds_in_head + 1) % RGUI_WL_MAX_FDS_IN;
        c->fds_in_count--;
    }
    for (size_t i = 0; i < c->fds_out_count; ++i) close(c->fds_out[i]);
    if (c->fd >= 0) close(c->fd);
    free(c->client);
    free(c->server);
    free(c->free_ids);
    free(c);
}

int rgui_wl_connection_fd(const rgui_wl_connection* c) { return c->fd; }

const char* rgui_wl_connection_error(const rgui_wl_connection* c) { return c->error; }

static const rgui_wl_message* FindMessage(const rgui_wl_message* list, uint16_t count, uint16_t opcode) {
    for (uint16_t i = 0; i < count; ++i) {
        if (list[i].opcode == opcode) return &list[i];
    }
    return NULL;
}

/* --- output ---------------------------------------------------------------------------------- */

int rgui_wl_flush(rgui_wl_connection* c) {
    while (c->out_len > 0) {
        struct iovec iov = {c->out, c->out_len};
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        union {
            char buffer[CMSG_SPACE(sizeof(int) * RGUI_WL_MAX_FDS_OUT)];
            struct cmsghdr align;
        } control;
        if (c->fds_out_count) {
            memset(&control, 0, sizeof control);
            msg.msg_control = control.buffer;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * c->fds_out_count);
            struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int) * c->fds_out_count);
            memcpy(CMSG_DATA(cmsg), c->fds_out, sizeof(int) * c->fds_out_count);
        }
        const ssize_t sent = sendmsg(c->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            SetError(c, "sending to the compositor: %s", strerror(errno));
            return -1;
        }
        /* The fds went with the first byte. */
        for (size_t i = 0; i < c->fds_out_count; ++i) close(c->fds_out[i]);
        c->fds_out_count = 0;
        memmove(c->out, c->out + sent, c->out_len - (size_t)sent);
        c->out_len -= (size_t)sent;
    }
    return 0;
}

static int64_t NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int rgui_wl_flush_blocking(rgui_wl_connection* c, int timeout_ms) {
    const int64_t deadline = NowMs() + timeout_ms;
    for (;;) {
        const int r = rgui_wl_flush(c);
        if (r <= 0) return r;
        const int64_t left = deadline - NowMs();
        if (left <= 0) {
            SetError(c, "the compositor stopped reading");
            return -1;
        }
        struct pollfd p = {c->fd, POLLOUT, 0};
        if (poll(&p, 1, (int)left) < 0 && errno != EINTR) {
            SetError(c, "poll: %s", strerror(errno));
            return -1;
        }
    }
}

int rgui_wl_send(rgui_wl_connection* c, uint32_t id, uint16_t opcode, const rgui_wl_arg* args) {
    Object* o = Lookup(c, id);
    if (!o || o->state != kLive) {
        SetError(c, "request %u to object %u, which does not exist", opcode, id);
        return -1;
    }
    const rgui_wl_message* m = FindMessage(o->iface->requests, o->iface->request_count, opcode);
    if (!m) {
        SetError(c, "%s has no request %u in these tables", o->iface->name, opcode);
        return -1;
    }
    uint8_t message[RGUI_WL_MAX_MESSAGE];
    int fds[RGUI_WL_MAX_ARGS];
    size_t fd_count = 0;
    size_t size = 8;
    size_t arg = 0;
    for (const char* s = m->signature; *s; ++s) {
        int nullable = 0;
        if (*s == '?') {
            nullable = 1;
            ++s;
        }
        const rgui_wl_arg* a = &args[arg++];
        switch (*s) {
        case 'i':
        case 'u':
        case 'f':
        case 'n':
        case 'o':
            if (*s == 'o' && a->o == 0 && !nullable) goto null_arg;
            if (size + 4 > sizeof message) goto too_long;
            WriteU32(message + size, a->u);
            size += 4;
            break;
        case 's': {
            if (!a->s) {
                if (!nullable) goto null_arg;
                if (size + 4 > sizeof message) goto too_long;
                WriteU32(message + size, 0);
                size += 4;
                break;
            }
            const size_t length = strlen(a->s) + 1;
            const size_t padded = (length + 3) & ~(size_t)3;
            if (length > sizeof message || size + 4 + padded > sizeof message) goto too_long;
            WriteU32(message + size, (uint32_t)length);
            memcpy(message + size + 4, a->s, length);
            memset(message + size + 4 + length, 0, padded - length);
            size += 4 + padded;
            break;
        }
        case 'a': {
            const size_t length = a->a.size;
            const size_t padded = (length + 3) & ~(size_t)3;
            if (length > sizeof message || size + 4 + padded > sizeof message) goto too_long;
            WriteU32(message + size, (uint32_t)length);
            if (length) memcpy(message + size + 4, a->a.data, length);
            memset(message + size + 4 + length, 0, padded - length);
            size += 4 + padded;
            break;
        }
        case 'h':
            fds[fd_count++] = a->h;
            break;
        default:
            SetError(c, "bad signature of %s.%s", o->iface->name, m->name);
            return -1;
        }
    }
    WriteU32(message, id);
    WriteU32(message + 4, (uint32_t)(size << 16) | opcode);

    if (c->out_len + size > sizeof c->out || c->fds_out_count + fd_count > RGUI_WL_MAX_FDS_OUT) {
        if (rgui_wl_flush_blocking(c, 5000) != 0) return -1;
    }
    for (size_t i = 0; i < fd_count; ++i) {
        const int copy = fcntl(fds[i], F_DUPFD_CLOEXEC, 0);
        if (copy < 0) {
            SetError(c, "%s.%s: fd %d: %s", o->iface->name, m->name, fds[i], strerror(errno));
            for (size_t k = 0; k < i; ++k) close(c->fds_out[--c->fds_out_count]);
            return -1;
        }
        c->fds_out[c->fds_out_count++] = copy;
    }
    if (c->debug) Trace(c, 1, o->iface->name, id, m, args);
    memcpy(c->out + c->out_len, message, size);
    c->out_len += size;
    if (m->flags & RGUI_WL_DESTRUCTOR) Retire(c, id);
    return 0;

null_arg:
    SetError(c, "%s.%s: null argument %zu", o->iface->name, m->name, arg);
    return -1;
too_long:
    SetError(c, "%s.%s: the message is longer than %d bytes", o->iface->name, m->name, RGUI_WL_MAX_MESSAGE);
    return -1;
}

/* --- input ----------------------------------------------------------------------------------- */

static void CompactInput(rgui_wl_connection* c) {
    if (c->depth > 0 || c->in_start == 0) return;
    memmove(c->in, c->in + c->in_start, c->in_end - c->in_start);
    c->in_end -= c->in_start;
    c->in_start = 0;
}

int rgui_wl_read(rgui_wl_connection* c) {
    CompactInput(c);
    if (c->in_end == sizeof c->in) {
        SetError(c, "the input buffer is full");
        return -1;
    }
    for (;;) {
        struct iovec iov = {c->in + c->in_end, sizeof c->in - c->in_end};
        union {
            char buffer[CMSG_SPACE(sizeof(int) * RGUI_WL_MAX_FDS_OUT)];
            struct cmsghdr align;
        } control;
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.buffer;
        msg.msg_controllen = sizeof control.buffer;
        const ssize_t n = recvmsg(c->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            SetError(c, "reading from the compositor: %s", strerror(errno));
            return -1;
        }
        int overflow = 0;
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
            if (cmsg->cmsg_len < CMSG_LEN(0)) continue;
            const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const unsigned char* data = CMSG_DATA(cmsg);
            for (size_t i = 0; i < count; ++i) {
                int fd;
                memcpy(&fd, data + i * sizeof(int), sizeof(int));
                if (c->fds_in_count == RGUI_WL_MAX_FDS_IN) {
                    close(fd);
                    overflow = 1;
                    continue;
                }
                c->fds_in[(c->fds_in_head + c->fds_in_count) % RGUI_WL_MAX_FDS_IN] = fd;
                c->fds_in_count++;
            }
        }
        if (overflow || (msg.msg_flags & MSG_CTRUNC)) {
            SetError(c, "the compositor sent too many file descriptors");
            return -1;
        }
        if (n == 0) {
            SetError(c, "the compositor closed the connection");
            return -1;
        }
        c->in_end += (size_t)n;
        return 1;
    }
}

static int PopFd(rgui_wl_connection* c) {
    if (!c->fds_in_count) return -1;
    const int fd = c->fds_in[c->fds_in_head];
    c->fds_in_head = (c->fds_in_head + 1) % RGUI_WL_MAX_FDS_IN;
    c->fds_in_count--;
    return fd;
}

/* Decodes the arguments of `m` from [p, p + length). 0, or -1 with every fd taken closed. */
static int Decode(rgui_wl_connection* c, const Object* target, const rgui_wl_message* m, const uint8_t* p,
                  size_t length, rgui_wl_arg* args, uint32_t* new_id) {
    const uint32_t version = target->version; /* creating a server object may move `target` */
    const char* interface_name = target->iface->name;
    size_t at = 0;
    size_t arg = 0;
    int taken[RGUI_WL_MAX_ARGS];
    size_t taken_count = 0;
    *new_id = 0;
    for (const char* s = m->signature; *s; ++s) {
        int nullable = 0;
        if (*s == '?') {
            nullable = 1;
            ++s;
        }
        if (arg == RGUI_WL_MAX_ARGS) goto bad;
        rgui_wl_arg* a = &args[arg++];
        switch (*s) {
        case 'i':
        case 'u':
        case 'f':
        case 'o':
        case 'n':
            if (length - at < 4) goto bad;
            a->u = ReadU32(p + at);
            at += 4;
            if (*s == 'o' && a->o == 0 && !nullable) goto bad;
            if (*s == 'n') {
                if (a->n < RGUI_WL_SERVER_ID_START ||
                    a->n - RGUI_WL_SERVER_ID_START >= RGUI_WL_MAX_SERVER_OBJECTS || !m->new_interface || *new_id) {
                    goto bad;
                }
                *new_id = a->n;
            }
            break;
        case 's':
        case 'a': {
            if (length - at < 4) goto bad;
            const size_t size = ReadU32(p + at);
            at += 4;
            const size_t padded = (size + 3) & ~(size_t)3;
            if (padded < size || padded > length - at) goto bad;
            if (*s == 's') {
                if (size == 0) {
                    if (!nullable) goto bad;
                    a->s = NULL;
                } else {
                    if (p[at + size - 1] != 0) goto bad;
                    a->s = (const char*)(p + at);
                }
            } else {
                a->a.data = p + at;
                a->a.size = (uint32_t)size;
            }
            at += padded;
            break;
        }
        case 'h': {
            const int fd = PopFd(c);
            if (fd < 0) goto bad;
            a->h = fd;
            taken[taken_count++] = fd;
            break;
        }
        default:
            goto bad;
        }
    }
    if (at != length) goto bad;
    if (*new_id) {
        Object* slot = ServerSlot(c, *new_id);
        if (!slot) goto bad;
        slot->iface = m->new_interface;
        slot->version = version;
        slot->dispatcher = NULL;
        slot->user = NULL;
        slot->state = kLive;
    }
    return 0;
bad:
    for (size_t i = 0; i < taken_count; ++i) close(taken[i]);
    SetError(c, "malformed %s.%s event", interface_name, m->name);
    return -1;
}

static void CloseFds(const rgui_wl_message* m, const rgui_wl_arg* args) {
    size_t arg = 0;
    for (const char* s = m->signature; *s; ++s) {
        if (*s == '?') continue;
        if (*s == 'h') close(args[arg].h);
        ++arg;
    }
}

static void HandleDisplay(rgui_wl_connection* c, uint16_t opcode, const rgui_wl_arg* args) {
    if (opcode == 0) { /* error: object, code, message */
        fprintf(stderr, "Wayland protocol error: object %u code %u: %s\n", args[0].o, args[1].u,
                args[2].s ? args[2].s : "");
        exit(1);
    }
    if (opcode == 1) { /* delete_id */
        if (args[0].u != RGUI_WL_DISPLAY_ID && args[0].u < RGUI_WL_SERVER_ID_START) ReleaseClientId(c, args[0].u);
    }
}

int rgui_wl_dispatch(rgui_wl_connection* c) {
    int count = 0;
    c->depth++;
    while (c->in_end - c->in_start >= 8) {
        const uint8_t* header = c->in + c->in_start;
        const uint32_t id = ReadU32(header);
        const uint32_t word = ReadU32(header + 4);
        const size_t size = word >> 16;
        const uint16_t opcode = (uint16_t)(word & 0xffff);
        if (size < 8 || (size & 3) != 0) {
            SetError(c, "malformed message: size %zu for object %u", size, id);
            c->depth--;
            return -1;
        }
        if (c->in_end - c->in_start < size) break;
        c->in_start += size; /* consumed before the handler runs: handlers may dispatch again */
        const Object* o = Lookup(c, id);
        if (!o || o->state == kFree) continue; /* destroyed and forgotten */
        const rgui_wl_message* m = FindMessage(o->iface->events, o->iface->event_count, opcode);
        if (!m) continue; /* not in our tables: we do not use it */
        rgui_wl_arg args[RGUI_WL_MAX_ARGS];
        uint32_t new_id;
        if (Decode(c, o, m, header + 8, size - 8, args, &new_id) != 0) {
            c->depth--;
            return -1;
        }
        o = Lookup(c, id);
        if (c->debug) Trace(c, 0, o->iface->name, id, m, args);
        if (id == RGUI_WL_DISPLAY_ID) {
            HandleDisplay(c, opcode, args);
        } else if (o->state == kLive && o->dispatcher) {
            o->dispatcher(o->user, id, opcode, args); /* `o` may move: the map can grow */
        } else {
            CloseFds(m, args);
        }
        if (m->flags & RGUI_WL_DESTRUCTOR) Retire(c, id);
        ++count;
    }
    c->depth--;
    CompactInput(c);
    return count;
}

/* --- roundtrip ------------------------------------------------------------------------------- */

static void OnSyncDone(void* user, uint32_t object, uint16_t opcode, const rgui_wl_arg* args) {
    (void)object;
    (void)opcode;
    (void)args;
    *(int*)user = 1;
}

int rgui_wl_roundtrip(rgui_wl_connection* c, const rgui_wl_interface* callback_interface, int timeout_ms) {
    int done = 0;
    const uint32_t callback = rgui_wl_new_object(c, callback_interface, 1, OnSyncDone, &done);
    if (!callback) return -1;
    rgui_wl_arg arg;
    arg.n = callback;
    if (rgui_wl_send(c, RGUI_WL_DISPLAY_ID, 0, &arg) != 0) return -1;
    const int64_t deadline = NowMs() + timeout_ms;
    while (!done) {
        if (rgui_wl_flush_blocking(c, timeout_ms) != 0) break;
        const int r = rgui_wl_read(c);
        if (r < 0) break;
        if (r > 0) {
            if (rgui_wl_dispatch(c) < 0) break;
            continue;
        }
        const int64_t left = deadline - NowMs();
        if (left <= 0) {
            SetError(c, "the compositor did not answer within %d ms", timeout_ms);
            break;
        }
        struct pollfd p = {c->fd, POLLIN, 0};
        if (poll(&p, 1, (int)left) < 0 && errno != EINTR) {
            SetError(c, "poll: %s", strerror(errno));
            break;
        }
    }
    if (!done) rgui_wl_set_dispatcher(c, callback, NULL, NULL);
    return done ? 0 : -1;
}
