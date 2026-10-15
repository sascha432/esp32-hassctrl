#include "keyboard.hpp"

#include "theme.hpp"

namespace hc::ui::keyboard {

namespace {

namespace t = theme;

// LVGL 8.4's default maps (lv_keyboard.c) with the last row changed: no LV_SYMBOL_KEYBOARD,
// the left arrow is one unit wider instead.

constexpr lv_btnmatrix_ctrl_t kPopover = LV_BTNMATRIX_CTRL_POPOVER;
constexpr lv_btnmatrix_ctrl_t kChecked = LV_BTNMATRIX_CTRL_CHECKED;
constexpr lv_btnmatrix_ctrl_t kControl = LV_KEYBOARD_CTRL_BTN_FLAGS;

const char *kLower[] = {"1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
                        "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
                        "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
                        LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};

const char *kUpper[] = {"1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
                        "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
                        "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
                        LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};

const lv_btnmatrix_ctrl_t kTextControl[] = {
    kControl | 5, kPopover | 4, kPopover | 4, kPopover | 4, kPopover | 4, kPopover | 4, kPopover | 4,
    kPopover | 4, kPopover | 4, kPopover | 4, kPopover | 4, kChecked | 7,
    kControl | 6, kPopover | 3, kPopover | 3, kPopover | 3, kPopover | 3, kPopover | 3, kPopover | 3,
    kPopover | 3, kPopover | 3, kPopover | 3, kChecked | 7,
    kChecked | kPopover | 1, kChecked | kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1,
    kPopover | 1, kPopover | 1, kPopover | 1, kChecked | kPopover | 1, kChecked | kPopover | 1,
    kChecked | kPopover | 1,
    kChecked | 3, 6, kChecked | 2, kControl | 2,
};

const char *kSpecial[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
                          "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
                          "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
                          LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};

const lv_btnmatrix_ctrl_t kSpecialControl[] = {
    kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1,
    kPopover | 1, kPopover | 1, kPopover | 1, kChecked | 2,
    kControl | 2, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1,
    kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1,
    kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1,
    kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1, kPopover | 1,
    kChecked | 3, 6, kChecked | 2, kControl | 2,
};

} // namespace

lv_obj_t *create(lv_obj_t *parent)
{
    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kLower, kTextControl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kUpper, kTextControl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kSpecial, kSpecialControl);

    lv_obj_set_style_text_font(kb, &t::font_normal(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, t::color(t::kBar), 0);
    lv_obj_set_style_bg_color(kb, t::color(t::kCard), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, t::color(t::kText), LV_PART_ITEMS);
    // the control keys (shift, enter, ...) are drawn "checked"
    lv_obj_set_style_bg_color(kb, t::color(t::kBorder), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, t::color(t::kText), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, t::color(t::kActive), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_PRESSED);
    return kb;
}

} // namespace hc::ui::keyboard
