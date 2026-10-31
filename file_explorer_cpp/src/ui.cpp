#include "ui.h"
#include <commctrl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>

static HINSTANCE hinst() { return GetModuleHandleW(nullptr); }

// ======================================================================
// Control
// ======================================================================
static LRESULT CALLBACK control_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Control* c = (Control*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        c = (Control*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)c);
        if (c) c->hwnd = h;
    }
    if (!c) return DefWindowProcW(h, m, w, l);
    if (m == WM_NCDESTROY) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        c->hwnd = nullptr;
        return DefWindowProcW(h, m, w, l);
    }
    return c->on_message(m, w, l);
}

static void register_classes() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = control_proc;
    wc.hInstance = hinst();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FECtl";
    RegisterClassExW(&wc);
    wc.lpszClassName = L"FETop";
    wc.hIcon = LoadIconW(hinst(), MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = make_hicon(32);
    wc.hIconSm = (HICON)LoadImageW(hinst(), MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    if (!wc.hIconSm) wc.hIconSm = make_hicon(16);
    RegisterClassExW(&wc);
}

Control::~Control() {
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        HWND h = hwnd;
        hwnd = nullptr;
        DestroyWindow(h);
    }
}

bool Control::create(HWND parent, DWORD style, DWORD ex) {
    register_classes();
    return CreateWindowExW(ex, L"FECtl", L"", style, 0, 0, 0, 0, parent, nullptr, hinst(), this) != nullptr;
}

bool Control::create_top(HWND owner, const wstr& title, DWORD style, DWORD ex, int x, int y, int w, int h) {
    register_classes();
    return CreateWindowExW(ex, L"FETop", title.c_str(), style, x, y, w, h, owner, nullptr, hinst(), this) != nullptr;
}

Control* Control::from(HWND h) {
    wchar_t cls[16] = {};
    GetClassNameW(h, cls, 16);
    if (wcscmp(cls, L"FECtl") && wcscmp(cls, L"FETop")) return nullptr;
    return (Control*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

void Control::track_leave() {
    TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
    TrackMouseEvent(&t);
    hovering_ = true;
}

POINT Control::cursor() const {
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(hwnd, &p);
    return p;
}

static HBRUSH g_field_brush = nullptr;
HBRUSH field_brush() {
    if (!g_field_brush) g_field_brush = CreateSolidBrush(T.field);
    return g_field_brush;
}
void reset_brushes() {
    if (g_field_brush) DeleteObject(g_field_brush);
    g_field_brush = nullptr;
}

LRESULT Control::on_message(UINT m, WPARAM w, LPARAM l) {
    int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
    switch (m) {
        case WM_PAINT: {
            Paint p(hwnd);
            on_paint(p.dc, p.rc);
            p.glass = glass_color();
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: on_size(LOWORD(l), HIWORD(l)); return 0;
        case WM_MOUSEMOVE:
            if (!hovering_) track_leave();
            on_mouse_move(x, y, w);
            return 0;
        case WM_MOUSELEAVE: hovering_ = false; on_mouse_leave(); return 0;
        case WM_LBUTTONDOWN: on_lbutton_down(x, y, w); return 0;
        case WM_LBUTTONUP: on_lbutton_up(x, y, w); return 0;
        case WM_LBUTTONDBLCLK: on_lbutton_dbl(x, y, w); return 0;
        case WM_RBUTTONUP: on_rbutton_up(x, y, w); return 0;
        case WM_MBUTTONDOWN: on_mbutton_down(x, y, w); return 0;
        case WM_MOUSEWHEEL: {
            POINT p{x, y};
            ScreenToClient(hwnd, &p);
            if (on_wheel(GET_WHEEL_DELTA_WPARAM(w), GET_KEYSTATE_WPARAM(w), p.x, p.y)) return 0;
            break;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (on_key((UINT)w, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0)) return 0;
            break;
        case WM_CHAR: on_char((wchar_t)w); return 0;
        case WM_TIMER: on_timer(w); return 0;
        case WM_SETFOCUS: on_focus(true); return 0;
        case WM_KILLFOCUS: on_focus(false); return 0;
        case WM_CLOSE: on_close(); return 0;
        case WM_CAPTURECHANGED: on_capture_lost(); return 0;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
        case WM_COMMAND: on_command(HIWORD(w), (HWND)l); return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)w;
            SetTextColor(dc, T.fg);
            SetBkColor(dc, T.field);
            return (LRESULT)field_brush();
        }
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ======================================================================
// Menu
// ======================================================================
Menu::Menu() : h_(CreatePopupMenu()), acts_(std::make_shared<std::vector<std::function<void()>>>()) {}
Menu::Menu(const Menu& parent, int) : h_(CreatePopupMenu()), acts_(parent.acts_) {}
Menu::~Menu() {
    if (owns_ && h_) DestroyMenu(h_);
}

void Menu::item(const wstr& label, std::function<void()> fn, bool enabled, bool checked, HBITMAP bmp) {
    acts_->push_back(std::move(fn));
    UINT id = (UINT)acts_->size();
    MENUITEMINFOW mi{sizeof mi};
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | (bmp ? MIIM_BITMAP : 0);
    mi.wID = id;
    mi.dwTypeData = (LPWSTR)label.c_str();
    mi.fState = (enabled ? MFS_ENABLED : MFS_DISABLED) | (checked ? MFS_CHECKED : 0);
    mi.hbmpItem = bmp;
    InsertMenuItemW(h_, GetMenuItemCount(h_), TRUE, &mi);
}

void Menu::disabled(const wstr& label) { item(label, nullptr, false); }
void Menu::sep() { if (GetMenuItemCount(h_) > 0) AppendMenuW(h_, MF_SEPARATOR, 0, nullptr); }

void Menu::sub(const wstr& label, Menu& m, bool enabled) {
    AppendMenuW(h_, MF_POPUP | (enabled ? 0 : MF_GRAYED), (UINT_PTR)m.h_, label.c_str());
    m.owns_ = false;  // destroyed with this menu
}

void Menu::show(HWND owner, POINT pt, bool align_right) {
    Tip::hide();
    SetForegroundWindow(owner);
    UINT id = TrackPopupMenuEx(h_, TPM_RETURNCMD | TPM_RIGHTBUTTON | (align_right ? TPM_RIGHTALIGN : 0), pt.x, pt.y,
                               owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    if (id > 0 && id <= acts_->size() && (*acts_)[id - 1]) {
        auto fn = (*acts_)[id - 1];
        fn();
    }
}

// ======================================================================
// Drag & drop
// ======================================================================
bool g_dragging = false;

static std::vector<wstr> paths_from(IDataObject* d) {
    std::vector<wstr> out;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (d && SUCCEEDED(d->GetData(&fe, &sm))) {
        HDROP h = (HDROP)GlobalLock(sm.hGlobal);
        if (h) {
            UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; i++) {
                UINT len = DragQueryFileW(h, i, nullptr, 0);
                wstr p(len, L'\0');
                DragQueryFileW(h, i, &p[0], len + 1);
                out.push_back(p);
            }
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
    }
    return out;
}

DropTarget::DropTarget(HWND h) : hwnd_(h) {
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    RegisterDragDrop(h, this);
}

void DropTarget::revoke() {
    if (hwnd_) RevokeDragDrop(hwnd_);
    hwnd_ = nullptr;
    if (helper_) { helper_->Release(); helper_ = nullptr; }
}

HRESULT DropTarget::QueryInterface(REFIID riid, void** p) {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) { *p = this; AddRef(); return S_OK; }
    *p = nullptr;
    return E_NOINTERFACE;
}

ULONG DropTarget::Release() {
    ULONG r = --refs_;
    if (!r) delete this;
    return r;
}

HRESULT DropTarget::DragEnter(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) {
    paths_ = paths_from(d);
    allowed_ = *eff;
    POINT p{pt.x, pt.y};
    if (helper_) helper_->DragEnter(hwnd_, d, &p, *eff);
    return DragOver(keys, pt, eff);
}

HRESULT DropTarget::DragOver(DWORD keys, POINTL pt, DWORD* eff) {
    POINT p{pt.x, pt.y};
    POINT c = p;
    ScreenToClient(hwnd_, &c);
    DWORD e = paths_.empty() || !over ? DROPEFFECT_NONE : over(c, keys, allowed_, paths_);
    *eff = e;
    if (helper_) helper_->DragOver(&p, e);
    return S_OK;
}

HRESULT DropTarget::DragLeave() {
    if (helper_) helper_->DragLeave();
    if (leave) leave();
    paths_.clear();
    return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) {
    POINT p{pt.x, pt.y};
    POINT c = p;
    ScreenToClient(hwnd_, &c);
    std::vector<wstr> paths = paths_.empty() ? paths_from(d) : paths_;
    DWORD e = paths.empty() || !over ? DROPEFFECT_NONE : over(c, keys, allowed_, paths);
    if (helper_) helper_->Drop(d, &p, e);
    if (leave) leave();
    *eff = e;
    paths_.clear();
    if (e != DROPEFFECT_NONE && drop) {
        // run after the drag has finished (the source may be waiting on us)
        auto fn = drop;
        post_ui([fn, c, e, paths] { fn(c, e, paths); });
    }
    return S_OK;
}

DWORD choose_effect(const std::vector<wstr>& paths, const wstr& dest, DWORD keys, DWORD allowed) {
    DWORD e;
    if (keys & MK_CONTROL) e = DROPEFFECT_COPY;
    else if (keys & MK_SHIFT) e = DROPEFFECT_MOVE;
    else {
        bool same = true;
        for (auto& p : paths) if (!same_drive(p, dest)) { same = false; break; }
        e = same ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    }
    if (!(allowed & e)) e = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (allowed & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
    return e;
}

DWORD start_drag(HWND src, const std::vector<wstr>& paths) {
    if (paths.empty()) return DROPEFFECT_NONE;
    std::vector<PIDLIST_ABSOLUTE> abs;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) abs.push_back(pidl);
    }
    DWORD eff = DROPEFFECT_NONE;
    if (!abs.empty()) {
        bool same_parent = true;
        wstr parent = norm(dir_name(paths[0]));
        for (auto& p : paths) if (norm(dir_name(p)) != parent) same_parent = false;
        IDataObject* dobj = nullptr;
        PIDLIST_ABSOLUTE folder = nullptr;
        std::vector<PCUITEMID_CHILD> kids;
        if (same_parent) {
            folder = ILClone(abs[0]);
            ILRemoveLastID(folder);
            for (auto a : abs) kids.push_back(ILFindLastID(a));
            SHCreateDataObject(folder, (UINT)kids.size(), kids.data(), nullptr, IID_PPV_ARGS(&dobj));
        } else {
            for (auto a : abs) kids.push_back((PCUITEMID_CHILD)a);
            SHCreateDataObject(nullptr, (UINT)kids.size(), kids.data(), nullptr, IID_PPV_ARGS(&dobj));
        }
        if (dobj) {
            g_dragging = true;
            SHDoDragDrop(src, dobj, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
            g_dragging = false;
            dobj->Release();
        }
        if (folder) ILFree(folder);
    }
    for (auto a : abs) ILFree(a);
    return eff;
}

// ======================================================================
// tooltip
// ======================================================================
namespace {
class TipWindow : public Control {
  public:
    wstr text_;
    void on_paint(HDC dc, const RECT& rc) override {
        fill(dc, rc, T.field);
        RECT b = rc;
        HBRUSH br = CreateSolidBrush(T.border);
        FrameRect(dc, &b, br);
        DeleteObject(br);
        RECT t{rc.left + S(8), rc.top, rc.right - S(8), rc.bottom};
        text(dc, text_, t, T.fg, F.small);
    }
};
TipWindow* g_tip = nullptr;
}  // namespace

void Tip::show(HWND owner, const wstr& t, POINT pt) {
    if (!g_tip) {
        g_tip = new TipWindow();
        g_tip->create_top(owner, L"", WS_POPUP, WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, 0, 0, 10, 10);
    }
    g_tip->text_ = t;
    HDC dc = GetDC(g_tip->hwnd);
    int w = text_width(dc, F.small, t) + S(16), h = font_height(F.small) + S(10);
    ReleaseDC(g_tip->hwnd, dc);
    SetWindowPos(g_tip->hwnd, HWND_TOPMOST, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_tip->invalidate();
}

void Tip::hide() {
    if (g_tip && g_tip->hwnd) ShowWindow(g_tip->hwnd, SW_HIDE);
}

// ======================================================================
// Button / Check / Label
// ======================================================================
void Button::on_paint(HDC dc, const RECT& rc) {
    HWND parent = GetParent(hwnd);
    Control* pc = Control::from(parent);
    (void)pc;
    fill(dc, rc, T.bg);
    COLORREF bg = !enabled ? T.border : primary ? T.accent : T.field;
    COLORREF fg = !enabled ? T.faint : primary ? T.on_accent : T.fg;
    if (enabled && hovering_) bg = mix(bg, primary ? RGB(255, 255, 255) : T.accent, 0.12);
    fill_round(dc, rc, S(6), bg);
    text(dc, label, rc, fg, primary ? F.ui_bold : F.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void Button::on_mouse_move(int, int, WPARAM) {
    if (!hovering_) { track_leave(); }
    SetCursor(LoadCursorW(nullptr, enabled ? IDC_HAND : IDC_ARROW));
    invalidate();
}

bool Button::on_key(UINT vk, bool, bool, bool) {
    if ((vk == VK_RETURN || vk == VK_SPACE) && enabled && click) { click(); return true; }
    return false;
}

int Check::ideal_width() {
    HDC dc = GetDC(hwnd);
    int w = text_width(dc, F.small, label) + S(28);
    ReleaseDC(hwnd, dc);
    return w;
}

void Check::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.bg);
    int s = S(16), y = (rc.bottom - s) / 2;
    RECT box{0, y, s, y + s};
    if (checked) {
        fill_round(dc, box, S(4), T.accent);
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(T.on_accent), GetGValue(T.on_accent), GetBValue(T.on_accent)), S(2));
        Gdiplus::PointF pts[3] = {{(float)S(4), (float)(y + S(8))}, {(float)S(7), (float)(y + S(11))}, {(float)S(12), (float)(y + S(5))}};
        g.DrawLines(&pen, pts, 3);
    } else {
        fill_round(dc, box, S(4), T.field);
        frame_round(dc, box, S(4), T.faint, 1.2f);
    }
    RECT t{s + S(8), 0, rc.right, rc.bottom};
    text(dc, label, t, T.fg, F.small);
}

void Label::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.bg);
    text(dc, text_, rc, color == CLR_INVALID ? T.muted : color, font ? font : F.small, flags);
}

// ======================================================================
// TextBox
// ======================================================================
static LRESULT CALLBACK edit_sub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    TextBox* tb = (TextBox*)ref;
    switch (m) {
        case WM_KEYDOWN:
            if (tb->key && tb->key((UINT)w, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0)) return 0;
            if (w == 'A' && GetKeyState(VK_CONTROL) < 0) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
            break;
        case WM_CHAR:
            if (w == L'\r' || w == 27 || w == L'\t' || w == 1 /*Ctrl+A*/) return 0;  // no beep
            if (w == 127 /*Ctrl+Backspace*/) return 0;
            break;
        case WM_SETFOCUS:
            tb->focused = true;
            tb->invalidate();
            if (tb->focus_changed) tb->focus_changed(true);
            break;
        case WM_KILLFOCUS:
            tb->focused = false;
            tb->invalidate();
            if (tb->focus_changed) tb->focus_changed(false);
            break;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_HASSETSEL | DLGC_WANTCHARS;
        case WM_NCDESTROY: RemoveWindowSubclass(h, edit_sub, 1); break;
    }
    return DefSubclassProc(h, m, w, l);
}

bool TextBox::create_in(HWND parent, bool) {
    if (!create(parent)) return false;
    edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, (WPARAM)F.ui, TRUE);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    if (!hint.empty()) SendMessageW(edit, EM_SETCUEBANNER, FALSE, (LPARAM)hint.c_str());
    SetWindowSubclass(edit, edit_sub, 1, (DWORD_PTR)this);
    solid_edit(edit);
    return true;
}

wstr TextBox::get() const {
    int n = GetWindowTextLengthW(edit);
    wstr s(n, L'\0');
    if (n) GetWindowTextW(edit, &s[0], n + 1);
    return s;
}

void TextBox::set(const wstr& s, bool notify) {
    quiet = !notify;
    SetWindowTextW(edit, s.c_str());
    quiet = false;
}

void TextBox::select_all() { SendMessageW(edit, EM_SETSEL, 0, -1); }
void TextBox::focus() { SetFocus(edit); }

void TextBox::on_size(int w, int h) {
    int fh = font_height(F.ui);
    MoveWindow(edit, S(10), (h - fh) / 2, std::max(10, w - S(20)), fh, TRUE);
}

void TextBox::on_paint(HDC dc, const RECT& rc) {
    HWND parent = GetParent(hwnd);
    (void)parent;
    fill(dc, rc, T.bg);
    fill_round(dc, rc, S(6), T.field);
    if (focused) frame_round(dc, rc, S(6), T.accent, 1.2f);
}

LRESULT TextBox::on_message(UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND && HIWORD(w) == EN_CHANGE) {
        if (!quiet && changed) changed();
        return 0;
    }
    if (m == WM_SETFOCUS) { SetFocus(edit); return 0; }
    return Control::on_message(m, w, l);
}

// ======================================================================
// ScrollBar
// ======================================================================
RECT ScrollBar::thumb() const {
    int h = track.bottom - track.top;
    if (total <= page || h <= 0) return RECT{0, 0, 0, 0};
    int th = std::max(S(28), (int)((double)h * page / total));
    int y = track.top + (int)((double)(h - th) * pos / std::max(1, total - page));
    return RECT{track.left, y, track.right, y + th};
}

void ScrollBar::paint(HDC dc) const {
    if (total <= page) return;
    RECT t = thumb();
    int inset = hot || dragging ? 0 : S(2);
    RECT r{t.left + inset, t.top, t.right - inset, t.bottom};
    fill_round(dc, r, (r.right - r.left) / 2, dragging ? T.accent : mix(T.border, T.muted, hot ? 0.45 : 0.25));
}

bool ScrollBar::press(int x, int y) {
    if (!hit(x, y)) return false;
    RECT t = thumb();
    if (y >= t.top && y < t.bottom) {
        dragging = true;
        grab = y - t.top;
    } else {
        pos = std::max(0, std::min(max_pos(), pos + (y < t.top ? -page : page)));
    }
    return true;
}

bool ScrollBar::drag_to(int y) {
    if (!dragging) return false;
    RECT t = thumb();
    int th = t.bottom - t.top, h = track.bottom - track.top;
    int top = y - grab - track.top;
    int np = (int)((double)top * std::max(1, total - page) / std::max(1, h - th));
    np = std::max(0, std::min(max_pos(), np));
    bool ch = np != pos;
    pos = np;
    return ch;
}

// ======================================================================
// SimpleList
// ======================================================================
int SimpleList::row_h() const { return S(26); }
int SimpleList::max_scroll() const { return std::max(0, (int)rows.size() * row_h() - (height() - head_h())); }

void SimpleList::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.surface);
    int hh = head_h(), rh = row_h();
    int sbw = S(10);
    int avail = rc.right - sbw;
    std::vector<int> xs;
    int fixed = 0, flex = 0;
    for (size_t i = 0; i < widths.size(); i++) { if (widths[i]) fixed += S(widths[i]); else flex++; }
    int x = 0;
    for (size_t i = 0; i < std::max(widths.size(), headers.size()); i++) {
        xs.push_back(x);
        int w = i < widths.size() && widths[i] ? S(widths[i]) : std::max(S(80), (avail - fixed) / std::max(1, flex));
        x += w;
    }
    xs.push_back(avail);
    if (hh) {
        RECT h{0, 0, rc.right, hh};
        fill(dc, h, T.surface);
        for (size_t i = 0; i < headers.size(); i++) {
            RECT c{xs[i] + S(10), 0, xs[i + 1] - S(6), hh};
            text(dc, headers[i], c, T.head, F.header);
        }
        hline(dc, 0, rc.right, hh - 1, T.border);
    }
    HRGN clip = CreateRectRgn(0, hh, rc.right, rc.bottom);
    SelectClipRgn(dc, clip);
    int first = scroll / rh;
    for (int i = first; i < (int)rows.size(); i++) {
        int y = hh + i * rh - scroll;
        if (y > rc.bottom) break;
        const Row& r = rows[i];
        COLORREF c = r.color == CLR_INVALID ? T.fg : r.color;
        for (size_t k = 0; k < r.cells.size() && k + 1 < xs.size(); k++) {
            int left = xs[k] + S(10);
            if (k == 0) {
                left += r.indent * S(16);
                if (r.icon || r.sys_icon >= 0) {
                    int is = S(16);
                    if (r.icon) r.icon->draw(dc, left, y + (rh - is) / 2, is, is);
                    else draw_sys_icon(dc, r.sys_icon, left, y + (rh - is) / 2, is);
                    left += is + S(6);
                }
            }
            RECT cr{left, y, xs[k + 1] - S(6), y + rh};
            text(dc, r.cells[k], cr, k == 0 ? c : (r.color == CLR_INVALID ? T.muted : c), F.ui);
        }
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    ScrollBar sb;
    sb.total = (int)rows.size() * rh;
    sb.page = rc.bottom - hh;
    sb.pos = scroll;
    sb.track = RECT{rc.right - S(8), hh + S(2), rc.right - S(2), rc.bottom - S(2)};
    sb.dragging = drag_;
    sb.paint(dc);
}

bool SimpleList::on_wheel(int delta, WPARAM, int, int) {
    scroll = std::max(0, std::min(max_scroll(), scroll - delta * 3 * row_h() / 120));
    invalidate();
    return true;
}

void SimpleList::on_lbutton_down(int x, int y, WPARAM) {
    if (x > width() - S(12) && max_scroll() > 0) {
        drag_ = true;
        drag_y_ = y;
        drag_scroll_ = scroll;
        SetCapture(hwnd);
    }
}

void SimpleList::on_mouse_move(int x, int y, WPARAM keys) {
    if (!drag_) return;
    int h = std::max(1, height() - head_h());
    int total = (int)rows.size() * row_h();
    scroll = std::max(0, std::min(max_scroll(), drag_scroll_ + (y - drag_y_) * total / h));
    invalidate();
}

// ======================================================================
// Dialogs
// ======================================================================
void center_on(HWND w, HWND owner) {
    RECT ro, rw;
    GetWindowRect(owner, &ro);
    GetWindowRect(w, &rw);
    int x = ro.left + ((ro.right - ro.left) - (rw.right - rw.left)) / 2;
    int y = ro.top + ((ro.bottom - ro.top) - (rw.bottom - rw.top)) / 3;
    SetWindowPos(w, nullptr, std::max(0, x), std::max(0, y), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT Dialog::on_message(UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ACTIVATE && LOWORD(w) != WA_INACTIVE) {
        // put the focus on the first text box
        HWND child = GetWindow(hwnd, GW_CHILD);
        while (child) {
            Control* c = Control::from(child);
            if (auto tb = dynamic_cast<TextBox*>(c)) { tb->focus(); break; }
            child = GetWindow(child, GW_HWNDNEXT);
        }
    }
    return Control::on_message(m, w, l);
}

void Dialog::on_paint(HDC dc, const RECT& rc) { fill(dc, rc, T.bg); }

bool Dialog::on_key(UINT vk, bool, bool, bool) {
    if (vk == VK_ESCAPE) { finish(0); return true; }
    if (vk == VK_RETURN && enter) { enter(); return true; }
    return false;
}

void Dialog::run(HWND owner) {
    style_window(hwnd);
    center_on(hwnd, owner);
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    MSG msg;
    while (!done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (ui_dispatch(msg)) continue;
        if (msg.message == WM_KEYDOWN && (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd))) {
            if (msg.wParam == VK_ESCAPE) { finish(0); continue; }
            if (msg.wParam == VK_RETURN && enter) { enter(); continue; }
            if (msg.wParam == VK_TAB) {  // next / previous text box
                std::vector<HWND> edits;
                HWND c = GetWindow(hwnd, GW_CHILD);
                while (c) {
                    if (auto tb = dynamic_cast<TextBox*>(Control::from(c))) if (IsWindowVisible(c)) edits.push_back(tb->edit);
                    c = GetWindow(c, GW_HWNDNEXT);
                }
                if (!edits.empty()) {
                    auto it = std::find(edits.begin(), edits.end(), GetFocus());
                    int i = it == edits.end() ? 0 : (int)(it - edits.begin());
                    i = (i + (GetKeyState(VK_SHIFT) < 0 ? -1 : 1) + (int)edits.size()) % (int)edits.size();
                    SetFocus(edits[i]);
                    SendMessageW(edits[i], EM_SETSEL, 0, -1);
                }
                continue;
            }
        }
        if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    ShowWindow(hwnd, SW_HIDE);
}

namespace {
struct TextDialog : Dialog {
    Label prompt;
    TextBox box;
    Button ok, cancel;
};
struct ConfirmDialog : Dialog {
    Label msg;
    Button ok, cancel;
};
}  // namespace

bool ask_text(HWND owner, const wstr& title, const wstr& prompt_text, wstr& value) {
    TextDialog d;
    int w = S(420), h = S(170);
    RECT wr{0, 0, w, h};
    AdjustWindowRectEx(&wr, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    d.create_top(owner, title, WS_CAPTION | WS_SYSMENU | WS_POPUP, WS_EX_DLGMODALFRAME, 0, 0, wr.right - wr.left,
                 wr.bottom - wr.top);
    d.prompt.create(d.hwnd);
    d.prompt.text_ = prompt_text;
    d.prompt.flags = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX;
    d.prompt.move(S(18), S(14), w - S(36), S(36));
    d.box.create_in(d.hwnd);
    d.box.move(S(18), S(54), w - S(36), S(34));
    d.box.set(value, false);
    d.ok.create(d.hwnd);
    d.ok.label = L"OK";
    d.ok.primary = true;
    d.cancel.create(d.hwnd);
    d.cancel.label = L"Cancel";
    d.cancel.move(w - S(18) - S(96), h - S(52), S(96), S(34));
    d.ok.move(w - S(18) - S(96) * 2 - S(8), h - S(52), S(96), S(34));
    d.ok.click = [&] { d.finish(1); };
    d.cancel.click = [&] { d.finish(0); };
    d.enter = [&] { d.finish(1); };
    d.box.select_all();
    d.run(owner);
    if (d.result == 1) value = d.box.get();
    DestroyWindow(d.hwnd);
    return d.result == 1;
}

bool confirm(HWND owner, const wstr& title, const wstr& message, const wstr& ok_label, bool danger) {
    ConfirmDialog d;
    int w = S(440);
    HDC dc = GetDC(owner);
    RECT tr{0, 0, w - S(36), 0};
    HGDIOBJ old = SelectObject(dc, F.ui);
    DrawTextW(dc, message.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(owner, dc);
    int h = std::max(S(140), (int)tr.bottom + S(100));
    RECT wr{0, 0, w, h};
    AdjustWindowRectEx(&wr, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    d.create_top(owner, title, WS_CAPTION | WS_SYSMENU | WS_POPUP, WS_EX_DLGMODALFRAME, 0, 0, wr.right - wr.left,
                 wr.bottom - wr.top);
    d.msg.create(d.hwnd);
    d.msg.text_ = message;
    d.msg.color = T.fg;
    d.msg.font = F.ui;
    d.msg.flags = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX;
    d.msg.move(S(18), S(18), w - S(36), tr.bottom + S(4));
    d.ok.create(d.hwnd);
    d.ok.label = ok_label;
    d.ok.primary = true;
    d.cancel.create(d.hwnd);
    d.cancel.label = L"Cancel";
    d.cancel.move(w - S(18) - S(100), h - S(52), S(100), S(34));
    d.ok.move(w - S(18) - S(100) * 2 - S(8), h - S(52), S(100), S(34));
    d.ok.click = [&] { d.finish(1); };
    d.cancel.click = [&] { d.finish(0); };
    d.enter = [&] { d.finish(1); };
    (void)danger;
    d.run(owner);
    DestroyWindow(d.hwnd);
    return d.result == 1;
}
