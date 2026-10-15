// SPDX-License-Identifier: Apache-2.0
// Widgets in the Omarchy look (Omawrite's footer, Hype's controls, the Omarchy shell's tokens).
// Rectangles are logical pixels the caller lays out (RectCut); sizes and colours come from the
// Context's Tokens, control corners are Tokens::controlRadius. Inside BeginDisabled they draw at
// 40 % opacity and take no input or focus. Labels may carry "##suffix" to make their ids unique.
#pragma once

#include "ui/ui.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rgui::ui {

// ---- IconButton ----
enum class IconStyle : uint8_t {
    Muted,   // the muted colour at 0.55 opacity, full opacity on hover (Omawrite's footer)
    Accent,  // the accent colour, accent hover on hover
    Text,    // the text colour (window controls)
};
// A 16 px icon centred in `rect` (its hit area, 28 x 28 in the footer); on hover the hover-cursor
// fill, and after 400 ms `tooltip` (if any). True when clicked.
bool IconButton(Context& ctx, std::string_view id, Icon icon, RectF rect, std::string_view tooltip,
                IconStyle style = IconStyle::Muted);

// A tooltip for widget `owner` once it has been hot for 400 ms: above `anchor`, centred, kept
// inside the window (below it when there is no room above); tooltip tokens, a 1 px border,
// body-small text, padding sm vertically and md horizontally.
void Tooltip(Context& ctx, Id owner, RectF anchor, std::string_view text);

// ---- Button ----
enum class ButtonStyle : uint8_t { Normal, Primary };
// The width a button needs for `label`: its text plus control-padding-x on both sides.
float ButtonWidth(Context& ctx, std::string_view label);
// A control-height button; Enter or Space activates it while focused. True when activated.
bool Button(Context& ctx, std::string_view label, RectF rect, ButtonStyle style = ButtonStyle::Normal);

// ---- Checkbox ----
// A row with a 14 px box and the label; the row is the hit area, Space toggles it while focused.
// True when `value` changed.
bool Checkbox(Context& ctx, std::string_view label, bool& value, RectF row);

// ---- Dropdown ----
// A button showing items[selected] with a chevron; its popup opens below (above when there is
// no room), at most 12 rows visible, wheel-scrolled. Open: Up / Down move, Enter picks, Esc
// closes; closed and focused: Enter or Space opens, Up / Down step the choice. True when
// `selected` changed.
bool Dropdown(Context& ctx, std::string_view id, int& selected, const std::vector<std::string>& items, RectF rect);

// ---- Text fields ----
// The height of a one-line field: the body line plus input-padding-y above and below, at least
// control-height.
float TextFieldHeight(Context& ctx);
// One line of text: caret, selection (mouse and Shift), word motions with Ctrl, clipboard
// (Ctrl+C / X / V, Shift+Insert), Ctrl+A, and Ctrl+Z undoing the last edit (again: redo).
// `placeholder` shows, muted, while it is empty. True when `text` changed.
bool TextField(Context& ctx, std::string_view id, std::string& text, RectF rect, std::string_view placeholder = {},
               bool readOnly = false);

// A number in a number-field-width field: digits and one '.'; Up / Down and the wheel step it,
// Enter or losing focus commits the text, clamped to [min, max] and rounded to `decimals`;
// text that does not parse reverts. `suffix` follows the value while it is not being edited.
// True when `value` changed.
bool NumberField(Context& ctx, std::string_view id, double& value, double min, double max, double step,
                 int decimals, std::string_view suffix, RectF rect);

// ---- Text ----
// Body text (or `size`), in the text colour unless `color` is given, vertically centred.
void Label(Context& ctx, std::string_view text, RectF rect, std::optional<Rgba> color = std::nullopt,
           Align align = Align::Left, float size = 0);
// A section title at subtitle size in the muted colour: cuts panel-gap (unless `first`) and a
// row off the top of `column` and draws into it.
void SectionHeader(Context& ctx, std::string_view text, RectF& column, bool first = false);
float SectionHeaderHeight(Context& ctx, bool first = false);
// Greedy word wrap at spaces; a word wider than `width` is cut where it no longer fits.
std::vector<std::string_view> WrapText(Context& ctx, std::string_view text, float size, float width);

// ---- ScrollArea ----
// A vertical scroll view of `viewport`: returns the scroll offset to draw the content at
// (content y = viewport.y - offset) and clips to the viewport until EndScrollArea, which takes
// the content's height. Wheel notches move 120 px eased (SmoothScroll), touchpad pixels
// directly; the thumb is 6 px wide (8 hovered or dragged), visible while scrolling or hovered
// and 600 ms after, and can be dragged.
float BeginScrollArea(Context& ctx, std::string_view id, RectF viewport);
void EndScrollArea(Context& ctx, float contentHeight);
// The scroll position of the area `id` (in the same id scope as its BeginScrollArea), to jump.
SmoothScroll& ScrollAreaScroll(Context& ctx, std::string_view id);

// ---- Splitter ----
// A 1 px vertical hairline at area.x + ratio * area.w with a 6 px hit zone and the ResizeH
// cursor; dragging sets `ratio`, always kept in [0.2, 0.8]. Returns the hairline's x; `changed`
// says whether the ratio moved.
float Splitter(Context& ctx, std::string_view id, RectF area, float& ratio, bool* changed = nullptr);

// ---- Modal ----
struct ModalButton {
    std::string_view label;
    bool primary = false;
    Key altKey = Key::None;   // Alt+key activates it
    bool enter = false;       // Enter activates it
    bool escape = false;      // Esc activates it
};
// The scrim and a centred card min(420, width - 48) wide, padding 20, popup background and
// border, the title at heading size in bold. Returns the body's rectangle, `bodyHeight` high;
// draw the body, then call EndModal. Everything below takes no pointer or keys meanwhile.
RectF BeginModal(Context& ctx, std::string_view id, std::string_view title, float bodyHeight);
// The buttons, right-aligned with control-gap between them; the index of the one activated
// (click, Enter / Esc / Alt+key), else -1.
int EndModal(Context& ctx, const ModalButton* buttons, size_t count);
// A modal whose body is `body` wrapped at body size.
int MessageModal(Context& ctx, std::string_view id, std::string_view title, std::string_view body,
                 const ModalButton* buttons, size_t count);
// The width of a modal's body in the current window.
float ModalBodyWidth(Context& ctx);

} // namespace rgui::ui
