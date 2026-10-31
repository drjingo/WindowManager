// UI building blocks: a base class for custom-drawn windows, menus, drag & drop (real OLE, works with Windows
// Explorer), text fields, buttons, check boxes, tooltips, a simple list and modal dialogs.
#pragma once
#include "gfx.h"
#include <oleidl.h>
#include <shlobj.h>

// ======================================================================
// Control: a custom-drawn window
// ======================================================================
class Control {
  public:
    HWND hwnd = nullptr;
    virtual ~Control();
    bool create(HWND parent, DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, DWORD ex = 0);
    bool create_top(HWND owner, const wstr& title, DWORD style, DWORD ex, int x, int y, int w, int h);
    static Control* from(HWND h);

    virtual LRESULT on_message(UINT m, WPARAM w, LPARAM l);
    virtual void on_paint(HDC dc, const RECT& rc) {}
    virtual COLORREF glass_color() const { return CLR_INVALID; }  // see-through under Mica (CLR_INVALID: solid)
    virtual void on_size(int w, int h) {}
    virtual void on_mouse_move(int x, int y, WPARAM keys) {}
    virtual void on_mouse_leave() {}
    virtual void on_lbutton_down(int x, int y, WPARAM keys) {}
    virtual void on_lbutton_up(int x, int y, WPARAM keys) {}
    virtual void on_lbutton_dbl(int x, int y, WPARAM keys) { on_lbutton_down(x, y, keys); }
    virtual void on_rbutton_up(int x, int y, WPARAM keys) {}
    virtual void on_mbutton_down(int x, int y, WPARAM keys) {}
    virtual bool on_wheel(int delta, WPARAM keys, int x, int y) { return false; }
    virtual bool on_key(UINT vk, bool ctrl, bool shift, bool alt) { return false; }
    virtual void on_char(wchar_t c) {}
    virtual void on_timer(UINT_PTR id) {}
    virtual void on_focus(bool got) {}
    virtual void on_close() { DestroyWindow(hwnd); }
    virtual void on_capture_lost() {}
    virtual void on_command(int code, HWND ctl) {}

    void invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    void move(int x, int y, int w, int h) { if (hwnd) MoveWindow(hwnd, x, y, std::max(0, w), std::max(0, h), TRUE); }
    void show(bool on) { if (hwnd) ShowWindow(hwnd, on ? SW_SHOWNA : SW_HIDE); }
    bool visible() const { return hwnd && IsWindowVisible(hwnd); }
    int width() const { RECT r{}; if (hwnd) GetClientRect(hwnd, &r); return r.right; }
    int height() const { RECT r{}; if (hwnd) GetClientRect(hwnd, &r); return r.bottom; }
    void track_leave();
    POINT cursor() const;  // mouse position in client coordinates
    bool hovering_ = false;
};

// colours for native edit boxes inside a Control
HBRUSH field_brush();
void reset_brushes();  // after a theme change

// ======================================================================
// Menu: popup menus that run a function when you pick an item
// ======================================================================
class Menu {
  public:
    Menu();
    explicit Menu(const Menu& parent_for_sub, int);  // a submenu sharing the parent's actions
    ~Menu();
    Menu(const Menu&) = delete;
    void item(const wstr& label, std::function<void()> fn, bool enabled = true, bool checked = false,
              HBITMAP bmp = nullptr);
    void disabled(const wstr& label);
    void sep();
    void sub(const wstr& label, Menu& m, bool enabled = true);
    void show(HWND owner, POINT screen, bool align_right = false);
    bool empty() const { return GetMenuItemCount(h_) == 0; }
    HMENU h_;
    std::shared_ptr<std::vector<std::function<void()>>> acts_;
    bool owns_ = true;
};

// ======================================================================
// Drag & drop
// ======================================================================
class DropTarget : public IDropTarget {
  public:
    // over: point in client coordinates → the effect (0: can't drop here); called often
    std::function<DWORD(POINT pt, DWORD keys, DWORD allowed, const std::vector<wstr>& paths)> over;
    std::function<void()> leave;
    std::function<void(POINT pt, DWORD effect, const std::vector<wstr>& paths)> drop;

    explicit DropTarget(HWND h);
    virtual ~DropTarget() = default;
    void revoke();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* eff) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override;

  private:
    HWND hwnd_;
    ULONG refs_ = 1;
    std::vector<wstr> paths_;
    DWORD allowed_ = 0;
    IDropTargetHelper* helper_ = nullptr;
};
// the effect Windows Explorer would use: Ctrl copies, Shift moves, else move on the same drive / copy across
DWORD choose_effect(const std::vector<wstr>& paths, const wstr& dest, DWORD keys, DWORD allowed);
DWORD start_drag(HWND src, const std::vector<wstr>& paths);  // runs until the drop; returns the effect
extern bool g_dragging;  // true while we're the drag source

// ======================================================================
// small controls
// ======================================================================
class Tip {  // one tooltip window for the whole app
  public:
    static void show(HWND owner, const wstr& text, POINT screen);
    static void hide();
};

class Button : public Control {
  public:
    wstr label;
    bool primary = false, enabled = true;
    std::function<void()> click;
    void on_paint(HDC dc, const RECT& rc) override;
    void on_mouse_move(int x, int y, WPARAM) override;
    void on_mouse_leave() override { hovering_ = false; invalidate(); }
    void on_lbutton_down(int x, int y, WPARAM) override { if (enabled && click) click(); }
    bool on_key(UINT vk, bool, bool, bool) override;
    void set_enabled(bool on) { enabled = on; invalidate(); }
};

class Check : public Control {
  public:
    wstr label;
    bool checked = false;
    std::function<void()> changed;
    void on_paint(HDC dc, const RECT& rc) override;
    void on_lbutton_down(int, int, WPARAM) override { checked = !checked; invalidate(); if (changed) changed(); }
    int ideal_width();
};

class Label : public Control {
  public:
    wstr text_;
    COLORREF color = CLR_INVALID;
    HFONT font = nullptr;
    UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
    void set(const wstr& t, COLORREF c = CLR_INVALID) { text_ = t; color = c; invalidate(); }
    void on_paint(HDC dc, const RECT& rc) override;
};

// a native edit box with the theme's colours; on_key gets Enter / Escape / Tab / arrows
class TextBox : public Control {
  public:
    HWND edit = nullptr;
    wstr hint;
    std::function<bool(UINT vk, bool ctrl, bool shift)> key;
    std::function<void()> changed;
    std::function<void(bool)> focus_changed;
    bool focused = false;
    bool create_in(HWND parent, bool border = true);
    wstr get() const;
    void set(const wstr& s, bool notify = true);
    void select_all();
    void focus();
    void on_paint(HDC dc, const RECT& rc) override;
    void on_size(int w, int h) override;
    void on_lbutton_down(int, int, WPARAM) override { focus(); }
    LRESULT on_message(UINT m, WPARAM w, LPARAM l) override;
    COLORREF glass_color() const override { return T.bg; }
    bool quiet = false;
};

// a list of rows with a few columns (bulk rename preview, what's inside a zip)
class SimpleList : public Control {
  public:
    struct Row {
        std::vector<wstr> cells;
        COLORREF color = CLR_INVALID;
        int indent = 0;
        BitmapPtr icon;
        int sys_icon = -1;
    };
    std::vector<wstr> headers;
    std::vector<int> widths;  // 0 = the rest
    std::vector<Row> rows;
    int scroll = 0;
    void set_rows(std::vector<Row> r) { rows = std::move(r); scroll = std::min(scroll, max_scroll()); invalidate(); }
    void on_paint(HDC dc, const RECT& rc) override;
    bool on_wheel(int delta, WPARAM, int, int) override;
    void on_lbutton_down(int x, int y, WPARAM) override;
    void on_mouse_move(int x, int y, WPARAM keys) override;
    void on_lbutton_up(int, int, WPARAM) override { drag_ = false; ReleaseCapture(); }
    int row_h() const;
    int head_h() const { return headers.empty() ? 0 : S(28); }
    int max_scroll() const;
  private:
    bool drag_ = false;
    int drag_y_ = 0, drag_scroll_ = 0;
};

// slim custom scrollbar helper (drawn by its owner)
struct ScrollBar {
    int total = 0, page = 0, pos = 0;  // content height, visible height, offset
    RECT track{};
    bool dragging = false, hot = false;
    int grab = 0;
    RECT thumb() const;
    void paint(HDC dc) const;
    bool hit(int x, int y) const { return total > page && x >= track.left - S(4) && x < track.right && y >= track.top && y < track.bottom; }
    bool press(int x, int y);       // → true if it took the mouse
    bool drag_to(int y);             // while dragging → pos changed
    int max_pos() const { return std::max(0, total - page); }
};

// ======================================================================
// modal dialogs
// ======================================================================
class Dialog : public Control {
  public:
    bool done = false;
    int result = 0;
    void run(HWND owner);  // shows it and waits until done
    void finish(int r) { result = r; done = true; }
    LRESULT on_message(UINT m, WPARAM w, LPARAM l) override;
    void on_close() override { finish(0); }
    void on_paint(HDC dc, const RECT& rc) override;
    bool on_key(UINT vk, bool ctrl, bool shift, bool alt) override;
    std::function<void()> enter;  // Enter pressed anywhere
};
bool ask_text(HWND owner, const wstr& title, const wstr& prompt, wstr& value);
bool confirm(HWND owner, const wstr& title, const wstr& message, const wstr& ok_label = L"OK", bool danger = false);
void center_on(HWND w, HWND owner);
