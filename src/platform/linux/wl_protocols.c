/* SPDX-License-Identifier: Apache-2.0 */
/* Signatures copied from wayland.xml (wayland 1.23) and wayland-protocols 1.41. Each entry is
 * { opcode, flags, name, signature, interface of the 'n' argument }.
 *
 * Rule: every event that carries a file descriptor ('h') of an interface listed here is listed,
 * whether or not we use it: wl_wire skips unlisted events without taking fds off the queue. */
#include "platform/linux/wl_protocols.h"

#include <string.h>

#define COUNT(a) ((uint16_t)(sizeof(a) / sizeof((a)[0])))
#define D RGUI_WL_DESTRUCTOR
#define INTERFACE(var, name, version, requests, events) \
    const rgui_wl_interface var = {name, version, COUNT(requests), COUNT(events), requests, events}
#define REQUESTS_ONLY(var, name, version, requests) \
    const rgui_wl_interface var = {name, version, COUNT(requests), 0, requests, NULL}
#define EVENTS_ONLY(var, name, version, events) \
    const rgui_wl_interface var = {name, version, 0, COUNT(events), NULL, events}

/* wayland.xml */

static const rgui_wl_message display_requests[] = {
    {0, 0, "sync", "n", &rgui_wl_callback_interface},
    {1, 0, "get_registry", "n", &rgui_wl_registry_interface},
};
static const rgui_wl_message display_events[] = {
    {0, 0, "error", "ous", NULL},
    {1, 0, "delete_id", "u", NULL},
};
INTERFACE(rgui_wl_display_interface, "wl_display", 1, display_requests, display_events);

static const rgui_wl_message registry_requests[] = {
    {0, 0, "bind", "usun", NULL}, /* new_id without an interface: name, version, id */
};
static const rgui_wl_message registry_events[] = {
    {0, 0, "global", "usu", NULL},
    {1, 0, "global_remove", "u", NULL},
};
INTERFACE(rgui_wl_registry_interface, "wl_registry", 1, registry_requests, registry_events);

static const rgui_wl_message callback_events[] = {
    {0, D, "done", "u", NULL},
};
EVENTS_ONLY(rgui_wl_callback_interface, "wl_callback", 1, callback_events);

static const rgui_wl_message compositor_requests[] = {
    {0, 0, "create_surface", "n", &rgui_wl_surface_interface},
};
REQUESTS_ONLY(rgui_wl_compositor_interface, "wl_compositor", 6, compositor_requests);

static const rgui_wl_message surface_requests[] = {
    {1, 0, "attach", "?oii", NULL},
    {3, 0, "frame", "n", &rgui_wl_callback_interface},
    {6, 0, "commit", "", NULL},
    {8, 0, "set_buffer_scale", "i", NULL},
    {9, 0, "damage_buffer", "iiii", NULL},
};
static const rgui_wl_message surface_events[] = {
    {0, 0, "enter", "o", NULL},
    {1, 0, "leave", "o", NULL},
    {2, 0, "preferred_buffer_scale", "i", NULL},
};
INTERFACE(rgui_wl_surface_interface, "wl_surface", 6, surface_requests, surface_events);

static const rgui_wl_message shm_requests[] = {
    {0, 0, "create_pool", "nhi", &rgui_wl_shm_pool_interface},
};
REQUESTS_ONLY(rgui_wl_shm_interface, "wl_shm", 1, shm_requests);

static const rgui_wl_message shm_pool_requests[] = {
    {0, 0, "create_buffer", "niiiiu", &rgui_wl_buffer_interface},
    {1, D, "destroy", "", NULL},
    {2, 0, "resize", "i", NULL},
};
REQUESTS_ONLY(rgui_wl_shm_pool_interface, "wl_shm_pool", 1, shm_pool_requests);

static const rgui_wl_message buffer_requests[] = {
    {0, D, "destroy", "", NULL},
};
static const rgui_wl_message buffer_events[] = {
    {0, 0, "release", "", NULL},
};
INTERFACE(rgui_wl_buffer_interface, "wl_buffer", 1, buffer_requests, buffer_events);

static const rgui_wl_message seat_requests[] = {
    {0, 0, "get_pointer", "n", &rgui_wl_pointer_interface},
    {1, 0, "get_keyboard", "n", &rgui_wl_keyboard_interface},
};
static const rgui_wl_message seat_events[] = {
    {0, 0, "capabilities", "u", NULL},
};
INTERFACE(rgui_wl_seat_interface, "wl_seat", 9, seat_requests, seat_events);

static const rgui_wl_message pointer_requests[] = {
    {0, 0, "set_cursor", "u?oii", NULL},
};
static const rgui_wl_message pointer_events[] = {
    {0, 0, "enter", "uoff", NULL},
    {1, 0, "leave", "uo", NULL},
    {2, 0, "motion", "uff", NULL},
    {3, 0, "button", "uuuu", NULL},
    {4, 0, "axis", "uuf", NULL},
    {5, 0, "frame", "", NULL},
    {6, 0, "axis_source", "u", NULL},
    {9, 0, "axis_value120", "ui", NULL},
};
INTERFACE(rgui_wl_pointer_interface, "wl_pointer", 9, pointer_requests, pointer_events);

static const rgui_wl_message keyboard_events[] = {
    {0, 0, "keymap", "uhu", NULL},
    {1, 0, "enter", "uoa", NULL},
    {2, 0, "leave", "uo", NULL},
    {3, 0, "key", "uuuu", NULL},
    {4, 0, "modifiers", "uuuuu", NULL},
    {5, 0, "repeat_info", "ii", NULL},
};
EVENTS_ONLY(rgui_wl_keyboard_interface, "wl_keyboard", 9, keyboard_events);

static const rgui_wl_message output_events[] = {
    {2, 0, "done", "", NULL},
    {3, 0, "scale", "i", NULL},
};
EVENTS_ONLY(rgui_wl_output_interface, "wl_output", 4, output_events);

static const rgui_wl_message data_device_manager_requests[] = {
    {0, 0, "create_data_source", "n", &rgui_wl_data_source_interface},
    {1, 0, "get_data_device", "no", &rgui_wl_data_device_interface},
};
REQUESTS_ONLY(rgui_wl_data_device_manager_interface, "wl_data_device_manager", 3, data_device_manager_requests);

static const rgui_wl_message data_source_requests[] = {
    {0, 0, "offer", "s", NULL},
    {1, D, "destroy", "", NULL},
    {2, 0, "set_actions", "u", NULL},
};
static const rgui_wl_message data_source_events[] = {
    {1, 0, "send", "sh", NULL},
    {2, 0, "cancelled", "", NULL},
};
INTERFACE(rgui_wl_data_source_interface, "wl_data_source", 3, data_source_requests, data_source_events);

static const rgui_wl_message data_device_requests[] = {
    {1, 0, "set_selection", "?ou", NULL},
};
static const rgui_wl_message data_device_events[] = {
    {0, 0, "data_offer", "n", &rgui_wl_data_offer_interface},
    {1, 0, "enter", "uoff?o", NULL},
    {2, 0, "leave", "", NULL},
    {3, 0, "motion", "uff", NULL},
    {4, 0, "drop", "", NULL},
    {5, 0, "selection", "?o", NULL},
};
INTERFACE(rgui_wl_data_device_interface, "wl_data_device", 3, data_device_requests, data_device_events);

static const rgui_wl_message data_offer_requests[] = {
    {0, 0, "accept", "u?s", NULL},
    {1, 0, "receive", "sh", NULL},
    {2, D, "destroy", "", NULL},
    {3, 0, "finish", "", NULL},
    {4, 0, "set_actions", "uu", NULL},
};
static const rgui_wl_message data_offer_events[] = {
    {0, 0, "offer", "s", NULL},
    {1, 0, "source_actions", "u", NULL},
    {2, 0, "action", "u", NULL},
};
INTERFACE(rgui_wl_data_offer_interface, "wl_data_offer", 3, data_offer_requests, data_offer_events);

/* stable/xdg-shell/xdg-shell.xml */

static const rgui_wl_message xdg_wm_base_requests[] = {
    {2, 0, "get_xdg_surface", "no", &rgui_xdg_surface_interface},
    {3, 0, "pong", "u", NULL},
};
static const rgui_wl_message xdg_wm_base_events[] = {
    {0, 0, "ping", "u", NULL},
};
INTERFACE(rgui_xdg_wm_base_interface, "xdg_wm_base", 6, xdg_wm_base_requests, xdg_wm_base_events);

static const rgui_wl_message xdg_surface_requests[] = {
    {1, 0, "get_toplevel", "n", &rgui_xdg_toplevel_interface},
    {4, 0, "ack_configure", "u", NULL},
};
static const rgui_wl_message xdg_surface_events[] = {
    {0, 0, "configure", "u", NULL},
};
INTERFACE(rgui_xdg_surface_interface, "xdg_surface", 6, xdg_surface_requests, xdg_surface_events);

static const rgui_wl_message xdg_toplevel_requests[] = {
    {2, 0, "set_title", "s", NULL},
    {3, 0, "set_app_id", "s", NULL},
    {5, 0, "move", "ou", NULL},
    {6, 0, "resize", "ouu", NULL},
    {8, 0, "set_min_size", "ii", NULL},
    {9, 0, "set_maximized", "", NULL},
    {10, 0, "unset_maximized", "", NULL},
    {13, 0, "set_minimized", "", NULL},
};
static const rgui_wl_message xdg_toplevel_events[] = {
    {0, 0, "configure", "iia", NULL},
    {1, 0, "close", "", NULL},
};
INTERFACE(rgui_xdg_toplevel_interface, "xdg_toplevel", 6, xdg_toplevel_requests, xdg_toplevel_events);

/* unstable/xdg-decoration/xdg-decoration-unstable-v1.xml */

static const rgui_wl_message decoration_manager_requests[] = {
    {1, 0, "get_toplevel_decoration", "no", &rgui_zxdg_toplevel_decoration_v1_interface},
};
REQUESTS_ONLY(rgui_zxdg_decoration_manager_v1_interface, "zxdg_decoration_manager_v1", 1,
              decoration_manager_requests);

static const rgui_wl_message toplevel_decoration_requests[] = {
    {1, 0, "set_mode", "u", NULL},
};
static const rgui_wl_message toplevel_decoration_events[] = {
    {0, 0, "configure", "u", NULL},
};
INTERFACE(rgui_zxdg_toplevel_decoration_v1_interface, "zxdg_toplevel_decoration_v1", 1,
          toplevel_decoration_requests, toplevel_decoration_events);

/* staging/cursor-shape/cursor-shape-v1.xml */

static const rgui_wl_message cursor_shape_manager_requests[] = {
    {1, 0, "get_pointer", "no", &rgui_wp_cursor_shape_device_v1_interface},
};
REQUESTS_ONLY(rgui_wp_cursor_shape_manager_v1_interface, "wp_cursor_shape_manager_v1", 1,
              cursor_shape_manager_requests);

static const rgui_wl_message cursor_shape_device_requests[] = {
    {1, 0, "set_shape", "uu", NULL},
};
REQUESTS_ONLY(rgui_wp_cursor_shape_device_v1_interface, "wp_cursor_shape_device_v1", 1,
              cursor_shape_device_requests);

/* stable/viewporter/viewporter.xml */

static const rgui_wl_message viewporter_requests[] = {
    {1, 0, "get_viewport", "no", &rgui_wp_viewport_interface},
};
REQUESTS_ONLY(rgui_wp_viewporter_interface, "wp_viewporter", 1, viewporter_requests);

static const rgui_wl_message viewport_requests[] = {
    {2, 0, "set_destination", "ii", NULL},
};
REQUESTS_ONLY(rgui_wp_viewport_interface, "wp_viewport", 1, viewport_requests);

/* staging/fractional-scale/fractional-scale-v1.xml */

static const rgui_wl_message fractional_scale_manager_requests[] = {
    {1, 0, "get_fractional_scale", "no", &rgui_wp_fractional_scale_v1_interface},
};
REQUESTS_ONLY(rgui_wp_fractional_scale_manager_v1_interface, "wp_fractional_scale_manager_v1", 1,
              fractional_scale_manager_requests);

static const rgui_wl_message fractional_scale_events[] = {
    {0, 0, "preferred_scale", "u", NULL},
};
EVENTS_ONLY(rgui_wp_fractional_scale_v1_interface, "wp_fractional_scale_v1", 1, fractional_scale_events);

/* unstable/text-input/text-input-unstable-v3.xml */

static const rgui_wl_message text_input_manager_requests[] = {
    {1, 0, "get_text_input", "no", &rgui_zwp_text_input_v3_interface},
};
REQUESTS_ONLY(rgui_zwp_text_input_manager_v3_interface, "zwp_text_input_manager_v3", 1,
              text_input_manager_requests);

static const rgui_wl_message text_input_requests[] = {
    {1, 0, "enable", "", NULL},
    {2, 0, "disable", "", NULL},
    {5, 0, "set_content_type", "uu", NULL},
    {6, 0, "set_cursor_rectangle", "iiii", NULL},
    {7, 0, "commit", "", NULL},
};
static const rgui_wl_message text_input_events[] = {
    {0, 0, "enter", "o", NULL},
    {1, 0, "leave", "o", NULL},
    {2, 0, "preedit_string", "?sii", NULL},
    {3, 0, "commit_string", "?s", NULL},
    {5, 0, "done", "u", NULL},
};
INTERFACE(rgui_zwp_text_input_v3_interface, "zwp_text_input_v3", 1, text_input_requests, text_input_events);

/* Globals: minimum and maximum versions bound. */

const rgui_wl_global_spec rgui_wl_global_specs[RGUI_WL_GLOBAL_COUNT] = {
    [RGUI_WL_GLOBAL_COMPOSITOR] = {&rgui_wl_compositor_interface, 4, 6, 1},
    [RGUI_WL_GLOBAL_SHM] = {&rgui_wl_shm_interface, 1, 1, 1},
    [RGUI_WL_GLOBAL_SEAT] = {&rgui_wl_seat_interface, 5, 9, 1},
    [RGUI_WL_GLOBAL_OUTPUT] = {&rgui_wl_output_interface, 2, 4, 0}, /* scale and done came in 2 */
    [RGUI_WL_GLOBAL_DATA_DEVICE_MANAGER] = {&rgui_wl_data_device_manager_interface, 1, 3, 0},
    [RGUI_WL_GLOBAL_XDG_WM_BASE] = {&rgui_xdg_wm_base_interface, 1, 6, 1},
    [RGUI_WL_GLOBAL_DECORATION_MANAGER] = {&rgui_zxdg_decoration_manager_v1_interface, 1, 1, 0},
    [RGUI_WL_GLOBAL_CURSOR_SHAPE_MANAGER] = {&rgui_wp_cursor_shape_manager_v1_interface, 1, 1, 0},
    [RGUI_WL_GLOBAL_VIEWPORTER] = {&rgui_wp_viewporter_interface, 1, 1, 0},
    [RGUI_WL_GLOBAL_FRACTIONAL_SCALE_MANAGER] = {&rgui_wp_fractional_scale_manager_v1_interface, 1, 1, 0},
    [RGUI_WL_GLOBAL_TEXT_INPUT_MANAGER] = {&rgui_zwp_text_input_manager_v3_interface, 1, 1, 0},
};

int rgui_wl_global_from_name(const char* interface_name) {
    if (!interface_name) return -1;
    for (int i = 0; i < RGUI_WL_GLOBAL_COUNT; ++i) {
        if (strcmp(rgui_wl_global_specs[i].iface->name, interface_name) == 0) return i;
    }
    return -1;
}

uint32_t rgui_wl_global_bind_version(rgui_wl_global global, uint32_t advertised) {
    if ((int)global < 0 || global >= RGUI_WL_GLOBAL_COUNT) return 0;
    const rgui_wl_global_spec* spec = &rgui_wl_global_specs[global];
    if (advertised < spec->min_version) return 0;
    return advertised < spec->max_version ? advertised : spec->max_version;
}
