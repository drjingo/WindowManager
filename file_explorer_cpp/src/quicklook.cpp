#include "quicklook.h"
#include <algorithm>

namespace {
const int HEAD_H = 56, INFO_H = 30, HINT_H = 26;
const size_t TEXT_LIMIT = 512 * 1024;
}

void QuickLook::init(App* a) {
    app = a;
    RECT r = app->ql_rect;
    RECT ar;
    GetWindowRect(app->hwnd, &ar);
    if (r.right - r.left < S(320) || r.bottom - r.top < S(240)) {
        int w = (int)((ar.right - ar.left) * 0.62), h = (int)((ar.bottom - ar.top) * 0.78);
        w = std::max(w, S(560));
        h = std::max(h, S(440));
        r = RECT{ar.left + (ar.right - ar.left - w) / 2, ar.top + (ar.bottom - ar.top - h) / 2, 0, 0};
        r.right = r.left + w;
        r.bottom = r.top + h;
    }
    create_top(app->hwnd, L"Quick Look", WS_OVERLAPPEDWINDOW, 0, r.left, r.top, r.right - r.left, r.bottom - r.top);
    HICON big = make_hicon(32), small = make_hicon(16);
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
    style_window(hwnd);
    list_.create(hwnd);
    list_.headers = {L"Inside the archive", L"Size"};
    list_.widths = {0, 100};
    list_.show(false);
}

void QuickLook::init_child(App* a, HWND parent) {
    app = a;
    embedded = true;
    create(parent, WS_CHILD | WS_CLIPCHILDREN);
    list_.create(hwnd);
    list_.headers = {L"Inside the archive", L"Size"};
    list_.widths = {0, 80};
    list_.show(false);
    clear(L"Select a file to see it here");
}

void QuickLook::clear(const wstr& msg) {
    token_++;
    entry.reset();
    picture_.reset();
    lines_.clear();
    line_spans_.clear();
    text_.clear();
    info_.clear();
    folder_info_.clear();
    if (list_.hwnd) list_.show(false);
    mode_ = M_MESSAGE;
    message_ = msg;
    invalidate();
}

void QuickLook::show_for(QLSource* src) {
    source = src;
    EntryPtr e = src->ql_entry();
    if (!e) { close_ql(); return; }
    if (!is_open()) {
        style_window(hwnd);
        ShowWindow(hwnd, SW_SHOW);
    }
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    render(e);
}

void QuickLook::close_ql() {
    if (!hwnd || !IsWindowVisible(hwnd)) return;
    token_++;
    WINDOWPLACEMENT wp{sizeof wp};
    if (GetWindowPlacement(hwnd, &wp) && wp.showCmd == SW_SHOWNORMAL) {
        GetWindowRect(hwnd, &app->ql_rect);
        app->save_state();
    }
    ShowWindow(hwnd, SW_HIDE);
    entry.reset();
    picture_.reset();
    lines_.clear();
    line_spans_.clear();
    text_.clear();
    list_.set_rows({});
    if (source) source->ql_focus();
    SetActiveWindow(app->hwnd);
}

void QuickLook::selection_changed() {
    if (!is_open() || !source) return;
    SetTimer(hwnd, 1, embedded ? 90 : 40, nullptr);  // the selection may move several times in a row
}

void QuickLook::on_timer(UINT_PTR id) {
    KillTimer(hwnd, id);
    if (!is_open() || !source) return;
    EntryPtr e = source->ql_entry();
    if (e && (!entry || norm(e->path) != norm(entry->path))) render(e);
    else if (!e && embedded && entry) clear(L"Select a file to see it here");
}

// ----------------------------------------------------------------------
void QuickLook::render(EntryPtr e) {
    int tok = ++token_;
    entry = e;
    picture_.reset();
    picture_is_icon_ = false;
    lines_.clear();
    line_spans_.clear();
    text_.clear();
    vscroll_ = hscroll_ = 0;
    wrap_width_ = -1;
    message_.clear();
    folder_info_.clear();
    list_.show(false);
    if (!embedded) SetWindowTextW(hwnd, (e->name + L" — Quick Look").c_str());

    std::vector<wstr> info;
    info.push_back(e->drive ? L"Drive" : e->dir ? L"Folder" : sys_type_name(e->ext, false));
    if (e->drive && e->total > 0) info.push_back(fmt_size(e->free) + L" free of " + fmt_size(e->total));
    if (!e->dir && e->size >= 0) info.push_back(fmt_size(e->size));
    if (e->mtime > 0) info.push_back(L"Modified " + fmt_time(e->mtime));
    auto tags = app->tags_of(e->path);
    if (!tags.empty()) {
        wstr t;
        for (auto& x : tags) t += (t.empty() ? L"" : L", ") + x;
        info.push_back(L"Tags: " + t);
    }
    if (!e->folder.empty()) info.push_back(e->folder);
    info_.clear();
    for (auto& s : info) info_ += (info_.empty() ? L"" : L"   ·   ") + s;

    wstr path = e->path;
    auto picture = [this, tok, path](bool icon_only) {
        mode_ = M_PICTURE;
        if (!icon_only)
            request_image(path, std::max(S(512), std::min(1600, (int)std::max(body_.right - body_.left, body_.bottom - body_.top))), true,
                          [this, tok, path](BitmapPtr b) {
                              if (tok != token_) return;
                              if (b) { picture_ = b; picture_is_icon_ = false; invalidate(); return; }
                              request_image(path, 256, false, [this, tok](BitmapPtr b2) {
                                  if (tok != token_) return;
                                  picture_ = b2;
                                  picture_is_icon_ = true;
                                  if (!b2) { mode_ = M_MESSAGE; message_ = L"No preview for this file."; }
                                  invalidate();
                              });
                          });
        else
            request_image(path, 256, false, [this, tok](BitmapPtr b) {
                if (tok != token_) return;
                picture_ = b;
                picture_is_icon_ = true;
                invalidate();
            });
    };

    if (e->drive) {
        picture(true);
    } else if (e->dir) {
        mode_ = M_FOLDER;
        folder_info_ = L"Counting…";
        bool hidden = app->show_hidden;
        int* tp = &token_;
        run_bg([this, tok, tp, path, hidden] {
            DWORD err = 0;
            size_t n = list_folder(path, hidden, &err).size();
            wstr head = fmt(L"%d item%s", (int)n, n == 1 ? L"" : L"s");
            post_ui([this, tok, head] { if (tok == token_) { folder_info_ = head; invalidate(); } });
            i64 bytes = 0, files = 0;
            if (dir_size(path, [tp, tok] { return *tp != tok; }, bytes, files)) {
                wstr t = head + L"  ·  " + fmt_size(bytes) + L" in " + fmt_count(files) + (files == 1 ? L" file" : L" files");
                post_ui([this, tok, t] { if (tok == token_) { folder_info_ = t; invalidate(); } });
            }
        });
    } else if (!archive_kind(e->name).empty() && archive_kind(e->name) != L"7z") {
        mode_ = M_ARCHIVE;
        message_ = L"Reading…";
        run_bg([this, tok, path] {
            std::vector<ArchItem> items;
            wstr err;
            bool ok = archive_list(path, items, err);
            post_ui([this, tok, ok, items, err] {
                if (tok != token_) return;
                if (!ok) { mode_ = M_MESSAGE; message_ = L"Can't read this archive" + (err.empty() ? L"." : L": " + err); invalidate(); return; }
                std::vector<SimpleList::Row> rows;
                i64 total = 0;
                int files = 0;
                for (auto& it : items) {
                    SimpleList::Row r;
                    wstr n = it.name;
                    while (!n.empty() && (n.back() == L'/' || n.back() == L'\\')) n.pop_back();
                    int depth = (int)std::count_if(n.begin(), n.end(), [](wchar_t c) { return c == L'/' || c == L'\\'; });
                    size_t cut = n.find_last_of(L"/\\");
                    wstr leaf = cut == wstr::npos ? n : n.substr(cut + 1);
                    r.cells = {leaf, it.dir ? L"" : fmt_size(it.size)};
                    r.indent = std::min(depth, 12);
                    r.sys_icon = sys_icon_index(leaf, it.dir, false);
                    if (!it.dir) { total += it.size; files++; }
                    rows.push_back(std::move(r));
                }
                if (rows.empty()) { mode_ = M_MESSAGE; message_ = L"This archive is empty."; invalidate(); return; }
                info_ += fmt(L"   ·   %d file%s inside (", files, files == 1 ? L"" : L"s") + fmt_size(total) + L" unpacked)";
                message_.clear();
                list_.scroll = 0;
                list_.set_rows(std::move(rows));
                list_.show(true);
                on_size(width(), height());
                invalidate();
            });
        });
    } else {
        bool known_text = TEXT_EXTS.count(e->ext) > 0;
        bool maybe_text = !known_text && !THUMB_EXTS.count(e->ext) && !is_type(L"audio", e->ext) &&
                          !is_type(L"video", e->ext) && !is_type(L"archive", e->ext) && e->size <= 4 * 1024 * 1024;
        if (known_text || maybe_text) {
            mode_ = M_MESSAGE;
            message_ = L"";
            wstr ext = e->ext;
            run_bg([this, tok, path, ext, known_text, picture] {
                wstr t;
                bool cut = false;
                bool ok = read_text(path, TEXT_LIMIT, t, cut, !known_text);
                post_ui([this, tok, ok, t, cut, ext, picture] {
                    if (tok != token_) return;
                    if (!ok) { picture(false); invalidate(); return; }
                    cut_ = cut;
                    set_text(t, ext);
                    mode_ = M_TEXT;
                    invalidate();
                });
            });
        } else {
            picture(false);
        }
    }
    invalidate();
}

void QuickLook::set_text(const wstr& raw, const wstr& ext) {
    // tabs → 4 spaces, \r\n → \n (before colouring, so the positions line up)
    wstr t;
    t.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        wchar_t c = raw[i];
        if (c == L'\r') { if (i + 1 < raw.size() && raw[i + 1] == L'\n') continue; t += L'\n'; }
        else if (c == L'\t') t += L"    ";
        else t += c;
    }
    text_ = t;
    code_ = is_code_ext(ext) || TEXT_EXTS.count(ext);
    auto spans = highlight(t, ext);
    lines_.clear();
    line_spans_.clear();
    std::vector<int> starts;
    size_t s = 0;
    while (true) {
        size_t n = t.find(L'\n', s);
        starts.push_back((int)s);
        lines_.push_back(t.substr(s, n == wstr::npos ? wstr::npos : n - s));
        if (n == wstr::npos) break;
        s = n + 1;
    }
    line_spans_.resize(lines_.size());
    size_t li = 0;
    for (auto& sp : spans) {
        int a = sp.start, b = sp.start + sp.len;
        while (li + 1 < starts.size() && starts[li + 1] <= a) li++;
        for (size_t k = li; k < lines_.size() && starts[k] < b; k++) {
            int ls = starts[k], le = ls + (int)lines_[k].size();
            int x0 = std::max(a, ls), x1 = std::min(b, le);
            if (x1 > x0) line_spans_[k].push_back(Span{x0 - ls, x1 - x0, sp.kind});
        }
    }
    wrap_ = !code_;  // prose wraps, code scrolls sideways
    wrap_width_ = -1;
}

// ----------------------------------------------------------------------
int QuickLook::text_line_h() const { return font_height(F.mono) + S(3); }

int QuickLook::visual_lines() const { return wrap_ ? (int)wrapped_.size() : (int)lines_.size(); }

static int char_w(HDC dc) {
    static int dpi = 0, w = 0;
    if (dpi != g_dpi || !w) { dpi = g_dpi; w = std::max(1, text_width(dc, F.mono, L"MMMMMMMMMM") / 10); }
    return w;
}

void QuickLook::rewrap(HDC dc) {
    int gutter = code_ ? S(56) : S(8);
    int avail = std::max(S(80), (int)(body_.right - body_.left) - gutter - S(16));
    if (avail == wrap_width_) return;
    wrap_width_ = avail;
    wrapped_.clear();
    int per = std::max(10, avail / char_w(dc));
    for (size_t i = 0; i < lines_.size(); i++) {
        const wstr& l = lines_[i];
        if ((int)l.size() <= per) { wrapped_.push_back({(int)i, 0}); continue; }
        int pos = 0;
        while (pos < (int)l.size()) {
            wrapped_.push_back({(int)i, pos});
            int end = std::min((int)l.size(), pos + per);
            if (end < (int)l.size()) {  // break at a space when there is one
                int sp = (int)l.rfind(L' ', end);
                if (sp > pos + per / 2) end = sp + 1;
            }
            pos = end;
        }
    }
}

static COLORREF tok_color(TokKind k) {
    switch (k) {
        case T_KEYWORD: return T.keyword;
        case T_STRING: return T.string;
        case T_COMMENT: return T.comment;
        case T_NUMBER: return T.number;
        case T_KEY: return T.key;
        case T_TAG: return T.tag;
        case T_HEADING: return T.heading;
        case T_BOLD: return T.bold;
        case T_CODE: return T.code;
    }
    return T.fg;
}

void QuickLook::on_size(int w, int h) {
    body_ = RECT{S(14), S(HEAD_H) + S(INFO_H), w - S(14), h - (embedded ? S(12) : S(HINT_H))};
    int bw = S(34);
    close_btn_ = RECT{w - S(12) - bw, (S(HEAD_H) - bw) / 2, w - S(12), (S(HEAD_H) + bw) / 2};
    open_btn_ = RECT{close_btn_.left - S(8) - S(72), (S(HEAD_H) - S(30)) / 2, close_btn_.left - S(8), (S(HEAD_H) + S(30)) / 2};
    if (list_.hwnd) list_.move(body_.left, body_.top, body_.right - body_.left, body_.bottom - body_.top);
    sb_.track = RECT{body_.right - S(9), body_.top + S(2), body_.right - S(2), body_.bottom - S(2)};
    wrap_width_ = -1;
    invalidate();
}

void QuickLook::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.bg);
    // ---- head
    RECT head{0, 0, rc.right, S(HEAD_H)};
    fill(dc, head, T.surface);
    hline(dc, 0, rc.right, head.bottom - 1, T.border);
    int x = S(16);
    if (entry) {
        int is = S(32), y = (S(HEAD_H) - is) / 2;
        if (entry->dir && !entry->drive) folder_bitmap(app->folder_color(entry->path), is)->draw(dc, x, y);
        else draw_sys_icon(dc, app->icon_index(*entry, [this] { invalidate(); }), x, y, is);
        x += is + S(12);
        text(dc, entry->name, RECT{x, 0, open_btn_.left - S(12), S(HEAD_H)}, T.fg, F.title);
    }
    if (entry) {
        fill_round(dc, open_btn_, S(6), hot_ == 1 ? T.accent : T.select);
        text(dc, L"Open", open_btn_, hot_ == 1 ? T.on_accent : T.fg, F.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (hot_ == 2) fill_round(dc, close_btn_, S(6), mix(T.surface, T.bad, 0.3));
    draw_icon(dc, G_CLOSE, close_btn_, hot_ == 2 ? T.fg : T.muted, 14);
    // ---- info line
    text(dc, info_, RECT{S(16), S(HEAD_H), rc.right - S(16), S(HEAD_H) + S(INFO_H)}, T.muted, F.small);
    // ---- hint
    if (!embedded) text(dc, L"Space / Esc close   ·   ↑ ↓ ← → next item   ·   Enter opens" +
                 wstr(mode_ == M_TEXT ? L"   ·   W wraps lines   ·   Ctrl+C copies" : L""),
         RECT{0, rc.bottom - S(HINT_H), rc.right, rc.bottom}, T.faint, F.small, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT b = body_;
    if (b.right <= b.left || b.bottom <= b.top) return;
    switch (mode_) {
        case M_PICTURE: {
            if (!picture_) {
                if (entry) {  // a placeholder until the picture arrives
                    int n = S(96);
                    draw_sys_icon(dc, app->icon_index(*entry, [this] { invalidate(); }), (b.left + b.right - n) / 2, (b.top + b.bottom - n) / 2, n);
                }
                break;
            }
            int bw = b.right - b.left, bh = b.bottom - b.top;
            double k = std::min({(double)bw / picture_->w, (double)bh / picture_->h, picture_is_icon_ ? 1.0 : 1.0});
            if (picture_is_icon_) k = std::min(k, (double)S(256) / std::max(1, picture_->w));
            int w = std::max(1, (int)(picture_->w * k)), h = std::max(1, (int)(picture_->h * k));
            int px = b.left + (bw - w) / 2, py = b.top + (bh - h) / 2;
            if (!picture_is_icon_) {  // a soft frame behind photos with see-through parts
                RECT fr{px - 1, py - 1, px + w + 1, py + h + 1};
                frame_round(dc, fr, S(2), T.border);
            }
            picture_->draw(dc, px, py, w, h);
            if (!message_.empty()) text(dc, message_, RECT{b.left, py + h + S(12), b.right, py + h + S(40)}, T.muted, F.ui, DT_CENTER | DT_TOP | DT_SINGLELINE);
            break;
        }
        case M_FOLDER: {
            int n = std::min(S(192), (int)(b.bottom - b.top) * 2 / 3);
            int cy = b.top + (b.bottom - b.top - n - S(50)) / 2;
            if (entry) folder_bitmap(app->folder_color(entry->path), n)->draw(dc, (b.left + b.right - n) / 2, cy);
            text(dc, folder_info_, RECT{b.left, cy + n + S(12), b.right, cy + n + S(44)}, T.fg, F.header, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        case M_ARCHIVE:
            if (!message_.empty()) text(dc, message_, b, T.muted, F.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        case M_MESSAGE:
            if (!message_.empty()) {
                RECT r = b;
                text(dc, message_, r, T.muted, F.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            break;
        case M_TEXT: {
            fill_round(dc, b, S(8), T.surface);
            rewrap(dc);
            int lh = text_line_h();
            int gutter = code_ ? S(56) : S(8);
            int vis = visual_lines();
            int page = b.bottom - b.top - S(16);
            sb_.total = vis * lh + (cut_ ? lh * 2 : 0);
            sb_.page = page;
            vscroll_ = std::max(0, std::min(vscroll_, std::max(0, sb_.total - page)));
            sb_.pos = vscroll_;
            HRGN clip = CreateRectRgn(b.left, b.top + S(8), b.right - S(10), b.bottom - S(8));
            SelectClipRgn(dc, clip);
            HGDIOBJ old = SelectObject(dc, F.mono);
            SetBkMode(dc, TRANSPARENT);
            int first = vscroll_ / lh;
            int y = b.top + S(8) - vscroll_ % lh;
            int x0 = b.left + gutter - (wrap_ ? 0 : hscroll_);
            for (int v = first; v < vis && y < b.bottom; v++, y += lh) {
                int li = wrap_ ? wrapped_[v].first : v;
                int start = wrap_ ? wrapped_[v].second : 0;
                int end = wrap_ ? (v + 1 < vis && wrapped_[v + 1].first == li ? wrapped_[v + 1].second : (int)lines_[li].size())
                                : (int)lines_[li].size();
                const wstr& line = lines_[li];
                if (code_ && start == 0) {
                    SetTextColor(dc, T.faint);
                    wstr num = std::to_wstring(li + 1);
                    RECT nr{b.left, y, b.left + gutter - S(14), y + lh};
                    DrawTextW(dc, num.c_str(), (int)num.size(), &nr, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                }
                // plain first, then the coloured spans on top of it
                int col = start;
                auto draw_part = [&](int a, int z, COLORREF c) {
                    a = std::max(a, start);
                    z = std::min(z, end);
                    if (z <= a) return;
                    SetTextColor(dc, c);
                    SIZE ext{0, 0};  // measured: right even when the font isn't fixed-width
                    if (a > start) GetTextExtentPoint32W(dc, line.c_str() + start, a - start, &ext);
                    TextOutW(dc, x0 + ext.cx, y, line.c_str() + a, z - a);
                };
                for (auto& sp : line_spans_[li]) {
                    if (sp.start + sp.len <= start || sp.start >= end) continue;
                    draw_part(col, sp.start, T.fg);
                    draw_part(sp.start, sp.start + sp.len, tok_color(sp.kind));
                    col = std::max(col, sp.start + sp.len);
                }
                draw_part(col, end, T.fg);
            }
            if (cut_ && y < b.bottom) {
                SetTextColor(dc, T.faint);
                TextOutW(dc, b.left + gutter, y + lh / 2, L"… (only the first 512 KB is shown)", 35);
            }
            SelectObject(dc, old);
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
            sb_.paint(dc);
            break;
        }
        default: break;
    }
}

bool QuickLook::on_key(UINT vk, bool ctrl, bool shift, bool alt) {
    if (ctrl && vk == 'C' && mode_ == M_TEXT) { copy_text(); return true; }
    if (ctrl || alt) return false;
    int lh = text_line_h();
    switch (vk) {
        case VK_SPACE:
        case VK_ESCAPE: close_ql(); return true;
        case VK_RETURN: {
            EntryPtr e = entry;
            QLSource* s = source;
            close_ql();
            if (e && s) s->ql_open(e);
            return true;
        }
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
            if (source) { source->ql_step(vk); selection_changed(); }
            return true;
        case VK_PRIOR: vscroll_ -= sb_.page - lh; invalidate(); return true;
        case VK_NEXT: vscroll_ += sb_.page - lh; invalidate(); return true;
        case VK_HOME: vscroll_ = 0; hscroll_ = 0; invalidate(); return true;
        case VK_END: vscroll_ = 1 << 28; invalidate(); return true;
        case 'W':
            if (mode_ == M_TEXT) { wrap_ = !wrap_; wrap_width_ = -1; vscroll_ = hscroll_ = 0; invalidate(); }
            return true;
    }
    return false;
}

void QuickLook::copy_text() {
    wstr t = text_;
    t = replace_all(t, L"\n", L"\r\n");
    clipboard_set_text(hwnd, t);
}

bool QuickLook::on_wheel(int delta, WPARAM keys, int x, int y) {
    if (mode_ == M_ARCHIVE && list_.visible()) return list_.on_wheel(delta, keys, x, y);
    if (mode_ != M_TEXT) return true;
    int lh = text_line_h();
    if ((keys & MK_SHIFT) && !wrap_) hscroll_ = std::max(0, hscroll_ - delta * lh * 4 / 120);
    else vscroll_ -= delta * lh * 3 / 120;
    invalidate();
    return true;
}

void QuickLook::on_lbutton_down(int x, int y, WPARAM) {
    if (!embedded) SetFocus(hwnd);
    if (entry && PtInRect(&open_btn_, POINT{x, y})) {
        if (embedded) { if (source) source->ql_open(entry); }
        else on_key(VK_RETURN, false, false, false);
        return;
    }
    if (PtInRect(&close_btn_, POINT{x, y})) { if (embedded) app->toggle_preview(); else close_ql(); return; }
    if (mode_ == M_TEXT && sb_.press(x, y)) {
        vscroll_ = sb_.pos;
        if (sb_.dragging) { sb_drag_ = true; SetCapture(hwnd); }
        invalidate();
    }
}

void QuickLook::on_mouse_move(int x, int y, WPARAM) {
    if (sb_drag_) { if (sb_.drag_to(y)) { vscroll_ = sb_.pos; invalidate(); } return; }
    int h = PtInRect(&open_btn_, POINT{x, y}) ? 1 : PtInRect(&close_btn_, POINT{x, y}) ? 2 : 0;
    bool sh = mode_ == M_TEXT && sb_.hit(x, y);
    if (h != hot_ || sh != sb_.hot) { hot_ = h; sb_.hot = sh; invalidate(); }
}

void QuickLook::on_lbutton_up(int, int, WPARAM) {
    if (sb_drag_) { sb_drag_ = false; sb_.dragging = false; ReleaseCapture(); invalidate(); }
}

LRESULT QuickLook::on_message(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_GETMINMAXINFO: {
            auto mm = (MINMAXINFO*)l;
            mm->ptMinTrackSize = POINT{S(360), S(280)};
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hot_ || sb_.hot) { hot_ = 0; sb_.hot = false; invalidate(); }
            break;
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE) SetFocus(hwnd);
            return 0;
        case WM_DPICHANGED: {
            RECT* r = (RECT*)l;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
    }
    return Control::on_message(m, w, l);
}
