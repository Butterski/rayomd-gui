// SPDX-License-Identifier: Apache-2.0
// UI tokens: Omarchy's shell.toml (font sizes, spacing, control states, popup, tooltip and menu
// surfaces) over the defaults of default/themed/shell.toml.tpl, resolved the way the Omarchy
// shell does (shell/Commons/Style.qml, Color.qml, Border.qml), plus the app's derived colours.
// Sizes are logical px.
#pragma once

#include "ui/theme.h"

#include <optional>

namespace rgui::ui {

struct FontSizes {
    int baseSize = 12;
    int caption = 10;
    int bodySmall = 11;
    int body = 12;
    int subtitle = 13;
    int title = 14;
    int heading = 16;
    int display = 24;
    int displayLarge = 28;
    int iconSmall = 11;
    int icon = 14;
    int iconLarge = 18;
};

struct Spacing {
    int hairline = 1;
    int xxs = 2;
    int xs = 3;
    int sm = 4;
    int md = 6;
    int lg = 8;
    int xl = 10;
    int xxl = 12;
    int xxxl = 14;
    int huge = 18;
    int controlGap = 8;
    int controlPaddingX = 10;
    int controlPaddingY = 6;
    int inputPaddingY = 7;
    int controlHeight = 28;
    int popupRowHeight = 28;
    int rowGap = 8;
    int rowPaddingX = 12;
    int labelGap = 4;
    int panelGap = 14;
    int panelPadding = 18;
    int popupPadding = 14;
    int dropdownWidth = 240;
    int searchableDropdownWidth = 260;
    int numberFieldWidth = 120;
    int searchablePopupMinHeight = 220;
};

// One interactive state of control chrome ([controls] <state>-*): the fill is `color` at
// `fillAlpha`, the border `border` (its alpha already applied) `borderWidth` px wide.
struct ControlState {
    Rgba color;
    double fillAlpha = 0;
    Rgba border;
    int borderWidth = 0;

    Rgba Fill() const { return color.WithAlpha(fillAlpha); }
};

// A surface section ([popups], [tooltip], [menu]); colours carry their alphas.
struct SurfaceTokens {
    Rgba background;
    Rgba text;
    Rgba border;
    int borderWidth = 0;
};

struct MenuTokens : SurfaceTokens {
    Rgba scrim;
    Rgba selectedBackground;
    Rgba selectedText;
    Rgba selectedBorder;
};

// The app's colour roles (one table in tokens.cpp, each row with its source).
struct DerivedColors {
    Rgba windowBackground;      // background
    Rgba text;                  // foreground
    Rgba mutedText;             // mix(background, foreground, 0.62)
    Rgba previewBackground;     // dark_background
    Rgba hairline;              // foreground at 0.12
    Rgba selection;             // selection
    Rgba selectedText;          // selection_foreground
    Rgba caret;                 // cursor
    Rgba accent;                // accent
    Rgba accentHover;           // mix(accent, foreground, 0.15)
    Rgba accentPressed;         // mix(accent, background, 0.15)
    Rgba textOnAccent;          // background or foreground, the higher WCAG contrast on accent
    Rgba error;                 // red
    Rgba warning;               // yellow
    Rgba success;               // green
    Rgba scrim;                 // background at 0.5
    Rgba scrollbarThumb;        // foreground at 0.25
    Rgba scrollbarThumbActive;  // foreground at 0.4
};

struct Tokens {
    FontSizes font;
    Spacing spacing;

    ControlState normal;
    ControlState hoverCursor;
    ControlState focus;
    ControlState selected;
    Rgba pressedFill;    // the pressed colour at pressed-fill-alpha
    Rgba selectionFill;  // the selection colour at selection-fill-alpha

    SurfaceTokens popups;
    SurfaceTokens tooltip;
    MenuTokens menu;

    // Corner radius of cards and popups; controls use controlRadius = min(4, radius).
    int radius = 0;
    int controlRadius = 0;
    // The factor for the system text size: 1 whenever an Omarchy shell.toml is in use, because
    // its base-size already carries the user's size; else the system's, clamped to 0.5-3.0.
    double textScale = 1.0;

    DerivedColors colors;
    bool dark = true;
};

struct TokenSources {
    // The theme's shell.toml (Omarchy's current/theme/shell.toml or a bundled theme's), and the
    // user's ~/.config/omarchy/shell.toml; null when there is none. User keys win, key by key.
    const Toml* themeShell = nullptr;
    const Toml* userShell = nullptr;
    // Hyprland's decoration:rounding when known, else the bundled theme's rounding (0 if none).
    std::optional<int> hyprlandRounding;
    int themeRounding = 0;
    // The system text-scaling factor (portal or Windows registry).
    double systemTextScale = 1.0;
};

// The defaults of shell.toml.tpl rendered with `palette`, overridden by the sources. Values out of
// range are clamped, not refused: base-size to [6, 48], [spacing] scale to [0, 4], a spacing
// token to [0, 256] px (width tokens to four times their default when that is more), a font
// size override to 256 px, border widths to [0, 16], alphas to [0, 1], the radius to [0, 64].
Tokens BuildTokens(const Palette& palette, const TokenSources& sources);

// The merged shell values ("section.key" -> value): the rendered template defaults, then the
// theme's shell.toml, then the user's. Exposed for tests and for tokens the app adds later.
Toml MergedShellValues(const Palette& palette, const TokenSources& sources);

// WCAG 2 contrast ratio of two opaque colours, 1..21.
double ContrastRatio(Rgba a, Rgba b);

} // namespace rgui::ui
