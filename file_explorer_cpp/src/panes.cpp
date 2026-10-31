#include "panes.h"
#include "quicklook.h"
#include <algorithm>

// ======================================================================
// the right-hand pane
// ======================================================================
void SecondPane::init(App* a, HWND parent) {
    app = a;
    create(parent);
    crumb.init(hwnd);
    crumb.navigate = [this](const wstr& p) { navigate(p); };
    crumb.escape = [this] { SetFocus(view.hwnd); };
    crumb.middle = [this](const wstr& p) { app->new_tab(p); };
    crumb.show_hidden = [this] { return app->show_hidden; };
    crumb.can_drop = [this](const wstr& d, const std::vector<wstr>& p, DWORD e) { return app->transfer_problem(p, d, e == DROPEFFECT_MOVE).empty(); };
    crumb.dropped = [this](const wstr& d, const std::vector<wstr>& p, DWORD e) { app->transfer(p, d, e == DROPEFFECT_MOVE); };
    view.create_view(hwnd, this, VM_LIST);
    view.columns = {{"name", L"Name", 160}, {"modified", L"Date modified", 125}, {"size", L"Size", 72, true}};
}

void SecondPane::on_size(int w, int h) {
    int bar = S(50), info = S(28);
    int bw = S(32), y = (bar - S(32)) / 2;
    b_back_ = RECT{S(2), y, S(2) + bw, y + S(32)};
    b_up_ = RECT{S(2) + bw, y, S(2) + 2 * bw, y + S(32)};
    b_close_ = RECT{w - bw - S(2), y, w - S(2), y + S(32)};
    crumb.move(b_up_.right + S(6), y - S(3), b_close_.left - S(6) - b_up_.right - S(6), S(38));
    view.move(0, bar + S(3), w, h - bar - S(3) - info);
}

void SecondPane::on_paint(HDC dc, const RECT& rc) {
    ::fill(dc, rc, T.bg);
    auto btn = [&](const RECT& r, Glyph g, int id, bool enabled) {
        if (hot_ == id && enabled) fill_round(dc, r, S(5), mix(T.bg, T.accent, 0.16));
        draw_icon(dc, g, r, !enabled ? T.faint : hot_ == id ? T.fg : T.muted, 16);
    };
    btn(b_back_, G_BACK, 0, !back.empty());
    btn(b_up_, G_UP, 1, has_path && !path.empty());
    btn(b_close_, G_CLOSE, 2, true);
    int bar = S(50);
    bool on = app->active_view == &view;
    fill_round(dc, RECT{S(8), bar, rc.right - S(8), bar + S(2)}, S(1), on ? T.accent : T.bg);
    RECT ir{S(10), rc.bottom - S(28), rc.right - S(10), rc.bottom};
    text(dc, info_, ir, T.faint, F.small);
}

void SecondPane::on_mouse_move(int x, int y, WPARAM) {
    int h = PtInRect(&b_back_, POINT{x, y}) ? 0 : PtInRect(&b_up_, POINT{x, y}) ? 1 : PtInRect(&b_close_, POINT{x, y}) ? 2 : -1;
    if (h != hot_) { hot_ = h; invalidate(); }
}

void SecondPane::on_lbutton_down(int x, int y, WPARAM) {
    if (PtInRect(&b_back_, POINT{x, y})) go_back();
    else if (PtInRect(&b_up_, POINT{x, y})) go_up();
    else if (PtInRect(&b_close_, POINT{x, y})) app->toggle_dual();
}

void SecondPane::navigate(const wstr& p, const std::vector<wstr>& select) {
    if (is_virtual(p)) return;
    if (!p.empty() && !is_dir(p)) {
        if (exists(p)) { navigate(dir_name(p), {base_name(p)}); return; }
        app->say(L"Can't find " + p, 8);
        return;
    }
    if (has_path && norm(p) != norm(path)) { back.push_back(path); fwd.clear(); }
    view.scroll_to_top();
    load(p, select);
    app->save_state();
}

void SecondPane::go_back() {
    if (back.empty()) return;
    fwd.push_back(path);
    wstr came = path, p = back.back();
    back.pop_back();
    load(p, !came.empty() && norm(dir_name(came)) == norm(p) ? std::vector<wstr>{display_name(came)} : std::vector<wstr>{});
}

void SecondPane::go_up() {
    if (!has_path || path.empty()) return;
    wstr parent = dir_name(path);
    if (norm(parent) == norm(path)) parent.clear();
    navigate(parent, {display_name(path)});
}

void SecondPane::reload() {
    if (!has_path) return;
    load(path);
}

void SecondPane::load(const wstr& p, const std::vector<wstr>& select) {
    path = p;
    has_path = true;
    int tok = ++token;
    if (!select.empty()) select_names = select;
    crumb.set_path(p);
    info_ = L"Loading…";
    invalidate();
    bool hidden = app->show_hidden;
    watcher.stop();
    run_bg([this, tok, p, hidden] {
        Entries list;
        DWORD err = 0;
        if (p.empty()) list = drive_entries(list_drives());
        else list = list_folder(p, hidden, &err);
        post_ui([this, tok, p, list, err] {
            if (tok != token) return;
            entries = list;
            for (auto& e : entries)
                if (e->dir && !e->drive) {
                    auto it = app->dir_sizes.find(norm(e->path));
                    if (it != app->dir_sizes.end()) e->size = it->second.first;
                }
            if (err) app->say(L"Can't open " + p + L" on the right", 8);
            fill();
            if (!select_names.empty()) {
                view.select_paths(select_names, true);
                select_names.clear();
            } else if (view.focus < 0 && !view.items.empty()) view.set_focus_index(0);
            update_info();
            if (!p.empty()) watcher.watch(p, [this] { if (!view.renaming()) reload(); });
        });
    });
}

void SecondPane::fill() {
    view.sort_key = app->sort_key;
    view.sort_desc = app->sort_desc;
    view.set_items(app->sorted(entries), true, true);
    update_info();
    if (app->active_view == &view) app->update_preview(this);
}

void SecondPane::update_info() {
    int n = (int)view.items.size();
    auto sel = view.selected_entries();
    info_ = fmt(L"%d item%s", n, n == 1 ? L"" : L"s");
    if (!sel.empty()) {
        i64 size = 0;
        for (auto& e : sel) if (!e->drive && e->size > 0) size += e->size;
        info_ += fmt(L"   ·   %d selected", (int)sel.size()) + (size ? L" (" + fmt_size(size) + L")" : L"");
    }
    if (has_path && !path.empty()) {
        ULARGE_INTEGER fr;
        if (GetDiskFreeSpaceExW(path.c_str(), &fr, nullptr, nullptr)) info_ += L"   ·   " + fmt_size((i64)fr.QuadPart) + L" free";
    }
    invalidate();
}

int SecondPane::fv_icon(FileView* v, Entry& e) { return app->icon_index(e, [v] { v->invalidate(); }); }

void SecondPane::fv_open(FileView*, int i) {
    EntryPtr e = view.items[i];
    if (e->dir) navigate(e->path);
    else app->open_entry(e);
}

void SecondPane::fv_selection(FileView*) {
    update_info();
    if (app->ql && app->ql->is_open() && app->ql->source == this) app->ql->selection_changed();
    app->update_preview(this);
}

void SecondPane::fv_middle(FileView*, int i) {
    if (view.items[i]->dir) app->new_tab(view.items[i]->path);
}

bool SecondPane::fv_key(FileView*, UINT vk, bool ctrl, bool shift, bool alt) {
    auto paths = [&] { std::vector<wstr> p; for (auto& e : selection()) if (!e->drive) p.push_back(e->path); return p; };
    if (ctrl && !alt) {
        if (vk == 'C') { app->copy_paths(paths(), false); return true; }
        if (vk == 'X') { app->copy_paths(paths(), true); return true; }
        if (vk == 'V') { app->paste(path); return true; }
        return false;
    }
    if (alt) {
        if (vk == VK_LEFT) { go_back(); return true; }
        if (vk == VK_UP) { go_up(); return true; }
        return false;
    }
    switch (vk) {
        case VK_RETURN: {
            auto sel = selection();
            if (sel.size() == 1 && sel[0]->dir) navigate(sel[0]->path);
            else for (auto& e : sel) if (!e->dir) app->open_entry(e);
            return true;
        }
        case VK_BACK: go_up(); return true;
        case VK_DELETE: { auto p = paths(); if (!p.empty()) app->delete_paths(p, shift); return true; }
        case VK_F2: {
            auto sel = selection();
            if (sel.size() > 1) app->bulk_rename(sel);
            else if (sel.size() == 1) view.start_rename(view.index_of(sel[0]->path));
            return true;
        }
        case VK_SPACE: app->quick_look(this); return true;
        case VK_TAB: app->focus_view(); app->set_active(nullptr); SetFocus(app->main_fv()->hwnd); return true;
    }
    return false;
}

void SecondPane::fv_rename(FileView*, int i, const wstr& name) {
    wstr old = view.items[i]->path;
    app->rename_paths({{old, path_join(dir_name(old), name)}});
}

bool SecondPane::fv_can_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) {
    return app->transfer_problem(paths, dest, effect == DROPEFFECT_MOVE).empty();
}

void SecondPane::fv_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) {
    app->transfer(paths, dest, effect == DROPEFFECT_MOVE);
}

EntryPtr SecondPane::ql_entry() {
    if (view.focus >= 0 && view.focus < (int)view.items.size() && view.sel[view.focus]) return view.items[view.focus];
    auto s = view.selected_entries();
    return s.empty() ? nullptr : s[0];
}

void SecondPane::fv_menu(FileView*, int i, POINT pt) {
    App* a = app;
    Menu m;
    if (i >= 0) {
        auto sel = selection();
        EntryPtr e = view.items[i];
        std::vector<wstr> paths;
        for (auto& x : sel) if (!x->drive) paths.push_back(x->path);
        m.item(L"Open", [this, e] { if (e->dir) navigate(e->path); else app->open_entry(e); });
        if (sel.size() == 1 && e->dir) {
            m.item(L"Open on the left", [a, e] { a->navigate(e->path); });
            m.item(L"Open in new tab", [a, e] { a->new_tab(e->path); });
        }
        m.item(L"Quick Look\tSpace", [this] { app->quick_look(this); });
        if (!paths.empty()) {
            m.sep();
            m.item(L"← Copy to the left\tF5", [a] { a->copy_across(false, true); });
            m.item(L"← Move to the left\tF6", [a] { a->copy_across(true, true); });
            m.sep();
            m.item(L"Cut\tCtrl+X", [a, paths] { a->copy_paths(paths, true); });
            m.item(L"Copy\tCtrl+C", [a, paths] { a->copy_paths(paths, false); });
            m.item(paths.size() > 1 ? fmt(L"Rename %d items…", (int)paths.size()) : L"Rename\tF2", [this] { fv_key(&view, VK_F2, false, false, false); });
            m.item(L"Delete\tDel", [a, paths] { a->delete_paths(paths, false); });
            m.sep();
            a->add_archive_items(m, sel, path);
            Menu tm(m, 0);
            a->tag_menu(tm, paths);
            m.sub(L"Tags", tm);
        }
        m.sep();
        m.item(L"Copy path", [a, sel] { wstr t; for (auto& x : sel) t += (t.empty() ? L"" : L"\r\n") + x->path; a->copy_text(t); });
    } else {
        if (has_path && !path.empty()) {
            wstr p = path;
            m.item(L"New folder", [a, p] { a->new_folder(p); });
            m.item(L"Paste\tCtrl+V", [a, p] { a->paste(p); });
            m.sep();
        }
        wstr p = path;
        m.item(L"Show the same folder on the left", [a, p] { a->navigate(p); });
        m.item(L"Refresh", [this] { reload(); });
        if (!p.empty()) m.item(L"Open in Windows Explorer", [p] { open_in_explorer(p, false); });
        m.sep();
        m.item(L"Close this pane\tF3", [a] { a->toggle_dual(); });
    }
    m.show(app->hwnd, pt);
}

// ======================================================================
// Columns view
// ======================================================================
enum { TIMER_OPEN = 21 };

void ColumnsView::init(App* a, HWND parent) {
    app = a;
    create(parent);
}

Entries ColumnsView::listing(const wstr& folder, double& mt) {
    if (folder.empty()) { mt = -1; return drive_entries(cached_drives(30)); }
    mt = folder_mtime(folder);
    auto it = cache_.find(norm(folder));
    if (it != cache_.end() && it->second.first == mt) return it->second.second;
    DWORD err;
    Entries list = app->sorted(list_folder(folder, app->show_hidden, &err));
    cache_[norm(folder)] = {mt, list};
    if (cache_.size() > 60) cache_.erase(cache_.begin());
    return list;
}

void ColumnsView::rebuild() {
    if (!hwnd || app->view_mode() != V_COLUMNS) return;
    auto chain = ancestors(app->cur().path);
    const size_t MAX = 6;
    if (chain.size() > MAX) chain.erase(chain.begin(), chain.end() - MAX);
    size_t k = 0;
    while (k < cols.size() && k < chain.size() && norm(cols[k].folder) == norm(chain[k]) && cols[k].mirror == (k + 1 == chain.size())) k++;
    cols.resize(k);
    for (size_t i = k; i < chain.size(); i++) {
        Col c;
        c.folder = chain[i];
        c.mirror = i + 1 == chain.size();
        c.fv.reset(new FileView());
        c.fv->create_view(hwnd, this, VM_COLUMN);
        cols.push_back(std::move(c));
    }
    for (size_t i = 0; i < cols.size(); i++) {
        Col& c = cols[i];
        if (c.mirror) {
            c.fv->sort_key = app->sort_key;
            c.fv->set_items(app->view.items, true, true);
        } else {
            double mt;
            Entries list = listing(c.folder, mt);
            if (i >= k || mt != c.mtime) { c.fv->set_items(list, false, true); c.mtime = mt; }
            int hit = c.fv->index_of(chain[i + 1]);
            c.fv->select_only(hit, false);
            if (hit >= 0) c.fv->ensure_visible(hit);
        }
    }
    bool moved = chain_ != chain;
    chain_ = chain;
    place();
    if (moved) { offset_ = std::min(max_offset(), std::max(0, (int)cols.size() - 1) * (col_w() + 1)); place(); }
    invalidate();
}

// scrolled all the way right, the first column you see is a whole one (the preview gets the spare room)
int ColumnsView::max_offset() const {
    int need = std::max(0, content_w() - width()), step = col_w() + 1;
    int cap = std::max(0, (int)cols.size() - 1) * step;  // never past the start of the last column
    return std::min((need + step - 1) / step * step, std::max(need, cap));
}

int ColumnsView::content_w() const { return (int)cols.size() * (col_w() + 1) + preview_w(); }

void ColumnsView::place() {
    int h = height() - S(12);
    offset_ = std::max(0, std::min(offset_, max_offset()));
    for (size_t i = 0; i < cols.size(); i++) cols[i].fv->move((int)i * (col_w() + 1) - offset_, 0, col_w(), h);
    invalidate();
}

void ColumnsView::sync_selection() { invalidate(); }

void ColumnsView::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.surface);
    for (size_t i = 0; i < cols.size(); i++) {
        int x = (int)(i + 1) * (col_w() + 1) - 1 - offset_;
        vline(dc, x, S(6), rc.bottom - S(18), T.border);
    }
    // preview of the one selected item
    FileView* m = mirror();
    int px = (int)cols.size() * (col_w() + 1) - offset_ + S(18);
    auto sel = m ? m->selected_entries() : std::vector<EntryPtr>{};
    if (sel.size() == 1) {
        EntryPtr e = sel[0];
        int size = S(140), y = S(30);
        int cx = px + (preview_w() - S(36) - size) / 2;
        if (e->dir && !e->drive) folder_bitmap(app->folder_color(e->path), size)->draw(dc, cx, y);
        else {
            BitmapPtr b = !e->drive && (THUMB_EXTS.count(e->ext) || OWN_ICON_EXTS.count(e->ext)) ? app->thumb(e->path, size, [this] { invalidate(); }) : nullptr;
            if (b) {
                double k = std::min((double)size / b->w, (double)size / b->h);
                int w = (int)(b->w * k), h = (int)(b->h * k);
                b->draw(dc, cx + (size - w) / 2, y + (size - h) / 2, w, h);
            } else draw_sys_icon(dc, app->icon_index(*e, [this] { invalidate(); }), cx + (size - S(96)) / 2, y + (size - S(96)) / 2, S(96));
        }
        y += size + S(16);
        RECT nr{px, y, px + preview_w() - S(36), y + S(48)};
        text(dc, e->name, nr, T.fg, F.title, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
        y += S(56);
        std::vector<std::pair<wstr, wstr>> rows;
        rows.push_back({L"Kind", e->drive ? L"Drive" : e->dir ? L"Folder" : sys_type_name(e->ext, false)});
        if (e->drive && e->total > 0) rows.push_back({L"Free", fmt_size(e->free) + L" of " + fmt_size(e->total)});
        if (e->size >= 0 && !e->drive) rows.push_back({L"Size", fmt_size(e->size)});
        if (e->mtime > 0) rows.push_back({L"Modified", fmt_time(e->mtime)});
        auto tags = app->tags_of(e->path);
        if (!tags.empty()) { wstr t; for (auto& x : tags) t += (t.empty() ? L"" : L", ") + x; rows.push_back({L"Tags", t}); }
        for (auto& [k, v] : rows) {
            RECT kr{px, y, px + S(90), y + S(22)}, vr{px + S(100), y, px + preview_w() - S(36), y + S(22)};
            text(dc, k, kr, T.faint, F.small, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            text(dc, v, vr, T.fg, F.small);
            y += S(24);
        }
    }
    hsb_.total = content_w();
    hsb_.page = rc.right;
    hsb_.pos = offset_;
    if (hsb_.total > hsb_.page) {
        int th = std::max(S(30), (int)((double)rc.right * rc.right / hsb_.total));
        int x = (int)((double)(rc.right - th) * offset_ / std::max(1, hsb_.total - hsb_.page));
        fill_round(dc, RECT{x, rc.bottom - S(8), x + th, rc.bottom - S(3)}, S(2), hdrag_ ? T.accent : mix(T.border, T.muted, 0.3));
    }
}

bool ColumnsView::on_wheel(int delta, WPARAM keys, int, int) {
    offset_ -= delta * S(60) / 120;
    place();
    return true;
}

void ColumnsView::on_lbutton_down(int x, int y, WPARAM) {
    if (y > height() - S(12) && content_w() > width()) { hdrag_ = true; hdrag_x_ = x; hdrag_off_ = offset_; SetCapture(hwnd); }
}

void ColumnsView::on_mouse_move(int x, int, WPARAM) {
    if (!hdrag_) return;
    int w = std::max(1, width());
    offset_ = hdrag_off_ + (x - hdrag_x_) * content_w() / w;
    place();
}

void ColumnsView::on_lbutton_up(int, int, WPARAM) {
    if (hdrag_) { hdrag_ = false; ReleaseCapture(); invalidate(); }
}

wstr ColumnsView::fv_folder(FileView* v) {
    for (auto& c : cols) if (c.fv.get() == v) return c.folder;
    return L"";
}

wstr ColumnsView::fv_empty(FileView* v) {
    for (auto& c : cols) if (c.fv.get() == v) return c.folder.empty() ? L"" : L"Empty";
    return L"";
}

void ColumnsView::fv_open(FileView* v, int i) {
    EntryPtr e = v->items[i];
    if (e->dir) { wstr p = e->path; post_ui([this, p] { app->navigate(p); }); }
    else app->open_entry(e);
}

void ColumnsView::fv_selection(FileView* v) {
    if (v == mirror()) { app->fv_selection(v); invalidate(); return; }
    // a parent column: open what was picked (after the click is over)
    auto sel = v->selected_entries();
    if (sel.size() != 1) return;
    EntryPtr e = sel[0];
    wstr folder = fv_folder(v);
    if (e->dir) {
        size_t idx = 0;
        for (; idx < cols.size(); idx++) if (cols[idx].fv.get() == v) break;
        if (idx + 1 < chain_.size() && norm(chain_[idx + 1]) == norm(e->path)) return;  // already open
        wstr p = e->path;
        post_ui([this, p] { app->navigate(p); });
    } else {
        wstr n = e->name;
        post_ui([this, folder, n] { app->navigate(folder, {n}); });
    }
}

void ColumnsView::fv_clicked(FileView* v, int i, WPARAM keys) {
    if (v != mirror() || (keys & (MK_CONTROL | MK_SHIFT))) return;
    EntryPtr e = v->items[i];
    if (!e->dir) return;
    pending_open_ = e->path;
    SetTimer(hwnd, TIMER_OPEN, 150, nullptr);
}

void ColumnsView::on_timer(UINT_PTR id) {
    KillTimer(hwnd, id);
    if (id != TIMER_OPEN || !mirror()) return;
    auto sel = mirror()->selected_entries();
    if (sel.size() == 1 && norm(sel[0]->path) == norm(pending_open_) && app->view_mode() == V_COLUMNS) app->navigate(pending_open_);
}

void ColumnsView::fv_middle(FileView* v, int i) {
    if (v->items[i]->dir) app->new_tab(v->items[i]->path);
}

bool ColumnsView::fv_key(FileView* v, UINT vk, bool ctrl, bool shift, bool alt) {
    if (!alt && !ctrl && vk == VK_RIGHT) {
        auto sel = v->selected_entries();
        if (sel.size() == 1 && sel[0]->dir) { wstr p = sel[0]->path; post_ui([this, p] { app->navigate(p); }); }
        return true;
    }
    if (!alt && !ctrl && vk == VK_LEFT) { post_ui([this] { app->go_up(); }); return true; }
    if (v == mirror()) return app->fv_key(v, vk, ctrl, shift, alt);
    return false;
}

void ColumnsView::fv_menu(FileView* v, int i, POINT pt) {
    if (v == mirror()) { app->fv_menu(v, i, pt); return; }
    if (i < 0) return;
    EntryPtr e = v->items[i];
    App* a = app;
    Menu m;
    m.item(L"Open", [a, e] { a->navigate(e->path); });
    if (e->dir) m.item(L"Open in new tab", [a, e] { a->new_tab(e->path); });
    m.item(L"Copy path", [a, e] { a->copy_text(e->path); });
    m.show(app->hwnd, pt);
}
