// SPDX-License-Identifier: Apache-2.0
// The D-Bus wire reader: a whole message (rgui_dbus_message_size, rgui_dbus_parse_message, its
// body walked with the iterator, a little-endian message encoded again and re-parsed), and the
// iterator alone over a signature and body taken from the input.
#include "harness.h"

#include "platform/linux/dbus_wire.h"

#include <cstring>
#include <string>

namespace rgui::fuzz {
namespace {

void Walk(rgui_dbus_iter* it, unsigned& steps) {
    for (char type; (type = rgui_dbus_iter_type(it)) != 0 && steps < 100000; ++steps) {
        bool ok = true;
        if ((steps & 7) == 7) {
            ok = rgui_dbus_skip(it);
        } else {
            switch (type) {
            case 'y': { uint8_t v; ok = rgui_dbus_get_byte(it, &v); break; }
            case 'b': { bool v; ok = rgui_dbus_get_bool(it, &v); break; }
            case 'n': { int16_t v; ok = rgui_dbus_get_int16(it, &v); break; }
            case 'q': { uint16_t v; ok = rgui_dbus_get_uint16(it, &v); break; }
            case 'i': { int32_t v; ok = rgui_dbus_get_int32(it, &v); break; }
            case 'u':
            case 'h': { uint32_t v; ok = rgui_dbus_get_uint32(it, &v); break; }
            case 'x': { int64_t v; ok = rgui_dbus_get_int64(it, &v); break; }
            case 't': { uint64_t v; ok = rgui_dbus_get_uint64(it, &v); break; }
            case 'd': { double v; ok = rgui_dbus_get_double(it, &v); break; }
            case 's':
            case 'o':
            case 'g': {
                const char* v = nullptr;
                uint32_t length = 0;
                ok = rgui_dbus_get_string(it, &v, &length);
                if (ok && (v == nullptr || std::strlen(v) != length)) Fail("dbus string length");
                break;
            }
            case 'a':
            case '(':
            case '{':
            case 'v': {
                rgui_dbus_iter sub;
                ok = rgui_dbus_recurse(it, &sub);
                if (ok) {
                    Walk(&sub, steps);
                    ok = rgui_dbus_leave(it, &sub);
                }
                break;
            }
            default:
                Fail("rgui_dbus_iter_type gave an unknown type");
            }
        }
        if (!ok) return;
    }
}

void FuzzMessage(const uint8_t* data, size_t size) {
    if (size >= 16) {
        const size_t declared = rgui_dbus_message_size(data);
        if (declared != 0 && declared <= size) {
            rgui_dbus_message prefix;
            rgui_dbus_parse_message(data, declared, &prefix);
        }
    }
    rgui_dbus_message message;
    if (!rgui_dbus_parse_message(data, size, &message)) return;
    if (size < 16) Fail("parse_message accepted fewer than 16 bytes");
    if (rgui_dbus_message_size(data) != size) Fail("parse_message accepted a size message_size disagrees with");
    rgui_dbus_iter it;
    rgui_dbus_body_iter(&message, &it);
    unsigned steps = 0;
    Walk(&it, steps);

    if (message.big_endian) return;
    rgui_dbus_buf buf;
    std::memset(&buf, 0, sizeof buf);
    if (rgui_dbus_encode_message(&buf, &message) && !buf.failed) {
        rgui_dbus_message again;
        if (!rgui_dbus_parse_message(buf.data, buf.size, &again)) Fail("an encoded message does not parse");
        if (again.type != message.type || again.serial != message.serial || again.body_size != message.body_size ||
            std::strcmp(again.signature, message.signature) != 0) {
            Fail("an encoded message parses differently");
        }
    }
    rgui_dbus_buf_free(&buf);
}

void FuzzIterator(const uint8_t* data, size_t size) {
    if (size < 2) return;
    const bool bigEndian = (data[0] & 0x80) != 0;
    const size_t sigLength = data[0] & 0x7f;
    if (size - 1 < sigLength) return;
    const std::string signature(reinterpret_cast<const char*>(data + 1), sigLength);
    (void)rgui_dbus_object_path_valid(signature.data(), signature.size());
    if (!rgui_dbus_signature_valid(signature.data(), signature.size(), false)) return;
    if (signature.find('\0') != std::string::npos) Fail("a signature with NUL is valid");
    // The body at an 8-byte aligned address, as in a message.
    const std::string body(reinterpret_cast<const char*>(data + 1 + sigLength), size - 1 - sigLength);
    rgui_dbus_iter it;
    rgui_dbus_iter_init(&it, reinterpret_cast<const uint8_t*>(body.data()), body.size(), signature.c_str(), bigEndian);
    unsigned steps = 0;
    Walk(&it, steps);
}

} // namespace

int FuzzDbus(const uint8_t* data, size_t size) {
    FuzzMessage(data, size);
    FuzzIterator(data, size);
    return 0;
}

} // namespace rgui::fuzz
