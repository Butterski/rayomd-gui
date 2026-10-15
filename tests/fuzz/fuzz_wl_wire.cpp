// SPDX-License-Identifier: Apache-2.0
// The Wayland wire decoder: the input is what a compositor sends, written into a socketpair
// (the first byte's low two bits: how many file descriptors ride along with it), then read and
// dispatched to one object of every interface the app binds. wl_display's error event is left
// out of this display's table: it ends the process by design.
#include "harness.h"

#include "platform/linux/wl_protocols.h"
#include "platform/linux/wl_wire.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace rgui::fuzz {
namespace {

const rgui_wl_message kDisplayRequests[] = {
    {0, 0, "sync", "n", &rgui_wl_callback_interface},
    {1, 0, "get_registry", "n", &rgui_wl_registry_interface},
};
const rgui_wl_message kDisplayEvents[] = {
    {1, 0, "delete_id", "u", nullptr},
};
const rgui_wl_interface kDisplay = {"wl_display", 1, 2, 1, kDisplayRequests, kDisplayEvents};

// Closes the descriptors an event carried, as a real handler takes them over.
void Dispatch(void* user, uint32_t, uint16_t opcode, const rgui_wl_arg* args) {
    const auto* iface = static_cast<const rgui_wl_interface*>(user);
    for (uint16_t i = 0; i < iface->event_count; ++i) {
        const rgui_wl_message& m = iface->events[i];
        if (m.opcode != opcode) continue;
        size_t arg = 0;
        for (const char* s = m.signature; *s; ++s) {
            if (*s == '?') continue;
            if (*s == 'h' && args[arg].h >= 0) close(args[arg].h);
            if (*s == 's' && args[arg].s) (void)std::strlen(args[arg].s);
            if (*s == 'a' && args[arg].a.size) {
                volatile uint8_t sink = static_cast<const uint8_t*>(args[arg].a.data)[args[arg].a.size - 1];
                (void)sink;
            }
            ++arg;
        }
        return;
    }
}

const rgui_wl_interface* const kObjects[] = {
    &rgui_wl_registry_interface,      &rgui_wl_callback_interface,       &rgui_wl_surface_interface,
    &rgui_wl_buffer_interface,        &rgui_wl_shm_interface,            &rgui_wl_seat_interface,
    &rgui_wl_pointer_interface,       &rgui_wl_keyboard_interface,       &rgui_wl_output_interface,
    &rgui_wl_data_device_interface,   &rgui_wl_data_source_interface,    &rgui_wl_data_offer_interface,
    &rgui_xdg_wm_base_interface,      &rgui_xdg_surface_interface,       &rgui_xdg_toplevel_interface,
    &rgui_zxdg_toplevel_decoration_v1_interface, &rgui_wp_fractional_scale_v1_interface,
    &rgui_zwp_text_input_v3_interface,
};

} // namespace

size_t WlWireObjects(const rgui_wl_interface* const** objects) {
    *objects = kObjects;
    return sizeof kObjects / sizeof kObjects[0];
}

int FuzzWlWire(const uint8_t* data, size_t size) {
    if (size == 0 || size > RGUI_WL_BUFFER_SIZE) return 0;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) Fail("socketpair");
    rgui_wl_connection* c = rgui_wl_connection_create(sv[0], &kDisplay);
    if (!c) Fail("rgui_wl_connection_create");
    for (const rgui_wl_interface* iface : kObjects) {
        rgui_wl_new_object(c, iface, iface->version, Dispatch, const_cast<rgui_wl_interface*>(iface));
    }

    const int fdCount = data[0] & 3;
    int fds[3];
    for (int i = 0; i < fdCount; ++i) fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    iovec iov{const_cast<uint8_t*>(data + 1), size - 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof fds)] = {};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (fdCount > 0) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fdCount);
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fdCount);
        std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fdCount);
    }
    if (size > 1 && sendmsg(sv[1], &msg, MSG_NOSIGNAL) != static_cast<ssize_t>(size - 1)) Fail("sendmsg");
    for (int i = 0; i < fdCount; ++i) close(fds[i]);
    shutdown(sv[1], SHUT_WR);

    while (rgui_wl_read(c) > 0) {
        if (rgui_wl_dispatch(c) < 0) break;
    }
    rgui_wl_connection_destroy(c);
    close(sv[1]);
    return 0;
}

} // namespace rgui::fuzz
