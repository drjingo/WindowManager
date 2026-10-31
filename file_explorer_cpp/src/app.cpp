// The main window: layout, keys, saved state, navigation, the listing, search, folder sizes, views.
#include "app.h"
#include "dialogs.h"
#include "transfer.h"
#include "everything.h"
#include "panes.h"
#include "quicklook.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <thread>

App* g_app = nullptr;
const char* VIEW_KEYS[3] = {"list", "grid", "columns"};
enum { TIMER_MSG = 11, TIMER_RECENT = 12 };
static const int GRID_SIZES[] = {48, 64, 96, 128, 192};

App::App() { g_app = this; }
App::~App() {
    delete ql;
    delete preview;
    delete pane2;
    delete columns;
}

int App::view_mode() {
    int v = cur().view < 0 ? default_view : cur().view;
    if (v == V_COLUMNS && is_virtual(cur().path)) v = V_LIST;
    return v;
}

wstr App::real_folder() {
    const wstr& p = cur().path;
    return !p.empty() && !is_virtual(p) ? p : L"";
}

FileView* App::main_fv() {
    if (view_mode() == V_COLUMNS && columns && columns->mirror()) return columns->mirror();
    return &view;
}

static wstr win_error(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   0, (LPWSTR)&msg, 0, nullptr);
    wstr s = msg ? trim(msg) : fmt(L"error %lu", e);
    if (msg) LocalFree(msg);
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s;
}

// ======================================================================
// start
// ======================================================================
static UINT dpi_of(HWND h) {
    using F = UINT(WINAPI*)(HWND);
    static F fn = (F)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && h) return fn(h);
    HDC dc = GetDC(nullptr);
    UINT d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d;
}

bool App::start(const wstr& start_path) {
    load_state();
    set_theme(theme_name);
    theme_name = T.name;
    use_dark_menus(!T.light);
    if (!start_path.empty()) {  // opened with a folder: its tab if there is one, else a new tab
        auto it = std::find_if(tabs.begin(), tabs.end(), [&](const Tab& t) { return norm(t.path) == norm(start_path); });
        if (it != tabs.end()) active = (int)(it - tabs.begin());
        else {
            Tab t;
            t.path = start_path;
            tabs.push_back(t);
            active = (int)tabs.size() - 1;
        }
    }
    if (tabs.empty()) { Tab t; t.path = home_dir(); tabs.push_back(t); }
    active = std::max(0, std::min(active, (int)tabs.size() - 1));

    // the window first (hidden), so we know the screen's DPI before making fonts
    Json st;
    std::string raw;
    if (read_file(path_join(app_dir(), L"file_explorer.json"), raw)) st = Json::parse(raw);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1200, h = 780;
    wstr geo = st.gstr("geometry");
    int gw, gh, gx, gy;
    bool has_geo = swscanf(geo.c_str(), L"%dx%d%d%d", &gw, &gh, &gx, &gy) == 4 && gw > 300 && gh > 200;
    if (!create_top(nullptr, L"File Explorer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, x, y, w, h)) return false;
    ui_init(hwnd);
    SetPropW(hwnd, INSTANCE_PROP, (HANDLE)1);  // so a second start can find us
    make_fonts((int)dpi_of(hwnd));
    if (has_geo) {
        RECT wr{0, 0, gw, gh};
        AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
        // keep it on a screen
        RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int px = std::max((int)vs.left, std::min(gx, (int)vs.right - 200)), py = std::max((int)vs.top, std::min(gy, (int)vs.bottom - 120));
        SetWindowPos(hwnd, nullptr, px, py, wr.right - wr.left, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(hwnd, nullptr, 0, 0, S(1200), S(780), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        center_on(hwnd, GetDesktopWindow());
    }
    style_window(hwnd);
    if (mica) set_mica(hwnd, true);
    HICON big = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
    HICON small = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)(big ? big : make_hicon(32)));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)(small ? small : make_hicon(16)));

    tabbar.init(this, hwnd);
    toolbar.init(this, hwnd);
    sidebar.init(this, hwnd);
    view.create_view(hwnd, this, VM_LIST);
    view.grid_size = grid_size;
    view.columns = {{"name", L"Name", 200}, {"modified", L"Date modified", 150}, {"type", L"Type", 150},
                    {"size", L"Size", 90, true}, {"tags", L"Tags", 140}, {"location", L"Folder", 230}};
    status.create(hwnd);
    columns = new ColumnsView();
    columns->init(this, hwnd);
    pane2 = new SecondPane();
    pane2->init(this, hwnd);
    pane2->show(false);
    preview = new QuickLook();
    preview->init_child(this, hwnd);
    preview->show(preview_open);

    auto& sb = toolbar.search;
    sb.changed = [this] { apply_filter(); };
    sb.search = [this] { start_search(); };
    sb.key = [this](UINT vk, bool ctrl, bool shift) {
        if (vk == VK_RETURN) { start_search(); return true; }
        if (vk == VK_ESCAPE) { filter_escape(); return true; }
        if (vk == VK_DOWN || vk == VK_TAB) { focus_view(); return true; }
        return false;
    };
    sb.focus_changed = [this](bool got) { if (got) say(wstr(L"Type to filter this folder  ·  ") + SEARCH_HELP, 12); };
    auto& cr = toolbar.crumb;
    cr.navigate = [this](const wstr& p) { navigate(p); };
    cr.escape = [this] { focus_view(); };
    cr.middle = [this](const wstr& p) { new_tab(p); };
    cr.show_hidden = [this] { return show_hidden; };
    cr.can_drop = [this](const wstr& d, const std::vector<wstr>& p, DWORD e) { return transfer_problem(p, d, e == DROPEFFECT_MOVE).empty(); };
    cr.dropped = [this](const wstr& d, const std::vector<wstr>& p, DWORD e) { transfer(p, d, e == DROPEFFECT_MOVE); };

    sidebar.rebuild();
    layout();
    ShowWindow(hwnd, st.gbool("maximized") ? SW_SHOWMAXIMIZED : SW_SHOW);
    load(cur().path, cur().select);
    if (pane2_saved_valid && st.get("dual") && st.get("dual")->gbool("open")) toggle_dual();
    return true;
}

// ======================================================================
// saved state (file_explorer.json, shared with the Python version)
// ======================================================================
void App::load_state() {
    std::string raw;
    Json st;
    if (read_file(path_join(app_dir(), L"file_explorer.json"), raw)) st = Json::parse(raw);
    if (!st.is_obj()) st = Json::object();
    extra_state = st;
    if (auto f = st.get("favorites"); f && f->is_arr())
        for (auto& x : f->a) if (x.is_obj() && !x.gstr("path").empty()) favorites.push_back({x.gstr("name", display_name(x.gstr("path"))), x.gstr("path")});
    show_hidden = st.gbool("show_hidden");
    sort_key = utf8(st.gstr("sort", L"name"));
    if (sort_key != "name" && sort_key != "modified" && sort_key != "size" && sort_key != "type" && sort_key != "tags" && sort_key != "location") sort_key = "name";
    sort_desc = st.gbool("sort_desc");
    wstr v = st.gstr("view", L"list");
    for (int i = 0; i < 3; i++) if (utf8(v) == VIEW_KEYS[i]) default_view = i;
    int gs = (int)st.gnum("grid_size", 64);
    for (int s : GRID_SIZES) if (s == gs) grid_size = gs;
    if (auto fc = st.get("folder_colors"); fc && fc->is_obj())
        for (auto& [k, x] : fc->o)
            if (is_folder_color(utf8(x.gstr("color")))) folder_colors[norm(x.gstr("path"))] = {x.gstr("path"), utf8(x.gstr("color"))};
    std::string dc = utf8(st.gstr("default_folder_color", L"yellow"));
    if (is_folder_color(dc)) default_folder_color = dc;
    color_in_explorer = st.gbool("color_in_explorer");
    calc_sizes = st.gbool("folder_sizes", true);
    if (auto vs = st.get("visits"); vs && vs->is_obj())
        for (auto& [k, x] : vs->o)
            if (!x.gstr("path").empty()) visits[norm(x.gstr("path"))] = {x.gstr("path"), x.gnum("n"), x.gnum("t")};
    if (auto t = st.get("tags"); t && t->is_arr()) {
        for (auto& x : t->a)
            if (!x.gstr("name").empty() && is_folder_color(utf8(x.gstr("color")))) tag_defs.push_back({x.gstr("name"), utf8(x.gstr("color"))});
    } else {
        tag_defs = {{L"Urgent", "red"}, {L"Exam", "orange"}, {L"Important", "yellow"}, {L"Done", "green"},
                    {L"Work", "blue"}, {L"Personal", "purple"}};
    }
    if (auto ft = st.get("file_tags"); ft && ft->is_obj())
        for (auto& [k, x] : ft->o) {
            TagRec r{x.gstr("path"), {}};
            if (auto tl = x.get("tags"); tl && tl->is_arr()) for (auto& s : tl->a) r.tags.push_back(s.str());
            if (!r.path.empty() && !r.tags.empty()) file_tags[norm(r.path)] = r;
        }
    theme_name = st.gstr("theme", default_theme());
    bool known = false;
    for (auto& n : theme_names()) if (n == theme_name) known = true;
    if (!known) theme_name = default_theme();
    if (auto ts = st.get("tab_sets"); ts && ts->is_arr())
        for (auto& x : ts->a) {
            TabSet s;
            s.name = x.gstr("name");
            s.active = (int)x.gnum("active");
            if (auto tl = x.get("tabs"); tl && tl->is_arr())
                for (auto& t : tl->a) {
                    int vv = -1;
                    for (int i = 0; i < 3; i++) if (utf8(t.gstr("view")) == VIEW_KEYS[i]) vv = i;
                    if (t.get("path")) s.tabs.push_back({t.gstr("path"), vv});
                }
            if (!s.name.empty()) tab_sets.push_back(s);
        }
    current_set = st.gstr("current_set");
    bool set_ok = false;
    for (auto& s : tab_sets) if (s.name == current_set) set_ok = true;
    if (!set_ok) current_set.clear();
    sidebar_w = std::max(150, std::min(600, (int)st.gnum("sidebar", 230)));
    preview_open = st.gbool("preview");
    mica = st.gbool("mica", true);
    use_everything = st.gbool("everything", true);
    preview_w = std::max(200, std::min(900, (int)st.gnum("preview_w", 340)));
    collapsed.clear();
    if (auto c = st.get("collapsed")) if (c->is_arr()) for (auto& x : c->a) if (x.is_str()) collapsed.insert(x.str());
    if (auto d = st.get("dual"); d && d->is_obj() && d->get("path")) {
        pane2_saved_path = d->gstr("path");
        pane2_saved_valid = pane2_saved_path.empty() || is_dir(pane2_saved_path);
        if (!pane2_saved_valid) { pane2_saved_path = home_dir(); pane2_saved_valid = true; }
    }
    wstr q = st.gstr("quicklook");
    int qw, qh, qx, qy;
    if (swscanf(q.c_str(), L"%dx%d%d%d", &qw, &qh, &qx, &qy) == 4) ql_rect = RECT{qx, qy, qx + qw, qy + qh};
    if (auto t = st.get("tabs"); t && t->is_arr())
        for (auto& x : t->a) {
            Tab tab;
            int vv = -1;
            if (x.is_str()) tab.path = x.str();
            else {
                tab.path = x.gstr("path");
                for (int i = 0; i < 3; i++) if (utf8(x.gstr("view")) == VIEW_KEYS[i]) vv = i;
            }
            tab.view = vv;
            if (tab.path.empty() || is_dir(tab.path)) tabs.push_back(tab);
        }
    active = (int)st.gnum("active", 0);
}

void App::save_state() {
    Json st = extra_state;
    if (!st.is_obj()) st = Json::object();
    Json fav = Json::array();
    for (auto& f : favorites) {
        Json o = Json::object();
        o.set("name", Json::string(f.name));
        o.set("path", Json::string(f.path));
        fav.push(o);
    }
    st.set("favorites", fav);
    Json ts = Json::array();
    for (auto& t : tabs) {
        Json o = Json::object();
        wstr p = t.path;
        if (is_virtual(p)) {
            wstr k, a, b;
            parse_virtual(p, k, a, b);
            p = k == L"search" ? a : home_dir();
        }
        o.set("path", Json::string(p));
        o.set("view", Json::string(wide(VIEW_KEYS[t.view < 0 ? default_view : t.view])));
        ts.push(o);
    }
    st.set("tabs", ts);
    st.set("active", Json::number(active));
    st.set("show_hidden", Json::boolean(show_hidden));
    st.set("sort", Json::string(wide(sort_key)));
    st.set("sort_desc", Json::boolean(sort_desc));
    st.set("view", Json::string(wide(VIEW_KEYS[default_view])));
    st.set("grid_size", Json::number(grid_size));
    Json fc = Json::object();
    for (auto& [k, v] : folder_colors) {
        Json o = Json::object();
        o.set("path", Json::string(v.path));
        o.set("color", Json::string(wide(v.color)));
        fc.set(utf8(k), o);
    }
    st.set("folder_colors", fc);
    st.set("default_folder_color", Json::string(wide(default_folder_color)));
    st.set("color_in_explorer", Json::boolean(color_in_explorer));
    Json vs = Json::object();
    for (auto& [k, v] : visits) {
        Json o = Json::object();
        o.set("path", Json::string(v.path));
        o.set("n", Json::number(v.n));
        o.set("t", Json::number(v.t));
        vs.set(utf8(k), o);
    }
    st.set("visits", vs);
    Json td = Json::array();
    for (auto& t : tag_defs) {
        Json o = Json::object();
        o.set("name", Json::string(t.name));
        o.set("color", Json::string(wide(t.color)));
        td.push(o);
    }
    st.set("tags", td);
    Json ft = Json::object();
    for (auto& [k, v] : file_tags) {
        Json o = Json::object();
        o.set("path", Json::string(v.path));
        Json tl = Json::array();
        for (auto& t : v.tags) tl.push(Json::string(t));
        o.set("tags", tl);
        ft.set(utf8(k), o);
    }
    st.set("file_tags", ft);
    st.set("folder_sizes", Json::boolean(calc_sizes));
    st.set("theme", Json::string(theme_name));
    Json sets = Json::array();
    for (auto& s : tab_sets) {
        Json o = Json::object();
        o.set("name", Json::string(s.name));
        Json tl = Json::array();
        for (auto& [p, v] : s.tabs) {
            Json t = Json::object();
            t.set("path", Json::string(p));
            t.set("view", Json::string(wide(VIEW_KEYS[v < 0 ? default_view : v])));
            tl.push(t);
        }
        o.set("tabs", tl);
        o.set("active", Json::number(s.active));
        sets.push(o);
    }
    st.set("tab_sets", sets);
    st.set("current_set", Json::string(current_set));
    st.set("sidebar", Json::number(sidebar_w));
    st.set("preview", Json::boolean(preview_open));
    st.set("mica", Json::boolean(mica));
    st.set("everything", Json::boolean(use_everything));
    st.set("preview_w", Json::number(preview_w));
    {
        Json c = Json::array();
        for (auto& x : collapsed) c.push(Json::string(x));
        st.set("collapsed", c);
    }
    Json dual = Json::object();
    dual.set("open", Json::boolean(pane2_open));
    dual.set("path", Json::string(pane2 && pane2->has_path ? pane2->path : pane2_saved_path));
    st.set("dual", dual);
    if (ql_rect.right > ql_rect.left)
        st.set("quicklook", Json::string(fmt(L"%dx%d+%d+%d", ql_rect.right - ql_rect.left, ql_rect.bottom - ql_rect.top, ql_rect.left, ql_rect.top)));
    if (hwnd) {
        WINDOWPLACEMENT wp{sizeof wp};
        GetWindowPlacement(hwnd, &wp);
        RECT r = wp.rcNormalPosition;
        RECT adj{0, 0, 0, 0};
        AdjustWindowRectEx(&adj, WS_OVERLAPPEDWINDOW, FALSE, 0);
        int cw = (r.right - r.left) - (adj.right - adj.left), ch = (r.bottom - r.top) - (adj.bottom - adj.top);
        st.set("geometry", Json::string(fmt(L"%dx%d+%d+%d", cw, ch, r.left, r.top)));
        st.set("maximized", Json::boolean(wp.showCmd == SW_SHOWMAXIMIZED));
    }
    extra_state = st;
    if (!write_file_atomic(path_join(app_dir(), L"file_explorer.json"), st.dump(2))) log_line(L"couldn't save file_explorer.json");
}

// ======================================================================
// window, layout, splitters
// ======================================================================
LRESULT App::on_message(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_DPICHANGED: {
            make_fonts(HIWORD(w));
            clear_art_cache();
            RECT* r = (RECT*)l;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            redraw_all();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)l;
            mm->ptMinTrackSize = POINT{S(720), S(420)};
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && (HWND)w == hwnd) {
                POINT p = cursor();
                int sx = S(sidebar_w);
                bool on_split = (p.x >= sx - S(2) && p.x <= sx + S(6)) ||
                                (pane2_open && pane2->hwnd && [&] { RECT r; GetWindowRect(pane2->hwnd, &r); POINT tl{r.left, r.top}; ScreenToClient(hwnd, &tl); return p.x >= tl.x - S(8) && p.x < tl.x; }());
                if (preview_open && p.x >= preview_rc_.left - S(8) && p.x < preview_rc_.left && p.y >= body_.top) on_split = true;
                if (on_split || split_drag_) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE) post_ui([this] { if (!GetFocus() || GetFocus() == hwnd) focus_view(); });
            break;
        case WM_COPYDATA: {
            auto* cd = (COPYDATASTRUCT*)l;
            if (cd && cd->dwData == COPYDATA_OPEN) {
                wstr p(cd->lpData ? (const wchar_t*)cd->lpData : L"", cd->cbData / sizeof(wchar_t));
                while (!p.empty() && p.back() == 0) p.pop_back();
                post_ui([this, p] { open_from_other(p); });  // not inside SendMessage
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            RemovePropW(hwnd, INSTANCE_PROP);
            PostQuitMessage(0);
            return 0;
    }
    return Control::on_message(m, w, l);
}

void App::on_close() {
    if (transfers_running()) {
        if (!confirm(hwnd, L"Still copying", L"Files are still being copied or moved.\n\nStop and close anyway? The file being copied right now is left out.", L"Stop and close", true))
            return;
        cancel_transfers();
    }
    if (ql) ql->close_ql();
    save_state();
    watcher.stop();
    DestroyWindow(hwnd);
}

void App::layout() {
    if (!hwnd) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int th = tabbar.ideal_height();
    tabbar.move(0, 0, w, th);
    int sw = std::max(S(150), std::min(S(sidebar_w), w / 2));
    sidebar.move(0, th, sw, h - th);
    int x0 = sw + S(4);
    int tbh = toolbar.ideal_height();
    toolbar.move(x0, th, w - x0, tbh);
    int sh = status.ideal_height();
    status.move(x0, h - sh, w - x0, sh);
    body_ = RECT{x0 + S(6), th + tbh, w - S(10), h - sh - S(6)};
    area_ = body_;
    if (preview_open && preview) {  // the preview panel on the far right
        int pw = std::max(S(200), std::min(S(preview_w), (int)(body_.right - body_.left) / 2));
        preview_rc_ = RECT{body_.right - pw, body_.top, body_.right, body_.bottom};
        preview->move(preview_rc_.left, preview_rc_.top, pw, preview_rc_.bottom - preview_rc_.top);
        area_.right = preview_rc_.left - S(8);
    } else preview_rc_ = RECT{};
    RECT main = area_;
    if (pane2_open) {
        int bw = area_.right - area_.left;
        int split = area_.left + (int)(bw * pane2_frac_);
        split = std::max((int)area_.left + S(240), std::min((int)area_.right - S(240), split));
        main.right = split - S(4);
        pane2->move(split + S(4), area_.top, area_.right - split - S(4), area_.bottom - area_.top);
    }
    int mode = view_mode();
    int top = main.top + S(3);  // room for the "active side" line
    view.move(main.left, top, main.right - main.left, main.bottom - top);
    columns->move(main.left, top, main.right - main.left, main.bottom - top);
    view.show(mode != V_COLUMNS);
    columns->show(mode == V_COLUMNS);
    invalidate();
}

void App::on_size(int, int) { layout(); }

void App::on_paint(HDC dc, const RECT& rc) {
    ::fill(dc, rc, T.bg);
    int sw = std::max(S(150), std::min(S(sidebar_w), (int)rc.right / 2));
    ::fill(dc, RECT{sw, 0, sw + S(4), rc.bottom}, T.panel);
    vline(dc, sw + S(4) - 1, tabbar.ideal_height(), rc.bottom, T.border);
    if (preview_open && preview_rc_.right > preview_rc_.left)
        vline(dc, preview_rc_.left - S(4), body_.top + S(8), body_.bottom - S(8), T.border);
    if (pane2_open) {
        RECT vr, pr;
        GetWindowRect(view.hwnd, &vr);
        GetWindowRect(pane2->hwnd, &pr);
        POINT a{vr.left, vr.top}, b{vr.right, vr.top}, c{pr.left, pr.top};
        ScreenToClient(hwnd, &a);
        ScreenToClient(hwnd, &b);
        ScreenToClient(hwnd, &c);
        int split = (b.x + c.x) / 2;
        vline(dc, split, body_.top + S(8), body_.bottom - S(8), T.border);
        bool main_on = active_view == nullptr;
        fill_round(dc, RECT{a.x + S(8), a.y - S(3), b.x - S(8), a.y - S(1)}, S(1), main_on ? T.accent : T.bg);
    }
}

void App::on_mouse_move(int x, int y, WPARAM) {
    if (!split_drag_) return;
    if (split_drag_ == 1) {
        sidebar_w = std::max(150, std::min(600, (split_w0_ + x - split_x0_) * 96 / g_dpi));
    } else if (split_drag_ == 3) {
        preview_w = std::max(200, std::min(900, (int)(body_.right - x) * 96 / g_dpi));
    } else {
        int bw = std::max(1L, area_.right - area_.left);
        pane2_frac_ = std::max(0.2, std::min(0.8, (double)(x - area_.left) / bw));
    }
    layout();
}

void App::on_lbutton_down(int x, int y, WPARAM) {
    int sx = std::max(S(150), std::min(S(sidebar_w), width() / 2));
    if (x >= sx - S(2) && x <= sx + S(6)) {
        split_drag_ = 1;
        split_x0_ = x;
        split_w0_ = sx;
        SetCapture(hwnd);
        return;
    }
    if (preview_open && x >= preview_rc_.left - S(8) && x < preview_rc_.left && y >= body_.top) {
        split_drag_ = 3;
        SetCapture(hwnd);
        return;
    }
    if (pane2_open && y >= body_.top) { split_drag_ = 2; SetCapture(hwnd); }
}

void App::on_lbutton_up(int, int, WPARAM) {
    if (split_drag_) {
        split_drag_ = 0;
        ReleaseCapture();
        save_state();
    }
}

void App::redraw_all() {
    style_window(hwnd);
    if (ql && ql->hwnd) style_window(ql->hwnd);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

void App::on_timer(UINT_PTR id) {
    if (id == TIMER_MSG) { KillTimer(hwnd, id); msg_hold_ = 0; update_status(); }
    else if (id == TIMER_RECENT) { KillTimer(hwnd, id); sidebar.rebuild(); }
}

void App::say(const wstr& t, int seconds) {
    status.set(t, true);
    msg_hold_ = GetTickCount64() + std::min(seconds, 4) * 1000;
    SetTimer(hwnd, TIMER_MSG, seconds * 1000, nullptr);
}

void App::update_status() {
    if (GetTickCount64() < msg_hold_) return;
    FileView* fv = main_fv();
    int n = (int)fv->items.size();
    auto sel = fv->selected_entries();
    std::vector<wstr> parts;
    parts.push_back(fmt(L"%d item%s", n, n == 1 ? L"" : L"s"));
    wstr p = cur().path, k, a, b;
    parse_virtual(p, k, a, b);
    if (k == L"tag") parts[0] = fmt(L"%d item%s tagged “%s”", n, n == 1 ? L"" : L"s", a.c_str());
    else if (k == L"search" && search.token == token.load()) {
        wstr where = display_name(search.root);
        if (!search.done)
            parts[0] = search.dirs == 0 && search.found == 0 ? L"Searching “" + b + L"” in " + where + L"…  (Esc stops)"
                       : L"Searching “" + b + L"” in " + where + L"…  " + fmt_count(search.found) + L" found · " +
                             fmt_count(search.dirs) + L" folders  (Esc stops)";
        else
            parts[0] = fmt_count(search.found) + L" found in " + where + L" (" +
                       (search.everything ? wstr(L"Everything, ") : fmt_count(search.dirs) + L" folders, ") +
                       fmt(search.everything ? L"%.2f s" : L"%.1f s", now_unix() - search.start) + L")" + (search.stopped ? L" — stopped" : L"") +
                       (search.full ? L" — showing the first " + fmt_count(SEARCH_LIMIT) : L"");
    }
    if (!sel.empty()) {
        i64 size = 0;
        for (auto& e : sel) if (!e->drive && e->size > 0) size += e->size;
        parts.push_back(fmt(L"%d selected", (int)sel.size()) + (size ? L" (" + fmt_size(size) + L")" : L""));
    }
    if (!is_virtual(p) && !trim(cur().filter).empty()) parts.push_back(fmt(L"filtered from %d", (int)entries.size()));
    if (!sizing.empty() && calc_sizes && !is_virtual(p)) parts.push_back(L"working out folder sizes…");
    if (!p.empty() && !is_virtual(p)) {
        ULARGE_INTEGER fr;
        if (GetDiskFreeSpaceExW(p.c_str(), &fr, nullptr, nullptr)) parts.push_back(fmt_size((i64)fr.QuadPart) + L" free");
    }
    wstr s;
    for (size_t i = 0; i < parts.size(); i++) s += (i ? L"   ·   " : L"") + parts[i];
    status.set(s, false);
}

// ======================================================================
// keys
// ======================================================================
bool App::pre_key(MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    if (!hwnd || GetAncestor(msg.hwnd, GA_ROOT) != hwnd || !IsWindowEnabled(hwnd)) return false;
    UINT vk = (UINT)msg.wParam;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    wchar_t cls[16] = {};
    GetClassNameW(GetFocus(), cls, 16);
    bool in_edit = !wcscmp(cls, L"Edit");
    if (vk == VK_ESCAPE && !in_edit && ql && ql->is_open()) { ql->close_ql(); return true; }
    if (ctrl && !alt) {
        switch (vk) {
            case 'T': if (!shift) { new_tab(cur().path); return true; } break;
            case 'W': if (!shift) { close_tab(active); return true; } break;
            case VK_TAB: switch_tab((active + (shift ? (int)tabs.size() - 1 : 1)) % (int)tabs.size()); return true;
            case 'L': if (!shift) { toolbar.crumb.edit(); return true; } break;
            case 'F': if (!shift) { toolbar.search.focus(); toolbar.search.select_all(); return true; } break;
            case 'H': if (!shift) { show_hidden = !show_hidden; say(wstr(L"Hidden files: ") + (show_hidden ? L"shown" : L"hidden"), 3); refresh(); save_state(); return true; } break;
            case 'D': if (!shift) { toggle_favorite(); return true; } break;
            case 'R': if (!shift) { refresh(); return true; } break;
            case 'Z': if (!shift && !in_edit) { undo_rename(); return true; } break;
            case 'N': if (shift) { new_folder(); return true; } break;
            case 'S': if (shift) { save_tab_set(); return true; } break;
        }
        if (vk >= '1' && vk <= '9') {
            if (shift && vk <= '3') { set_view((int)(vk - '1')); return true; }
            if (!shift) { switch_tab(std::min((int)(vk - '1'), (int)tabs.size() - 1)); return true; }
        }
    }
    if (alt && !ctrl) {
        if (vk == VK_LEFT) { go_back(); return true; }
        if (vk == VK_RIGHT) { go_forward(); return true; }
        if (vk == VK_UP) { go_up(); return true; }
        if (vk == 'P') { toggle_preview(); return true; }
    }
    if (!ctrl && !alt) {
        if (vk == VK_F3) { toggle_dual(); return true; }
        if (vk == VK_F5) { if (pane2_open) copy_across(false, active_view != nullptr); else refresh(); return true; }
        if (vk == VK_F6 && pane2_open) { copy_across(true, active_view != nullptr); return true; }
    }
    return false;
}

bool App::fv_key(FileView* v, UINT vk, bool ctrl, bool shift, bool alt) {
    if (alt) return false;
    if (ctrl) {
        if (vk == 'C') { copy_paths(selected_paths(), false); return true; }
        if (vk == 'X') { copy_paths(selected_paths(), true); return true; }
        if (vk == 'V') { paste(); return true; }
        return false;
    }
    switch (vk) {
        case VK_RETURN: open_selected(); return true;
        case VK_BACK: go_up(); return true;
        case VK_F2: start_rename(); return true;
        case VK_DELETE: delete_selected(shift); return true;
        case VK_SPACE: quick_look(this); return true;
        case VK_ESCAPE:
            if (stop_search()) say(L"Search stopped.", 4);
            else if (!cur().filter.empty() && !is_virtual(cur().path)) toolbar.search.set(L"");
            return true;
        case VK_TAB:
            if (pane2_open) { SetFocus(pane2->view.hwnd); return true; }
            return false;
    }
    return false;
}

// ======================================================================
// navigation and the listing
// ======================================================================
void App::navigate(const wstr& path0, const std::vector<wstr>& select) {
    wstr path = path0;
    if (!path.empty() && !is_virtual(path) && !is_dir(path)) {
        if (exists(path)) { navigate(dir_name(path), {base_name(path)}); return; }
        say(L"Can't find " + path, 8);
        return;
    }
    Tab& t = cur();
    if (norm(path) != norm(t.path)) {
        t.back.push_back(t.path);
        t.fwd.clear();
        t.filter.clear();
        t.scroll = 0;
    }
    record_visit(path);
    load(path, select);
    save_state();
}

void App::go_back() {
    Tab& t = cur();
    if (t.back.empty()) return;
    t.fwd.push_back(t.path);
    wstr came = t.path, p = t.back.back();
    t.back.pop_back();
    t.filter.clear();
    record_visit(p);
    std::vector<wstr> sel;
    if (!came.empty() && !is_virtual(came) && !is_virtual(p) && norm(dir_name(came)) == norm(p)) sel = {display_name(came)};
    load(p, sel);
}

void App::go_forward() {
    Tab& t = cur();
    if (t.fwd.empty()) return;
    t.back.push_back(t.path);
    t.filter.clear();
    wstr p = t.fwd.back();
    t.fwd.pop_back();
    record_visit(p);
    load(p);
}

void App::go_up() {
    wstr p = cur().path;
    if (is_virtual(p)) {
        wstr k, a, b;
        parse_virtual(p, k, a, b);
        if (k == L"search") navigate(a);
        return;
    }
    if (p.empty()) return;
    wstr parent = dir_name(p);
    if (norm(parent) == norm(p)) parent.clear();
    navigate(parent, parent.empty() ? std::vector<wstr>{} : std::vector<wstr>{display_name(p)});
}

void App::refresh() {
    remember_view();
    load(cur().path, cur().select, true);
}

void App::remember_view() {
    Tab& t = cur();
    FileView* fv = main_fv();
    t.scroll = fv->scroll_fraction();
    t.select.clear();
    for (auto& e : fv->selected_entries()) t.select.push_back(e->name);
}

void App::load(const wstr& path, const std::vector<wstr>& select, bool keep_scroll) {
    Tab& t = cur();
    t.path = path;
    int tok = ++token;
    sizing.clear();
    cancel_images();
    wstr k, a, b;
    parse_virtual(path, k, a, b);
    toolbar.crumb.set_path(path);
    toolbar.enabled[Toolbar::B_BACK] = !t.back.empty();
    toolbar.enabled[Toolbar::B_FWD] = !t.fwd.empty();
    toolbar.enabled[Toolbar::B_UP] = !path.empty() && k != L"tag";
    toolbar.enabled[Toolbar::B_STAR] = !path.empty() && k.empty();
    toolbar.star_on = is_favorite(path);
    toolbar.invalidate();
    status.set(L"Loading…");
    msg_hold_ = 0;  // a new place: its own status replaces any message
    loading = true;
    if (k == L"search") t.filter = b;
    toolbar.search.set(t.filter, false);
    pending_select_ = select.empty() ? t.select : select;
    t.select.clear();
    SetWindowTextW(hwnd, (display_name(path) + L" — File Explorer").c_str());
    show_view();
    tabbar.invalidate();
    sidebar.set_current(path);
    watcher.stop();
    if (k == L"search") {
        Query q;
        wstr problem;
        bool ok = parse_query(b, q, problem);
        loading = false;
        entries.clear();
        search = SearchState();
        search.token = tok;
        search.done = !ok;
        search.query = b;
        search.root = a;
        search.start = now_unix();
        fill(false);
        if (!ok) { say(problem, 12); return; }
        bool hidden = show_hidden;
        wstr root = a;
        std::atomic<int>* tokp = &token;
        std::atomic<int>* cancel = &search_cancel;
        bool use_ev = use_everything;
        run_bg([this, tok, root, q, hidden, tokp, cancel, use_ev] {
            Entries batch;
            int found = 0, dirs = 0;
            // 1. Everything (an index of every file): instant, when it is running and the drive is indexed
            if (use_ev && everything_can_search(root) && everything_running()) {
                std::vector<EvItem> items;
                bool filters = q.has_size || q.has_when;  // checked on our side, so ask for more
                auto stop = [&] { return tokp->load() != tok || cancel->load() == tok; };
                if (everything_query(everything_search_text(q, root, hidden), filters ? 200000 : SEARCH_LIMIT + 1, items, stop)) {
                    for (auto& it : items) {
                        if (it.folder.empty()) continue;  // a drive itself
                        bool hid = hidden_attrs(it.attrs, it.name);
                        if (hid && !hidden) continue;
                        if (!query_match(q, it.name, it.dir, it.size, it.mtime)) continue;
                        auto e = std::make_shared<Entry>();
                        e->name = it.name;
                        e->path = path_join(it.folder, it.name);
                        e->dir = it.dir;
                        e->hidden = hid;
                        e->size = it.dir ? -1 : it.size;
                        e->mtime = it.mtime;
                        e->folder = it.folder;
                        if (!it.dir) e->ext = ext_lower(e->name);
                        batch.push_back(e);
                        if (++found >= SEARCH_LIMIT) break;
                    }
                    post_ui([this, tok, batch] {
                        if (search.token == tok) search.everything = true;
                        search_found(tok, batch, 0, true);
                    });
                    return;
                }
            }
            // 2. folder by folder
            std::deque<wstr> todo;
            if (root.empty()) for (auto& d : list_drives()) todo.push_back(d.root);
            else todo.push_back(root);
            DWORD last = GetTickCount();
            while (!todo.empty() && found < SEARCH_LIMIT) {
                if (tokp->load() != tok || cancel->load() == tok) break;
                wstr folder = todo.front();
                todo.pop_front();
                dirs++;
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileExW(path_join(folder, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                            nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                        bool hid = hidden_attrs(fd.dwFileAttributes, fd.cFileName);
                        if (hid && !hidden) continue;
                        bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        if (dir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) todo.push_back(path_join(folder, fd.cFileName));
                        i64 size = dir ? -1 : (((i64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
                        double mt = filetime_unix(fd.ftLastWriteTime);
                        if (query_match(q, fd.cFileName, dir, size, mt)) {
                            auto e = std::make_shared<Entry>();
                            e->name = fd.cFileName;
                            e->path = path_join(folder, e->name);
                            e->dir = dir;
                            e->hidden = hid;
                            e->size = size;
                            e->mtime = mt;
                            e->folder = folder;
                            if (!dir) e->ext = ext_lower(e->name);
                            batch.push_back(e);
                            if (++found >= SEARCH_LIMIT) break;
                        }
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
                if ((!batch.empty() && GetTickCount() - last > 250) || GetTickCount() - last > 1000) {
                    post_ui([this, tok, batch, dirs] { search_found(tok, batch, dirs, false); });
                    batch.clear();
                    last = GetTickCount();
                }
            }
            post_ui([this, tok, batch, dirs] { search_found(tok, batch, dirs, true); });
        });
        return;
    }
    std::vector<TagRec> records;
    if (k == L"tag") for (auto& [key, r] : file_tags) records.push_back(r);
    bool hidden = show_hidden;
    run_bg([this, tok, path, k, a, records, hidden, keep_scroll] {
        Entries list;
        DWORD err = 0;
        double mt = -1;
        if (k == L"tag") {
            for (auto& r : records)
                if (std::find(r.tags.begin(), r.tags.end(), a) != r.tags.end())
                    if (auto e = entry_for(r.path)) list.push_back(e);
        } else if (path.empty()) {
            list = drive_entries(list_drives());
        } else {
            list = list_folder(path, hidden, &err);
            mt = folder_mtime(path);
        }
        post_ui([this, tok, path, list, err, mt, keep_scroll] {
            show_listing(tok, path, list, err, mt);
            if (keep_scroll) main_fv()->set_scroll_fraction(cur().scroll);
        });
    });
}

void App::show_listing(int tok, const wstr& path, Entries list, DWORD error, double) {
    if (tok != token.load()) return;
    loading = false;
    entries = std::move(list);
    for (auto& e : entries) {
        if (e->dir && !e->drive) {
            auto it = dir_sizes.find(norm(e->path));
            if (it != dir_sizes.end()) e->size = it->second.first;
        }
    }
    if (error) {
        entries.clear();
        fill(false);
        say(L"Can't open " + path + L": " + win_error(error), 10);
        return;
    }
    start_sizes(tok);
    fill(false);
    FileView* fv = main_fv();
    if (!pending_select_.empty()) {
        fv->select_paths(pending_select_, true);
        pending_select_.clear();
        if (!pending_rename_.empty()) {
            wstr name = pending_rename_;
            pending_rename_.clear();
            post_ui([this, name] {
                FileView* f = main_fv();
                for (size_t i = 0; i < f->items.size(); i++) if (f->items[i]->name == name) { f->start_rename((int)i); break; }
            });
        }
    } else {
        fv->set_scroll_fraction(cur().scroll);
        if (!fv->items.empty() && fv->focus < 0) fv->set_focus_index(0);
    }
    wchar_t cls[16] = {};
    GetClassNameW(GetFocus(), cls, 16);
    if (wcscmp(cls, L"Edit") || GetFocus() == toolbar.search.edit) focus_view();
    if (view_mode() == V_COLUMNS) columns->rebuild();
    update_status();
    if (!path.empty() && !is_virtual(path))
        watcher.watch(path, [this, path] {
            if (norm(path) == norm(cur().path) && !main_fv()->renaming()) refresh();
        });
}

Entries App::sorted(const Entries& in) {
    Entries dirs, files;
    for (auto& e : in) (e->dir ? dirs : files).push_back(e);
    std::string k = sort_key;
    bool desc = sort_desc;
    auto less = [&](const EntryPtr& a, const EntryPtr& b) -> bool {
        int c = 0;
        if (k == "modified") c = a->mtime < b->mtime ? -1 : a->mtime > b->mtime ? 1 : 0;
        else if (k == "size") c = a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
        else if (k == "type") c = natural_cmp(a->dir ? L"" : sys_type_name(a->ext, false), b->dir ? L"" : sys_type_name(b->ext, false));
        else if (k == "tags") {
            auto ta = tags_of(a->path), tb = tags_of(b->path);
            if (ta.empty() != tb.empty()) c = ta.empty() ? 1 : -1;
            else {
                wstr sa, sb;
                for (auto& t : ta) sa += lower(t) + L",";
                for (auto& t : tb) sb += lower(t) + L",";
                c = natural_cmp(sa, sb);
            }
        } else if (k == "location") c = natural_cmp(dir_name(a->path), dir_name(b->path));
        if (c == 0) c = natural_cmp(a->name, b->name);
        return desc ? c > 0 : c < 0;
    };
    std::stable_sort(dirs.begin(), dirs.end(), less);
    std::stable_sort(files.begin(), files.end(), less);
    dirs.insert(dirs.end(), files.begin(), files.end());
    return dirs;
}

void App::fill(bool keep_scroll) {
    bool virt = is_virtual(cur().path);
    wstr q = virt ? L"" : lower(trim(cur().filter));
    Entries shown;
    for (auto& e : sorted(entries))
        if (q.empty() || lower(e->name).find(q) != wstr::npos) shown.push_back(e);
    // which columns
    std::vector<std::string> want;
    if (virt) want = {"name", "location", "modified", "size"};
    else if (pane2_open) want = {"name", "modified", "size"};
    else want = {"name", "modified", "type", "size"};
    if (!file_tags.empty() && !pane2_open) want.push_back("tags");
    static std::map<std::string, Column> all;
    for (auto& c : view.columns) all[c.key] = c;
    if (all.empty()) all = {{"name", {"name", L"Name", 200}}};
    std::vector<Column> cols;
    for (auto& k : want) {
        auto it = all.find(k);
        if (it != all.end()) cols.push_back(it->second);
        else if (k == "location") cols.push_back({"location", L"Folder", 230});
        else if (k == "tags") cols.push_back({"tags", L"Tags", 140});
    }
    // keep the ones we don't show (their widths) for later
    for (auto& c : view.columns) all[c.key] = c;
    view.columns = cols;
    view.sort_key = sort_key;
    view.sort_desc = sort_desc;
    view.set_items(shown, true, keep_scroll);
    if (view_mode() == V_COLUMNS) columns->rebuild();
    update_status();
    if (!active_view) update_preview(this);  // a new listing: show what's selected now (or nothing)
}

void App::sort_by(const std::string& key) {
    sort_desc = key == sort_key ? !sort_desc : (key == "modified" || key == "size");
    sort_key = key;
    fill(true);
    if (pane2_open) pane2->fill();
    save_state();
}

// ======================================================================
// tabs
// ======================================================================
void App::new_tab(const wstr& path) {
    remember_view();
    Tab t;
    t.path = path;
    t.view = view_mode();
    tabs.push_back(t);
    active = (int)tabs.size() - 1;
    record_visit(path);
    load(path);
    save_state();
}

void App::switch_tab(int i) {
    if (i == active || i < 0 || i >= (int)tabs.size()) return;
    remember_view();
    active = i;
    load(cur().path, cur().select, true);
    save_state();
}

void App::close_tab(int i) {
    if (tabs.size() == 1) { on_close(); return; }
    remember_view();
    bool was = i == active;
    tabs.erase(tabs.begin() + i);
    if (i < active) active--;
    active = std::min(active, (int)tabs.size() - 1);
    if (was) load(cur().path, cur().select, true);
    else tabbar.invalidate();
    save_state();
}

void App::close_others(int i) {
    Tab keep = tabs[i];
    tabs = {keep};
    active = 0;
    load(cur().path, cur().select, true);
    save_state();
}

// ======================================================================
// views
// ======================================================================
void App::set_view(int v) {
    if (v == V_COLUMNS && is_virtual(cur().path)) {
        say(L"Columns show one folder's path — search results and tags use List or Grid.", 5);
        return;
    }
    remember_view();
    cur().view = v;
    default_view = v;
    show_view();
    fill(true);
    focus_view();
    save_state();
}

void App::show_view() {
    int mode = view_mode();
    toolbar.active[Toolbar::B_LIST] = mode == V_LIST;
    toolbar.active[Toolbar::B_GRID] = mode == V_GRID;
    toolbar.active[Toolbar::B_COLUMNS] = mode == V_COLUMNS;
    toolbar.active[Toolbar::B_DUAL] = pane2_open;
    toolbar.active[Toolbar::B_PREVIEW] = preview_open;
    toolbar.invalidate();
    ViewMode vm = mode == V_GRID ? VM_GRID : VM_LIST;
    if (view.mode != vm) {
        view.mode = vm;
        view.grid_size = grid_size;
        SendMessageW(view.hwnd, WM_SIZE, 0, 0);
    }
    bool show_cols = mode == V_COLUMNS;
    if (view.visible() == show_cols || columns->visible() != show_cols) layout();
}

void App::focus_view() {
    if (pane2_open && active_view == &pane2->view) { SetFocus(pane2->view.hwnd); return; }
    FileView* fv = main_fv();
    if (fv && fv->hwnd) SetFocus(fv->hwnd);
}

void App::fv_zoom(FileView*, int dir) {
    int i = 1;
    for (int k = 0; k < 5; k++) if (GRID_SIZES[k] == grid_size) i = k;
    i = std::max(0, std::min(4, i + dir));
    if (GRID_SIZES[i] == grid_size) return;
    grid_size = GRID_SIZES[i];
    view.grid_size = grid_size;
    SendMessageW(view.hwnd, WM_SIZE, 0, 0);
    view.invalidate();
    save_state();
}

// ======================================================================
// filter & search
// ======================================================================
void App::apply_filter() {
    cur().filter = toolbar.search.get();
    if (is_virtual(cur().path) || loading) return;
    fill(false);
}

void App::start_search(const wstr& text0) {
    wstr text = trim(text0 == L"\x01" ? toolbar.search.get() : text0);
    if (text.empty()) { focus_view(); return; }
    Query q;
    wstr problem;
    if (!parse_query(text, q, problem)) { say(problem, 10); return; }
    wstr root;
    if (is_virtual(cur().path)) {
        wstr k, a, b;
        parse_virtual(cur().path, k, a, b);
        root = k == L"search" ? a : real_folder();
    } else root = cur().path;
    navigate(make_search_path(root, text));
}

bool App::stop_search() {
    if (search.token == token.load() && !search.done) {
        search_cancel = token.load();
        return true;
    }
    return false;
}

void App::filter_escape() {
    if (stop_search()) return;
    wstr k, a, b;
    parse_virtual(cur().path, k, a, b);
    if (k == L"search") {
        if (!cur().back.empty()) go_back();
        else navigate(a);
        return;
    }
    toolbar.search.set(L"");
    focus_view();
}

void App::search_found(int tok, Entries batch, int dirs, bool done) {
    if (tok != token.load()) return;
    search.dirs = dirs;
    search.done = done;
    search.found += (int)batch.size();
    search.full = search.found >= SEARCH_LIMIT;
    search.stopped = done && search_cancel.load() == tok;
    entries.insert(entries.end(), batch.begin(), batch.end());
    if (!batch.empty() || done) {
        FileView* fv = main_fv();
        if (done) fill(true);
        else {  // add the new ones at the end while it's still searching (sorted when it's done)
            Entries items = fv->items;
            items.insert(items.end(), batch.begin(), batch.end());
            fv->set_items(items, true, true);
        }
        if (done && fv->focus < 0 && !fv->items.empty()) fv->set_focus_index(0);
    }
    update_status();
}

wstr App::location_of(const Entry& e) {
    wstr folder = e.folder.empty() ? dir_name(e.path) : e.folder;
    wstr k, root, b;
    parse_virtual(cur().path, k, root, b);
    if (k == L"search" && !root.empty() && is_inside(folder, root)) {
        if (norm(folder) == norm(root)) return display_name(root);
        wstr rel = clean_path(folder).substr(clean_path(root).size());
        while (!rel.empty() && rel[0] == L'\\') rel.erase(0, 1);
        return path_join(display_name(root), rel);
    }
    return folder;
}

wstr App::empty_text() {
    wstr p = cur().path, k, a, b;
    parse_virtual(p, k, a, b);
    if (k == L"tag") return L"Nothing is tagged “" + a + L"” yet — right-click files → Tags, or drag them onto the tag";
    if (k == L"search") return search.done ? L"Nothing found for “" + b + L"”" : L"Searching…";
    if (loading) return L"";
    return p.empty() ? L"" : L"This folder is empty";
}

// ======================================================================
// folder sizes (in the background)
// ======================================================================
void App::start_sizes(int tok) {
    if (!calc_sizes || real_folder().empty()) return;
    double now = now_unix();
    std::vector<wstr> todo;
    for (auto& e : entries) {
        if (!e->dir || e->drive) continue;
        auto it = dir_sizes.find(norm(e->path));
        if (it == dir_sizes.end() || now - it->second.second > 300) todo.push_back(e->path);
    }
    if (todo.empty()) return;
    for (auto& p : todo) sizing.insert(norm(p));
    std::atomic<int>* tokp = &token;
    run_bg([this, todo, tok, tokp] {
        for (auto& p : todo) {
            i64 bytes = 0, files = 0;
            if (!dir_size(p, [tokp, tok] { return tokp->load() != tok; }, bytes, files)) return;
            post_ui([this, tok, p, bytes] {
                dir_sizes[norm(p)] = {bytes, now_unix()};
                if (tok != token.load()) return;
                sizing.erase(norm(p));
                for (auto& e : entries) if (e->path == p) e->size = bytes;
                main_fv()->invalidate();
                if (view_mode() == V_COLUMNS) columns->refresh_preview();
            });
        }
        post_ui([this, tok] {
            if (tok != token.load()) return;
            sizing.clear();
            if (sort_key == "size") fill(true);
            else update_status();
        });
    });
}

void App::toggle_sizes() {
    calc_sizes = !calc_sizes;
    say(calc_sizes ? L"Folder sizes: on — worked out in the background" : L"Folder sizes: off", 4);
    save_state();
    refresh();
}

// ======================================================================
// what each row shows; icons; thumbnails
// ======================================================================
void App::row_info(const Entry& e, RowInfo& out, bool with_location) {
    if (!file_tags.empty()) {
        auto tags = tags_of(e.path);
        for (auto& t : tags) {
            out.tags.push_back(tag_color(t));
            out.tags_text += (out.tags_text.empty() ? L"" : L", ") + t;
        }
    }
    if (e.dir) out.folder_color = folder_color(e.path);
    out.type = e.drive ? L"" : sys_type_name(e.ext, e.dir);
    out.cut = !cut_paths.empty() && cut_paths.count(norm(e.path));
    out.pending_size = e.dir && sizing.count(norm(e.path));
    if (with_location) out.location = location_of(e);
}

void App::fv_row(FileView*, const Entry& e, RowInfo& out) { row_info(e, out, is_virtual(cur().path)); }

int App::icon_index(Entry& e, std::function<void()> arrived) {
    if (e.icon != -2) return e.icon;
    if (e.drive || OWN_ICON_EXTS.count(e.ext)) {  // ask the file itself (in the background)
        auto it = icon_path_.find(e.path);
        if (it != icon_path_.end()) return e.icon = it->second;
        if (!icon_pending_.count(e.path)) {
            icon_pending_.insert(e.path);
            wstr p = e.path;
            bool dir = e.dir;
            run_bg([this, p, dir, arrived] {
                int idx = sys_icon_index(p, dir, true);
                post_ui([this, p, idx, arrived] {
                    icon_path_[p] = idx;
                    icon_pending_.erase(p);
                    if (arrived) arrived();
                });
            });
        }
        auto ex = icon_ext_.find(e.ext);
        return ex != icon_ext_.end() ? ex->second : -1;
    }
    auto it = icon_ext_.find(e.ext);
    if (it == icon_ext_.end()) it = icon_ext_.emplace(e.ext, sys_icon_index(L"x" + e.ext, false, false)).first;
    return e.icon = it->second;
}

int App::fv_icon(FileView* v, Entry& e) {
    return icon_index(e, [v] { v->invalidate(); });
}

BitmapPtr App::thumb(const wstr& path, int size, std::function<void()> arrived) {
    wstr key = fmt(L"%d|", size) + path;
    auto it = thumbs_.find(key);
    if (it != thumbs_.end()) return it->second;
    if (thumb_pending_.count(key)) return nullptr;
    thumb_pending_.insert(key);
    request_image(path, size, true, [this, key, arrived](BitmapPtr b) {
        thumb_pending_.erase(key);
        thumbs_[key] = b;
        thumb_order_.push_back(key);
        while (thumb_order_.size() > 600) {
            thumbs_.erase(thumb_order_.front());
            thumb_order_.pop_front();
        }
        if (b && arrived) arrived();
    });
    return nullptr;
}

BitmapPtr App::fv_thumb(FileView* v, const Entry& e, int size) {
    return thumb(e.path, size, [v] { v->invalidate(); });
}

// ======================================================================
// the main view's callbacks
// ======================================================================
void App::fv_open(FileView* v, int i) {
    if (i >= 0 && i < (int)v->items.size()) open_entry(v->items[i]);
}

void App::fv_menu(FileView*, int i, POINT pt) {
    if (i >= 0) item_menu(i, pt);
    else background_menu(pt);
}

void App::fv_selection(FileView*) {
    update_status();
    if (ql && ql->is_open() && ql->source == this) ql->selection_changed();
    update_preview(this);
    if (view_mode() == V_COLUMNS) columns->sync_selection();
}

void App::fv_middle(FileView* v, int i) {
    if (i >= 0 && v->items[i]->dir) new_tab(v->items[i]->path);
}

void App::fv_rename(FileView* v, int i, const wstr& name) {
    if (i < 0 || i >= (int)v->items.size()) return;
    wstr old = v->items[i]->path;
    rename_paths({{old, path_join(dir_name(old), name)}});
}

bool App::fv_can_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) {
    return transfer_problem(paths, dest, effect == DROPEFFECT_MOVE).empty();
}

void App::fv_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) {
    transfer(paths, dest, effect == DROPEFFECT_MOVE);
}

EntryPtr App::ql_entry() {
    FileView* fv = main_fv();
    if (fv->focus >= 0 && fv->focus < (int)fv->items.size() && fv->sel[fv->focus]) return fv->items[fv->focus];
    auto s = fv->selected_entries();
    return s.empty() ? nullptr : s[0];
}

void App::ql_step(UINT vk) {
    FileView* fv = main_fv();
    if (fv->mode != VM_GRID) vk = vk == VK_LEFT ? VK_UP : vk == VK_RIGHT ? VK_DOWN : vk;
    fv->step(vk, false);
}

// ---- single instance: another start hands its folder to this window
const wchar_t* INSTANCE_PROP = L"FileExplorer.Cpp.Main";

void App::open_from_other(const wstr& p) {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    if (p.empty()) return;
    wstr folder = p;
    std::vector<wstr> sel;
    if (!is_dir(p)) {
        if (!exists(p)) { say(L"Can't find " + p, 8); return; }
        folder = dir_name(p);
        sel.push_back(base_name(p));
    }
    for (size_t i = 0; i < tabs.size(); i++)
        if (norm(tabs[i].path) == norm(folder)) {
            switch_tab((int)i);
            if (!sel.empty()) navigate(folder, sel);
            return;
        }
    new_tab(folder);
    if (!sel.empty()) navigate(folder, sel);
}

// ---- the preview panel
void App::toggle_preview() {
    preview_open = !preview_open;
    preview->show(preview_open);
    toolbar.active[Toolbar::B_PREVIEW] = preview_open;
    toolbar.invalidate();
    layout();
    if (preview_open) update_preview(active_view && pane2_open ? (QLSource*)pane2 : (QLSource*)this);
    else preview->clear(L"Select a file to see it here");
    save_state();
}

void App::update_preview(QLSource* src) {
    if (!preview || !preview_open) return;
    preview->source = src;
    preview->selection_changed();
}

void pane2_navigate(App* a, const wstr& path) {
    if (a->pane2) a->pane2->navigate(path);
}
