#include "chrome.h"
#include "app.h"
#include <algorithm>

enum { TIMER_TIP = 7 };

static int sys_drive_icon() {
    static int idx = -2;
    if (idx == -2) idx = sys_icon_index(L"C:\\", true, true);
    return idx;
}

// ======================================================================
// TabBar
// ======================================================================
TabBar::~TabBar() {
    if (drop_) { drop_->revoke(); drop_->Release(); }
}

void TabBar::init(App* a, HWND parent) {
    app = a;
    create(parent);
    drop_ = new DropTarget(hwnd);
    drop_->over = [this](POINT pt, DWORD keys, DWORD allowed, const std::vector<wstr>& paths) -> DWORD {
        Hit h = hit(pt.x, pt.y);
        int t = h.tab;
        DWORD e = DROPEFFECT_NONE;
        if (t >= 0) {
            wstr dest = app->tabs[t].path;
            if (!dest.empty() && !is_virtual(dest)) {
                e = choose_effect(paths, dest, keys, allowed);
                if (!app->transfer_problem(paths, dest, e == DROPEFFECT_MOVE).empty()) e = DROPEFFECT_NONE;
            }
        }
        int nt = e ? t : -1;
        if (nt != drop_tab_) { drop_tab_ = nt; invalidate(); }
        return e;
    };
    drop_->leave = [this] { drop_tab_ = -1; invalidate(); };
    drop_->drop = [this](POINT pt, DWORD e, const std::vector<wstr>& paths) {
        Hit h = hit(pt.x, pt.y);
        if (h.tab >= 0) app->transfer(paths, app->tabs[h.tab].path, e == DROPEFFECT_MOVE);
    };
}

void TabBar::compute(HDC dc) {
    int n = (int)app->tabs.size();
    rects_.assign(n, RECT{});
    wstr sets_label = app->current_set.empty() ? L"Tab sets" : app->current_set;
    int sets_w = text_width(dc, F.small, sets_label) + S(44);
    int avail = width() - sets_w - S(48);
    std::vector<int> ws(n);
    int total = 0;
    for (int i = 0; i < n; i++) {
        wstr name = display_name(app->tabs[i].path);
        ws[i] = std::max(S(96), std::min(S(230), text_width(dc, F.header, name) + S(78)));
        total += ws[i];
    }
    if (total > avail && n) {
        int each = std::max(S(64), avail / n);
        for (auto& w : ws) w = std::min(w, each);
    }
    int x = 0;
    for (int i = 0; i < n; i++) {
        rects_[i] = RECT{x, 0, x + ws[i], height()};
        x += ws[i] + 1;
    }
    plus_ = RECT{x + S(4), S(5), x + S(4) + S(30), height() - S(5)};
    sets_ = RECT{width() - sets_w, 0, width(), height()};
}

TabBar::Hit TabBar::hit(int x, int y) {
    Hit h;
    for (size_t i = 0; i < rects_.size(); i++) {
        const RECT& r = rects_[i];
        if (x >= r.left && x < r.right) {
            h.tab = (int)i;
            h.close = x >= r.right - S(30);
            return h;
        }
    }
    if (x >= plus_.left && x < plus_.right) h.plus = true;
    if (x >= sets_.left && x < sets_.right) h.sets = true;
    return h;
}

void TabBar::on_paint(HDC dc, const RECT& rc) {
    compute(dc);
    fill(dc, rc, T.panel);
    hline(dc, 0, rc.right, rc.bottom - 1, T.border);
    for (size_t i = 0; i < rects_.size(); i++) {
        RECT r = rects_[i];
        bool on = (int)i == app->active;
        const wstr& p = app->tabs[i].path;
        if (on) {
            RECT b{r.left, r.top, r.right, r.bottom};
            fill(dc, b, T.bg);
            fill(dc, RECT{r.left, 0, r.right, S(2)}, T.accent);
        } else if ((int)i == drop_tab_) {
            fill(dc, r, T.drop);
        } else if ((int)i == hover_tab_) {
            fill(dc, RECT{r.left, r.top, r.right, r.bottom - 1}, T.hover);
        }
        vline(dc, r.right, S(8), r.bottom - S(8), T.border);
        int isz = S(16), ix = r.left + S(14), iy = (r.bottom - isz) / 2;
        wstr kind, a, b;
        parse_virtual(p, kind, a, b);
        if (kind == L"search") draw_icon(dc, G_SEARCH, RECT{ix, iy, ix + isz, iy + isz}, T.accent, 14);
        else if (kind == L"tag") fill_circle(dc, ix + isz / 2.0f, iy + isz / 2.0f, S(5), app->tag_color(a));
        else if (p.empty()) draw_sys_icon(dc, sys_drive_icon(), ix, iy, isz);
        else folder_bitmap(app->folder_color(p), isz)->draw(dc, ix, iy);
        RECT tr{ix + isz + S(8), 0, r.right - S(34), r.bottom};
        text(dc, display_name(p), tr, on ? T.fg : T.faint, on ? F.header : F.small);
        RECT cr{r.right - S(30), (r.bottom - S(22)) / 2, r.right - S(8), (r.bottom + S(22)) / 2};
        bool hot = (int)i == hover_tab_ && hover_close_;
        if (hot) fill_round(dc, cr, S(4), mix(on ? T.bg : T.hover, T.bad, 0.18));
        if (on || (int)i == hover_tab_) draw_icon(dc, G_CLOSE, cr, hot ? T.bad : T.faint, 12);
    }
    if (hover_plus_) fill_round(dc, plus_, S(6), T.hover);
    draw_icon(dc, G_ADD, plus_, hover_plus_ ? T.accent : T.faint, 14);
    RECT s = sets_;
    if (hover_sets_) fill(dc, RECT{s.left, s.top, s.right, s.bottom - 1}, T.hover);
    vline(dc, s.left, S(8), s.bottom - S(8), T.border);
    RECT si{s.left + S(10), 0, s.left + S(30), s.bottom};
    COLORREF sc = app->current_set.empty() ? T.muted : T.accent;
    draw_icon(dc, G_SETS, si, sc, 14);
    RECT st{s.left + S(32), 0, s.right - S(8), s.bottom};
    text(dc, app->current_set.empty() ? L"Tab sets" : app->current_set, st, sc, F.small);
}

void TabBar::on_mouse_move(int x, int y, WPARAM) {
    Hit h = hit(x, y);
    bool ch = h.tab != hover_tab_ || h.close != hover_close_ || h.plus != hover_plus_ || h.sets != hover_sets_;
    hover_tab_ = h.tab;
    hover_close_ = h.close;
    hover_plus_ = h.plus;
    if (h.sets != hover_sets_) {
        hover_sets_ = h.sets;
        if (h.sets) SetTimer(hwnd, TIMER_TIP, 600, nullptr);
        else Tip::hide();
    }
    if (h.plus && !hover_plus_) SetTimer(hwnd, TIMER_TIP, 600, nullptr);
    if (ch) invalidate();
}

void TabBar::on_timer(UINT_PTR id) {
    KillTimer(hwnd, id);
    POINT p = cursor();
    Hit h = hit(p.x, p.y);
    POINT s{p.x, height() + S(4)};
    ClientToScreen(hwnd, &s);
    if (h.sets) Tip::show(hwnd, L"Tab sets: open a saved group of tabs, or save these  (Ctrl+Shift+S)", s);
    else if (h.plus) Tip::show(hwnd, L"New tab  (Ctrl+T)", s);
}

void TabBar::on_mouse_leave() {
    hover_tab_ = -1;
    hover_close_ = hover_plus_ = hover_sets_ = false;
    Tip::hide();
    invalidate();
}

void TabBar::on_lbutton_down(int x, int y, WPARAM) {
    Tip::hide();
    Hit h = hit(x, y);
    if (h.tab >= 0) {
        if (h.close) app->close_tab(h.tab);
        else app->switch_tab(h.tab);
    } else if (h.plus) app->new_tab(home_dir());
    else if (h.sets) {
        POINT p{sets_.left, sets_.bottom};
        ClientToScreen(hwnd, &p);
        app->sets_menu(p);
    }
}

void TabBar::on_lbutton_dbl(int x, int y, WPARAM k) {
    Hit h = hit(x, y);
    if (h.tab < 0 && !h.plus && !h.sets) app->new_tab(app->real_folder().empty() ? home_dir() : app->real_folder());
    else on_lbutton_down(x, y, k);
}

void TabBar::on_mbutton_down(int x, int y, WPARAM) {
    Hit h = hit(x, y);
    if (h.tab >= 0) app->close_tab(h.tab);
}

void TabBar::on_rbutton_up(int x, int y, WPARAM) {
    Hit h = hit(x, y);
    if (h.tab < 0) return;
    int i = h.tab;
    Menu m;
    App* a = app;
    m.item(L"New tab\tCtrl+T", [a] { a->new_tab(a->cur().path); });
    m.item(L"Duplicate tab", [a, i] { a->new_tab(a->tabs[i].path); });
    m.sep();
    m.item(L"Close tab\tCtrl+W", [a, i] { a->close_tab(i); });
    m.item(L"Close other tabs", [a, i] { a->close_others(i); }, a->tabs.size() > 1);
    m.sep();
    m.item(L"Save these tabs as a set\u2026\tCtrl+Shift+S", [a] { a->save_tab_set(); });
    POINT p{x, y};
    ClientToScreen(hwnd, &p);
    m.show(app->hwnd, p);
}

// ======================================================================
// Breadcrumb
// ======================================================================
Breadcrumb::~Breadcrumb() {
    if (drop_) { drop_->revoke(); drop_->Release(); }
}

void Breadcrumb::init(HWND parent) {
    create(parent);
    box_.create_in(hwnd);
    box_.show(false);
    box_.key = [this](UINT vk, bool, bool) {
        if (vk == VK_RETURN) {
            wstr here = is_virtual(path) ? L"" : path;
            wstr target = resolve_typed_path(box_.get(), here);
            stop_edit();
            if (navigate) navigate(target);
            return true;
        }
        if (vk == VK_ESCAPE) {
            stop_edit();
            if (escape) escape();
            return true;
        }
        return false;
    };
    box_.focus_changed = [this](bool got) {
        if (!got) post_ui([this] { if (editing_ && GetFocus() != box_.edit) stop_edit(); });
    };
    drop_ = new DropTarget(hwnd);
    drop_->over = [this](POINT pt, DWORD keys, DWORD allowed, const std::vector<wstr>& paths) -> DWORD {
        int found = -1;
        for (size_t i = 0; i < segs_.size(); i++)
            if (PtInRect(&segs_[i].r, pt) && !segs_[i].path.empty() && !is_virtual(segs_[i].path)) found = (int)i;
        DWORD e = DROPEFFECT_NONE;
        if (found >= 0) {
            e = choose_effect(paths, segs_[found].path, keys, allowed);
            if (can_drop && !can_drop(segs_[found].path, paths, e)) { e = DROPEFFECT_NONE; found = -1; }
        }
        if (found != drop_seg_) { drop_seg_ = found; invalidate(); }
        return e;
    };
    drop_->leave = [this] { drop_seg_ = -1; invalidate(); };
    drop_->drop = [this](POINT pt, DWORD e, const std::vector<wstr>& paths) {
        for (auto& s : segs_)
            if (PtInRect(&s.r, pt) && !s.path.empty() && !is_virtual(s.path) && dropped) { dropped(s.path, paths, e); break; }
    };
}

void Breadcrumb::set_path(const wstr& p) {
    path = p;
    if (editing_) {
        wstr k, a, b;
        parse_virtual(p, k, a, b);
        box_.set(is_virtual(p) ? (k == L"search" ? (a.empty() ? L"This PC" : a) : L"") : (p.empty() ? L"This PC" : p), false);
    }
    invalidate();
}

void Breadcrumb::edit() {
    if (editing_) { box_.focus(); box_.select_all(); return; }
    editing_ = true;
    wstr k, a, b;
    parse_virtual(path, k, a, b);
    box_.set(is_virtual(path) ? (k == L"search" ? (a.empty() ? L"This PC" : a) : L"") : (path.empty() ? L"This PC" : path), false);
    box_.move(0, 0, width(), height());
    box_.show(true);
    box_.focus();
    box_.select_all();
    invalidate();
}

void Breadcrumb::stop_edit() {
    if (!editing_) return;
    editing_ = false;
    box_.show(false);
    invalidate();
}

void Breadcrumb::on_size(int w, int h) {
    if (editing_) box_.move(0, 0, w, h);
}

void Breadcrumb::compute(HDC dc) {
    segs_.clear();
    folded_.clear();
    std::vector<Seg> all;
    auto real = [&](const wstr& p) {
        for (auto& q : ancestors(p)) {
            Seg s;
            s.path = q;
            s.arrow = true;
            s.label = q.empty() ? L"This PC" : display_name(q);
            if (is_drive_root(s.label) && s.label.size() == 3) s.label = s.label.substr(0, 2);
            all.push_back(s);
        }
    };
    wstr k, a, b;
    parse_virtual(path, k, a, b);
    if (k == L"search") {
        real(a);
        Seg s;
        s.label = L"\u2315  \u201c" + b + L"\u201d";
        s.path = path;
        s.arrow = false;
        all.push_back(s);
    } else if (k == L"tag") {
        Seg s;
        s.label = L"Tag: " + a;
        s.path = path;
        s.arrow = false;
        all.push_back(s);
    } else real(path);
    int avail = width() - S(16);
    std::vector<int> ws;
    int arrow_w = S(18);
    for (auto& s : all) ws.push_back(text_width(dc, F.ui, s.label) + S(14) + (s.arrow ? arrow_w : 0));
    size_t start = 0;
    int more_w = S(30);
    auto sum = [&](size_t from) { int t = 0; for (size_t i = from; i < ws.size(); i++) t += ws[i]; return t; };
    while (start + 1 < all.size() && sum(start) + (start ? more_w : 0) > avail) start++;
    int x = S(6);
    if (start) {
        Seg more;
        more.label = L"\u00ab";
        more.hidden_menu = true;
        more.arrow = false;
        more.r = RECT{x, S(4), x + more_w - S(4), height() - S(4)};
        x += more_w;
        segs_.push_back(more);
        for (size_t i = 0; i < start; i++) folded_.push_back(all[i]);
    }
    for (size_t i = start; i < all.size(); i++) {
        Seg s = all[i];
        int tw = text_width(dc, F.ui, s.label) + S(14);
        s.r = RECT{x, S(4), x + tw, height() - S(4)};
        x += tw;
        if (s.arrow) {
            s.ar = RECT{x, S(4), x + arrow_w, height() - S(4)};
            x += arrow_w;
        }
        segs_.push_back(s);
    }
}

void Breadcrumb::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.bg);
    fill_round(dc, rc, S(6), T.field);
    if (editing_) return;
    compute(dc);
    COLORREF hot = mix(T.field, T.accent, 0.16);
    for (size_t i = 0; i < segs_.size(); i++) {
        Seg& s = segs_[i];
        bool last = i + 1 == segs_.size();
        if (hot_ == (int)i * 2 || drop_seg_ == (int)i) fill_round(dc, s.r, S(4), drop_seg_ == (int)i ? T.drop : hot);
        text(dc, s.label, s.r, last ? T.fg : T.muted, F.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (s.arrow) {
            if (hot_ == (int)i * 2 + 1) fill_round(dc, s.ar, S(4), hot);
            draw_icon(dc, G_CHEVRON, s.ar, T.faint, 12);
        }
    }
}

void Breadcrumb::on_mouse_move(int x, int y, WPARAM) {
    int h = -1;
    for (size_t i = 0; i < segs_.size(); i++) {
        if (PtInRect(&segs_[i].r, POINT{x, y})) h = (int)i * 2;
        if (segs_[i].arrow && PtInRect(&segs_[i].ar, POINT{x, y})) h = (int)i * 2 + 1;
    }
    SetCursor(LoadCursorW(nullptr, h >= 0 ? IDC_HAND : IDC_IBEAM));
    if (h != hot_) { hot_ = h; invalidate(); }
}

void Breadcrumb::on_lbutton_down(int x, int y, WPARAM) {
    for (size_t i = 0; i < segs_.size(); i++) {
        Seg& s = segs_[i];
        if (PtInRect(&s.r, POINT{x, y})) {
            if (s.hidden_menu) {
                Menu m;
                for (auto it = folded_.rbegin(); it != folded_.rend(); ++it) {
                    wstr p = it->path;
                    m.item(it->label, [this, p] { if (navigate) navigate(p); });
                }
                POINT pt{s.r.left, s.r.bottom};
                ClientToScreen(hwnd, &pt);
                m.show(GetAncestor(hwnd, GA_ROOT), pt);
            } else if (i + 1 == segs_.size()) edit();
            else if (navigate) navigate(s.path);
            return;
        }
        if (s.arrow && PtInRect(&s.ar, POINT{x, y})) { menu_for((int)i); return; }
    }
    edit();
}

void Breadcrumb::on_mbutton_down(int x, int y, WPARAM) {
    for (auto& s : segs_)
        if (PtInRect(&s.r, POINT{x, y}) && !s.hidden_menu && !is_virtual(s.path) && middle) { middle(s.path); return; }
}

void Breadcrumb::menu_for(int i) {
    Seg s = segs_[i];
    wstr next = i + 1 < (int)segs_.size() ? segs_[i + 1].path : L"";
    Menu m;
    std::vector<std::pair<wstr, wstr>> items;
    if (s.path.empty()) {
        for (auto& e : drive_entries(cached_drives())) items.push_back({e->name, e->path});
    } else {
        DWORD err = 0;
        Entries list = list_folder(s.path, show_hidden && show_hidden(), &err);
        for (auto& e : list) if (e->dir) items.push_back({e->name, e->path});
        std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return natural_cmp(a.first, b.first) < 0; });
        if (err) m.disabled(L"Can't open this folder");
    }
    size_t limit = 150;
    for (size_t k = 0; k < items.size() && k < limit; k++) {
        wstr p = items[k].second;
        bool on = !next.empty() && norm(p) == norm(next);
        m.item(items[k].first, [this, p] { if (navigate) navigate(p); }, true, on);
    }
    if (items.size() > limit) m.disabled(fmt(L"\u2026 %d more \u2014 open the folder to see them all", (int)(items.size() - limit)));
    if (items.empty() && !s.path.empty()) m.disabled(L"(no folders inside)");
    POINT pt{s.ar.left, s.ar.bottom};
    ClientToScreen(hwnd, &pt);
    m.show(GetAncestor(hwnd, GA_ROOT), pt);
}

// ======================================================================
// SearchBox
// ======================================================================
void SearchBox::on_size(int w, int h) {
    int fh = font_height(F.ui);
    MoveWindow(edit, S(32), (h - fh) / 2, std::max(10, w - S(32) - S(28)), fh, TRUE);
}

void SearchBox::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.bg);
    fill_round(dc, rc, S(6), T.field);
    if (focused) frame_round(dc, rc, S(6), T.accent, 1.2f);
    POINT p = cursor();
    bool lens_hot = hovering_ && p.x < S(30);
    draw_icon(dc, G_SEARCH, RECT{S(6), 0, S(30), rc.bottom}, lens_hot ? T.accent : T.muted, 14);
    if (GetWindowTextLengthW(edit) > 0) {
        bool hot = hovering_ && p.x > rc.right - S(28);
        draw_icon(dc, G_CLOSE, RECT{rc.right - S(28), 0, rc.right - S(4), rc.bottom}, hot ? T.fg : T.faint, 12);
    }
}

void SearchBox::on_lbutton_down(int x, int y, WPARAM) {
    if (x < S(30)) { if (search) search(); return; }
    if (x > width() - S(28) && GetWindowTextLengthW(edit) > 0) {
        set(L"");
        focus();
        return;
    }
    focus();
}

void SearchBox::on_mouse_move(int x, int, WPARAM) {
    SetCursor(LoadCursorW(nullptr, x < S(30) || x > width() - S(28) ? IDC_HAND : IDC_IBEAM));
    invalidate();
}

// ======================================================================
// Toolbar
// ======================================================================
static const Glyph BTN_GLYPH[] = {G_BACK, G_FWD, G_UP, G_REFRESH, G_STAR, G_LIST, G_GRID, G_COLUMNS, G_DUAL, G_PREVIEW, G_THEME};

void Toolbar::init(App* a, HWND parent) {
    app = a;
    for (int i = 0; i < B_COUNT; i++) { enabled[i] = true; active[i] = false; }
    create(parent);
    crumb.init(hwnd);
    search.hint = L"Filter \u00b7 Enter searches inside";
    search.create_in(hwnd);
}

void Toolbar::on_size(int w, int h) {
    int bw = S(34), bh = S(32), y = (h - bh) / 2, x = S(10), pad = S(3);
    groups_.clear();
    // left: back, forward, up, refresh
    RECT g{x, y - pad, x + 4 * bw + 2 * pad, y + bh + pad};
    groups_.push_back(g);
    for (int i = 0; i < 4; i++) rects_[i] = RECT{x + pad + i * bw, y, x + pad + (i + 1) * bw, y + bh};
    int left_end = g.right + S(8);
    // right side, from the right
    int r = w - S(10);
    RECT ge{r - 3 * bw - 2 * pad, y - pad, r, y + bh + pad};
    groups_.push_back(ge);
    rects_[B_DUAL] = RECT{ge.left + pad, y, ge.left + pad + bw, y + bh};
    rects_[B_PREVIEW] = RECT{ge.left + pad + bw, y, ge.left + pad + 2 * bw, y + bh};
    rects_[B_THEME] = RECT{ge.left + pad + 2 * bw, y, ge.left + pad + 3 * bw, y + bh};
    r = ge.left - S(8);
    int sw = std::min(S(250), std::max(S(150), w / 5));
    search.move(r - sw, y - pad, sw, bh + 2 * pad);
    r -= sw + S(8);
    RECT gv{r - 3 * bw - 2 * pad, y - pad, r, y + bh + pad};
    groups_.push_back(gv);
    for (int i = 0; i < 3; i++) rects_[B_LIST + i] = RECT{gv.left + pad + i * bw, y, gv.left + pad + (i + 1) * bw, y + bh};
    r = gv.left - S(6);
    rects_[B_STAR] = RECT{r - bw, y, r, y + bh};
    r -= bw + S(6);
    crumb.move(left_end, y - pad, std::max(S(80), r - left_end), bh + 2 * pad);
}

void Toolbar::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.bg);
    for (auto& g : groups_) fill_round(dc, g, S(7), T.field);
    for (int i = 0; i < B_COUNT; i++) {
        RECT r = rects_[i];
        bool in_group = i != B_STAR;
        COLORREF base = in_group ? T.field : T.bg;
        if (active[i]) fill_round(dc, RECT{r.left + S(1), r.top + S(1), r.right - S(1), r.bottom - S(1)}, S(5), T.select);
        else if (hot_ == i && enabled[i]) fill_round(dc, r, S(5), mix(base, T.accent, 0.16));
        COLORREF c = !enabled[i] ? T.faint : active[i] ? T.accent : hot_ == i ? T.fg : T.muted;
        Glyph g = BTN_GLYPH[i];
        if (i == B_STAR) { g = star_on ? G_STAR_FILLED : G_STAR; if (star_on && enabled[i]) c = T.star; }
        draw_icon(dc, g, r, c, 16);
    }
}

int Toolbar::hit(int x, int y) const {
    for (int i = 0; i < B_COUNT; i++) if (PtInRect(&rects_[i], POINT{x, y})) return i;
    return -1;
}

void Toolbar::on_mouse_move(int x, int y, WPARAM) {
    int h = hit(x, y);
    if (h != hot_) {
        hot_ = h;
        Tip::hide();
        if (h >= 0) SetTimer(hwnd, TIMER_TIP, 550, nullptr);
        invalidate();
    }
    SetCursor(LoadCursorW(nullptr, h >= 0 && enabled[h] ? IDC_HAND : IDC_ARROW));
}

void Toolbar::on_timer(UINT_PTR id) {
    KillTimer(hwnd, id);
    if (hot_ < 0) return;
    static const wchar_t* tips[] = {L"Back  (Alt+\u2190)", L"Forward  (Alt+\u2192)", L"Up  (Backspace)",
                                    L"Refresh  (Ctrl+R)", L"Add to / remove from favorites  (Ctrl+D)",
                                    L"List  (Ctrl+Shift+1)", L"Grid  (Ctrl+Shift+2)", L"Columns  (Ctrl+Shift+3)",
                                    L"Two panes  (F3)", L"Preview panel  (Alt+P)", L"Colours"};
    wstr t = tips[hot_];
    if (hot_ == B_THEME) t = L"Colours: " + app->theme_name;
    POINT p{rects_[hot_].left, rects_[hot_].bottom + S(6)};
    ClientToScreen(hwnd, &p);
    Tip::show(hwnd, t, p);
}

void Toolbar::on_mouse_leave() {
    hot_ = -1;
    Tip::hide();
    KillTimer(hwnd, TIMER_TIP);
    invalidate();
}

void Toolbar::on_lbutton_down(int x, int y, WPARAM) {
    Tip::hide();
    int h = hit(x, y);
    if (h < 0 || !enabled[h]) return;
    switch (h) {
        case B_BACK: app->go_back(); break;
        case B_FWD: app->go_forward(); break;
        case B_UP: app->go_up(); break;
        case B_REFRESH: app->refresh(); break;
        case B_STAR: app->toggle_favorite(); break;
        case B_LIST: app->set_view(V_LIST); break;
        case B_GRID: app->set_view(V_GRID); break;
        case B_COLUMNS: app->set_view(V_COLUMNS); break;
        case B_DUAL: app->toggle_dual(); break;
        case B_PREVIEW: app->toggle_preview(); break;
        case B_THEME: app->theme_menu(); break;
    }
}

// ======================================================================
// Sidebar
// ======================================================================
Sidebar::~Sidebar() {
    if (drop_) { drop_->revoke(); drop_->Release(); }
}

void Sidebar::init(App* a, HWND parent) {
    app = a;
    create(parent);
    drop_ = new DropTarget(hwnd);
    drop_->over = [this](POINT pt, DWORD keys, DWORD allowed, const std::vector<wstr>& paths) -> DWORD {
        int i = row_at(pt.y);
        DWORD e = DROPEFFECT_NONE;
        if (i >= 0) {
            Row& r = rows_[i];
            if (r.kind == K_TAG) e = DROPEFFECT_LINK & allowed ? DROPEFFECT_LINK : DROPEFFECT_COPY;
            else if (r.kind == K_FAV || r.kind == K_RECENT || r.kind == K_FOLDER || r.kind == K_DRIVE) {
                e = choose_effect(paths, r.path, keys, allowed);
                if (!app->transfer_problem(paths, r.path, e == DROPEFFECT_MOVE).empty()) e = DROPEFFECT_NONE;
            }
        }
        int nd = e ? i : -1;
        if (nd != drop_row_) { drop_row_ = nd; invalidate(); }
        return e;
    };
    drop_->leave = [this] { drop_row_ = -1; invalidate(); };
    drop_->drop = [this](POINT pt, DWORD e, const std::vector<wstr>& paths) {
        int i = row_at(pt.y);
        if (i < 0) return;
        Row r = rows_[i];
        if (r.kind == K_TAG) app->add_tag(paths, r.text);
        else app->transfer(paths, r.path, e == DROPEFFECT_MOVE);
    };
}

void Sidebar::rebuild() {
    rows_.clear();
    // a header you can click to fold / unfold its section; folded, it shows how many items it hides
    size_t head = 0;
    auto header = [&](const wstr& t) { head = rows_.size(); rows_.push_back({K_HEADER, t, t}); };
    auto folded = [&] { return app->collapsed.count(rows_[head].path) > 0; };
    auto add = [&](const Row& r) {
        if (r.kind != K_HINT && r.kind != K_NEWTAG) rows_[head].index = std::max(0, rows_[head].index) + 1;
        if (!folded()) rows_.push_back(r);
    };
    header(L"FAVORITES");
    if (app->favorites.empty()) add({K_HINT, L"Ctrl+D adds this folder"});
    for (size_t i = 0; i < app->favorites.size(); i++) {
        auto& f = app->favorites[i];
        Row r{K_FAV, f.name, f.path};
        r.index = (int)i;
        if (!is_dir(f.path)) r.extra = L"(missing)";
        add(r);
    }
    header(L"RECENT");
    auto rec = app->recent_folders();
    if (rec.empty()) add({K_HINT, L"Folders you open show up here"});
    std::vector<wstr> names;
    for (auto& p : rec) names.push_back(display_name(p));
    for (size_t i = 0; i < rec.size(); i++) {
        Row r{K_RECENT, names[i], rec[i]};
        if (std::count(names.begin(), names.end(), names[i]) > 1) r.extra = display_name(dir_name(rec[i]));
        add(r);
    }
    header(L"FOLDERS");
    for (auto& [name, path] : quick_access()) add({K_FOLDER, name, path});
    header(L"THIS PC");
    for (auto& d : cached_drives()) {
        Row r{K_DRIVE, d.label + L" (" + d.root.substr(0, 2) + L")", d.root};
        if (d.free >= 0) r.extra = fmt_size(d.free) + L" free";
        add(r);
    }
    header(L"TAGS");
    auto counts = app->tag_counts();
    for (size_t i = 0; i < app->tag_defs.size(); i++) {
        auto& t = app->tag_defs[i];
        Row r{K_TAG, t.name, make_tag_path(t.name)};
        r.index = (int)i;
        r.dot = folder_rgb(t.color);
        if (counts[t.name]) r.extra = fmt(L"%d", counts[t.name]);
        add(r);
    }
    add({K_NEWTAG, L"New tag\u2026"});
    scroll_ = std::max(0, std::min(scroll_, total_h() - height()));
    invalidate();
}

void Sidebar::toggle_section(const wstr& name) {
    if (app->collapsed.count(name)) app->collapsed.erase(name);
    else app->collapsed.insert(name);
    rebuild();
    app->save_state();
}

void Sidebar::set_all_sections(bool fold) {
    app->collapsed.clear();
    if (fold) for (auto& r : rows_) if (r.kind == K_HEADER) app->collapsed.insert(r.path);
    rebuild();
    app->save_state();
}

int Sidebar::row_h(const Row& r) const { return r.kind == K_HEADER ? S(34) : S(30); }
int Sidebar::row_top(int i) const {
    int y = S(6) - scroll_;
    for (int k = 0; k < i; k++) y += row_h(rows_[k]);
    return y;
}
int Sidebar::total_h() const {
    int t = S(12);
    for (auto& r : rows_) t += row_h(r);
    return t;
}
int Sidebar::row_at(int y) const {
    int top = S(6) - scroll_;
    for (size_t i = 0; i < rows_.size(); i++) {
        int h = row_h(rows_[i]);
        if (y >= top && y < top + h) return (int)i;
        top += h;
    }
    return -1;
}

void Sidebar::on_size(int, int) {
    scroll_ = std::max(0, std::min(scroll_, total_h() - height()));
    invalidate();
}

void Sidebar::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.panel);
    int y = S(6) - scroll_;
    wstr cur = norm(current_);
    for (size_t i = 0; i < rows_.size(); i++) {
        Row& r = rows_[i];
        int h = row_h(r);
        if (y + h < 0) { y += h; continue; }
        if (y > rc.bottom) break;
        if (r.kind == K_HEADER) {
            bool fold = app->collapsed.count(r.path) > 0, hot = (int)i == hover_;
            RECT t{S(16), y + S(10), rc.right - S(8), y + h};
            text(dc, r.text, t, hot ? T.fg : T.head, F.small_bold);
            int cx = rc.right - S(24), cy = y + S(10) + (h - S(10)) / 2;
            if (fold && r.index > 0) {  // how many are hidden
                wstr n = std::to_wstring(r.index);
                RECT nr{rc.right - S(110), y + S(10), cx - S(10), y + h};
                text(dc, n, nr, T.faint, F.small, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
            // chevron: down when open, right when folded
            int a = S(4);
            POINT pts[3];
            if (fold) { pts[0] = {cx - a / 2, cy - a}; pts[1] = {cx + a / 2, cy}; pts[2] = {cx - a / 2, cy + a}; }
            else { pts[0] = {cx - a, cy - a / 2}; pts[1] = {cx, cy + a / 2}; pts[2] = {cx + a, cy - a / 2}; }
            HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1.5)), hot ? T.fg : T.faint);
            HGDIOBJ op = SelectObject(dc, pen);
            Polyline(dc, pts, 3);
            SelectObject(dc, op);
            DeleteObject(pen);
            y += h;
            continue;
        }
        RECT row{S(8), y + S(1), rc.right - S(8), y + h - S(1)};
        bool selected = !cur.empty() && r.kind != K_HINT && r.kind != K_NEWTAG && norm(r.path) == cur &&
                        (r.kind != K_RECENT || [&] { for (auto& rr : rows_) if (rr.kind == K_FAV && norm(rr.path) == cur) return false; return true; }());
        if ((int)i == drop_row_) fill_round(dc, row, S(6), T.drop);
        else if (selected) fill_round(dc, row, S(6), T.select);
        else if ((int)i == hover_ && r.kind != K_HINT) fill_round(dc, row, S(6), T.hover);
        if (drag_fav_ >= 0 && drag_moved_ && (int)i == drag_over_) fill(dc, RECT{row.left, row.top - S(1), row.right, row.top + S(1)}, T.accent);
        if (selected) fill_round(dc, RECT{row.left + S(1), y + S(8), row.left + S(4), y + h - S(8)}, S(2), T.accent);
        int isz = S(16), ix = S(20), iy = y + (h - isz) / 2;
        int tx = ix + isz + S(10);
        COLORREF tc = T.fg;
        switch (r.kind) {
            case K_FAV:
            case K_RECENT:
            case K_FOLDER:
                if (r.kind == K_FAV && !r.extra.empty()) { tc = T.faint; draw_icon(dc, G_STAR, RECT{ix, iy, ix + isz, iy + isz}, T.faint, 14); }
                else folder_bitmap(app->folder_color(r.path), isz)->draw(dc, ix, iy);
                break;
            case K_TAG: fill_circle(dc, ix + isz / 2.0f, iy + isz / 2.0f, S(5), r.dot); break;
            case K_DRIVE: draw_sys_icon(dc, sys_drive_icon(), ix, iy, isz); break;
            case K_NEWTAG: draw_icon(dc, G_ADD, RECT{ix, iy, ix + isz, iy + isz}, T.faint, 12); tc = T.faint; break;
            case K_HINT: tx = ix; tc = T.faint; break;
            default: break;
        }
        int ew = r.extra.empty() ? 0 : text_width(dc, F.small, r.extra) + S(12);
        RECT t{tx, y, rc.right - S(14) - ew, y + h};
        text(dc, r.text, t, tc, r.kind == K_HINT ? F.small : F.ui);
        if (ew) {
            RECT e{rc.right - S(12) - ew, y, rc.right - S(14), y + h};
            text(dc, r.extra, e, T.faint, F.small, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        y += h;
    }
    sb_.total = total_h();
    sb_.page = rc.bottom;
    sb_.pos = scroll_;
    sb_.track = RECT{rc.right - S(8), S(2), rc.right - S(2), rc.bottom - S(2)};
    sb_.dragging = sb_drag_;
    sb_.paint(dc);
}

void Sidebar::on_mouse_move(int x, int y, WPARAM keys) {
    if (sb_drag_) {
        if (sb_.drag_to(y)) { scroll_ = sb_.pos; invalidate(); }
        return;
    }
    if (drag_fav_ >= 0) {
        if (!drag_moved_ && std::abs(y - press_.y) > S(6)) drag_moved_ = true;
        if (drag_moved_) {
            int i = row_at(y);
            if (i != drag_over_) { drag_over_ = i; invalidate(); }
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        }
        return;
    }
    bool hot = sb_.hit(x, y);
    if (hot != sb_.hot) { sb_.hot = hot; invalidate(); }
    int i = row_at(y);
    if (i >= 0 && rows_[i].kind == K_HINT) i = -1;
    if (i != hover_) { hover_ = i; invalidate(); }
}

void Sidebar::on_mouse_leave() {
    if (hover_ != -1 || sb_.hot) { hover_ = -1; sb_.hot = false; invalidate(); }
}

void Sidebar::on_lbutton_down(int x, int y, WPARAM) {
    if (sb_.press(x, y)) {
        scroll_ = sb_.pos;
        if (sb_.dragging) { sb_drag_ = true; SetCapture(hwnd); }
        invalidate();
        return;
    }
    int i = row_at(y);
    if (i < 0) return;
    Row r = rows_[i];
    press_ = POINT{x, y};
    if (r.kind == K_FAV) {  // maybe a drag to reorder
        drag_fav_ = r.index;
        drag_moved_ = false;
        drag_over_ = -1;
        SetCapture(hwnd);
        return;
    }
    if (r.kind == K_NEWTAG) { app->new_tag(); return; }
    if (r.kind == K_HEADER) { toggle_section(r.path); return; }
    if (r.kind == K_HINT) return;
    if (norm(r.path) != norm(app->cur().path)) app->navigate(r.path);
}

void Sidebar::on_lbutton_up(int x, int y, WPARAM) {
    if (sb_drag_) { sb_drag_ = false; sb_.dragging = false; ReleaseCapture(); invalidate(); return; }
    if (drag_fav_ < 0) return;
    int from = drag_fav_;
    bool moved = drag_moved_;
    drag_fav_ = -1;
    ReleaseCapture();
    if (!moved) {
        if (from < (int)app->favorites.size()) {
            wstr p = app->favorites[from].path;
            if (norm(p) != norm(app->cur().path)) app->navigate(p);
        }
        return;
    }
    int i = row_at(y);
    if (i >= 0 && rows_[i].kind == K_FAV) app->move_favorite(from, rows_[i].index);
    else if (i >= 0 && rows_[i].kind == K_HEADER && rows_[i].text == L"FAVORITES") app->move_favorite(from, 0);
    invalidate();
}

void Sidebar::on_capture_lost() {
    drag_fav_ = -1;
    sb_drag_ = false;
    sb_.dragging = false;
    invalidate();
}

void Sidebar::on_mbutton_down(int x, int y, WPARAM) {
    int i = row_at(y);
    if (i >= 0 && !rows_[i].path.empty() && rows_[i].kind != K_HEADER && rows_[i].kind != K_HINT) app->new_tab(rows_[i].path);
}

bool Sidebar::on_wheel(int delta, WPARAM, int, int) {
    scroll_ = std::max(0, std::min(std::max(0, total_h() - height()), scroll_ - delta * S(90) / 120));
    invalidate();
    return true;
}

void Sidebar::on_rbutton_up(int x, int y, WPARAM) {
    int i = row_at(y);
    if (i < 0) return;
    Row r = rows_[i];
    if (r.kind == K_HINT) return;
    App* a = app;
    Menu m;
    POINT pt{x, y};
    ClientToScreen(hwnd, &pt);
    if (r.kind == K_HEADER) {
        wstr name = r.path;
        bool fold = a->collapsed.count(name) > 0;
        m.item(fold ? L"Expand" : L"Collapse", [this, name] { toggle_section(name); });
        m.sep();
        m.item(L"Collapse all", [this] { set_all_sections(true); });
        m.item(L"Expand all", [this] { set_all_sections(false); }, !a->collapsed.empty());
        m.show(a->hwnd, pt);
        return;
    }
    if (r.kind == K_NEWTAG) { m.item(L"New tag\u2026", [a] { a->new_tag(); }); m.show(a->hwnd, pt); return; }
    wstr path = r.path;
    m.item(L"Open", [a, path] { a->navigate(path); });
    m.item(L"Open in new tab", [a, path] { a->new_tab(path); });
    if (r.kind == K_TAG) {
        int t = r.index;
        m.sep();
        m.item(L"Rename / recolour tag\u2026", [a, t] { a->edit_tag(t); });
        m.item(L"Move up", [a, t] { a->move_tag(t, -1); }, t > 0);
        m.item(L"Move down", [a, t] { a->move_tag(t, 1); }, t + 1 < (int)a->tag_defs.size());
        m.item(L"New tag\u2026", [a] { a->new_tag(); });
        m.sep();
        m.item(L"Delete tag", [a, t] { a->delete_tag(t); });
        m.show(a->hwnd, pt);
        return;
    }
    if (a->pane2_open) m.item(L"Open on the right", [a, path] { pane2_navigate(a, path); });
    m.item(L"Open in Windows Explorer", [path] { open_in_explorer(path, false); });
    m.sep();
    if (r.kind == K_RECENT) {
        m.item(L"\u2605 Add to favorites", [a, path] { a->add_favorite(path); });
        m.item(L"Remove from recent", [a, path] { a->forget_recent(path); });
        m.item(L"Clear the recent list", [a] { a->forget_recent(L""); });
        m.sep();
    } else if (r.kind == K_FAV) {
        int f = r.index;
        m.item(L"Rename\u2026", [a, f] { a->rename_favorite(f); });
        m.item(L"Move up", [a, f] { a->move_favorite(f, f - 1); }, f > 0);
        m.item(L"Move down", [a, f] { a->move_favorite(f, f + 1); }, f + 1 < (int)a->favorites.size());
        m.sep();
        m.item(L"Remove from favorites", [a, f] { a->remove_favorite(f); });
    } else {
        m.item(L"\u2605 Add to favorites", [a, path] { a->add_favorite(path); }, !a->is_favorite(path));
    }
    m.item(L"Copy path", [a, path] { a->copy_text(path); });
    if (r.kind != K_DRIVE) {
        Menu c(m, 0);
        a->color_menu(c, {path});
        m.sub(L"Folder colour", c);
    }
    m.show(a->hwnd, pt);
}

// ======================================================================
// StatusBar
// ======================================================================
void StatusBar::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.panel);
    hline(dc, 0, rc.right, 0, T.border);
    RECT t{S(14), 0, rc.right - S(14), rc.bottom};
    text(dc, text_, t, message_ ? T.fg : T.faint, F.small);
}
