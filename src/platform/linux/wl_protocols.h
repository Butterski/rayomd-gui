/* SPDX-License-Identifier: Apache-2.0 */
/* The Wayland interfaces RayoMD GUI uses, written by hand from the protocol XML (wayland 1.23,
 * wayland-protocols 1.41: xdg-shell, xdg-decoration-unstable-v1, cursor-shape-v1, viewporter,
 * fractional-scale-v1, text-input-unstable-v3). Only the messages used are listed; an opcode is
 * the message's position among its interface's requests or events in the XML. Adding a message
 * means adding its XML signature here, with its opcode. */
#ifndef RGUI_PLATFORM_LINUX_WL_PROTOCOLS_H
#define RGUI_PLATFORM_LINUX_WL_PROTOCOLS_H

#include "platform/linux/wl_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const rgui_wl_interface rgui_wl_display_interface;
extern const rgui_wl_interface rgui_wl_registry_interface;
extern const rgui_wl_interface rgui_wl_callback_interface;
extern const rgui_wl_interface rgui_wl_compositor_interface;
extern const rgui_wl_interface rgui_wl_surface_interface;
extern const rgui_wl_interface rgui_wl_shm_interface;
extern const rgui_wl_interface rgui_wl_shm_pool_interface;
extern const rgui_wl_interface rgui_wl_buffer_interface;
extern const rgui_wl_interface rgui_wl_seat_interface;
extern const rgui_wl_interface rgui_wl_pointer_interface;
extern const rgui_wl_interface rgui_wl_keyboard_interface;
extern const rgui_wl_interface rgui_wl_output_interface;
extern const rgui_wl_interface rgui_wl_data_device_manager_interface;
extern const rgui_wl_interface rgui_wl_data_source_interface;
extern const rgui_wl_interface rgui_wl_data_device_interface;
extern const rgui_wl_interface rgui_wl_data_offer_interface;
extern const rgui_wl_interface rgui_xdg_wm_base_interface;
extern const rgui_wl_interface rgui_xdg_surface_interface;
extern const rgui_wl_interface rgui_xdg_toplevel_interface;
extern const rgui_wl_interface rgui_zxdg_decoration_manager_v1_interface;
extern const rgui_wl_interface rgui_zxdg_toplevel_decoration_v1_interface;
extern const rgui_wl_interface rgui_wp_cursor_shape_manager_v1_interface;
extern const rgui_wl_interface rgui_wp_cursor_shape_device_v1_interface;
extern const rgui_wl_interface rgui_wp_viewporter_interface;
extern const rgui_wl_interface rgui_wp_viewport_interface;
extern const rgui_wl_interface rgui_wp_fractional_scale_manager_v1_interface;
extern const rgui_wl_interface rgui_wp_fractional_scale_v1_interface;
extern const rgui_wl_interface rgui_zwp_text_input_manager_v3_interface;
extern const rgui_wl_interface rgui_zwp_text_input_v3_interface;

/* Requests (R) and events (E), named <interface>_<message>. */
enum {
    WL_DISPLAY_R_SYNC = 0, WL_DISPLAY_R_GET_REGISTRY = 1,
    WL_DISPLAY_E_ERROR = 0, WL_DISPLAY_E_DELETE_ID = 1,
    WL_REGISTRY_R_BIND = 0,
    WL_REGISTRY_E_GLOBAL = 0, WL_REGISTRY_E_GLOBAL_REMOVE = 1,
    WL_CALLBACK_E_DONE = 0,
    WL_COMPOSITOR_R_CREATE_SURFACE = 0,
    WL_SURFACE_R_ATTACH = 1, WL_SURFACE_R_FRAME = 3, WL_SURFACE_R_COMMIT = 6, WL_SURFACE_R_SET_BUFFER_SCALE = 8,
    WL_SURFACE_R_DAMAGE_BUFFER = 9,
    WL_SURFACE_E_ENTER = 0, WL_SURFACE_E_LEAVE = 1, WL_SURFACE_E_PREFERRED_BUFFER_SCALE = 2,
    WL_SHM_R_CREATE_POOL = 0,
    WL_SHM_POOL_R_CREATE_BUFFER = 0, WL_SHM_POOL_R_DESTROY = 1, WL_SHM_POOL_R_RESIZE = 2,
    WL_BUFFER_R_DESTROY = 0,
    WL_BUFFER_E_RELEASE = 0,
    WL_SEAT_R_GET_POINTER = 0, WL_SEAT_R_GET_KEYBOARD = 1,
    WL_SEAT_E_CAPABILITIES = 0,
    WL_POINTER_R_SET_CURSOR = 0,
    WL_POINTER_E_ENTER = 0, WL_POINTER_E_LEAVE = 1, WL_POINTER_E_MOTION = 2, WL_POINTER_E_BUTTON = 3,
    WL_POINTER_E_AXIS = 4, WL_POINTER_E_FRAME = 5, WL_POINTER_E_AXIS_SOURCE = 6, WL_POINTER_E_AXIS_VALUE120 = 9,
    WL_KEYBOARD_E_KEYMAP = 0, WL_KEYBOARD_E_ENTER = 1, WL_KEYBOARD_E_LEAVE = 2, WL_KEYBOARD_E_KEY = 3,
    WL_KEYBOARD_E_MODIFIERS = 4, WL_KEYBOARD_E_REPEAT_INFO = 5,
    WL_OUTPUT_E_DONE = 2, WL_OUTPUT_E_SCALE = 3,
    WL_DATA_DEVICE_MANAGER_R_CREATE_DATA_SOURCE = 0, WL_DATA_DEVICE_MANAGER_R_GET_DATA_DEVICE = 1,
    WL_DATA_SOURCE_R_OFFER = 0, WL_DATA_SOURCE_R_DESTROY = 1, WL_DATA_SOURCE_R_SET_ACTIONS = 2,
    WL_DATA_SOURCE_E_SEND = 1, WL_DATA_SOURCE_E_CANCELLED = 2,
    WL_DATA_DEVICE_R_SET_SELECTION = 1,
    WL_DATA_DEVICE_E_DATA_OFFER = 0, WL_DATA_DEVICE_E_ENTER = 1, WL_DATA_DEVICE_E_LEAVE = 2,
    WL_DATA_DEVICE_E_MOTION = 3, WL_DATA_DEVICE_E_DROP = 4, WL_DATA_DEVICE_E_SELECTION = 5,
    WL_DATA_OFFER_R_ACCEPT = 0, WL_DATA_OFFER_R_RECEIVE = 1, WL_DATA_OFFER_R_DESTROY = 2,
    WL_DATA_OFFER_R_FINISH = 3, WL_DATA_OFFER_R_SET_ACTIONS = 4,
    WL_DATA_OFFER_E_OFFER = 0, WL_DATA_OFFER_E_SOURCE_ACTIONS = 1, WL_DATA_OFFER_E_ACTION = 2,
    XDG_WM_BASE_R_GET_XDG_SURFACE = 2, XDG_WM_BASE_R_PONG = 3,
    XDG_WM_BASE_E_PING = 0,
    XDG_SURFACE_R_GET_TOPLEVEL = 1, XDG_SURFACE_R_ACK_CONFIGURE = 4,
    XDG_SURFACE_E_CONFIGURE = 0,
    XDG_TOPLEVEL_R_SET_TITLE = 2, XDG_TOPLEVEL_R_SET_APP_ID = 3, XDG_TOPLEVEL_R_MOVE = 5, XDG_TOPLEVEL_R_RESIZE = 6,
    XDG_TOPLEVEL_R_SET_MIN_SIZE = 8, XDG_TOPLEVEL_R_SET_MAXIMIZED = 9, XDG_TOPLEVEL_R_UNSET_MAXIMIZED = 10,
    XDG_TOPLEVEL_R_SET_MINIMIZED = 13,
    XDG_TOPLEVEL_E_CONFIGURE = 0, XDG_TOPLEVEL_E_CLOSE = 1,
    ZXDG_DECORATION_MANAGER_V1_R_GET_TOPLEVEL_DECORATION = 1,
    ZXDG_TOPLEVEL_DECORATION_V1_R_SET_MODE = 1,
    ZXDG_TOPLEVEL_DECORATION_V1_E_CONFIGURE = 0,
    WP_CURSOR_SHAPE_MANAGER_V1_R_GET_POINTER = 1,
    WP_CURSOR_SHAPE_DEVICE_V1_R_SET_SHAPE = 1,
    WP_VIEWPORTER_R_GET_VIEWPORT = 1,
    WP_VIEWPORT_R_SET_DESTINATION = 2,
    WP_FRACTIONAL_SCALE_MANAGER_V1_R_GET_FRACTIONAL_SCALE = 1,
    WP_FRACTIONAL_SCALE_V1_E_PREFERRED_SCALE = 0,
    ZWP_TEXT_INPUT_MANAGER_V3_R_GET_TEXT_INPUT = 1,
    ZWP_TEXT_INPUT_V3_R_ENABLE = 1, ZWP_TEXT_INPUT_V3_R_DISABLE = 2, ZWP_TEXT_INPUT_V3_R_SET_CONTENT_TYPE = 5,
    ZWP_TEXT_INPUT_V3_R_SET_CURSOR_RECTANGLE = 6, ZWP_TEXT_INPUT_V3_R_COMMIT = 7,
    ZWP_TEXT_INPUT_V3_E_ENTER = 0, ZWP_TEXT_INPUT_V3_E_LEAVE = 1, ZWP_TEXT_INPUT_V3_E_PREEDIT_STRING = 2,
    ZWP_TEXT_INPUT_V3_E_COMMIT_STRING = 3, ZWP_TEXT_INPUT_V3_E_DONE = 5,
};

/* Enum values used. */
enum {
    WL_SHM_FORMAT_XRGB8888 = 1,
    WL_SEAT_CAPABILITY_POINTER = 1, WL_SEAT_CAPABILITY_KEYBOARD = 2,
    WL_POINTER_BUTTON_STATE_PRESSED = 1,
    WL_POINTER_AXIS_VERTICAL_SCROLL = 0, WL_POINTER_AXIS_HORIZONTAL_SCROLL = 1,
    WL_POINTER_AXIS_SOURCE_WHEEL = 0, WL_POINTER_AXIS_SOURCE_FINGER = 1, WL_POINTER_AXIS_SOURCE_CONTINUOUS = 2,
    WL_POINTER_AXIS_SOURCE_WHEEL_TILT = 3,
    WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 = 1,
    WL_KEYBOARD_KEY_STATE_PRESSED = 1,
    WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE = 0, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY = 1,
    XDG_TOPLEVEL_STATE_MAXIMIZED = 1,
    ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE = 1, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE = 2,
    WP_CURSOR_SHAPE_DEFAULT = 1, WP_CURSOR_SHAPE_POINTER = 4, WP_CURSOR_SHAPE_TEXT = 9,
    WP_CURSOR_SHAPE_EW_RESIZE = 26, WP_CURSOR_SHAPE_NS_RESIZE = 27, WP_CURSOR_SHAPE_NESW_RESIZE = 28,
    WP_CURSOR_SHAPE_NWSE_RESIZE = 29,
    ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE = 0, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL = 0,
};

/* The globals bound from wl_registry. */
typedef enum rgui_wl_global {
    RGUI_WL_GLOBAL_COMPOSITOR,
    RGUI_WL_GLOBAL_SHM,
    RGUI_WL_GLOBAL_SEAT,
    RGUI_WL_GLOBAL_OUTPUT,             /* each one is bound */
    RGUI_WL_GLOBAL_DATA_DEVICE_MANAGER,
    RGUI_WL_GLOBAL_XDG_WM_BASE,
    RGUI_WL_GLOBAL_DECORATION_MANAGER,
    RGUI_WL_GLOBAL_CURSOR_SHAPE_MANAGER,
    RGUI_WL_GLOBAL_VIEWPORTER,
    RGUI_WL_GLOBAL_FRACTIONAL_SCALE_MANAGER,
    RGUI_WL_GLOBAL_TEXT_INPUT_MANAGER,
    RGUI_WL_GLOBAL_COUNT
} rgui_wl_global;

typedef struct rgui_wl_global_spec {
    const rgui_wl_interface* iface;
    uint32_t min_version; /* an older global is not used */
    uint32_t max_version; /* the version bound is min(advertised, max_version) */
    int required;         /* without it the program cannot run */
} rgui_wl_global_spec;

extern const rgui_wl_global_spec rgui_wl_global_specs[RGUI_WL_GLOBAL_COUNT];

/* The global an interface name is, or -1 when it is not one we bind. */
int rgui_wl_global_from_name(const char* interface_name);
/* The version to bind an advertised global with, 0 when it is too old to use. */
uint32_t rgui_wl_global_bind_version(rgui_wl_global global, uint32_t advertised);

#ifdef __cplusplus
}
#endif

#endif
