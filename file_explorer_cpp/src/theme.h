// Colours (the exact Tokyo Night / Nord / Catppuccin palettes), fonts and DPI.
#pragma once
#include "util.h"

struct Theme {
    wstr name;
    bool light = false;
    COLORREF bg, panel, surface, field, border, fg, muted, faint, accent, accent2, select, hover, drop, star,
        on_accent, good, bad, head;
    // Quick Look code colours
    COLORREF keyword, string, comment, number, key, tag, heading, bold, code;
};

extern Theme T;
const std::vector<wstr>& theme_names();
const wstr& default_theme();
void set_theme(const wstr& name);
Theme theme_by_name(const wstr& name);

COLORREF mix(COLORREF a, COLORREF b, double t);
COLORREF hex(const char* s);

// folder & tag colours. A colour "spec" is a palette name ("blue"), a custom colour ("#1F5FE8"), or a gradient of
// two of them ("amber>rose").
extern const std::vector<std::string> FOLDER_COLOR_NAMES;                  // the palette, in display order
extern const std::vector<std::pair<wstr, std::string>> GRADIENT_PRESETS;   // name, spec
COLORREF folder_rgb(const std::string& spec);   // the (first) colour
COLORREF folder_rgb2(const std::string& spec);  // the gradient's second colour (or the same)
bool is_folder_color(const std::string& spec);
bool is_gradient(const std::string& spec);
wstr color_label(const std::string& spec);      // "Blue", "Sunset", "Amber → Rose", "#1F5FE8"
std::string color_file_key(const std::string& spec);  // safe in a file name
std::string hex_of(COLORREF c);

// ---- DPI + fonts
struct Fonts {
    HFONT ui = nullptr, ui_bold = nullptr, small = nullptr, small_bold = nullptr, header = nullptr, title = nullptr,
          icon = nullptr, icon_small = nullptr, mono = nullptr, big = nullptr;
    bool fluent_icons = false;  // Segoe Fluent Icons / MDL2 Assets present
    wstr family;
};
extern Fonts F;
extern int g_dpi;
inline int S(double px) { return (int)(px * g_dpi / 96.0 + 0.5); }
void make_fonts(int dpi);
int text_width(HDC dc, HFONT f, const wstr& s);
int font_height(HFONT f);

// glyphs: Fluent icon font when there, plain symbols otherwise
enum Glyph { G_BACK, G_FWD, G_UP, G_REFRESH, G_STAR, G_STAR_FILLED, G_LIST, G_GRID, G_COLUMNS, G_SEARCH, G_DUAL,
             G_THEME, G_CLOSE, G_ADD, G_CHEVRON, G_SETS, G_MORE, G_TAG, G_PREVIEW };
wstr glyph(Glyph g);

// window chrome
void style_window(HWND hwnd);          // dark / light title bar + caption colour
void use_dark_menus(bool dark);        // popup menus follow the theme (Windows 10 1903+)
void set_control_theme(HWND hwnd);     // scrollbars etc. of standard controls
