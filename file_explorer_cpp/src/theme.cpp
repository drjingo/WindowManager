#include "theme.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <cmath>

Theme T;
Fonts F;
int g_dpi = 96;

COLORREF hex(const char* s) {
    if (*s == '#') s++;
    unsigned v = strtoul(s, nullptr, 16);
    return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

COLORREF mix(COLORREF a, COLORREF b, double t) {
    auto m = [t](int x, int y) { return (int)std::lround(x + (y - x) * t); };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

namespace {
struct Def {
    const wchar_t* name;
    bool light;
    const char *bg, *panel, *surface, *field, *border, *fg, *muted, *faint, *accent, *accent2, *select, *hover, *drop,
        *star, *on_accent, *good, *bad, *head;
    const char *keyword, *string, *comment, *number, *key, *tag, *heading, *bold, *code;
    double fs, fl;  // folder colour saturation / lightness
};

const Def DEFS[] = {
    {L"Tokyo Night", false, "#1a1b26", "#16161e", "#1a1b26", "#1f2335", "#292e42", "#c0caf5", "#a9b1d6", "#565f89",
     "#7aa2f7", "#bb9af7", "#283457", "#1f2335", "#3d59a1", "#e0af68", "#16161e", "#9ece6a", "#f7768e", "#737aa2",
     "#bb9af7", "#9ece6a", "#565f89", "#ff9e64", "#7aa2f7", "#f7768e", "#7aa2f7", "#c0caf5", "#e0af68", 0.82, 0.64},
    {L"Tokyo Night Storm", false, "#24283b", "#1f2335", "#24283b", "#292e42", "#3b4261", "#c0caf5", "#a9b1d6",
     "#565f89", "#7aa2f7", "#bb9af7", "#2e3c64", "#292e42", "#3d59a1", "#e0af68", "#1f2335", "#9ece6a", "#f7768e",
     "#737aa2", "#bb9af7", "#9ece6a", "#565f89", "#ff9e64", "#7aa2f7", "#f7768e", "#7aa2f7", "#c0caf5", "#e0af68",
     0.82, 0.64},
    {L"Nord", false, "#2e3440", "#292e39", "#2e3440", "#3b4252", "#3b4252", "#eceff4", "#d8dee9", "#7b88a1", "#88c0d0",
     "#81a1c1", "#434c5e", "#353b49", "#4c566a", "#ebcb8b", "#2e3440", "#a3be8c", "#bf616a", "#81a1c1", "#81a1c1",
     "#a3be8c", "#616e88", "#b48ead", "#88c0d0", "#81a1c1", "#88c0d0", "#eceff4", "#ebcb8b", 0.50, 0.60},
    {L"Nord Light", true, "#eceff4", "#e5e9f0", "#f4f6f9", "#e5e9f0", "#d8dee9", "#2e3440", "#3b4252", "#7b88a1",
     "#5e81ac", "#b48ead", "#d0dbea", "#e8ecf2", "#b9c9df", "#d08770", "#eceff4", "#5d8a3e", "#bf616a", "#5e81ac",
     "#5e81ac", "#5d8a3e", "#8a96ad", "#a07b9a", "#3d7a8c", "#5e81ac", "#3d6a9e", "#2e3440", "#b0782a", 0.55, 0.52},
    {L"Catppuccin Mocha", false, "#1e1e2e", "#181825", "#1e1e2e", "#313244", "#313244", "#cdd6f4", "#bac2de",
     "#6c7086", "#cba6f7", "#89b4fa", "#45475a", "#262637", "#585b70", "#f9e2af", "#11111b", "#a6e3a1", "#f38ba8",
     "#b4befe", "#cba6f7", "#a6e3a1", "#6c7086", "#fab387", "#89b4fa", "#f38ba8", "#89b4fa", "#cdd6f4", "#f9e2af",
     0.80, 0.70},
    {L"Catppuccin Macchiato", false, "#24273a", "#1e2030", "#24273a", "#363a4f", "#363a4f", "#cad3f5", "#b8c0e0",
     "#6e738d", "#c6a0f6", "#8aadf4", "#494d64", "#2c3045", "#5b6078", "#eed49f", "#181926", "#a6da95", "#ed8796",
     "#b7bdf8", "#c6a0f6", "#a6da95", "#6e738d", "#f5a97f", "#8aadf4", "#ed8796", "#8aadf4", "#cad3f5", "#eed49f",
     0.78, 0.68},
    {L"Catppuccin Frappé", false, "#303446", "#292c3c", "#303446", "#414559", "#414559", "#c6d0f5", "#b5bfe2",
     "#737994", "#ca9ee6", "#8caaee", "#51576d", "#383c50", "#626880", "#e5c890", "#232634", "#a6d189", "#e78284",
     "#babbf1", "#ca9ee6", "#a6d189", "#737994", "#ef9f76", "#8caaee", "#e78284", "#8caaee", "#c6d0f5", "#e5c890",
     0.70, 0.66},
    {L"Catppuccin Latte", true, "#eff1f5", "#e6e9ef", "#eff1f5", "#ccd0da", "#ccd0da", "#4c4f69", "#5c5f77",
     "#9ca0b0", "#8839ef", "#1e66f5", "#bcc0cc", "#e9ecf2", "#acb0be", "#df8e1d", "#eff1f5", "#40a02b", "#d20f39",
     "#7287fd", "#8839ef", "#40a02b", "#9ca0b0", "#fe640b", "#1e66f5", "#d20f39", "#1e66f5", "#4c4f69", "#df8e1d",
     0.78, 0.50},
};

double g_fs = 0.82, g_fl = 0.64;
std::map<std::string, COLORREF> g_folder;

COLORREF hls(double h, double l, double s) {
    auto f = [](double p, double q, double t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    return RGB((int)std::lround(f(p, q, h + 1.0 / 3) * 255), (int)std::lround(f(p, q, h) * 255),
               (int)std::lround(f(p, q, h - 1.0 / 3) * 255));
}

void make_folder_palette() {
    const std::pair<const char*, double> hues[] = {{"red", 0},     {"orange", 26},  {"yellow", 45},
                                                   {"green", 118}, {"teal", 174},   {"blue", 214},
                                                   {"purple", 266}, {"pink", 318}};
    g_folder.clear();
    for (auto& h : hues) {
        double s = g_fs, l = g_fl;
        std::string n = h.first;
        if (n == "yellow") l -= 0.06;
        else if (n == "red") { s = std::min(1.0, s + 0.1); l -= 0.05; }
        else if (n == "pink") l += 0.04;
        else if (n == "green" || n == "teal") { s *= 0.72; l -= 0.04; }
        else if (n == "orange") { s = std::min(1.0, s + 0.08); l -= 0.03; }
        g_folder[n] = hls(h.second / 360.0, std::max(0.25, std::min(0.85, l)), s);
    }
    g_folder["grey"] = T.faint;
}
}  // namespace

const std::vector<std::string> FOLDER_COLOR_NAMES = {"yellow", "blue", "red", "orange", "green",
                                                     "teal",   "purple", "pink", "grey"};

COLORREF folder_rgb(const std::string& name) {
    auto it = g_folder.find(name);
    return it != g_folder.end() ? it->second : g_folder["yellow"];
}

bool is_folder_color(const std::string& name) {
    for (auto& n : FOLDER_COLOR_NAMES)
        if (n == name) return true;
    return false;
}

const std::vector<wstr>& theme_names() {
    static std::vector<wstr> names;
    if (names.empty())
        for (auto& d : DEFS) names.push_back(d.name);
    return names;
}

const wstr& default_theme() {
    static wstr d = L"Tokyo Night";
    return d;
}

Theme theme_by_name(const wstr& name) {
    const Def* d = &DEFS[0];
    for (auto& x : DEFS)
        if (name == x.name) d = &x;
    Theme t;
    t.name = d->name;
    t.light = d->light;
    t.bg = hex(d->bg); t.panel = hex(d->panel); t.surface = hex(d->surface); t.field = hex(d->field);
    t.border = hex(d->border); t.fg = hex(d->fg); t.muted = hex(d->muted); t.faint = hex(d->faint);
    t.accent = hex(d->accent); t.accent2 = hex(d->accent2); t.select = hex(d->select); t.hover = hex(d->hover);
    t.drop = hex(d->drop); t.star = hex(d->star); t.on_accent = hex(d->on_accent); t.good = hex(d->good);
    t.bad = hex(d->bad); t.head = hex(d->head);
    t.keyword = hex(d->keyword); t.string = hex(d->string); t.comment = hex(d->comment); t.number = hex(d->number);
    t.key = hex(d->key); t.tag = hex(d->tag); t.heading = hex(d->heading); t.bold = hex(d->bold); t.code = hex(d->code);
    return t;
}

void set_theme(const wstr& name) {
    T = theme_by_name(name);
    for (auto& d : DEFS)
        if (T.name == d.name) { g_fs = d.fs; g_fl = d.fl; }
    make_folder_palette();
}

// ======================================================================
// fonts
// ======================================================================
static int CALLBACK enum_font(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *(bool*)lp = true;
    return 0;
}

static bool has_font(const wchar_t* family) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, family, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, enum_font, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

static HFONT mkfont(const wstr& family, double pt, int weight, bool italic = false) {
    LOGFONTW lf{};
    lf.lfHeight = -(int)std::lround(pt * g_dpi / 72.0);
    lf.lfWeight = weight;
    lf.lfItalic = italic;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, family.c_str(), LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

static wstr g_icon_family;

void make_fonts(int dpi) {
    g_dpi = dpi;
    HFONT* all[] = {&F.ui, &F.ui_bold, &F.small, &F.small_bold, &F.header, &F.title, &F.icon, &F.icon_small, &F.mono, &F.big};
    for (auto f : all)
        if (*f) { DeleteObject(*f); *f = nullptr; }
    if (F.family.empty()) {
        F.family = has_font(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        if (has_font(L"Segoe Fluent Icons")) g_icon_family = L"Segoe Fluent Icons";
        else if (has_font(L"Segoe MDL2 Assets")) g_icon_family = L"Segoe MDL2 Assets";
        F.fluent_icons = !g_icon_family.empty();
    }
    F.ui = mkfont(F.family, 10, FW_NORMAL);
    F.ui_bold = mkfont(F.family, 10, FW_SEMIBOLD);
    F.small = mkfont(F.family, 9, FW_NORMAL);
    F.small_bold = mkfont(F.family, 8, FW_BOLD);
    F.header = mkfont(F.family, 9, FW_SEMIBOLD);
    F.title = mkfont(F.family, 12, FW_SEMIBOLD);
    F.big = mkfont(F.family, 11, FW_NORMAL);
    F.icon = F.fluent_icons ? mkfont(g_icon_family, 11, FW_NORMAL) : mkfont(F.family, 12, FW_NORMAL);
    F.icon_small = F.fluent_icons ? mkfont(g_icon_family, 8, FW_NORMAL) : mkfont(F.family, 9, FW_NORMAL);
    F.mono = mkfont(has_font(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas", 10, FW_NORMAL);
}

int text_width(HDC dc, HFONT f, const wstr& s) {
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

int font_height(HFONT f) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return tm.tmHeight;
}

wstr glyph(Glyph g) {
    if (F.fluent_icons) {
        switch (g) {
            case G_BACK: return L"";
            case G_FWD: return L"";
            case G_UP: return L"";
            case G_REFRESH: return L"";
            case G_STAR: return L"";
            case G_STAR_FILLED: return L"";
            case G_LIST: return L"";
            case G_GRID: return L"";
            case G_COLUMNS: return L"";
            case G_SEARCH: return L"";
            case G_DUAL: return L"";
            case G_THEME: return L"";
            case G_CLOSE: return L"";
            case G_ADD: return L"";
            case G_CHEVRON: return L"";
            case G_SETS: return L"";
            case G_MORE: return L"";
            case G_PREVIEW: return L"\ue8a1";
            case G_TAG: return L"";
        }
    }
    switch (g) {
        case G_BACK: return L"←";
        case G_FWD: return L"→";
        case G_UP: return L"↑";
        case G_REFRESH: return L"⟳";
        case G_STAR: return L"☆";
        case G_STAR_FILLED: return L"★";
        case G_LIST: return L"☰";
        case G_GRID: return L"▦";
        case G_COLUMNS: return L"▥";
        case G_SEARCH: return L"⌕";
        case G_DUAL: return L"◫";
        case G_THEME: return L"◐";
        case G_CLOSE: return L"×";
        case G_ADD: return L"+";
        case G_CHEVRON: return L"›";
        case G_SETS: return L"▤";
        case G_MORE: return L"…";
        case G_TAG: return L"●";
        case G_PREVIEW: return L"▣";
    }
    return L"?";
}

// ======================================================================
// window chrome
// ======================================================================
HWND g_mica_root = nullptr;

void style_window(HWND hwnd) {
    BOOL dark = !T.light;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    COLORREF cap = T.panel, text = T.fg, border = T.panel;
    if (hwnd == g_mica_root) cap = text = border = 0xFFFFFFFF;  // DWMWA_COLOR_DEFAULT: Mica shows in the title bar
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof cap);      // Windows 11
    DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof text);
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof border);
}

void use_dark_menus(bool dark) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    auto set = (SetPreferredAppMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemes)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    OSVERSIONINFOW v{sizeof v};
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto rgv = (RtlGetVersion)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv) rgv(&v);
    if (v.dwBuildNumber < 18362) return;  // the call only exists from Windows 10 1903
    if (set) set(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
}

void set_control_theme(HWND hwnd) {
    SetWindowTheme(hwnd, T.light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
}
