#include "gfx.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <commoncontrols.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::SolidBrush;
using Gdiplus::Pen;
using Gdiplus::Color;
using Gdiplus::RectF;
using Gdiplus::PointF;
using Gdiplus::REAL;
using Gdiplus::GdiplusStartupInput;
using Gdiplus::GdiplusStartup;
using Gdiplus::SmoothingModeAntiAlias;
using Gdiplus::PixelOffsetModeHalf;
using Gdiplus::LineCapRound;
using Gdiplus::LineJoinRound;
using Gdiplus::DashStyleDash;

// ======================================================================
// UI thread hand-off
// ======================================================================
static HWND g_ui = nullptr;
static ULONG_PTR g_gdiplus = 0;

void ui_init(HWND main_window) {
    g_ui = main_window;
    if (!g_gdiplus) {
        GdiplusStartupInput in;
        GdiplusStartup(&g_gdiplus, &in, nullptr);
    }
}

void post_ui(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    if (!g_ui || !PostMessageW(g_ui, WM_APP_TASK, 0, (LPARAM)p)) delete p;
}

bool ui_dispatch(MSG& msg) {
    if (msg.message != WM_APP_TASK || msg.hwnd != g_ui) return false;
    std::unique_ptr<std::function<void()>> p((std::function<void()>*)msg.lParam);
    try {
        (*p)();
    } catch (...) {
        log_line(L"error in a UI task");
    }
    return true;
}

void run_bg(std::function<void()> fn) {
    std::thread([f = std::move(fn)]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        try {
            f();
        } catch (...) {
            log_line(L"error in a background task");
        }
        CoUninitialize();
    }).detach();
}

// ======================================================================
// bitmaps
// ======================================================================
Bitmap::Bitmap(int w_, int h_) : w(w_), h(h_) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* p = nullptr;
    hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    bits = (uint32_t*)p;
    if (bits) memset(bits, 0, (size_t)w * h * 4);
}

Bitmap::~Bitmap() {
    if (hbm) DeleteObject(hbm);
}

void Bitmap::draw(HDC dc, int x, int y, int dw, int dh, BYTE alpha) const {
    if (!hbm) return;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, hbm);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    if (dw < 0) { dw = w; dh = h; }
    if (dw != w || dh != h) SetStretchBltMode(dc, HALFTONE);
    AlphaBlend(dc, x, y, dw, dh, mem, 0, 0, w, h, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

BitmapPtr bitmap_from_hbitmap(HBITMAP src) {
    BITMAP bm{};
    if (!GetObjectW(src, sizeof bm, &bm) || bm.bmWidth <= 0) return nullptr;
    int w = bm.bmWidth, h = std::abs(bm.bmHeight);
    auto out = std::make_shared<Bitmap>(w, h);
    if (!out->bits) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, src, 0, h, out->bits, &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines != h) return nullptr;
    bool has_alpha = false;
    for (int i = 0; i < w * h && !has_alpha; i++) has_alpha = (out->bits[i] >> 24) != 0;
    if (!has_alpha)
        for (int i = 0; i < w * h; i++) out->bits[i] |= 0xFF000000u;
    return out;
}

HBITMAP menu_bitmap(const Bitmap& b) {
    Bitmap* copy = new Bitmap(b.w, b.h);  // menus keep the HBITMAP: leak a few small bitmaps on purpose
    memcpy(copy->bits, b.bits, (size_t)b.w * b.h * 4);
    HBITMAP h = copy->hbm;
    copy->hbm = nullptr;
    delete copy;
    return h;
}

// ======================================================================
// painting
// ======================================================================
static bool under_mica(HWND h) { return g_mica_root && (h == g_mica_root || GetAncestor(h, GA_ROOT) == g_mica_root); }

Paint::Paint(HWND h) : hwnd(h) {
    wdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(wdc);
    if (under_mica(hwnd)) {  // a 32-bit buffer, so the alpha can be set before it goes on screen
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = std::max(1L, rc.right);
        bi.bmiHeader.biHeight = -std::max(1L, rc.bottom);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* p = nullptr;
        bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        bits = (uint32_t*)p;
    } else bmp = CreateCompatibleBitmap(wdc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    old = (HBITMAP)SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
}

Paint::~Paint() {
    if (bits) {
        GdiFlush();
        glass_fix(bits, std::max(1L, rc.right), std::max(1L, rc.bottom), glass);
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// ======================================================================
// Mica
// ======================================================================
static DWORD win_build() {
    static DWORD b = [] {
        OSVERSIONINFOW v{sizeof v};
        using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto rgv = (RtlGetVersion)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        if (rgv) rgv(&v);
        return v.dwBuildNumber;
    }();
    return b;
}

bool mica_supported() { return win_build() >= 22621; }  // Windows 11 22H2 (DWMWA_SYSTEMBACKDROP_TYPE)

bool set_mica(HWND top, bool on) {
    bool ok = false;
    if (mica_supported()) {
        int type = on ? 2 /*DWMSBT_MAINWINDOW (Mica)*/ : 1 /*DWMSBT_NONE*/;
        ok = SUCCEEDED(DwmSetWindowAttribute(top, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &type, sizeof type)) && on;
        MARGINS m = ok ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(top, &m);
    }
    g_mica_root = ok ? top : nullptr;
    style_window(top);
    RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_ERASE);
    return ok;
}

// GDI leaves the alpha at 0. Everything becomes solid, except the "glass" colour, which becomes a see-through tint of
// itself (so the theme's colour stays, with the blurred desktop under it). Pixels close to that colour (the soft edges
// of text and shapes) fade between the two, so there are no hard fringes.
void glass_fix(uint32_t* px, int w, int h, COLORREF glass) {
    size_t n = (size_t)w * h;
    if (glass == CLR_INVALID) {
        for (size_t i = 0; i < n; i++) px[i] |= 0xFF000000u;
        return;
    }
    const int A = T.light ? 150 : 125;  // how much of the theme colour covers the Mica
    int gr = GetRValue(glass), gg = GetGValue(glass), gb = GetBValue(glass);
    for (size_t i = 0; i < n; i++) {
        uint32_t p = px[i];
        int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        int d = std::max({std::abs(r - gr), std::abs(g - gg), std::abs(b - gb)});
        if (d >= 40) { px[i] = p | 0xFF000000u; continue; }
        int a = A + (255 - A) * d / 40;
        px[i] = ((uint32_t)a << 24) | ((uint32_t)(r * a / 255) << 16) | ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
    }
}

// Edit boxes draw themselves with GDI (alpha 0, which Mica would show through): paint them into a buffer instead,
// make it solid, and repaint after anything that changes what they show.
static LRESULT CALLBACK solid_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (!under_mica(h)) return DefSubclassProc(h, m, w, l);
    switch (m) {
        case WM_PAINT: {
            // let it paint as usual (cue banner, selection, everything), then read it back and make it solid
            LRESULT r = DefSubclassProc(h, m, w, l);
            RECT rc;
            GetClientRect(h, &rc);
            if (rc.right <= 0 || rc.bottom <= 0) return r;
            HDC wdc = GetDC(h);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = rc.right;
            bi.bmiHeader.biHeight = -rc.bottom;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* p = nullptr;
            HDC dc = CreateCompatibleDC(wdc);
            HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
            HGDIOBJ old = SelectObject(dc, bmp);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, wdc, 0, 0, SRCCOPY);
            GdiFlush();
            glass_fix((uint32_t*)p, rc.right, rc.bottom, CLR_INVALID);
            BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
            ReleaseDC(h, wdc);
            return r;
        }
        case WM_CHAR: case WM_KEYDOWN: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_MOUSEMOVE:
        case WM_SETTEXT: case EM_SETSEL: case EM_REPLACESEL: case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO:
        case WM_SETFOCUS: case WM_KILLFOCUS: {
            LRESULT r = DefSubclassProc(h, m, w, l);
            if (m != WM_MOUSEMOVE || (w & MK_LBUTTON)) InvalidateRect(h, nullptr, FALSE);
            return r;
        }
        case WM_NCDESTROY: RemoveWindowSubclass(h, solid_edit_proc, 9); break;
    }
    return DefSubclassProc(h, m, w, l);
}

void solid_edit(HWND edit) { SetWindowSubclass(edit, solid_edit_proc, 9, 0); }

void fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static Color gc(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

static void round_path(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    r = std::min(r, std::min(w, h) / 2);
    if (r <= 0.5f) { p.AddRectangle(RectF(x, y, w, h)); return; }
    REAL d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

void fill_round(HDC dc, const RECT& r, int radius, COLORREF c) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    GraphicsPath p;
    round_path(p, (REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top), (REAL)radius);
    SolidBrush b(gc(c));
    g.FillPath(&b, &p);
}

void frame_round(HDC dc, const RECT& r, int radius, COLORREF c, float width) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath p;
    REAL o = width / 2;
    round_path(p, r.left + o, r.top + o, r.right - r.left - width, r.bottom - r.top - width, (REAL)radius);
    Pen pen(gc(c), width);
    g.DrawPath(&pen, &p);
}

void fill_circle(HDC dc, float cx, float cy, float r, COLORREF c, COLORREF ring, float ring_w) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    SolidBrush b(gc(c));
    g.FillEllipse(&b, cx - r, cy - r, 2 * r, 2 * r);
    if (ring != CLR_INVALID && ring_w > 0) {
        Pen pen(gc(ring), ring_w);
        g.DrawEllipse(&pen, cx - r, cy - r, 2 * r, 2 * r);
    }
}

void hline(HDC dc, int x0, int x1, int y, COLORREF c) { fill(dc, RECT{x0, y, x1, y + 1}, c); }
void vline(HDC dc, int x, int y0, int y1, COLORREF c) { fill(dc, RECT{x, y0, x + 1, y1}, c); }

void text(HDC dc, const wstr& s, RECT r, COLORREF c, HFONT f, UINT flags) {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags);
    SelectObject(dc, old);
}

void dashed_rect(HDC dc, const RECT& r, COLORREF c) {
    Graphics g(dc);
    Pen pen(gc(c), 1);
    pen.SetDashStyle(DashStyleDash);
    g.DrawRectangle(&pen, (REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left - 1), (REAL)(r.bottom - r.top - 1));
}

void fill_alpha(HDC dc, const RECT& r, COLORREF c, BYTE alpha) {
    Graphics g(dc);
    SolidBrush b(gc(c, alpha));
    g.FillRectangle(&b, (REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
}

void draw_icon(HDC dc, Glyph gl, const RECT& r, COLORREF c, float size) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    float s = (float)S(size);
    float x0 = (r.left + r.right) / 2.0f - s / 2, y0 = (r.top + r.bottom) / 2.0f - s / 2;
    auto P = [&](float x, float y) { return PointF(x0 + x * s, y0 + y * s); };
    Pen pen(gc(c), std::max(1.2f, s / 11.0f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush br(gc(c));
    auto line = [&](float ax, float ay, float bx, float by) { g.DrawLine(&pen, P(ax, ay), P(bx, by)); };
    auto rrect = [&](float x, float y, float w, float h, float rad, bool filled) {
        GraphicsPath p;
        round_path(p, x0 + x * s, y0 + y * s, w * s, h * s, rad * s);
        if (filled) g.FillPath(&br, &p); else g.DrawPath(&pen, &p);
    };
    switch (gl) {
        case G_BACK: line(0.82f, 0.5f, 0.18f, 0.5f); line(0.45f, 0.22f, 0.18f, 0.5f); line(0.18f, 0.5f, 0.45f, 0.78f); break;
        case G_FWD: line(0.18f, 0.5f, 0.82f, 0.5f); line(0.55f, 0.22f, 0.82f, 0.5f); line(0.82f, 0.5f, 0.55f, 0.78f); break;
        case G_UP: line(0.5f, 0.82f, 0.5f, 0.18f); line(0.22f, 0.45f, 0.5f, 0.18f); line(0.5f, 0.18f, 0.78f, 0.45f); break;
        case G_REFRESH: {
            g.DrawArc(&pen, x0 + 0.18f * s, y0 + 0.18f * s, 0.64f * s, 0.64f * s, -60, 300);
            float ax = 0.5f + 0.32f * std::cos(-60 * 3.14159f / 180), ay = 0.5f + 0.32f * std::sin(-60 * 3.14159f / 180);
            line(ax, ay, ax - 0.02f, ay - 0.24f);
            line(ax, ay, ax - 0.24f, ay + 0.02f);
            break;
        }
        case G_STAR:
        case G_STAR_FILLED: {
            PointF pts[10];
            for (int i = 0; i < 10; i++) {
                float a = (float)(-3.14159265 / 2 + i * 3.14159265 / 5), rr = (i % 2 ? 0.2f : 0.42f);
                pts[i] = P(0.5f + rr * std::cos(a), 0.54f + rr * std::sin(a));
            }
            if (gl == G_STAR_FILLED) g.FillPolygon(&br, pts, 10);
            g.DrawPolygon(&pen, pts, 10);
            break;
        }
        case G_LIST:
            for (int i = 0; i < 3; i++) {
                float y = 0.27f + i * 0.23f;
                g.FillEllipse(&br, x0 + 0.13f * s, y0 + (y - 0.05f) * s, 0.1f * s, 0.1f * s);
                line(0.35f, y, 0.86f, y);
            }
            break;
        case G_GRID:
            rrect(0.14f, 0.14f, 0.3f, 0.3f, 0.06f, false); rrect(0.56f, 0.14f, 0.3f, 0.3f, 0.06f, false);
            rrect(0.14f, 0.56f, 0.3f, 0.3f, 0.06f, false); rrect(0.56f, 0.56f, 0.3f, 0.3f, 0.06f, false);
            break;
        case G_COLUMNS:
            rrect(0.1f, 0.18f, 0.8f, 0.64f, 0.08f, false); line(0.37f, 0.18f, 0.37f, 0.82f); line(0.63f, 0.18f, 0.63f, 0.82f);
            break;
        case G_SEARCH:
            g.DrawEllipse(&pen, x0 + 0.16f * s, y0 + 0.16f * s, 0.5f * s, 0.5f * s); line(0.6f, 0.6f, 0.84f, 0.84f);
            break;
        case G_DUAL:
            rrect(0.1f, 0.18f, 0.8f, 0.64f, 0.08f, false); line(0.5f, 0.18f, 0.5f, 0.82f);
            break;
        case G_THEME: {
            g.DrawEllipse(&pen, x0 + 0.16f * s, y0 + 0.16f * s, 0.68f * s, 0.68f * s);
            g.FillPie(&br, x0 + 0.16f * s, y0 + 0.16f * s, 0.68f * s, 0.68f * s, 90, 180);
            break;
        }
        case G_CLOSE: line(0.28f, 0.28f, 0.72f, 0.72f); line(0.72f, 0.28f, 0.28f, 0.72f); break;
        case G_ADD: line(0.5f, 0.2f, 0.5f, 0.8f); line(0.2f, 0.5f, 0.8f, 0.5f); break;
        case G_CHEVRON: line(0.4f, 0.25f, 0.62f, 0.5f); line(0.62f, 0.5f, 0.4f, 0.75f); break;
        case G_SETS: rrect(0.12f, 0.3f, 0.6f, 0.52f, 0.07f, false); line(0.3f, 0.18f, 0.88f, 0.18f); line(0.88f, 0.18f, 0.88f, 0.64f); break;
        case G_MORE:
            for (int i = 0; i < 3; i++) g.FillEllipse(&br, x0 + (0.18f + i * 0.27f) * s, y0 + 0.43f * s, 0.12f * s, 0.12f * s);
            break;
        case G_PREVIEW:  // a window with a panel on the right
            rrect(0.1f, 0.18f, 0.8f, 0.64f, 0.08f, false);
            line(0.6f, 0.18f, 0.6f, 0.82f);
            line(0.68f, 0.36f, 0.8f, 0.36f);
            line(0.68f, 0.5f, 0.8f, 0.5f);
            break;
        case G_TAG: g.FillEllipse(&br, x0 + 0.25f * s, y0 + 0.25f * s, 0.5f * s, 0.5f * s); break;
    }
}

// ======================================================================
// icon art: folders, the app icon, tag dots
// ======================================================================
namespace {
struct RGBAf { float r, g, b, a; };
using Shade = std::function<bool(float x, float y, RGBAf& out)>;

bool in_rrect(float x, float y, float x0, float y0, float x1, float y1, float r) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    float cx = std::min(std::max(x, x0 + r), x1 - r), cy = std::min(std::max(y, y0 + r), y1 - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

bool in_poly(float x, float y, const std::vector<std::pair<float, float>>& p) {
    bool in = false;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
        auto [xi, yi] = p[i];
        auto [xj, yj] = p[j];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

RGBAf lerp(RGBAf a, RGBAf b, float t) {
    t = std::max(0.f, std::min(1.f, t));
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
}
RGBAf rgbf(COLORREF c) { return {(float)GetRValue(c), (float)GetGValue(c), (float)GetBValue(c), 1}; }

// size×size pixels, ss×ss samples each; returns straight-alpha RGBA
std::vector<uint8_t> render(int size, const Shade& shade, int ss) {
    std::vector<uint8_t> out((size_t)size * size * 4, 0);
    float n = (float)size * ss;
    for (int oy = 0; oy < size; oy++)
        for (int ox = 0; ox < size; ox++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    RGBAf c;
                    if (shade((ox * ss + sx + 0.5f) / n, (oy * ss + sy + 0.5f) / n, c)) {
                        r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a;
                    }
                }
            if (a > 0) {
                uint8_t* p = &out[((size_t)oy * size + ox) * 4];
                p[0] = (uint8_t)(r / a); p[1] = (uint8_t)(g / a); p[2] = (uint8_t)(b / a);
                p[3] = (uint8_t)std::min(255.f, 255 * a / (ss * ss));
            }
        }
    return out;
}

BitmapPtr to_bitmap(const std::vector<uint8_t>& rgba, int size) {
    auto bm = std::make_shared<Bitmap>(size, size);
    if (!bm->bits) return bm;
    for (int i = 0; i < size * size; i++) {
        const uint8_t* p = &rgba[(size_t)i * 4];
        uint32_t a = p[3];
        bm->bits[i] = (a << 24) | ((p[0] * a / 255) << 16) | ((p[1] * a / 255) << 8) | (p[2] * a / 255);
    }
    return bm;
}

const std::vector<std::pair<float, float>> TAB = {{0.06f, 0.20f}, {0.06f, 0.17f}, {0.09f, 0.13f}, {0.35f, 0.13f},
                                                  {0.39f, 0.15f}, {0.45f, 0.22f}, {0.45f, 0.33f}, {0.06f, 0.33f}};

bool folder_shade(RGBAf base, float x, float y, int size, bool paper, RGBAf& out) {
    RGBAf white{255, 255, 255, 1};
    RGBAf dark{base.r * 0.7f, base.g * 0.7f, base.b * 0.7f, 1};
    RGBAf light = lerp(base, white, 0.22f);
    if (in_rrect(x, y, 0.05f, 0.36f, 0.95f, 0.86f, 0.075f)) {
        out = y < 0.36f + 1.4f / size ? lerp(light, white, 0.35f) : lerp(light, base, (y - 0.36f) / 0.42f);
        return true;
    }
    if (paper && in_rrect(x, y, 0.12f, 0.26f, 0.88f, 0.70f, 0.03f)) { out = {244, 245, 250, 1}; return true; }
    if (in_rrect(x, y, 0.05f, 0.20f, 0.95f, 0.84f, 0.07f) || in_poly(x, y, TAB)) { out = dark; return true; }
    return false;
}

std::map<std::pair<std::string, int>, BitmapPtr> g_folders;
std::map<std::pair<COLORREF, int>, BitmapPtr> g_dots;
}  // namespace

BitmapPtr folder_bitmap(const std::string& color, int size) {
    auto key = std::make_pair(color, size);
    auto it = g_folders.find(key);
    if (it != g_folders.end()) return it->second;
    RGBAf base = rgbf(folder_rgb(color));
    int ss = size <= 48 ? 4 : size <= 128 ? 2 : 1;
    auto rgba = render(size, [&](float x, float y, RGBAf& o) { return folder_shade(base, x, y, size, size >= 24, o); }, ss);
    return g_folders[key] = to_bitmap(rgba, size);
}

std::vector<uint8_t> app_icon_rgba(int size) {
    RGBAf c1{0x7a, 0xa2, 0xf7, 1}, c2{0xbb, 0x9a, 0xf7, 1}, white{255, 255, 255, 1};
    auto shade = [&](float x, float y, RGBAf& o) {
        if (!in_rrect(x, y, 0.04f, 0.04f, 0.96f, 0.96f, 0.23f)) return false;
        RGBAf tile = lerp(c1, c2, x * 0.6f + y * 0.4f);
        if (y < 0.5f) tile = lerp(tile, white, (0.5f - y) * 0.18f);
        float fx = (x - 0.19f) / 0.62f, fy = (y - 0.21f) / 0.62f;
        if (fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1) {
            if (in_rrect(fx, fy, 0.05f, 0.36f, 0.95f, 0.86f, 0.075f)) {
                o = lerp(white, RGBAf{232, 236, 255, 1}, (fy - 0.36f) / 0.5f);
                return true;
            }
            if (in_rrect(fx, fy, 0.05f, 0.20f, 0.95f, 0.84f, 0.07f) || in_poly(fx, fy, TAB)) {
                o = lerp(tile, white, 0.55f);
                return true;
            }
        }
        o = tile;
        return true;
    };
    return render(size, shade, size <= 64 ? 3 : size <= 128 ? 2 : 1);
}

HICON make_hicon(int size) {
    auto rgba = app_icon_rgba(size);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    uint32_t* px = (uint32_t*)bits;
    for (int i = 0; i < size * size; i++) {
        const uint8_t* p = &rgba[(size_t)i * 4];
        px[i] = ((uint32_t)p[3] << 24) | (p[0] << 16) | (p[1] << 8) | p[2];
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON h = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return h;
}

static std::string ico_from(const std::vector<int>& sizes, const std::function<std::vector<uint8_t>(int)>& art) {
    std::vector<std::string> frames;
    for (int s : sizes) {
        auto rgba = art(s);
        std::string f;
        BITMAPINFOHEADER bh{};
        bh.biSize = sizeof bh;
        bh.biWidth = s;
        bh.biHeight = s * 2;
        bh.biPlanes = 1;
        bh.biBitCount = 32;
        f.append((const char*)&bh, sizeof bh);
        for (int y = s - 1; y >= 0; y--)  // bottom-up BGRA
            for (int x = 0; x < s; x++) {
                const uint8_t* p = &rgba[((size_t)y * s + x) * 4];
                char px[4] = {(char)p[2], (char)p[1], (char)p[0], (char)p[3]};
                f.append(px, 4);
            }
        int mask_row = ((s + 31) / 32) * 4;
        f.append((size_t)mask_row * s, '\0');
        frames.push_back(f);
    }
    std::string out;
    auto u16 = [&](uint16_t v) { out.append((const char*)&v, 2); };
    auto u32 = [&](uint32_t v) { out.append((const char*)&v, 4); };
    u16(0); u16(1); u16((uint16_t)frames.size());
    uint32_t offset = 6 + 16 * (uint32_t)frames.size();
    for (size_t i = 0; i < frames.size(); i++) {
        out += (char)(sizes[i] % 256); out += (char)(sizes[i] % 256); out += '\0'; out += '\0';
        u16(1); u16(32); u32((uint32_t)frames[i].size()); u32(offset);
        offset += (uint32_t)frames[i].size();
    }
    for (auto& f : frames) out += f;
    return out;
}

std::string make_ico_file() { return ico_from({16, 20, 24, 32, 40, 48, 64, 96, 128, 256}, app_icon_rgba); }

std::string make_folder_ico(const std::string& color) {
    RGBAf base = rgbf(folder_rgb(color));
    return ico_from({16, 24, 32, 48, 64, 128, 256}, [&](int size) {
        int ss = size <= 48 ? 4 : size <= 128 ? 2 : 1;
        return render(size, [&](float x, float y, RGBAf& o) { return folder_shade(base, x, y, size, size >= 24, o); }, ss);
    });
}

BitmapPtr tag_dot(COLORREF c, int size) {
    auto key = std::make_pair(c, size);
    auto it = g_dots.find(key);
    if (it != g_dots.end()) return it->second;
    RGBAf col = rgbf(c), ring = rgbf(T.surface);
    float rr = 0.5f, edge = 1.2f / size;
    auto rgba = render(size, [&](float x, float y, RGBAf& o) {
        float d = std::sqrt((x - 0.5f) * (x - 0.5f) + (y - 0.5f) * (y - 0.5f));
        if (d > rr) return false;
        o = d < rr - edge ? col : ring;
        return true;
    }, 4);
    return g_dots[key] = to_bitmap(rgba, size);
}

void clear_art_cache() {
    g_folders.clear();
    g_dots.clear();
}

// ======================================================================
// Windows file icons
// ======================================================================
static const IID IID_IImageList_ = {0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};
static IImageList* g_lists[5] = {};
static std::map<std::pair<int, int>, HICON> g_icon_cache;

static IImageList* image_list(int which) {
    if (!g_lists[which]) {
        static const int ids[] = {SHIL_SMALL, SHIL_LARGE, SHIL_EXTRALARGE, SHIL_JUMBO};
        SHGetImageList(ids[which], IID_IImageList_, (void**)&g_lists[which]);
    }
    return g_lists[which];
}

int sys_icon_index(const wstr& path, bool dir, bool real_file) {
    SHFILEINFOW info{};
    UINT flags = SHGFI_SYSICONINDEX | (real_file ? 0 : SHGFI_USEFILEATTRIBUTES);
    if (!SHGetFileInfoW(path.c_str(), dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &info, sizeof info, flags))
        return -1;
    return info.iIcon;
}

wstr sys_type_name(const wstr& ext, bool dir) {
    static std::map<wstr, wstr> cache;
    wstr key = dir ? L"<dir>" : ext;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    SHFILEINFOW info{};
    wstr probe = dir ? L"folder" : L"x" + ext;
    wstr name;
    if (SHGetFileInfoW(probe.c_str(), dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &info, sizeof info,
                       SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
        name = info.szTypeName;
    if (name.empty()) {
        if (dir) name = L"File folder";
        else if (ext.size() > 1) {
            wstr up = ext.substr(1);
            CharUpperBuffW(&up[0], (DWORD)up.size());
            name = up + L" File";
        } else name = L"File";
    }
    return cache[key] = name;
}

void draw_sys_icon(HDC dc, int index, int x, int y, int size) {
    if (index < 0) return;
    int which = size <= S(16) + 2 ? IS_SMALL : size <= S(32) ? IS_LARGE : size <= 48 * g_dpi / 96 ? IS_XL : IS_JUMBO;
    IImageList* il = image_list(which);
    if (!il && which == IS_JUMBO) il = image_list(which = IS_XL);
    if (!il) return;
    int iw = 0, ih = 0;
    il->GetIconSize(&iw, &ih);
    if (iw == size) {
        ImageList_DrawEx((HIMAGELIST)il, index, dc, x, y, 0, 0, CLR_NONE, CLR_NONE, ILD_TRANSPARENT);
        return;
    }
    auto key = std::make_pair(index, which);
    HICON h = nullptr;
    auto it = g_icon_cache.find(key);
    if (it != g_icon_cache.end()) h = it->second;
    else {
        il->GetIcon(index, ILD_TRANSPARENT, &h);
        g_icon_cache[key] = h;
    }
    if (h) DrawIconEx(dc, x, y, h, size, size, 0, nullptr, DI_NORMAL);
}

// ======================================================================
// thumbnails: one background thread (COM single-threaded apartment)
// ======================================================================
namespace {
struct ImgJob {
    wstr path;
    int size;
    bool thumb;
    std::function<void(BitmapPtr)> done;
    unsigned gen;
};
std::mutex g_mx;
std::condition_variable g_cv;
std::deque<ImgJob> g_jobs;
std::atomic<unsigned> g_gen{0};
bool g_started = false;

BitmapPtr shell_image(const wstr& path, int size, bool thumb) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) || !f) return nullptr;
    HBITMAP hbm = nullptr;
    SIZE sz{size, size};
    HRESULT hr = f->GetImage(sz, thumb ? SIIGBF_RESIZETOFIT : SIIGBF_ICONONLY, &hbm);
    f->Release();
    if (FAILED(hr) || !hbm) return nullptr;
    BitmapPtr out = bitmap_from_hbitmap(hbm);
    DeleteObject(hbm);
    return out;
}

void worker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        ImgJob job;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        if (job.gen != g_gen.load()) continue;
        BitmapPtr img;
        try {
            img = shell_image(job.path, job.size, job.thumb);
            if (!img && job.thumb) img = shell_image(job.path, job.size, false);
        } catch (...) {
            img = nullptr;
        }
        auto done = job.done;
        post_ui([done, img] { done(img); });
    }
}
}  // namespace

void request_image(const wstr& path, int size, bool thumbnail, std::function<void(BitmapPtr)> done) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_started) {
        g_started = true;
        std::thread(worker).detach();
    }
    g_jobs.push_back({path, size, thumbnail, std::move(done), g_gen.load()});
    g_cv.notify_one();
}

void cancel_images() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_gen++;
    g_jobs.clear();
}
