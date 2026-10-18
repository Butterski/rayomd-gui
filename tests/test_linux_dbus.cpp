// SPDX-License-Identifier: Apache-2.0
// The D-Bus client and the portal: marshalling, strict unmarshalling, a fake bus for the
// connection, and (with RGUI_DBUS_TEST set, under dbus-run-session) a real bus with a fake
// portal service.
#include "runner.h"

#include "platform/linux/dbus_wire.h"
#include "platform/linux/portal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::string RenderAll(rgui_dbus_iter* it);

// One value as text: numbers, "strings", [arrays], (structs), {key:value}, <signature>value.
std::string RenderOne(rgui_dbus_iter* it) {
    char type = rgui_dbus_iter_type(it);
    switch (type) {
    case 'y': {
        uint8_t v;
        return rgui_dbus_get_byte(it, &v) ? std::to_string(v) : "!";
    }
    case 'b': {
        bool v;
        return rgui_dbus_get_bool(it, &v) ? (v ? "true" : "false") : "!";
    }
    case 'n': {
        int16_t v;
        return rgui_dbus_get_int16(it, &v) ? std::to_string(v) : "!";
    }
    case 'q': {
        uint16_t v;
        return rgui_dbus_get_uint16(it, &v) ? std::to_string(v) : "!";
    }
    case 'i': {
        int32_t v;
        return rgui_dbus_get_int32(it, &v) ? std::to_string(v) : "!";
    }
    case 'u': case 'h': {
        uint32_t v;
        return rgui_dbus_get_uint32(it, &v) ? std::to_string(v) : "!";
    }
    case 'x': {
        int64_t v;
        return rgui_dbus_get_int64(it, &v) ? std::to_string(v) : "!";
    }
    case 't': {
        uint64_t v;
        return rgui_dbus_get_uint64(it, &v) ? std::to_string(v) : "!";
    }
    case 'd': {
        double v;
        return rgui_dbus_get_double(it, &v) ? std::to_string(v) : "!";
    }
    case 's': case 'o': case 'g': {
        const char* s;
        uint32_t n;
        return rgui_dbus_get_string(it, &s, &n) ? "\"" + std::string(s, n) + "\"" : "!";
    }
    case 'a': case '(': case '{': case 'v': {
        rgui_dbus_iter sub;
        if (!rgui_dbus_recurse(it, &sub)) return "!";
        std::string inner;
        if (type == 'v') inner = "<" + std::string(sub.sig, static_cast<size_t>(sub.sig_end - sub.sig)) + ">";
        if (type == '{') {
            inner += RenderOne(&sub);
            inner += ":";
            inner += RenderOne(&sub);
        } else {
            inner += RenderAll(&sub);
        }
        if (!rgui_dbus_leave(it, &sub)) return "!";
        if (type == 'a') return "[" + inner + "]";
        if (type == '(') return "(" + inner + ")";
        if (type == '{') return "{" + inner + "}";
        return inner;
    }
    default:
        return "!";
    }
}

std::string RenderAll(rgui_dbus_iter* it) {
    std::string out;
    while (rgui_dbus_iter_type(it)) {
        if (!out.empty()) out += ",";
        out += RenderOne(it);
        if (it->bad) return out + "!";
    }
    return it->bad ? out + "!" : out;
}

// Set when a rendered body did not read cleanly (a failed read leaves the iterator bad).
bool gRenderFailed = false;

std::string RenderBody(const rgui_dbus_message& m) {
    rgui_dbus_iter it;
    rgui_dbus_body_iter(&m, &it);
    std::string out = RenderAll(&it);
    if (it.bad || it.pos != it.end) {
        gRenderFailed = true;
        out += "!failed";
    }
    return out;
}

std::vector<uint8_t> Bytes(const rgui_dbus_buf& buf) { return std::vector<uint8_t>(buf.data, buf.data + buf.size); }

// A signal carrying `body` (not checked by the encoder) as `signature`, encoded.
std::vector<uint8_t> SignalWith(const char* signature, const std::vector<uint8_t>& body) {
    rgui_dbus_message m{};
    m.type = RGUI_DBUS_SIGNAL;
    m.serial = 7;
    m.path = "/a/b";
    m.interface = "org.example.I";
    m.member = "M";
    m.signature = signature;
    m.body = body.data();
    m.body_size = static_cast<uint32_t>(body.size());
    rgui_dbus_buf out{};
    bool ok = rgui_dbus_encode_message(&out, &m);
    std::vector<uint8_t> bytes = ok ? Bytes(out) : std::vector<uint8_t>();
    rgui_dbus_buf_free(&out);
    return bytes;
}

bool Parses(const std::vector<uint8_t>& bytes) {
    rgui_dbus_message m;
    return !bytes.empty() && rgui_dbus_parse_message(bytes.data(), bytes.size(), &m);
}

bool BodyParses(const char* signature, const std::vector<uint8_t>& body) {
    return Parses(SignalWith(signature, body));
}

void PutU32At(std::vector<uint8_t>& bytes, size_t at, uint32_t v) {
    if (bytes.size() < at + 4) return;
    for (size_t i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// The body of the round-trip test: every type, nested containers and alignment cases.
constexpr const char* kAllSignature = "ybnqiuxtdsogaxva{sv}a(sa(us))(y(yt))ay";

void WriteAll(rgui_dbus_buf* b) {
    rgui_dbus_put_byte(b, 0xAB);
    rgui_dbus_put_bool(b, true);
    rgui_dbus_put_int16(b, -2);
    rgui_dbus_put_uint16(b, 65535);
    rgui_dbus_put_int32(b, -123456);
    rgui_dbus_put_uint32(b, 0xDEADBEEFu);
    rgui_dbus_put_int64(b, INT64_MIN);
    rgui_dbus_put_uint64(b, UINT64_MAX);
    rgui_dbus_put_double(b, 1.5);
    rgui_dbus_put_string(b, "h\xC3\xA9llo");
    rgui_dbus_put_string(b, "/a/b_c");
    rgui_dbus_put_signature(b, "a{sv}");
    rgui_dbus_array empty = rgui_dbus_open_array(b, 'x');  // empty, still padded to 8
    rgui_dbus_close_array(b, empty);
    rgui_dbus_put_signature(b, "(yt)");  // variant
    rgui_dbus_open_struct(b);
    rgui_dbus_put_byte(b, 9);
    rgui_dbus_put_uint64(b, 10);
    rgui_dbus_array dict = rgui_dbus_open_array(b, '{');
    rgui_dbus_open_struct(b);
    rgui_dbus_put_string(b, "k1");
    rgui_dbus_put_signature(b, "u");
    rgui_dbus_put_uint32(b, 7);
    rgui_dbus_open_struct(b);
    rgui_dbus_put_string(b, "k2");
    rgui_dbus_put_signature(b, "as");
    rgui_dbus_array strings = rgui_dbus_open_array(b, 's');
    rgui_dbus_put_string(b, "x");
    rgui_dbus_put_string(b, "yz");
    rgui_dbus_close_array(b, strings);
    rgui_dbus_close_array(b, dict);
    rgui_dbus_array filters = rgui_dbus_open_array(b, '(');
    rgui_dbus_open_struct(b);
    rgui_dbus_put_string(b, "Markdown");
    rgui_dbus_array globs = rgui_dbus_open_array(b, '(');
    rgui_dbus_open_struct(b);
    rgui_dbus_put_uint32(b, 0);
    rgui_dbus_put_string(b, "*.md");
    rgui_dbus_open_struct(b);
    rgui_dbus_put_uint32(b, 1);
    rgui_dbus_put_string(b, "text/plain");
    rgui_dbus_close_array(b, globs);
    rgui_dbus_close_array(b, filters);
    rgui_dbus_open_struct(b);
    rgui_dbus_put_byte(b, 1);
    rgui_dbus_open_struct(b);
    rgui_dbus_put_byte(b, 2);
    rgui_dbus_put_uint64(b, 3);
    rgui_dbus_array bytes = rgui_dbus_open_array(b, 'y');
    rgui_dbus_put_byte(b, '/');
    rgui_dbus_put_byte(b, 0);
    rgui_dbus_close_array(b, bytes);
}

constexpr const char* kAllRendered =
    "171,true,-2,65535,-123456,3735928559,-9223372036854775808,18446744073709551615,1.500000,"
    "\"h\xC3\xA9llo\",\"/a/b_c\",\"a{sv}\",[],<(yt)>(9,10),[{\"k1\":<u>7},{\"k2\":<as>[\"x\",\"yz\"]}],"
    "[(\"Markdown\",[(0,\"*.md\"),(1,\"text/plain\")])],(1,(2,3)),[47,0]";

} // namespace

RGUI_TEST("linux.dbus_marshal_roundtrip") {
    rgui_dbus_buf b{};
    WriteAll(&b);
    CHECK(!b.failed);
    rgui_dbus_iter it;
    rgui_dbus_iter_init(&it, b.data, b.size, kAllSignature, false);
    CHECK_EQ(RenderAll(&it), std::string(kAllRendered));
    CHECK_EQ(it.pos, b.size);
    rgui_dbus_buf_free(&b);

    // Alignment is to the value's natural boundary, with zero padding: y then t is 1 + 7 + 8.
    rgui_dbus_buf a{};
    rgui_dbus_put_byte(&a, 1);
    rgui_dbus_put_uint64(&a, 0x0102030405060708ull);
    CHECK_EQ(a.size, size_t(16));
    bool zero = true;
    for (size_t i = 1; i < 8; ++i) zero = zero && a.data[i] == 0;
    CHECK(zero);
    CHECK_EQ(a.data[8], 0x08);  // little-endian
    CHECK_EQ(a.data[15], 0x01);
    // An empty array of 8-aligned elements: length 0 at 16, padding to 24 not counted.
    rgui_dbus_array empty = rgui_dbus_open_array(&a, '(');
    rgui_dbus_close_array(&a, empty);
    CHECK_EQ(a.size, size_t(24));
    CHECK_EQ(a.data[16], 0);
    rgui_dbus_buf_free(&a);

    // Strings must be UTF-8 without NUL; the writer fails instead of making a bad message.
    rgui_dbus_buf s{};
    rgui_dbus_put_string(&s, "\xFF");
    CHECK(s.failed);
    rgui_dbus_buf_free(&s);
    rgui_dbus_put_string_n(&s, "a\0b", 3);
    CHECK(s.failed);
    rgui_dbus_buf_free(&s);
    rgui_dbus_put_signature(&s, "a{vs}");  // a dict key must be basic
    CHECK(s.failed);
    rgui_dbus_buf_free(&s);
}

RGUI_TEST("linux.dbus_message_layout") {
    // org.freedesktop.DBus.Hello, as every client sends it first: 128 bytes, fields array of 109.
    rgui_dbus_message m{};
    m.type = RGUI_DBUS_METHOD_CALL;
    m.serial = 1;
    m.destination = "org.freedesktop.DBus";
    m.path = "/org/freedesktop/DBus";
    m.interface = "org.freedesktop.DBus";
    m.member = "Hello";
    rgui_dbus_buf out{};
    CHECK(rgui_dbus_encode_message(&out, &m));
    std::vector<uint8_t> hello = Bytes(out);
    rgui_dbus_buf_free(&out);
    CHECK_EQ(hello.size(), size_t(128));
    if (hello.size() != 128) return;
    const uint8_t fixed[16] = {'l', 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 109, 0, 0, 0};
    CHECK(std::memcmp(hello.data(), fixed, 16) == 0);
    const uint8_t path[8] = {1, 1, 'o', 0, 21, 0, 0, 0};
    CHECK(std::memcmp(hello.data() + 16, path, 8) == 0);
    CHECK(std::memcmp(hello.data() + 24, "/org/freedesktop/DBus", 22) == 0);
    CHECK_EQ(hello[48], 2);  // INTERFACE, aligned to 8
    CHECK_EQ(hello[80], 3);  // MEMBER
    CHECK(std::memcmp(hello.data() + 88, "Hello", 6) == 0);
    CHECK_EQ(hello[96], 6);  // DESTINATION
    CHECK_EQ(rgui_dbus_message_size(hello.data()), size_t(128));

    rgui_dbus_message parsed;
    CHECK(rgui_dbus_parse_message(hello.data(), hello.size(), &parsed));
    CHECK_EQ(std::string(parsed.member), "Hello");
    CHECK_EQ(std::string(parsed.signature), "");

    // A duplicate field (INTERFACE's code turned into MEMBER's) and a field of the wrong type
    // (PATH as a string) are rejected.
    std::vector<uint8_t> dup = hello;
    dup[48] = 3;
    CHECK(!Parses(dup));
    std::vector<uint8_t> wrongType = hello;
    wrongType[18] = 's';
    CHECK(!Parses(wrongType));
    // A call without MEMBER: the field's code turned into an unknown one, which is skipped.
    std::vector<uint8_t> noMember = hello;
    noMember[80] = 200;
    CHECK(!Parses(noMember));

    // Every header field and a body survive a round trip, and big-endian input is read.
    rgui_dbus_buf body{};
    WriteAll(&body);
    rgui_dbus_message r{};
    r.type = RGUI_DBUS_ERROR;
    r.flags = RGUI_DBUS_NO_AUTO_START;
    r.serial = 0x01020304;
    r.reply_serial = 99;
    r.error_name = "org.example.Error.Bad";
    r.destination = ":1.5";
    r.sender = ":1.6";
    r.signature = kAllSignature;
    r.body = body.data;
    r.body_size = static_cast<uint32_t>(body.size);
    CHECK(rgui_dbus_encode_message(&out, &r));
    rgui_dbus_message back;
    CHECK(rgui_dbus_parse_message(out.data, out.size, &back));
    CHECK_EQ(back.type, uint8_t(RGUI_DBUS_ERROR));
    CHECK_EQ(back.flags, uint8_t(RGUI_DBUS_NO_AUTO_START));
    CHECK_EQ(back.serial, 0x01020304u);
    CHECK_EQ(back.reply_serial, 99u);
    CHECK_EQ(std::string(back.error_name), "org.example.Error.Bad");
    CHECK_EQ(std::string(back.destination), ":1.5");
    CHECK_EQ(std::string(back.sender), ":1.6");
    CHECK(back.path == nullptr);
    CHECK_EQ(RenderBody(back), std::string(kAllRendered));
    rgui_dbus_buf_free(&out);
    rgui_dbus_buf_free(&body);

    // The same signal written big-endian by hand: header 'B', fields and body swapped.
    const uint8_t big[] = {
        'B', 4, 0, 1, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0, 56,
        1, 1, 'o', 0, 0, 0, 0, 2, '/', 'a', 0, 0, 0, 0, 0, 0,                 // PATH "/a"
        2, 1, 's', 0, 0, 0, 0, 3, 'o', '.', 'I', 0, 0, 0, 0, 0,               // INTERFACE "o.I"
        3, 1, 's', 0, 0, 0, 0, 1, 'M', 0, 0, 0, 0, 0, 0, 0,                   // MEMBER "M"
        8, 1, 'g', 0, 2, 'q', 'u', 0,                                         // SIGNATURE "qu"
        0x12, 0x34, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF,                             // q, padding, u
    };
    rgui_dbus_message be;
    CHECK(rgui_dbus_parse_message(big, sizeof big, &be));
    CHECK(be.big_endian);
    CHECK_EQ(RenderBody(be), std::string("4660,3735928559"));
}

RGUI_TEST("linux.dbus_malformed") {
    using V = std::vector<uint8_t>;
    auto body = [](std::initializer_list<int> bytes) {
        V v;
        for (int b : bytes) v.push_back(static_cast<uint8_t>(b));
        return v;
    };
    // The baseline parses.
    CHECK(BodyParses("s", body({3, 0, 0, 0, 'a', 'b', 'c', 0})));
    // Strings: no NUL at the end, a length past the end, invalid UTF-8 (an overlong NUL), a
    // NUL inside, a surrogate.
    CHECK(!BodyParses("s", body({3, 0, 0, 0, 'a', 'b', 'c', 'x'})));
    CHECK(!BodyParses("s", body({0xF0, 0xFF, 0xFF, 0xFF, 'a', 'b', 'c', 0})));
    CHECK(!BodyParses("s", body({2, 0, 0, 0, 0xC0, 0x80, 0})));
    CHECK(!BodyParses("s", body({3, 0, 0, 0, 'a', 0, 'c', 0})));
    CHECK(!BodyParses("s", body({3, 0, 0, 0, 0xED, 0xA0, 0x80, 0})));
    // Object paths.
    CHECK(BodyParses("o", body({1, 0, 0, 0, '/', 0})));
    CHECK(!BodyParses("o", body({3, 0, 0, 0, 'a', '/', 'b', 0})));
    CHECK(!BodyParses("o", body({4, 0, 0, 0, '/', 'a', '/', '/', 0})));
    CHECK(!BodyParses("o", body({3, 0, 0, 0, '/', 'a', '/', 0})));
    CHECK(!BodyParses("o", body({3, 0, 0, 0, '/', 'a', '-', 0})));
    // Booleans are 0 or 1.
    CHECK(BodyParses("b", body({1, 0, 0, 0})));
    CHECK(!BodyParses("b", body({2, 0, 0, 0})));
    // Padding must be zero.
    CHECK(BodyParses("yt", body({1, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8})));
    CHECK(!BodyParses("yt", body({1, 0, 0, 5, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8})));
    // Arrays: an element cut by the array's end, an array past the body, a length over 64 MiB.
    CHECK(BodyParses("ai", body({4, 0, 0, 0, 1, 0, 0, 0})));
    CHECK(!BodyParses("ai", body({5, 0, 0, 0, 1, 0, 0, 0, 2})));
    CHECK(!BodyParses("ai", body({8, 0, 0, 0, 1, 0, 0, 0})));
    CHECK(!BodyParses("ay", body({0, 0, 0, 0x04 + 0x01, 1})));
    // Variants hold exactly one valid complete type.
    CHECK(BodyParses("v", body({1, 'y', 0, 5})));
    CHECK(!BodyParses("v", body({2, 'y', 'y', 0, 5, 5})));
    CHECK(!BodyParses("v", body({0, 0})));
    CHECK(!BodyParses("v", body({1, 'a', 0, 0})));
    CHECK(!BodyParses("v", body({1, 'y', 1, 5})));  // signature not NUL-terminated
    // A signature value must be valid.
    CHECK(!BodyParses("g", body({1, '(', 0})));
    // Bytes left over after the signature's values, and a body shorter than its signature.
    CHECK(!BodyParses("y", body({1, 2})));
    CHECK(!BodyParses("u", body({1, 2})));
    CHECK(!BodyParses("", body({1})));

    // Variant nesting: 64 levels are allowed, 65 are not.
    for (int levels : {64, 65}) {
        V v;
        for (int i = 0; i < levels; ++i) {
            v.push_back(1);
            v.push_back(static_cast<uint8_t>(i + 1 < levels ? 'v' : 'y'));
            v.push_back(0);
        }
        v.push_back(42);
        CHECK_EQ(BodyParses("v", v), levels == 64);
    }
    // Signature nesting: 32 arrays or structs, not 33.
    std::string arrays32 = std::string(32, 'a') + "y";
    std::string arrays33 = std::string(33, 'a') + "y";
    std::string structs32 = std::string(32, '(') + "y" + std::string(32, ')');
    std::string structs33 = std::string(33, '(') + "y" + std::string(33, ')');
    CHECK(rgui_dbus_signature_valid(arrays32.data(), arrays32.size(), true));
    CHECK(!rgui_dbus_signature_valid(arrays33.data(), arrays33.size(), true));
    CHECK(rgui_dbus_signature_valid(structs32.data(), structs32.size(), true));
    CHECK(!rgui_dbus_signature_valid(structs33.data(), structs33.size(), true));
    for (const char* bad : {"()", "a", "{sv}", "a{vs}", "a{s}", "a{svv}", "(y", "y)", "z", "a{sv"}) {
        CHECK(!rgui_dbus_signature_valid(bad, std::strlen(bad), false));
    }
    std::string tooLong(256, 'y');
    CHECK(!rgui_dbus_signature_valid(tooLong.data(), tooLong.size(), false));

    // The fixed header: endianness, version, sizes over the limits.
    uint8_t head[16] = {'l', 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    CHECK_EQ(rgui_dbus_message_size(head), size_t(16));
    head[0] = 'x';
    CHECK_EQ(rgui_dbus_message_size(head), size_t(0));
    head[0] = 'l';
    head[3] = 2;
    CHECK_EQ(rgui_dbus_message_size(head), size_t(0));
    head[3] = 1;
    head[7] = 0x08;  // body of 2^27
    CHECK_EQ(rgui_dbus_message_size(head), size_t(0));
    head[7] = 0;
    head[15] = 0x04;  // fields of 2^26 + ...
    head[12] = 1;
    CHECK_EQ(rgui_dbus_message_size(head), size_t(0));

    // Messages: serial 0, a return without REPLY_SERIAL, unix fds we never negotiated.
    V signal = SignalWith("s", body({3, 0, 0, 0, 'a', 'b', 'c', 0}));
    CHECK(Parses(signal));
    V zeroSerial = signal;
    PutU32At(zeroSerial, 8, 0);
    CHECK(!Parses(zeroSerial));
    rgui_dbus_message ret{};
    ret.type = RGUI_DBUS_METHOD_RETURN;
    ret.serial = 3;
    ret.reply_serial = 0;  // so the field is left out
    rgui_dbus_buf out{};
    CHECK(rgui_dbus_encode_message(&out, &ret));
    CHECK(!Parses(Bytes(out)));
    rgui_dbus_buf_free(&out);

    // Truncation: a prefix never parses.
    for (size_t n = 0; n < signal.size(); ++n) {
        rgui_dbus_message m;
        CHECK(!rgui_dbus_parse_message(signal.data(), n, &m));
    }

    // Mutations of a message with every type: whatever parses must also iterate cleanly; the
    // sanitizer build catches any read out of bounds.
    rgui_dbus_buf all{};
    WriteAll(&all);
    V message = SignalWith(kAllSignature, Bytes(all));
    rgui_dbus_buf_free(&all);
    CHECK(Parses(message));
    size_t accepted = 0;
    auto tryBytes = [&](const V& bytes) {
        rgui_dbus_message m;
        size_t size = rgui_dbus_message_size(bytes.data());
        if (size == 0 || size > bytes.size()) return;
        if (!rgui_dbus_parse_message(bytes.data(), size, &m)) return;
        ++accepted;
        gRenderFailed = false;
        RenderBody(m);
        CHECK(!gRenderFailed);
    };
    for (size_t i = 0; i < message.size(); ++i) {
        for (uint8_t mask : {0x01, 0x08, 0x80, 0xFF}) {
            V mutated = message;
            mutated[i] ^= mask;
            tryBytes(mutated);
        }
    }
    uint32_t seed = 12345;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int round = 0; round < 20000; ++round) {
        V mutated = message;
        int changes = 1 + static_cast<int>(next() % 4);
        for (int c = 0; c < changes; ++c) mutated[next() % mutated.size()] = static_cast<uint8_t>(next());
        // Sometimes shorten the message and its declared body to match.
        if (next() % 4 == 0) {
            size_t cut = next() % 64;
            uint32_t bodySize = mutated[4] | mutated[5] << 8 | mutated[6] << 16 | static_cast<uint32_t>(mutated[7]) << 24;
            if (cut < mutated.size() && cut <= bodySize) {
                mutated.resize(mutated.size() - cut);
                PutU32At(mutated, 4, bodySize - static_cast<uint32_t>(cut));
            }
        }
        tryBytes(mutated);
    }
    CHECK(accepted > 0);  // some mutations (inside string bytes, numbers) stay valid
}

RGUI_TEST("linux.dbus_iter_misuse") {
    rgui_dbus_buf b{};
    rgui_dbus_put_uint32(&b, 5);
    rgui_dbus_put_string(&b, "x");
    rgui_dbus_iter it;
    rgui_dbus_iter_init(&it, b.data, b.size, "us", false);
    int32_t wrong;
    CHECK(!rgui_dbus_get_int32(&it, &wrong));  // 'u' is not 'i'
    CHECK(it.bad);
    CHECK_EQ(rgui_dbus_iter_type(&it), '\0');

    // Leaving a container skips what was not read.
    rgui_dbus_buf c{};
    rgui_dbus_open_struct(&c);
    rgui_dbus_put_uint32(&c, 1);
    rgui_dbus_put_string(&c, "skip me");
    rgui_dbus_put_byte(&c, 77);
    rgui_dbus_iter top;
    rgui_dbus_iter_init(&top, c.data, c.size, "(us)y", false);
    rgui_dbus_iter sub;
    CHECK(rgui_dbus_recurse(&top, &sub));
    uint32_t first;
    CHECK(rgui_dbus_get_uint32(&sub, &first));
    CHECK(rgui_dbus_leave(&top, &sub));
    uint8_t last = 0;
    CHECK(rgui_dbus_get_byte(&top, &last));
    CHECK_EQ(last, 77);
    CHECK_EQ(rgui_dbus_iter_type(&top), '\0');
    // An invalid signature makes the iterator bad from the start.
    rgui_dbus_iter_init(&top, c.data, c.size, "(us", false);
    CHECK(top.bad);
    rgui_dbus_buf_free(&b);
    rgui_dbus_buf_free(&c);
}

RGUI_TEST("linux.dbus_address") {
    sockaddr_un sa;
    unsigned length = 0;
    CHECK(rgui_dbus_parse_address("unix:path=/run/user/1000/bus", &sa, &length));
    CHECK_EQ(std::string(sa.sun_path), "/run/user/1000/bus");
    CHECK_EQ(length, unsigned(offsetof(sockaddr_un, sun_path) + 19));
    // The first usable entry wins; other transports are skipped; keys around it are ignored.
    CHECK(rgui_dbus_parse_address("tcp:host=localhost,port=1;unix:guid=00,abstract=/tmp/dbus-AbC;unix:path=/x", &sa,
                                  &length));
    CHECK_EQ(sa.sun_path[0], '\0');
    CHECK(std::memcmp(sa.sun_path + 1, "/tmp/dbus-AbC", 13) == 0);
    CHECK_EQ(length, unsigned(offsetof(sockaddr_un, sun_path) + 14));
    CHECK(rgui_dbus_parse_address("unix:path=/tmp/a%20b%2c", &sa, &length));
    CHECK_EQ(std::string(sa.sun_path), "/tmp/a b,");
    CHECK(!rgui_dbus_parse_address("unix:path=/x%2", &sa, &length));
    CHECK(!rgui_dbus_parse_address("unix:path=/x%00", &sa, &length));
    CHECK(!rgui_dbus_parse_address("unix:tmpdir=/tmp", &sa, &length));
    CHECK(!rgui_dbus_parse_address("unix:path=", &sa, &length));
    CHECK(!rgui_dbus_parse_address("", &sa, &length));
    std::string tooLong = "unix:path=/" + std::string(120, 'a');
    CHECK(!rgui_dbus_parse_address(tooLong.c_str(), &sa, &length));

    // Without DBUS_SESSION_BUS_ADDRESS: $XDG_RUNTIME_DIR/bus, escaped for the address syntax.
    const char* savedAddress = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    const char* savedRuntime = std::getenv("XDG_RUNTIME_DIR");
    std::string keepAddress = savedAddress ? savedAddress : "";
    std::string keepRuntime = savedRuntime ? savedRuntime : "";
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    char address[512];
    CHECK(rgui_dbus_session_address(address, sizeof address));
    CHECK_EQ(std::string(address), "unix:path=/run/user/1000/bus");
    setenv("XDG_RUNTIME_DIR", "/tmp/a,b;c", 1);
    CHECK(rgui_dbus_session_address(address, sizeof address));
    CHECK(rgui_dbus_parse_address(address, &sa, &length));
    CHECK_EQ(std::string(sa.sun_path), "/tmp/a,b;c/bus");
    unsetenv("XDG_RUNTIME_DIR");
    CHECK(!rgui_dbus_session_address(address, sizeof address));
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:abstract=/tmp/x", 1);
    CHECK(rgui_dbus_session_address(address, sizeof address));
    CHECK_EQ(std::string(address), "unix:abstract=/tmp/x");
    if (savedAddress) {
        setenv("DBUS_SESSION_BUS_ADDRESS", keepAddress.c_str(), 1);
    } else {
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
    }
    if (savedRuntime) setenv("XDG_RUNTIME_DIR", keepRuntime.c_str(), 1);
}

RGUI_TEST("linux.dbus_file_uri") {
    using rgui::dbus::FileUriToPath;
    std::string path;
    CHECK(FileUriToPath("file:///home/u/My%20Notes/a%2Bb.md", path));
    CHECK_EQ(path, std::string("/home/u/My Notes/a+b.md"));
    CHECK(FileUriToPath("file://localhost/tmp/x.pdf", path));
    CHECK_EQ(path, std::string("/tmp/x.pdf"));
    CHECK(FileUriToPath("FILE:///tmp/%C5%BC.md", path));
    CHECK_EQ(path, std::string("/tmp/\xC5\xBC.md"));
    path = "unchanged";
    CHECK(!FileUriToPath("file://host/tmp/x", path));
    CHECK(!FileUriToPath("https://example.com/x", path));
    CHECK(!FileUriToPath("file:///tmp/%00x", path));
    CHECK(!FileUriToPath("file:///tmp/%4", path));
    CHECK(!FileUriToPath("file:///tmp/%zz", path));
    CHECK(!FileUriToPath("file://", path));
    CHECK_EQ(path, std::string("unchanged"));
    CHECK_EQ(rgui::dbus::RequestPath(":1.42", "rayomd_gui_1"),
             std::string("/org/freedesktop/portal/desktop/request/1_42/rayomd_gui_1"));
}

RGUI_TEST("linux.dbus_portal_offline") {
    // Without a bus: dialogs are Unavailable (the app asks for the path), settings keep the
    // defaults (dark, scale 1).
    rgui::dbus::Portal portal;
    CHECK(!portal.ConnectTo("unix:path=/nonexistent/rgui-bus", 200));
    CHECK_EQ(portal.Fd(), -1);
    int answers = 0;
    portal.ShowFileDialog(rgui::FileDialog::OpenDocument, "", [&](rgui::FileDialogResult r) {
        ++answers;
        CHECK_EQ(r.kind, rgui::FileDialogResult::Unavailable);
    });
    portal.ReadAppearance([&](rgui::dbus::Appearance a) {
        ++answers;
        CHECK(a.dark);
        CHECK_EQ(a.textScale, 1.0);
    });
    CHECK_EQ(answers, 2);
}

namespace {

// A bus of one connection on a socket file: checks the client's authentication bytes, answers
// Hello, then follows a script.
struct FakeBus {
    std::string dir;
    std::string socketPath;
    int listener = -1;
    std::string authLine;
    std::thread thread;
    int okDelayMs = 0;  // a slow bus: OK comes this late

    bool Start(std::function<void(int fd)> script) {
        char temp[] = "/tmp/rgui-fakebus-XXXXXX";
        if (!mkdtemp(temp)) return false;
        dir = temp;
        socketPath = dir + "/bus";
        listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, socketPath.c_str(), sizeof sa.sun_path - 1);
        if (bind(listener, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || listen(listener, 1) != 0) return false;
        timeval limit{5, 0};  // accept gives up when no client comes
        setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
        thread = std::thread([this, script] {
            int fd = accept(listener, nullptr, nullptr);
            if (fd < 0) return;
            timeval tv{5, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            std::string line;
            char c;
            while (recv(fd, &c, 1, 0) == 1) {
                line.push_back(c);
                if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0) break;
            }
            authLine = line;
            if (okDelayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(okDelayMs));
            static const char ok[] = "OK 0123456789abcdef0123456789abcdef\r\n";
            send(fd, ok, sizeof ok - 1, MSG_NOSIGNAL);
            line.clear();
            while (recv(fd, &c, 1, 0) == 1) {
                line.push_back(c);
                if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0) break;
            }
            if (line == "BEGIN\r\n") script(fd);
            close(fd);
        });
        return true;
    }

    ~FakeBus() {
        if (thread.joinable()) thread.join();
        if (listener >= 0) close(listener);
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    }
};

// Reads one whole message from a blocking socket.
bool ReadMessage(int fd, std::vector<uint8_t>& bytes, rgui_dbus_message& m) {
    bytes.assign(16, 0);
    size_t got = 0;
    while (got < 16) {
        ssize_t n = recv(fd, bytes.data() + got, 16 - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    size_t size = rgui_dbus_message_size(bytes.data());
    if (size == 0) return false;
    bytes.resize(size);
    while (got < size) {
        ssize_t n = recv(fd, bytes.data() + got, size - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return rgui_dbus_parse_message(bytes.data(), bytes.size(), &m);
}

void SendReply(int fd, uint32_t replySerial, const char* signature, const rgui_dbus_buf* body) {
    rgui_dbus_message r{};
    r.type = RGUI_DBUS_METHOD_RETURN;
    r.serial = 1000 + replySerial;
    r.reply_serial = replySerial;
    r.signature = signature;
    r.body = body ? body->data : nullptr;
    r.body_size = body ? static_cast<uint32_t>(body->size) : 0;
    rgui_dbus_buf out{};
    rgui_dbus_encode_message(&out, &r);
    send(fd, out.data, out.size, MSG_NOSIGNAL);
    rgui_dbus_buf_free(&out);
}

struct CallResult {
    int calls = 0;
    bool gotNull = false;
    std::string body;
};

void RecordReply(void* user, const rgui_dbus_message* reply) {
    auto* r = static_cast<CallResult*>(user);
    ++r->calls;
    r->gotNull = reply == nullptr;
    if (reply) r->body = RenderBody(*reply);
}

rgui_dbus_message Call(const char* member) {
    rgui_dbus_message m{};
    m.destination = "org.example";
    m.path = "/";
    m.interface = "org.example.I";
    m.member = member;
    return m;
}

int64_t Ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

RGUI_TEST("linux.dbus_fake_bus") {
    // Authentication, Hello, a reply routed by serial among several pending calls, a call that
    // times out, and a stream that stops being D-Bus closes the connection and fails what waits.
    FakeBus bus;
    CHECK(bus.Start([](int fd) {
        std::vector<uint8_t> bytes;
        rgui_dbus_message m;
        if (!ReadMessage(fd, bytes, m) || std::strcmp(m.member, "Hello") != 0) return;
        rgui_dbus_buf name{};
        rgui_dbus_put_string(&name, ":1.7");
        SendReply(fd, m.serial, "s", &name);
        rgui_dbus_buf_free(&name);
        // Three calls: answer the second only, wait out the first's timeout, then break.
        std::vector<uint8_t> b1, b2, b3;
        rgui_dbus_message m1, m2, m3;
        if (!ReadMessage(fd, b1, m1) || !ReadMessage(fd, b2, m2) || !ReadMessage(fd, b3, m3)) return;
        rgui_dbus_buf answer{};
        rgui_dbus_put_string(&answer, m2.member);
        SendReply(fd, m2.serial, "s", &answer);
        rgui_dbus_buf_free(&answer);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        static const char garbage[16] = {'X', 1, 0, 1};
        send(fd, garbage, sizeof garbage, MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }));
    std::string address = "unix:path=" + bus.socketPath;
    rgui_dbus_conn* conn = rgui_dbus_open(address.c_str(), 3000);
    CHECK(conn != nullptr);
    if (!conn) return;
    char hexUid[64] = {0};
    std::string uid = std::to_string(getuid());
    std::string expectedAuth = std::string(1, '\0') + "AUTH EXTERNAL ";
    for (char c : uid) {
        std::snprintf(hexUid, sizeof hexUid, "%02x", static_cast<unsigned char>(c));
        expectedAuth += hexUid;
    }
    expectedAuth += "\r\n";
    CHECK(bus.authLine == expectedAuth);
    CHECK_EQ(std::string(rgui_dbus_unique_name(conn)), ":1.7");

    CallResult r1, r2, r3;
    rgui_dbus_message c1 = Call("First");
    rgui_dbus_message c2 = Call("Second");
    rgui_dbus_message c3 = Call("Third");
    int64_t start = Ms();
    CHECK(rgui_dbus_call(conn, &c1, 150, RecordReply, &r1) != 0);
    CHECK(rgui_dbus_call(conn, &c2, 5000, RecordReply, &r2) != 0);
    CHECK(rgui_dbus_call(conn, &c3, 5000, RecordReply, &r3) != 0);
    while (Ms() - start < 3000 && (r1.calls == 0 || r2.calls == 0)) rgui_dbus_wait(conn, 50);
    CHECK_EQ(r2.calls, 1);
    CHECK_EQ(r2.body, std::string("\"Second\""));
    CHECK_EQ(r1.calls, 1);
    CHECK(r1.gotNull);
    int64_t timedOut = Ms() - start;
    CHECK(timedOut >= 140 && timedOut < 1000);
    CHECK_EQ(r3.calls, 0);
    // The garbage arrives: the connection closes and the third call gets its NULL.
    bool alive = true;
    while (alive && Ms() - start < 4000) alive = rgui_dbus_wait(conn, 100);
    CHECK(!alive);
    CHECK(!rgui_dbus_connected(conn));
    CHECK_EQ(r3.calls, 1);
    CHECK(r3.gotNull);
    CHECK_EQ(rgui_dbus_call(conn, &c1, 100, RecordReply, &r1), 0u);
    rgui_dbus_close(conn);
}

namespace {

// A signal of `bodyBytes` bytes (an 'ay'), from the fake bus.
std::vector<uint8_t> BigSignal(size_t bodyBytes) {
    rgui_dbus_buf body{};
    rgui_dbus_array bytes = rgui_dbus_open_array(&body, 'y');
    for (size_t i = 0; i + 4 < bodyBytes; ++i) rgui_dbus_put_byte(&body, static_cast<uint8_t>(i));
    rgui_dbus_close_array(&body, bytes);
    rgui_dbus_message m{};
    m.type = RGUI_DBUS_SIGNAL;
    m.serial = 77;
    m.path = "/org/example";
    m.interface = "org.example.Big";
    m.member = "Big";
    m.signature = "ay";
    m.body = body.data;
    m.body_size = static_cast<uint32_t>(body.size);
    rgui_dbus_buf out{};
    rgui_dbus_encode_message(&out, &m);
    std::vector<uint8_t> message(out.data, out.data + out.size);
    rgui_dbus_buf_free(&out);
    rgui_dbus_buf_free(&body);
    return message;
}

bool SendAllBytes(int fd, const std::vector<uint8_t>& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        ssize_t n = send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

RGUI_TEST("linux.dbus_big_messages") {
    // A 2 MiB message is skipped as it arrives, never buffered; an unwanted 512 KiB signal is
    // dropped by its header; the reply after them still arrives, and the input buffer gives its
    // memory back once drained.
    FakeBus bus;
    CHECK(bus.Start([](int fd) {
        std::vector<uint8_t> bytes;
        rgui_dbus_message m;
        if (!ReadMessage(fd, bytes, m) || std::strcmp(m.member, "Hello") != 0) return;
        rgui_dbus_buf name{};
        rgui_dbus_put_string(&name, ":1.9");
        SendReply(fd, m.serial, "s", &name);
        rgui_dbus_buf_free(&name);
        if (!ReadMessage(fd, bytes, m)) return;
        if (!SendAllBytes(fd, BigSignal(2u << 20)) || !SendAllBytes(fd, BigSignal(512u << 10))) return;
        rgui_dbus_buf answer{};
        rgui_dbus_put_string(&answer, "after");
        SendReply(fd, m.serial, "s", &answer);
        rgui_dbus_buf_free(&answer);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }));
    std::string address = "unix:path=" + bus.socketPath;
    rgui_dbus_conn* conn = rgui_dbus_open(address.c_str(), 3000);
    CHECK(conn != nullptr);
    if (!conn) return;
    size_t biggest = 0;
    CallResult r;
    rgui_dbus_message c = Call("Wait");
    CHECK(rgui_dbus_call(conn, &c, 5000, RecordReply, &r) != 0);
    int64_t start = Ms();
    while (r.calls == 0 && Ms() - start < 4000) {
        rgui_dbus_wait(conn, 50);
        biggest = std::max(biggest, rgui_dbus_input_capacity(conn));
    }
    CHECK_EQ(r.calls, 1);
    CHECK_EQ(r.body, std::string("\"after\""));
    CHECK(rgui_dbus_connected(conn));
    CHECK(biggest <= size_t(RGUI_DBUS_MAX_INCOMING) + (64u << 10));  // the 2 MiB one never was held
    CHECK(rgui_dbus_input_capacity(conn) <= size_t(64u << 10));
    rgui_dbus_close(conn);
}

RGUI_TEST("linux.dbus_async_open") {
    // A slow bus: opening returns at once, calls queue behind Hello and are answered once the
    // handshake ends on the loop.
    {
        FakeBus bus;
        bus.okDelayMs = 400;
        CHECK(bus.Start([](int fd) {
            std::vector<uint8_t> bytes;
            rgui_dbus_message m;
            if (!ReadMessage(fd, bytes, m) || std::strcmp(m.member, "Hello") != 0) return;
            rgui_dbus_buf name{};
            rgui_dbus_put_string(&name, ":1.5");
            SendReply(fd, m.serial, "s", &name);
            rgui_dbus_buf_free(&name);
            if (!ReadMessage(fd, bytes, m)) return;
            rgui_dbus_buf answer{};
            rgui_dbus_put_string(&answer, m.member);
            SendReply(fd, m.serial, "s", &answer);
            rgui_dbus_buf_free(&answer);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }));
        std::string address = "unix:path=" + bus.socketPath;
        int64_t start = Ms();
        rgui_dbus_conn* conn = rgui_dbus_open_async(address.c_str(), 3000);
        CHECK(conn != nullptr);
        CHECK(Ms() - start < 100);
        if (!conn) return;
        CHECK(!rgui_dbus_ready(conn));
        CHECK_EQ(std::string(rgui_dbus_unique_name(conn)), std::string());
        CallResult r;
        rgui_dbus_message c = Call("Early");
        CHECK(rgui_dbus_call(conn, &c, 5000, RecordReply, &r) != 0);
        while (r.calls == 0 && Ms() - start < 4000) rgui_dbus_wait(conn, 50);
        CHECK(rgui_dbus_ready(conn));
        CHECK_EQ(std::string(rgui_dbus_unique_name(conn)), std::string(":1.5"));
        CHECK_EQ(r.body, std::string("\"Early\""));
        CHECK(Ms() - start >= 350);
        rgui_dbus_close(conn);
    }
    {
        // A bus that never finishes the handshake: closed after the timeout, calls get NULL.
        FakeBus bus;
        bus.okDelayMs = 1500;
        CHECK(bus.Start([](int) {}));
        std::string address = "unix:path=" + bus.socketPath;
        int64_t start = Ms();
        rgui_dbus_conn* conn = rgui_dbus_open_async(address.c_str(), 300);
        CHECK(conn != nullptr);
        if (!conn) return;
        CallResult r;
        rgui_dbus_message c = Call("Never");
        CHECK(rgui_dbus_call(conn, &c, 5000, RecordReply, &r) != 0);
        while (rgui_dbus_connected(conn) && Ms() - start < 3000) rgui_dbus_wait(conn, 50);
        CHECK(!rgui_dbus_connected(conn));
        CHECK(Ms() - start < 1000);
        CHECK_EQ(r.calls, 1);
        CHECK(r.gotNull);
        rgui_dbus_close(conn);
    }
}

// ---- A real bus (dbus-run-session) with a fake portal ------------------------------------------

namespace {

bool SessionTestsEnabled(const char* test) {
    if (std::getenv("RGUI_DBUS_TEST")) return true;
    std::printf("skip %s: RGUI_DBUS_TEST not set\n", test);
    return false;
}

// Dispatches the connections (and the portal) until `done` or 5 s.
bool Pump(std::initializer_list<rgui_dbus_conn*> conns, rgui::dbus::Portal* portal, const std::function<bool()>& done) {
    int64_t deadline = Ms() + 5000;
    while (!done()) {
        if (Ms() > deadline) return false;
        std::vector<pollfd> fds;
        for (rgui_dbus_conn* c : conns) fds.push_back({rgui_dbus_fd(c), rgui_dbus_poll_events(c), 0});
        if (portal && portal->Fd() >= 0) fds.push_back({portal->Fd(), portal->PollEvents(), 0});
        poll(fds.data(), fds.size(), 20);
        for (rgui_dbus_conn* c : conns) rgui_dbus_dispatch(c);
        if (portal) portal->Dispatch();
    }
    return true;
}

// The portal service, on its own connection.
struct FakePortal {
    // Settings.
    bool readOneUnknown = false;
    int readOneCalls = 0;
    int readCalls = 0;
    // FileChooser.
    bool errorReply = false;
    bool otherHandle = false;
    uint32_t responseCode = 0;
    std::string uri;
    std::string member;
    std::string parent;
    std::string title;
    std::string options;
    std::string token;
    std::string caller;  // the unique name of the last caller
    // Hold the Response until SendDeferred (a forger gets to speak first).
    bool defer = false;
    std::string deferredHandle;
    std::vector<uint8_t> deferredBody;
};

void SendResponse(rgui_dbus_conn* conn, const std::string& handle, const std::string& destination,
                  const uint8_t* body, size_t size) {
    rgui_dbus_message signal{};
    signal.type = RGUI_DBUS_SIGNAL;
    signal.path = handle.c_str();
    signal.interface = "org.freedesktop.portal.Request";
    signal.member = "Response";
    signal.destination = destination.empty() ? nullptr : destination.c_str();
    signal.signature = "ua{sv}";
    signal.body = body;
    signal.body_size = static_cast<uint32_t>(size);
    rgui_dbus_send(conn, &signal);
}

// A Response body: the code and, when `uri` is not empty, results {"uris": [uri]}.
std::vector<uint8_t> ResponseBody(uint32_t code, const std::string& uri) {
    rgui_dbus_buf body{};
    rgui_dbus_put_uint32(&body, code);
    rgui_dbus_array results = rgui_dbus_open_array(&body, '{');
    if (!uri.empty()) {
        rgui_dbus_open_struct(&body);
        rgui_dbus_put_string(&body, "uris");
        rgui_dbus_put_signature(&body, "as");
        rgui_dbus_array uris = rgui_dbus_open_array(&body, 's');
        rgui_dbus_put_string(&body, uri.c_str());
        rgui_dbus_close_array(&body, uris);
    }
    rgui_dbus_close_array(&body, results);
    std::vector<uint8_t> bytes(body.data, body.data + body.size);
    rgui_dbus_buf_free(&body);
    return bytes;
}

std::string FindStringOption(rgui_dbus_iter* options, const char* name) {
    rgui_dbus_iter dict;
    if (!rgui_dbus_recurse(options, &dict)) return "";
    while (rgui_dbus_iter_type(&dict)) {
        rgui_dbus_iter entry;
        const char* key;
        if (!rgui_dbus_recurse(&dict, &entry) || !rgui_dbus_get_string(&entry, &key, nullptr)) return "";
        if (std::strcmp(key, name) == 0) {
            rgui_dbus_iter value;
            const char* s;
            if (rgui_dbus_recurse(&entry, &value) && rgui_dbus_iter_type(&value) == 's' &&
                rgui_dbus_get_string(&value, &s, nullptr)) {
                return s;
            }
            return "";
        }
        if (!rgui_dbus_leave(&dict, &entry)) return "";
    }
    return "";
}

bool FakePortalHandler(void* user, rgui_dbus_conn* conn, const rgui_dbus_message* call) {
    auto* fake = static_cast<FakePortal*>(user);
    if (!call->interface) return false;
    std::string interface = call->interface;
    std::string member = call->member;
    if (interface == "org.freedesktop.portal.Settings" && (member == "ReadOne" || member == "Read")) {
        bool one = member == "ReadOne";
        (one ? fake->readOneCalls : fake->readCalls)++;
        fake->caller = call->sender;
        if (one && fake->readOneUnknown) return false;  // -> UnknownMethod, as an old portal
        rgui_dbus_iter it;
        rgui_dbus_body_iter(call, &it);
        const char* nameSpace;
        const char* key;
        if (std::strcmp(call->signature, "ss") != 0 || !rgui_dbus_get_string(&it, &nameSpace, nullptr) ||
            !rgui_dbus_get_string(&it, &key, nullptr)) {
            return rgui_dbus_reply_error(conn, call, "org.freedesktop.DBus.Error.InvalidArgs", "ss");
        }
        rgui_dbus_buf body{};
        bool scheme = std::strcmp(key, "color-scheme") == 0;
        if (!one) rgui_dbus_put_signature(&body, "v");  // Read: a variant inside the variant
        rgui_dbus_put_signature(&body, scheme ? "u" : "d");
        if (scheme) {
            rgui_dbus_put_uint32(&body, one ? 2 : 1);
        } else {
            rgui_dbus_put_double(&body, one ? 1.25 : 1.5);
        }
        bool ok = rgui_dbus_reply(conn, call, "v", body.data, static_cast<uint32_t>(body.size));
        rgui_dbus_buf_free(&body);
        return ok;
    }
    if (interface == "org.freedesktop.portal.FileChooser" && (member == "OpenFile" || member == "SaveFile")) {
        fake->member = member;
        fake->caller = call->sender;
        rgui_dbus_iter it;
        rgui_dbus_body_iter(call, &it);
        const char* parent;
        const char* title;
        if (std::strcmp(call->signature, "ssa{sv}") != 0 || !rgui_dbus_get_string(&it, &parent, nullptr) ||
            !rgui_dbus_get_string(&it, &title, nullptr)) {
            return rgui_dbus_reply_error(conn, call, "org.freedesktop.DBus.Error.InvalidArgs", "ssa{sv}");
        }
        fake->parent = parent;
        fake->title = title;
        rgui_dbus_iter copy = it;
        fake->token = FindStringOption(&copy, "handle_token");
        fake->options = RenderOne(&it);
        if (fake->errorReply) return rgui_dbus_reply_error(conn, call, "org.freedesktop.portal.Error.Failed", "no");
        std::string handle = fake->otherHandle ? "/org/freedesktop/portal/desktop/request/old/7"
                                               : rgui::dbus::RequestPath(call->sender, fake->token);
        rgui_dbus_buf reply{};
        rgui_dbus_put_string(&reply, handle.c_str());
        rgui_dbus_reply(conn, call, "o", reply.data, static_cast<uint32_t>(reply.size));
        rgui_dbus_buf_free(&reply);
        // The Response, sent to the caller as xdg-desktop-portal does.
        rgui_dbus_buf body{};
        rgui_dbus_put_uint32(&body, fake->responseCode);
        rgui_dbus_array results = rgui_dbus_open_array(&body, '{');
        if (!fake->uri.empty()) {
            rgui_dbus_open_struct(&body);
            rgui_dbus_put_string(&body, "current_filter");
            rgui_dbus_put_signature(&body, "(sa(us))");
            rgui_dbus_open_struct(&body);
            rgui_dbus_put_string(&body, "Markdown");
            rgui_dbus_array none = rgui_dbus_open_array(&body, '(');
            rgui_dbus_close_array(&body, none);
            rgui_dbus_open_struct(&body);
            rgui_dbus_put_string(&body, "uris");
            rgui_dbus_put_signature(&body, "as");
            rgui_dbus_array uris = rgui_dbus_open_array(&body, 's');
            rgui_dbus_put_string(&body, fake->uri.c_str());
            rgui_dbus_close_array(&body, uris);
        }
        rgui_dbus_close_array(&body, results);
        if (fake->defer) {
            fake->deferredHandle = handle;
            fake->deferredBody.assign(body.data, body.data + body.size);
        } else {
            SendResponse(conn, handle, call->sender, body.data, body.size);
        }
        rgui_dbus_buf_free(&body);
        return true;
    }
    return false;
}

// Asks the bus for a name on `conn`; true when it became the primary owner.
bool RequestName(rgui_dbus_conn* conn, const char* name) {
    rgui_dbus_buf body{};
    rgui_dbus_put_string(&body, name);
    rgui_dbus_put_uint32(&body, 4);  // DBUS_NAME_FLAG_DO_NOT_QUEUE
    rgui_dbus_message m{};
    m.destination = "org.freedesktop.DBus";
    m.path = "/org/freedesktop/DBus";
    m.interface = "org.freedesktop.DBus";
    m.member = "RequestName";
    m.signature = "su";
    m.body = body.data;
    m.body_size = static_cast<uint32_t>(body.size);
    CallResult r;
    bool sent = rgui_dbus_call(conn, &m, 5000, RecordReply, &r) != 0;
    rgui_dbus_buf_free(&body);
    return sent && Pump({conn}, nullptr, [&] { return r.calls > 0; }) && r.body == "1";
}

// A round trip to the bus: what `conn` sent before has been routed when it returns.
bool Sync(rgui_dbus_conn* conn) {
    CallResult r;
    rgui_dbus_message getId{};
    getId.destination = "org.freedesktop.DBus";
    getId.path = "/org/freedesktop/DBus";
    getId.interface = "org.freedesktop.DBus";
    getId.member = "GetId";
    return rgui_dbus_call(conn, &getId, 5000, RecordReply, &r) != 0 && Pump({conn}, nullptr, [&] { return r.calls > 0; });
}

} // namespace

RGUI_TEST("linux.dbus_session") {
    if (!SessionTestsEnabled("linux.dbus_session")) return;
    rgui_dbus_conn* conn = rgui_dbus_open_session(3000);
    CHECK(conn != nullptr);
    if (!conn) return;
    std::string name = rgui_dbus_unique_name(conn);
    CHECK(name.size() > 1 && name[0] == ':');

    // GetConnectionCredentials (a{sv}): our UnixUserID.
    rgui_dbus_buf body{};
    rgui_dbus_put_string(&body, name.c_str());
    rgui_dbus_message m{};
    m.destination = "org.freedesktop.DBus";
    m.path = "/org/freedesktop/DBus";
    m.interface = "org.freedesktop.DBus";
    m.member = "GetConnectionCredentials";
    m.signature = "s";
    m.body = body.data;
    m.body_size = static_cast<uint32_t>(body.size);
    CallResult credentials;
    CHECK(rgui_dbus_call(conn, &m, 5000, RecordReply, &credentials) != 0);
    rgui_dbus_buf_free(&body);
    CHECK(Pump({conn}, nullptr, [&] { return credentials.calls > 0; }));
    std::string uid = "{\"UnixUserID\":<u>" + std::to_string(getuid()) + "}";
    CHECK(credentials.body.find(uid) != std::string::npos);

    // A portal ReadOne with no portal on the bus: an error reply, quickly.
    rgui_dbus_buf args{};
    rgui_dbus_put_string(&args, "org.freedesktop.appearance");
    rgui_dbus_put_string(&args, "color-scheme");
    rgui_dbus_message read{};
    read.flags = RGUI_DBUS_NO_AUTO_START;  // a desktop's real portal must not be started here
    read.destination = "org.freedesktop.portal.Desktop";
    read.path = "/org/freedesktop/portal/desktop";
    read.interface = "org.freedesktop.portal.Settings";
    read.member = "ReadOne";
    read.signature = "ss";
    read.body = args.data;
    read.body_size = static_cast<uint32_t>(args.size);
    struct ErrorResult {
        int calls = 0;
        uint8_t type = 0;
        std::string name;
    } error;
    auto onError = [](void* user, const rgui_dbus_message* reply) {
        auto* e = static_cast<ErrorResult*>(user);
        ++e->calls;
        if (reply) {
            e->type = reply->type;
            e->name = reply->error_name ? reply->error_name : "";
        }
    };
    int64_t start = Ms();
    CHECK(rgui_dbus_call(conn, &read, 25000, onError, &error) != 0);
    rgui_dbus_buf_free(&args);
    CHECK(Pump({conn}, nullptr, [&] { return error.calls > 0; }));
    CHECK_EQ(error.type, uint8_t(RGUI_DBUS_ERROR));
    // NameHasNoOwner without auto-start (ServiceUnknown when activation finds no service file).
    CHECK(error.name == "org.freedesktop.DBus.Error.NameHasNoOwner" ||
          error.name == "org.freedesktop.DBus.Error.ServiceUnknown");
    CHECK(Ms() - start < 2000);

    // The Portal on a bus without a portal service: settings keep their defaults and a dialog
    // is Unavailable, at once. (Skipped where a portal could be D-Bus-activated.)
    if (std::filesystem::exists("/usr/share/dbus-1/services/org.freedesktop.portal.Desktop.service")) {
        std::printf("note linux.dbus_session: a portal service file exists; Portal-without-portal not run\n");
    } else {
        rgui::dbus::Portal portal;
        CHECK(portal.Connect(3000));
        bool settingsDone = false;
        bool dialogDone = false;
        rgui::dbus::Appearance appearance;
        appearance.dark = false;
        rgui::FileDialogResult dialog;
        start = Ms();
        portal.ReadAppearance([&](rgui::dbus::Appearance a) {
            settingsDone = true;
            appearance = a;
        });
        portal.ShowFileDialog(rgui::FileDialog::ExportPdf, "/tmp/x.pdf", [&](rgui::FileDialogResult r) {
            dialogDone = true;
            dialog = r;
        });
        CHECK(Pump({}, &portal, [&] { return settingsDone && dialogDone; }));
        CHECK(appearance.dark);
        CHECK_EQ(appearance.textScale, 1.0);
        CHECK_EQ(dialog.kind, rgui::FileDialogResult::Unavailable);
        CHECK(Ms() - start < 2000);
    }

    // A signal to ourselves through a match rule; a removed rule no longer routes.
    struct Signals {
        int count = 0;
        std::string body;
    } signals;
    auto onSignal = [](void* user, const rgui_dbus_message* s) {
        auto* sig = static_cast<Signals*>(user);
        ++sig->count;
        sig->body = RenderBody(*s);
    };
    rgui_dbus_match match{"org.example.Test", "Ping", "/org/example", nullptr, nullptr};
    uint32_t id = rgui_dbus_add_match(conn, &match, onSignal, &signals);
    CHECK(id != 0);
    rgui_dbus_buf payload{};
    rgui_dbus_put_string(&payload, "hi");
    rgui_dbus_message ping{};
    ping.type = RGUI_DBUS_SIGNAL;
    ping.path = "/org/example";
    ping.interface = "org.example.Test";
    ping.member = "Ping";
    ping.signature = "s";
    ping.body = payload.data;
    ping.body_size = static_cast<uint32_t>(payload.size);
    CHECK(rgui_dbus_send(conn, &ping) != 0);
    ping.member = "Other";  // matches no rule
    CHECK(rgui_dbus_send(conn, &ping) != 0);
    CHECK(Pump({conn}, nullptr, [&] { return signals.count > 0; }));
    CHECK_EQ(signals.body, std::string("\"hi\""));
    rgui_dbus_remove_match(conn, id);
    ping.member = "Ping";
    CHECK(rgui_dbus_send(conn, &ping) != 0);
    // A round trip to the bus after the signal: had it been routed, it would be by now.
    CallResult sync;
    rgui_dbus_message getId{};
    getId.destination = "org.freedesktop.DBus";
    getId.path = "/org/freedesktop/DBus";
    getId.interface = "org.freedesktop.DBus";
    getId.member = "GetId";
    CHECK(rgui_dbus_call(conn, &getId, 5000, RecordReply, &sync) != 0);
    CHECK(Pump({conn}, nullptr, [&] { return sync.calls > 0; }));
    CHECK_EQ(signals.count, 1);
    rgui_dbus_buf_free(&payload);
    rgui_dbus_close(conn);
}

RGUI_TEST("linux.dbus_portal_settings") {
    if (!SessionTestsEnabled("linux.dbus_portal_settings")) return;
    rgui_dbus_conn* service = rgui_dbus_open_session(3000);
    CHECK(service != nullptr);
    if (!service) return;
    FakePortal fake;
    rgui_dbus_set_method_handler(service, FakePortalHandler, &fake);
    CHECK(RequestName(service, "org.freedesktop.portal.Desktop"));

    {
        // ReadOne: light (2) and 1.25.
        rgui::dbus::Portal portal;
        CHECK(portal.Connect(3000));
        int changes = 0;
        rgui::dbus::Appearance changed;
        portal.WatchAppearance([&](rgui::dbus::Appearance a) {
            ++changes;
            changed = a;
        });
        bool done = false;
        rgui::dbus::Appearance read;
        portal.ReadAppearance([&](rgui::dbus::Appearance a) {
            done = true;
            read = a;
        });
        CHECK(Pump({service}, &portal, [&] { return done; }));
        CHECK(!read.dark);
        CHECK_EQ(read.textScale, 1.25);
        CHECK_EQ(fake.readOneCalls, 2);
        CHECK_EQ(fake.readCalls, 0);

        // SettingChanged: back to dark; a key we do not follow changes nothing. The match was
        // added before ReadAppearance's calls, so the bus has it by now.
        auto emit = [&](const char* nameSpace, const char* key, uint32_t value) {
            rgui_dbus_buf body{};
            rgui_dbus_put_string(&body, nameSpace);
            rgui_dbus_put_string(&body, key);
            rgui_dbus_put_signature(&body, "u");
            rgui_dbus_put_uint32(&body, value);
            rgui_dbus_message s{};
            s.type = RGUI_DBUS_SIGNAL;
            s.path = "/org/freedesktop/portal/desktop";
            s.interface = "org.freedesktop.portal.Settings";
            s.member = "SettingChanged";
            s.signature = "ssv";
            s.body = body.data;
            s.body_size = static_cast<uint32_t>(body.size);
            rgui_dbus_send(service, &s);
            rgui_dbus_buf_free(&body);
        };
        emit("org.freedesktop.appearance", "accent-color", 3);
        emit("org.freedesktop.appearance", "color-scheme", 1);
        CHECK(Pump({service}, &portal, [&] { return changes > 0; }));
        CHECK_EQ(changes, 1);
        CHECK(changed.dark);
        CHECK_EQ(changed.textScale, 1.25);
    }
    {
        // An old portal without ReadOne: Read, whose value is a variant inside a variant.
        fake.readOneUnknown = true;
        rgui::dbus::Portal portal;
        CHECK(portal.Connect(3000));
        bool done = false;
        rgui::dbus::Appearance read;
        read.dark = false;
        portal.ReadAppearance([&](rgui::dbus::Appearance a) {
            done = true;
            read = a;
        });
        CHECK(Pump({service}, &portal, [&] { return done; }));
        CHECK(read.dark);
        CHECK_EQ(read.textScale, 1.5);
        CHECK_EQ(fake.readCalls, 2);
    }
    rgui_dbus_close(service);
}

RGUI_TEST("linux.dbus_portal_file_chooser") {
    if (!SessionTestsEnabled("linux.dbus_portal_file_chooser")) return;
    rgui_dbus_conn* service = rgui_dbus_open_session(3000);
    CHECK(service != nullptr);
    if (!service) return;
    FakePortal fake;
    rgui_dbus_set_method_handler(service, FakePortalHandler, &fake);
    CHECK(RequestName(service, "org.freedesktop.portal.Desktop"));
    rgui::dbus::Portal portal;
    CHECK(portal.Connect(3000));

    auto show = [&](rgui::FileDialog kind, const std::string& suggested) {
        bool done = false;
        rgui::FileDialogResult result;
        portal.ShowFileDialog(kind, suggested, [&](rgui::FileDialogResult r) {
            done = true;
            result = std::move(r);
        });
        CHECK(Pump({service}, &portal, [&] { return done; }));
        return result;
    };

    // Open: chosen, the URI percent-decoded; the options as the plan lists them.
    fake.responseCode = 0;
    fake.uri = "file:///tmp/My%20Notes/a.md";
    rgui::FileDialogResult r = show(rgui::FileDialog::OpenDocument, "/tmp/ignored.md");
    CHECK_EQ(r.kind, rgui::FileDialogResult::Chosen);
    CHECK_EQ(r.path, std::string("/tmp/My Notes/a.md"));
    CHECK_EQ(fake.member, std::string("OpenFile"));
    CHECK_EQ(fake.parent, std::string(""));
    CHECK_EQ(fake.title, std::string("Open Markdown or RayoMD PDF"));
    // A random token: rayomd_gui_ and 16 hex digits, another each time.
    CHECK_EQ(fake.token.size(), size_t(27));
    CHECK_EQ(fake.token.substr(0, 11), std::string("rayomd_gui_"));
    CHECK(fake.token.find_first_not_of("0123456789abcdef", 11) == std::string::npos);
    const std::string firstToken = fake.token;
    CHECK_EQ(fake.options, std::string("[{\"handle_token\":<s>\"") + fake.token + std::string("\"},{\"modal\":<b>true},"
                                       "{\"filters\":<a(sa(us))>[(\"Markdown\",[(0,\"*.md\"),(0,\"*.markdown\"),"
                                       "(0,\"*.txt\")]),(\"RayoMD PDF\",[(0,\"*.pdf\")])]}]"));

    // Export: SaveFile with current_name and current_folder (NUL-terminated bytes); cancelled.
    char temp[] = "/tmp/rgui-portal-XXXXXX";
    CHECK(mkdtemp(temp) != nullptr);
    std::string folder = temp;
    fake.responseCode = 1;
    fake.uri.clear();
    r = show(rgui::FileDialog::ExportPdf, folder + "/doc.pdf");
    CHECK_EQ(r.kind, rgui::FileDialogResult::Cancelled);
    CHECK_EQ(fake.member, std::string("SaveFile"));
    CHECK_EQ(fake.title, std::string("Export PDF"));
    std::string folderBytes;
    for (char c : folder) folderBytes += std::to_string(static_cast<unsigned char>(c)) + ",";
    folderBytes += "0";
    CHECK(fake.token != firstToken);
    CHECK_EQ(fake.options, std::string("[{\"handle_token\":<s>\"") + fake.token + std::string("\"},{\"modal\":<b>true},"
                                       "{\"filters\":<a(sa(us))>[(\"PDF\",[(0,\"*.pdf\")])]},"
                                       "{\"current_name\":<s>\"doc.pdf\"},{\"current_folder\":<ay>[") +
                               folderBytes + "]}]");
    rmdir(temp);

    // Save Markdown into a folder that does not exist: no current_folder; Response 2 fails.
    fake.responseCode = 2;
    r = show(rgui::FileDialog::SaveMarkdown, "/nonexistent/rgui/notes.md");
    CHECK_EQ(r.kind, rgui::FileDialogResult::Failed);
    CHECK_EQ(fake.title, std::string("Save Markdown"));
    CHECK(fake.options.find("\"current_name\":<s>\"notes.md\"") != std::string::npos);
    CHECK(fake.options.find("current_folder") == std::string::npos);
    CHECK(fake.options.find("[(\"Markdown\",[(0,\"*.md\")])]") != std::string::npos);

    // Response 0 without a usable URI is a failure too.
    fake.responseCode = 0;
    fake.uri = "https://example.com/x.md";
    r = show(rgui::FileDialog::OpenDocument, "");
    CHECK_EQ(r.kind, rgui::FileDialogResult::Failed);

    // An error reply to the call: Unavailable (the app asks for the path itself).
    fake.errorReply = true;
    r = show(rgui::FileDialog::ChooseTheme, "");
    CHECK_EQ(r.kind, rgui::FileDialogResult::Unavailable);
    CHECK_EQ(fake.title, std::string("Choose PDF theme file"));
    CHECK(fake.options.find("[(\"Theme files\",[(0,\"*.theme\"),(0,\"*.txt\")]),(\"All files\",[(0,\"*\")])]") !=
          std::string::npos);
    fake.errorReply = false;

    // The vault: OpenFile with `directory` and no filters, the folder itself as current_folder.
    char vaultTemp[] = "/tmp/rgui-portal-XXXXXX";
    CHECK(mkdtemp(vaultTemp) != nullptr);
    const std::string vault = vaultTemp;
    fake.responseCode = 0;
    fake.uri = "file://" + vault;
    r = show(rgui::FileDialog::ChooseVault, vault + "/");
    CHECK_EQ(r.kind, rgui::FileDialogResult::Chosen);
    CHECK_EQ(r.path, vault);
    CHECK_EQ(fake.member, std::string("OpenFile"));
    CHECK_EQ(fake.title, std::string("Choose vault folder"));
    std::string vaultBytes;
    for (char c : vault) vaultBytes += std::to_string(static_cast<unsigned char>(c)) + ",";
    vaultBytes += "0";
    CHECK_EQ(fake.options, std::string("[{\"handle_token\":<s>\"") + fake.token +
                               "\"},{\"modal\":<b>true},{\"directory\":<b>true},{\"current_folder\":<ay>[" +
                               vaultBytes + "]}]");
    rmdir(vaultTemp);

    // A portal that picks its own request path: the Response there is still followed.
    fake.otherHandle = true;
    fake.uri = "file:///tmp/old.md";
    r = show(rgui::FileDialog::OpenDocument, "");
    CHECK_EQ(r.kind, rgui::FileDialogResult::Chosen);
    CHECK_EQ(r.path, std::string("/tmp/old.md"));
    fake.otherHandle = false;
    CHECK_EQ(portal.PortalOwner(), std::string(rgui_dbus_unique_name(service)));

    // Another peer forges the Response while the dialog is open: sent to us by name (match
    // rules do not apply) and broadcast. Both are ignored; the portal's own answer counts.
    rgui_dbus_conn* forger = rgui_dbus_open_session(3000);
    CHECK(forger != nullptr);
    if (forger) {
        fake.defer = true;
        fake.uri = "file:///tmp/real.md";
        bool done = false;
        rgui::FileDialogResult result;
        portal.ShowFileDialog(rgui::FileDialog::SaveMarkdown, "", [&](rgui::FileDialogResult answer) {
            done = true;
            result = std::move(answer);
        });
        CHECK(Pump({service}, &portal, [&] { return !fake.deferredHandle.empty(); }));
        const std::vector<uint8_t> evil = ResponseBody(0, "file:///tmp/evil.md");
        SendResponse(forger, fake.deferredHandle, fake.caller, evil.data(), evil.size());
        SendResponse(forger, fake.deferredHandle, "", evil.data(), evil.size());
        CHECK(Sync(forger));  // the forgeries are queued for the portal's client before the real one
        CHECK(!done);
        SendResponse(service, fake.deferredHandle, fake.caller, fake.deferredBody.data(), fake.deferredBody.size());
        CHECK(Pump({service, forger}, &portal, [&] { return done; }));
        CHECK_EQ(result.kind, rgui::FileDialogResult::Chosen);
        CHECK_EQ(result.path, std::string("/tmp/real.md"));
        fake.defer = false;
        rgui_dbus_close(forger);
    }

    rgui_dbus_close(service);
}

RGUI_TEST("linux.dbus_portal_forged_settings") {
    if (!SessionTestsEnabled("linux.dbus_portal_forged_settings")) return;
    rgui_dbus_conn* service = rgui_dbus_open_session(3000);
    rgui_dbus_conn* forger = rgui_dbus_open_session(3000);
    CHECK(service != nullptr && forger != nullptr);
    if (!service || !forger) {
        rgui_dbus_close(service);
        rgui_dbus_close(forger);
        return;
    }
    FakePortal fake;
    rgui_dbus_set_method_handler(service, FakePortalHandler, &fake);
    CHECK(RequestName(service, "org.freedesktop.portal.Desktop"));
    rgui::dbus::Portal portal;
    CHECK(portal.Connect(3000));
    int changes = 0;
    rgui::dbus::Appearance changed;
    portal.WatchAppearance([&](rgui::dbus::Appearance a) {
        ++changes;
        changed = a;
    });
    bool read = false;
    portal.ReadAppearance([&](rgui::dbus::Appearance) { read = true; });
    CHECK(Pump({service, forger}, &portal, [&] { return read; }));
    CHECK(!portal.CurrentAppearance().dark);  // ReadOne answered light
    CHECK_EQ(portal.PortalOwner(), std::string(rgui_dbus_unique_name(service)));

    auto emit = [](rgui_dbus_conn* from, const std::string& to, const char* key, const char* signature, double value) {
        rgui_dbus_buf body{};
        rgui_dbus_put_string(&body, std::strcmp(key, "color-scheme") == 0 ? "org.freedesktop.appearance"
                                                                          : "org.gnome.desktop.interface");
        rgui_dbus_put_string(&body, key);
        rgui_dbus_put_signature(&body, signature);
        if (signature[0] == 'u') {
            rgui_dbus_put_uint32(&body, static_cast<uint32_t>(value));
        } else {
            rgui_dbus_put_double(&body, value);
        }
        rgui_dbus_message s{};
        s.type = RGUI_DBUS_SIGNAL;
        s.path = "/org/freedesktop/portal/desktop";
        s.interface = "org.freedesktop.portal.Settings";
        s.member = "SettingChanged";
        s.destination = to.empty() ? nullptr : to.c_str();
        s.signature = "ssv";
        s.body = body.data;
        s.body_size = static_cast<uint32_t>(body.size);
        rgui_dbus_send(from, &s);
        rgui_dbus_buf_free(&body);
    };
    // Forged: dark, sent to us by name and broadcast; then the portal's real text scale.
    emit(forger, fake.caller, "color-scheme", "u", 1);
    emit(forger, "", "color-scheme", "u", 1);
    CHECK(Sync(forger));
    emit(service, "", "text-scaling-factor", "d", 1.75);
    CHECK(Pump({service, forger}, &portal, [&] { return changes > 0; }));
    CHECK_EQ(changes, 1);
    CHECK(!changed.dark);
    CHECK_EQ(changed.textScale, 1.75);
    rgui_dbus_close(forger);
    rgui_dbus_close(service);
}
