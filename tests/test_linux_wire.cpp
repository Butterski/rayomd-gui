// SPDX-License-Identifier: Apache-2.0
// The Wayland wire: every argument type through a socketpair, fds included; the exact bytes and
// padding; messages split across reads; malformed input rejected without reading past it.
#include "platform/linux/wl_wire.h"
#include "runner.h"

#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

// A display whose error and delete_id can also be sent (by the peer playing the compositor).
const rgui_wl_message kDisplayMessages[] = {
    {0, 0, "error", "ous", nullptr},
    {1, 0, "delete_id", "u", nullptr},
};
const rgui_wl_interface kDisplay = {"test_display", 1, 2, 2, kDisplayMessages, kDisplayMessages};

extern const rgui_wl_interface kChild;
const rgui_wl_message kChildMessages[] = {
    {0, 0, "ping", "u", nullptr},
};
const rgui_wl_interface kChild = {"test_child", 1, 1, 1, kChildMessages, kChildMessages};

// Requests and events mirror each other, so one side's request is the other side's event.
const rgui_wl_message kThingMessages[] = {
    {0, 0, "all", "iufsonah?s?o", &kChild},
    {1, 0, "strings", "sa", nullptr},
    {2, RGUI_WL_DESTRUCTOR, "destroy", "", nullptr},
    {5, 0, "fd", "h", nullptr},
};
const rgui_wl_interface kThing = {"test_thing", 3, 4, 4, kThingMessages, kThingMessages};

struct Received {
    int count = 0;
    uint16_t opcode = 0xffff;
    rgui_wl_arg args[RGUI_WL_MAX_ARGS] = {};
    std::string s;
    std::string array;
    bool nullString = false;
    int fd = -1;
    std::vector<int> fds;
};

void Capture(void* user, uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    auto* r = static_cast<Received*>(user);
    r->count++;
    r->opcode = opcode;
    std::memcpy(r->args, args, sizeof r->args);
    if (opcode == 0) {
        r->s = args[3].s ? args[3].s : "";
        r->array.assign(static_cast<const char*>(args[6].a.data), args[6].a.size);
        r->fd = args[7].h;
        r->nullString = args[8].s == nullptr;
    } else if (opcode == 1) {
        r->s = args[0].s;
        r->array.assign(static_cast<const char*>(args[1].a.data), args[1].a.size);
    } else if (opcode == 5) {
        r->fd = args[0].h;
        r->fds.push_back(args[0].h);
    }
}

struct Pair {
    rgui_wl_connection* client = nullptr;  // sends requests
    rgui_wl_connection* server = nullptr;  // receives them as events
    uint32_t clientThing = 0;
    uint32_t serverThing = 0;
    Received received;

    Pair() {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) return;
        client = rgui_wl_connection_create(fds[0], &kDisplay);
        server = rgui_wl_connection_create(fds[1], &kDisplay);
        clientThing = rgui_wl_new_object(client, &kThing, 3, nullptr, nullptr);
        serverThing = rgui_wl_new_object(server, &kThing, 3, Capture, &received);
    }
    ~Pair() {
        rgui_wl_connection_destroy(client);
        rgui_wl_connection_destroy(server);
    }
    // Flushes the client and dispatches everything on the server.
    int Deliver() {
        if (rgui_wl_flush(client) != 0) return -100;
        if (rgui_wl_read(server) < 0) return -101;
        return rgui_wl_dispatch(server);
    }
};

// A raw socket end playing the compositor, writing exact bytes to a client connection.
struct RawPeer {
    int raw = -1;
    rgui_wl_connection* connection = nullptr;
    uint32_t thing = 0;
    Received received;

    RawPeer() {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) return;
        raw = fds[0];
        connection = rgui_wl_connection_create(fds[1], &kDisplay);
        thing = rgui_wl_new_object(connection, &kThing, 3, Capture, &received);
    }
    ~RawPeer() {
        rgui_wl_connection_destroy(connection);
        if (raw >= 0) close(raw);
    }
    void Write(const std::vector<uint8_t>& bytes) const {
        CHECK_EQ(write(raw, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
    }
    int ReadAndDispatch() {
        if (rgui_wl_read(connection) < 0) return -101;
        return rgui_wl_dispatch(connection);
    }
};

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void Header(std::vector<uint8_t>& out, uint32_t id, uint32_t size, uint16_t opcode) {
    Put32(out, id);
    Put32(out, (size << 16) | opcode);
}

void PutBytes(std::vector<uint8_t>& out, const char* bytes, size_t n) {
    out.insert(out.end(), bytes, bytes + n);
}

bool SameFile(int a, int b) {
    struct stat sa, sb;
    if (fstat(a, &sa) != 0 || fstat(b, &sb) != 0) return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

} // namespace

RGUI_TEST("linux.wire.every_argument_type") {
    Pair p;
    CHECK(p.client && p.server);
    if (!p.client || !p.server) return;
    CHECK_EQ(p.clientThing, 2u);
    CHECK_EQ(p.serverThing, 2u);

    const int file = memfd_create("rgui-wire-test", MFD_CLOEXEC);
    CHECK(file >= 0);
    const uint8_t bytes[5] = {1, 2, 3, 4, 5};
    rgui_wl_arg args[10];
    args[0].i = -5;
    args[1].u = 0xdeadbeefu;
    args[2].f = rgui_wl_fixed_from_int(-3) - 128;  // -3.5
    args[3].s = "hello";
    args[4].o = p.clientThing;
    args[5].n = RGUI_WL_SERVER_ID_START + 7;
    args[6].a = {bytes, 5};
    args[7].h = file;
    args[8].s = nullptr;
    args[9].o = 0;
    CHECK_EQ(rgui_wl_send(p.client, p.clientThing, 0, args), 0);
    CHECK_EQ(p.Deliver(), 1);
    const Received& r = p.received;
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.opcode, 0);
    CHECK_EQ(r.args[0].i, -5);
    CHECK_EQ(r.args[1].u, 0xdeadbeefu);
    CHECK_NEAR(rgui_wl_fixed_to_double(r.args[2].f), -3.5, 1e-9);
    CHECK_EQ(r.s, std::string("hello"));
    CHECK_EQ(r.args[4].o, 2u);
    CHECK_EQ(r.args[5].n, RGUI_WL_SERVER_ID_START + 7);
    CHECK_EQ(r.array, std::string("\1\2\3\4\5", 5));
    CHECK(r.nullString);
    CHECK_EQ(r.args[9].o, 0u);
    // The fd arrived as a new descriptor of the same file, close-on-exec.
    CHECK(r.fd >= 0 && r.fd != file);
    CHECK(SameFile(r.fd, file));
    CHECK((fcntl(r.fd, F_GETFD) & FD_CLOEXEC) != 0);
    // The new_id made a server object of the message's interface, with the parent's version.
    CHECK_EQ(rgui_wl_object_version(p.server, RGUI_WL_SERVER_ID_START + 7), 3u);
    if (r.fd >= 0) close(r.fd);
    // The sender keeps its own descriptor: send duplicated it.
    CHECK(fcntl(file, F_GETFD) >= 0);
    close(file);
}

RGUI_TEST("linux.wire.exact_bytes_and_padding") {
    int fds[2];
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
    rgui_wl_connection* c = rgui_wl_connection_create(fds[0], &kDisplay);
    const uint32_t thing = rgui_wl_new_object(c, &kThing, 1, nullptr, nullptr);
    const uint8_t bytes[5] = {9, 8, 7, 6, 5};
    rgui_wl_arg args[2];
    args[0].s = "abcd";  // 5 bytes with the NUL: 3 of padding
    args[1].a = {bytes, 5};
    CHECK_EQ(rgui_wl_send(c, thing, 1, args), 0);
    args[0].s = "abc";   // 4 bytes: no padding
    args[1].a = {nullptr, 0};
    CHECK_EQ(rgui_wl_send(c, thing, 1, args), 0);
    CHECK_EQ(rgui_wl_flush(c), 0);
    std::vector<uint8_t> expected;
    Header(expected, thing, 8 + 4 + 8 + 4 + 8, 1);
    Put32(expected, 5);
    PutBytes(expected, "abcd\0\0\0\0", 8);
    Put32(expected, 5);
    for (uint8_t b : {9, 8, 7, 6, 5, 0, 0, 0}) expected.push_back(b);
    Header(expected, thing, 8 + 4 + 4 + 4, 1);
    Put32(expected, 4);
    PutBytes(expected, "abc\0", 4);
    Put32(expected, 0);
    std::vector<uint8_t> got(expected.size() + 16);
    const ssize_t n = read(fds[1], got.data(), got.size());
    got.resize(n > 0 ? static_cast<size_t>(n) : 0);
    CHECK(got == expected);
    rgui_wl_connection_destroy(c);
    close(fds[1]);
}

RGUI_TEST("linux.wire.encode_refuses_bad_arguments") {
    Pair p;
    if (!p.client) return;
    rgui_wl_arg args[2];
    args[0].s = nullptr;  // 's' is not nullable
    args[1].a = {nullptr, 0};
    CHECK_EQ(rgui_wl_send(p.client, p.clientThing, 1, args), -1);
    std::string big(RGUI_WL_MAX_MESSAGE, 'x');
    args[0].s = big.c_str();
    CHECK_EQ(rgui_wl_send(p.client, p.clientThing, 1, args), -1);
    CHECK_EQ(rgui_wl_send(p.client, p.clientThing, 3, args), -1);  // no such opcode
    CHECK_EQ(rgui_wl_send(p.client, 77, 1, args), -1);             // no such object
    // Nothing reached the socket.
    CHECK_EQ(p.Deliver(), 0);
    CHECK_EQ(p.received.count, 0);
}

RGUI_TEST("linux.wire.split_across_reads") {
    RawPeer peer;
    if (!peer.connection) return;
    std::vector<uint8_t> m;
    Header(m, peer.thing, 8 + 4 + 8 + 4 + 4, 1);
    Put32(m, 6);
    PutBytes(m, "split\0\0\0", 8);
    Put32(m, 3);
    PutBytes(m, "xyz\0", 4);
    // Two complete copies after the first, the split at every boundary of the first message.
    for (size_t cut = 1; cut < m.size(); ++cut) {
        peer.received = Received{};
        peer.Write(std::vector<uint8_t>(m.begin(), m.begin() + static_cast<ptrdiff_t>(cut)));
        CHECK_EQ(peer.ReadAndDispatch(), 0);
        std::vector<uint8_t> rest(m.begin() + static_cast<ptrdiff_t>(cut), m.end());
        rest.insert(rest.end(), m.begin(), m.end());
        peer.Write(rest);
        CHECK_EQ(peer.ReadAndDispatch(), 2);
        CHECK_EQ(peer.received.count, 2);
        CHECK_EQ(peer.received.s, std::string("split"));
        CHECK_EQ(peer.received.array, std::string("xyz"));
    }
}

RGUI_TEST("linux.wire.unknown_messages_are_skipped") {
    RawPeer peer;
    if (!peer.connection) return;
    std::vector<uint8_t> m;
    Header(m, peer.thing, 16, 9);   // an opcode the tables do not list
    Put32(m, 1);
    Put32(m, 2);
    Header(m, 999, 12, 0);          // an object that does not exist
    Put32(m, 3);
    Header(m, peer.thing, 8 + 4 + 4 + 4, 1);  // then a message we know
    Put32(m, 2);
    PutBytes(m, "k\0\0\0", 4);
    Put32(m, 0);
    peer.Write(m);
    CHECK_EQ(peer.ReadAndDispatch(), 1);
    CHECK_EQ(peer.received.count, 1);
    CHECK_EQ(peer.received.s, std::string("k"));
}

RGUI_TEST("linux.wire.malformed_input_fails_closed") {
    struct Case {
        const char* name;
        std::vector<uint8_t> bytes;
    };
    std::vector<Case> cases;
    {
        Case c{"size below the header", {}};
        Header(c.bytes, 2, 6, 1);
        cases.push_back(c);
    }
    {
        Case c{"size not a multiple of 4", {}};
        Header(c.bytes, 2, 13, 1);
        for (int i = 0; i < 8; ++i) c.bytes.push_back(0);
        cases.push_back(c);
    }
    {
        Case c{"string longer than the message", {}};
        Header(c.bytes, 2, 20, 1);
        Put32(c.bytes, 1000);
        Put32(c.bytes, 0x41414141);
        Put32(c.bytes, 0);
        cases.push_back(c);
    }
    {
        Case c{"string length near 2^32", {}};
        Header(c.bytes, 2, 20, 1);
        Put32(c.bytes, 0xfffffffeu);
        Put32(c.bytes, 0);
        Put32(c.bytes, 0);
        cases.push_back(c);
    }
    {
        Case c{"string without its NUL", {}};
        Header(c.bytes, 2, 20, 1);
        Put32(c.bytes, 4);
        PutBytes(c.bytes, "abcd", 4);
        Put32(c.bytes, 0);
        cases.push_back(c);
    }
    {
        Case c{"trailing bytes", {}};
        Header(c.bytes, 2, 24, 1);
        Put32(c.bytes, 2);
        PutBytes(c.bytes, "a\0\0\0", 4);
        Put32(c.bytes, 0);
        Put32(c.bytes, 0);
        cases.push_back(c);
    }
    {
        Case c{"fd that never came", {}};
        Header(c.bytes, 2, 8, 5);
        cases.push_back(c);
    }
    {
        Case c{"new_id in the client range", {}};
        Header(c.bytes, 2, 48, 0);
        for (uint32_t v : {0u, 0u, 0u, 2u, 0x61u, 2u, 5u, 0u, 0u, 0u}) Put32(c.bytes, v);
        cases.push_back(c);
    }
    for (const Case& c : cases) {
        RawPeer peer;
        if (!peer.connection) return;
        peer.Write(c.bytes);
        const int r = peer.ReadAndDispatch();
        if (r != -1) ::rgui::test::Fail(__FILE__, __LINE__, std::string("accepted: ") + c.name);
        CHECK_EQ(peer.received.count, 0);
    }
}

RGUI_TEST("linux.wire.fds_and_id_reuse") {
    Pair p;
    if (!p.client) return;
    // Several fds in one flush arrive in order, each with its own message.
    int pipes[3][2];
    for (auto& fds : pipes) CHECK_EQ(pipe2(fds, O_CLOEXEC), 0);
    for (auto& fds : pipes) {
        rgui_wl_arg a;
        a.h = fds[0];
        CHECK_EQ(rgui_wl_send(p.client, p.clientThing, 5, &a), 0);
    }
    CHECK_EQ(p.Deliver(), 3);
    CHECK_EQ(p.received.fds.size(), 3u);
    for (size_t i = 0; i < p.received.fds.size() && i < 3; ++i) {
        CHECK(SameFile(p.received.fds[i], pipes[i][0]));
        close(p.received.fds[i]);
    }
    for (auto& fds : pipes) {
        close(fds[0]);
        close(fds[1]);
    }

    // Ids: a destroyed object's id returns only after delete_id.
    const uint32_t a = rgui_wl_new_object(p.client, &kThing, 1, nullptr, nullptr);
    CHECK_EQ(a, 3u);
    CHECK_EQ(rgui_wl_send(p.client, a, 2, nullptr), 0);  // destructor
    CHECK_EQ(rgui_wl_object_version(p.client, a), 0u);
    const uint32_t b = rgui_wl_new_object(p.client, &kThing, 1, nullptr, nullptr);
    CHECK_EQ(b, 4u);
    rgui_wl_arg del;
    del.u = a;
    CHECK_EQ(rgui_wl_send(p.server, RGUI_WL_DISPLAY_ID, 1, &del), 0);  // delete_id
    CHECK_EQ(rgui_wl_flush(p.server), 0);
    CHECK_EQ(rgui_wl_read(p.client), 1);
    CHECK_EQ(rgui_wl_dispatch(p.client), 1);
    CHECK_EQ(rgui_wl_new_object(p.client, &kThing, 1, nullptr, nullptr), a);
}

RGUI_TEST("linux.wire.random_mutations") {
    // Valid messages of every shape, then thousands of copies with a few bytes changed or the tail
    // cut: each is dispatched or refused, never read past (run under linux-asan).
    std::vector<uint8_t> stream;
    Header(stream, 2, 8 + 4 + 8 + 4 + 8, 1);
    Put32(stream, 6);
    PutBytes(stream, "hello\0\0\0", 8);
    Put32(stream, 5);
    PutBytes(stream, "\1\2\3\4\5\0\0\0", 8);
    Header(stream, 2, 8 + 4 * 3 + 8 + 4 * 2 + 4 + 4 + 4, 0);
    for (uint32_t v : {1u, 2u, 3u}) Put32(stream, v);
    Put32(stream, 2);
    PutBytes(stream, "x\0\0\0", 4);
    Put32(stream, 2);
    Put32(stream, RGUI_WL_SERVER_ID_START + 1);
    Put32(stream, 0);
    Put32(stream, 0);
    Put32(stream, 0);
    Header(stream, 2, 12, 9);
    Put32(stream, 7);
    Header(stream, 2, 8, 2);
    uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1103515245u + 12345u;
        return seed >> 8;
    };
    int dispatched = 0;
    int refused = 0;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        std::vector<uint8_t> bytes = stream;
        const int changes = 1 + static_cast<int>(next() % 3);
        for (int i = 0; i < changes; ++i) bytes[next() % bytes.size()] = static_cast<uint8_t>(next());
        if (next() % 4 == 0) bytes.resize(next() % bytes.size());
        // A message to wl_display would be a protocol error, which exits: skip those.
        bool display = false;
        for (size_t at = 0; at + 8 <= bytes.size();) {
            uint32_t id, word;
            std::memcpy(&id, &bytes[at], 4);
            std::memcpy(&word, &bytes[at + 4], 4);
            if (id == RGUI_WL_DISPLAY_ID) display = true;
            const size_t size = word >> 16;
            if (size < 8 || (size & 3) != 0) break;
            at += size;
        }
        if (display) continue;
        RawPeer peer;
        if (!peer.connection) return;
        if (!bytes.empty()) peer.Write(bytes);
        const int r = bytes.empty() ? 0 : peer.ReadAndDispatch();
        CHECK(r >= -1 && r <= 4);
        if (r < 0) ++refused;
        else dispatched += r;
        for (int fd : peer.received.fds) close(fd);
        if (peer.received.fd >= 0 && peer.received.fds.empty()) close(peer.received.fd);
    }
    CHECK(dispatched > 0);
    CHECK(refused > 0);
}

namespace {

// A server-created child whose event carries a fd, made by its parent's event.
extern const rgui_wl_interface kFdChild;
const rgui_wl_message kFdChildRequests[] = {
    {0, RGUI_WL_DESTRUCTOR, "destroy", "", nullptr},
};
const rgui_wl_message kFdChildEvents[] = {
    {0, 0, "fd", "h", nullptr},
};
const rgui_wl_interface kFdChild = {"test_fd_child", 1, 1, 1, kFdChildRequests, kFdChildEvents};
const rgui_wl_message kParentEvents[] = {
    {0, 0, "create", "n", &kFdChild},
};
const rgui_wl_interface kParent = {"test_parent", 1, 0, 1, nullptr, kParentEvents};

// Writes `bytes` with `fds` attached, as a compositor does.
bool WriteWithFds(int socket, const std::vector<uint8_t>& bytes, const std::vector<int>& fds) {
    iovec iov{const_cast<uint8_t*>(bytes.data()), bytes.size()};
    union {
        char buffer[CMSG_SPACE(sizeof(int) * 4)];
        cmsghdr align;
    } control{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (!fds.empty()) {
        msg.msg_control = control.buffer;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());
    }
    return sendmsg(socket, &msg, 0) == static_cast<ssize_t>(bytes.size());
}

} // namespace

RGUI_TEST("linux.wire.zombie_fds") {
    // A server object we destroyed still gets an fd event in flight: the zombie keeps its
    // interface, so that fd is taken and closed, and the next event gets its own.
    RawPeer peer;
    if (!peer.connection) return;
    const uint32_t parent = rgui_wl_new_object(peer.connection, &kParent, 1, nullptr, nullptr);
    const uint32_t child = RGUI_WL_SERVER_ID_START + 1;
    std::vector<uint8_t> m;
    Header(m, parent, 12, 0);
    Put32(m, child);
    peer.Write(m);
    CHECK_EQ(peer.ReadAndDispatch(), 1);
    CHECK_EQ(rgui_wl_object_version(peer.connection, child), 1u);
    CHECK_EQ(rgui_wl_send(peer.connection, child, 0, nullptr), 0);  // destroy: now a zombie
    CHECK_EQ(rgui_wl_object_version(peer.connection, child), 0u);

    int first[2];
    int second[2];
    CHECK_EQ(pipe2(first, O_CLOEXEC), 0);
    CHECK_EQ(pipe2(second, O_CLOEXEC), 0);
    m.clear();
    Header(m, child, 8, 0);         // to the zombie, carrying `first`
    Header(m, peer.thing, 8, 5);    // to a live object, carrying `second`
    CHECK(WriteWithFds(peer.raw, m, {first[0], second[0]}));
    CHECK_EQ(peer.ReadAndDispatch(), 2);
    CHECK_EQ(peer.received.fds.size(), 1u);
    if (!peer.received.fds.empty()) {
        CHECK(SameFile(peer.received.fds[0], second[0]));
        close(peer.received.fds[0]);
    }
    // The zombie's copy was closed: no descriptor but our own refers to `first`'s read end.
    int holders = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        // Read ends only: both ends of a pipe are the same inode.
        if (fcntl(fd, F_GETFD) >= 0 && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY && SameFile(fd, first[0])) ++holders;
    }
    CHECK_EQ(holders, 1);
    close(first[0]);
    close(first[1]);
    close(second[0]);
    close(second[1]);

    // An event for an object that never existed while fds are queued: nothing tells whose fds
    // they are, so the connection fails instead of handing them to a later event.
    RawPeer other;
    if (!other.connection) return;
    int third[2];
    CHECK_EQ(pipe2(third, O_CLOEXEC), 0);
    m.clear();
    Header(m, 999, 8, 0);
    CHECK(WriteWithFds(other.raw, m, {third[0]}));
    CHECK_EQ(other.ReadAndDispatch(), -1);
    close(third[0]);
    close(third[1]);
}
