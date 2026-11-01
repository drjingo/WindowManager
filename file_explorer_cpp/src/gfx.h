// Drawing: double buffering, smooth shapes (GDI+), text, vector toolbar icons, folder / app icon art,
// Windows file icons and thumbnails, and running work in the background.
#pragma once
#include "theme.h"

// ---- run something on the UI thread (from any thread)
void ui_init(HWND main_window);
void post_ui(std::function<void()> fn);
bool ui_dispatch(MSG& msg);  // call in the message loop; true if handled
constexpr UINT WM_APP_TASK = WM_APP + 77;
void run_bg(std::function<void()> fn);  // a short-lived background thread

// ---- a 32-bit premultiplied bitmap
struct Bitmap {
    HBITMAP hbm = nullptr;
    int w = 0, h = 0;
    uint32_t* bits = nullptr;  // BGRA premultiplied, top-down
    Bitmap() = default;
    Bitmap(int w, int h);
    ~Bitmap();
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;
    void draw(HDC dc, int x, int y, int dw = -1, int dh = -1, BYTE alpha = 255) const;
};
using BitmapPtr = std::shared_ptr<Bitmap>;
BitmapPtr bitmap_from_hbitmap(HBITMAP hbm);  // copies (thumbnails); fixes missing alpha
HBITMAP menu_bitmap(const Bitmap& b);         // for menu item pictures (copy)

// ---- painting helpers
struct Paint {  // double-buffered WM_PAINT
    HWND hwnd;
    PAINTSTRUCT ps;
    HDC wdc, dc;
    HBITMAP bmp, old;
    RECT rc;
    explicit Paint(HWND h);
    ~Paint();
};
void fill(HDC dc, const RECT& r, COLORREF c);
void fill_round(HDC dc, const RECT& r, int radius, COLORREF c);
void frame_round(HDC dc, const RECT& r, int radius, COLORREF c, float width = 1.0f);
void fill_circle(HDC dc, float cx, float cy, float r, COLORREF c, COLORREF ring = CLR_INVALID, float ring_w = 0);
void hline(HDC dc, int x0, int x1, int y, COLORREF c);
void vline(HDC dc, int x, int y0, int y1, COLORREF c);
void text(HDC dc, const wstr& s, RECT r, COLORREF c, HFONT f, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
void dashed_rect(HDC dc, const RECT& r, COLORREF c);
void fill_alpha(HDC dc, const RECT& r, COLORREF c, BYTE alpha);  // see-through fill (rubber band)
void draw_icon(HDC dc, Glyph g, const RECT& r, COLORREF c, float size = 16);  // vector toolbar icons

// ---- icon art
BitmapPtr folder_bitmap(const std::string& color, int size);   // cached
std::vector<uint8_t> app_icon_rgba(int size);                  // RGBA, straight alpha
HICON make_hicon(int size);
std::string make_ico_file();                                    // the app icon: .ico bytes (16 … 256)
std::string make_folder_ico(const std::string& color);          // a coloured folder as .ico (Windows Explorer)
BitmapPtr tag_dot(COLORREF c, int size);                        // cached
BitmapPtr gradient_dot(const std::string& spec, int size);     // a colour dot; gradients shown as such (cached)
void clear_art_cache();                                         // after a theme change

// ---- Windows file icons (system image lists) and thumbnails
enum IconSize { IS_SMALL, IS_LARGE, IS_XL, IS_JUMBO };  // 16, 32, 48, 256 (at 96 dpi)
int sys_icon_index(const wstr& path, bool is_dir, bool real_file);  // real_file: ask the file itself (.exe, .lnk…)
wstr sys_type_name(const wstr& ext, bool is_dir);
void draw_sys_icon(HDC dc, int index, int x, int y, int size);
// thumbnails come back on the UI thread; thumbnail=false: the file's big icon
void request_image(const wstr& path, int size, bool thumbnail, std::function<void(BitmapPtr)> done);
void cancel_images();  // drop queued requests (you went to another folder)
