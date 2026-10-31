#include "fileview.h"
#include <commctrl.h>
#include <algorithm>
#include <cwctype>
#include <set>

enum { TIMER_BAND = 2 };

FileView::~FileView() {
    if (drop_) { drop_->revoke(); drop_->Release(); }
}

bool FileView::create_view(HWND parent, FileViewHost* h, ViewMode m) {
    host = h;
    mode = m;
    if (!create(parent, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN)) return false;
    drop_ = new DropTarget(hwnd);
    drop_->over = [this](POINT pt, DWORD keys, DWORD allowed, const std::vector<wstr>& paths) -> DWORD {
        int i = hit(pt.x, pt.y);
        wstr dest;
        int hl = -1;
        if (i >= 0 && items[i]->dir) {
            bool dragged = false;
            for (auto& p : paths) if (norm(p) == norm(items[i]->path)) dragged = true;
            if (!dragged) { dest = items[i]->path; hl = i; }
        }
        if (dest.empty()) dest = host->fv_folder(this);
        DWORD e = DROPEFFECT_NONE;
        if (!dest.empty()) {
            e = choose_effect(paths, dest, keys, allowed);
            if (!host->fv_can_drop(this, dest, paths, e)) { e = DROPEFFECT_NONE; hl = -1; }
        }
        if (hl != drop_hl) { drop_hl = hl; invalidate(); }
        int hh = header_h();
        if (pt.y < hh + S(24)) scroll_by(-S(16));
        else if (pt.y > height() - S(24)) scroll_by(S(16));
        return e;
    };
    drop_->leave = [this] { if (drop_hl != -1) { drop_hl = -1; invalidate(); } };
    drop_->drop = [this](POINT pt, DWORD e, const std::vector<wstr>& paths) {
        int i = hit(pt.x, pt.y);
        wstr dest;
        if (i >= 0 && items[i]->dir) {
            bool dragged = false;
            for (auto& p : paths) if (norm(p) == norm(items[i]->path)) dragged = true;
            if (!dragged) dest = items[i]->path;
        }
        if (dest.empty()) dest = host->fv_folder(this);
        if (!dest.empty()) host->fv_drop(this, dest, paths, e);
    };
    return true;
}

// ======================================================================
// geometry
// ======================================================================
int FileView::header_h() const { return mode == VM_LIST && show_header ? S(34) : 0; }
int FileView::row_h() const { return mode == VM_COLUMN ? S(28) : S(30); }
int FileView::icon_px() const { return S(16); }
int FileView::line_h() const { return font_height(F.small); }

void FileView::tile(int& tw, int& th) const {
    tw = std::max(S(grid_size) + S(40), S(104));
    th = S(grid_size) + S(22) + 2 * line_h();
}

int FileView::view_height() const { return std::max(1, height() - header_h()); }

int FileView::content_height() const {
    int n = (int)items.size();
    if (mode == VM_GRID) {
        int tw, th;
        tile(tw, th);
        return ((n + cols_ - 1) / cols_) * th + S(16);
    }
    return n * row_h() + S(8);
}

void FileView::layout() {
    if (mode == VM_GRID) {
        int tw, th;
        tile(tw, th);
        cols_ = std::max(1, (width() - S(16) - S(10)) / tw);
    } else cols_ = 1;
    update_scrollbar();
}

void FileView::update_scrollbar() {
    sb_.total = content_height();
    sb_.page = view_height();
    scroll_ = std::max(0, std::min(scroll_, std::max(0, sb_.total - sb_.page)));
    sb_.pos = scroll_;
    sb_.track = RECT{width() - S(9), header_h() + S(2), width() - S(2), height() - S(2)};
}

void FileView::item_rect(int i, RECT& r) const {
    if (mode == VM_GRID) {
        int tw, th;
        tile(tw, th);
        int left = std::max(S(8), (width() - S(10) - cols_ * tw) / 2);
        int row = i / cols_, col = i % cols_;
        r = RECT{left + col * tw, S(8) + row * th - scroll_, left + (col + 1) * tw, S(8) + (row + 1) * th - scroll_};
    } else {
        int y = header_h() + i * row_h() - scroll_;
        r = RECT{0, y, width() - S(10), y + row_h()};
    }
}

std::vector<int> FileView::col_x() const {
    std::vector<int> xs;
    int avail = width() - S(10);
    if (mode != VM_LIST || columns.empty()) return {0, avail};
    int others = 0;
    for (size_t i = 1; i < columns.size(); i++) others += S(columns[i].width);
    int name_w = std::max(S(columns[0].width), avail - others);
    int x = 0;
    xs.push_back(x);
    x += name_w;
    for (size_t i = 1; i < columns.size(); i++) { xs.push_back(x); x += S(columns[i].width); }
    xs.push_back(x);
    return xs;
}

int FileView::header_hit_resize(int x) const {
    if (mode != VM_LIST || columns.size() < 2) return -1;
    auto xs = col_x();
    for (size_t k = 1; k + 1 < xs.size(); k++)
        if (std::abs(x - xs[k + 1]) <= S(4)) return (int)k;  // right edge of column k
    if (std::abs(x - xs[1]) <= S(4)) return 0;               // between name and the next
    return -1;
}

std::string FileView::header_hit(int x) const {
    auto xs = col_x();
    for (size_t k = 0; k < columns.size() && k + 1 < xs.size(); k++)
        if (x >= xs[k] && x < xs[k + 1]) return columns[k].key;
    return "";
}

int FileView::hit(int x, int y, bool* on_name) const {
    if (on_name) *on_name = false;
    int n = (int)items.size();
    if (mode == VM_GRID) {
        int tw, th;
        tile(tw, th);
        int left = std::max(S(8), (width() - S(10) - cols_ * tw) / 2);
        int c = (x - left) / tw, r = (y + scroll_ - S(8)) / th;
        if (x < left || c >= cols_ || y + scroll_ < S(8)) return -1;
        int i = r * cols_ + c;
        if (i < 0 || i >= n) return -1;
        RECT ir;
        item_rect(i, ir);
        if (x < ir.left + S(4) || x > ir.right - S(4) || y > ir.bottom - S(4)) return -1;
        if (on_name) *on_name = true;
        return i;
    }
    if (y < header_h()) return -1;
    int i = (y - header_h() + scroll_) / row_h();
    if (i < 0 || i >= n || x > width() - S(10)) return -1;
    if (on_name) {
        auto xs = col_x();
        HDC dc = GetDC(hwnd);
        int end = xs[0] + S(14) + icon_px() + S(10) + text_width(dc, F.ui, items[i]->name) + S(8);
        ReleaseDC(hwnd, dc);
        *on_name = x >= xs[0] && x <= std::min(end, xs[1]);
    }
    return i;
}

RECT FileView::name_rect(int i) {
    RECT r;
    item_rect(i, r);
    if (mode == VM_GRID) {
        return RECT{r.left + S(4), r.top + S(grid_size) + S(10), r.right - S(4), r.top + S(grid_size) + S(12) + line_h() + S(8)};
    }
    auto xs = col_x();
    int x = xs[0] + S(14) + icon_px() + S(6);
    return RECT{x, r.top + S(3), std::max(x + S(160), xs[1] - S(8)), r.bottom - S(3)};
}

double FileView::scroll_fraction() const { return sb_.total > sb_.page ? (double)scroll_ / (sb_.total - sb_.page) : 0; }

void FileView::set_scroll_fraction(double f) {
    update_scrollbar();
    scroll_ = (int)(f * std::max(0, sb_.total - sb_.page));
    update_scrollbar();
    invalidate();
}

void FileView::on_size(int, int) {
    layout();
    if (edit_) cancel_rename();
    invalidate();
}

// ======================================================================
// contents & selection
// ======================================================================
void FileView::set_items(Entries e, bool keep_selection, bool keep_scroll) {
    std::set<wstr> keep;
    wstr focus_path;
    if (keep_selection) {
        for (size_t i = 0; i < items.size() && i < sel.size(); i++) if (sel[i]) keep.insert(items[i]->path);
        if (focus >= 0 && focus < (int)items.size()) focus_path = items[focus]->path;
    }
    if (edit_) cancel_rename();
    items = std::move(e);
    sel.assign(items.size(), 0);
    focus = -1;
    for (size_t i = 0; i < items.size(); i++) {
        if (keep.count(items[i]->path)) sel[i] = 1;
        if (!focus_path.empty() && items[i]->path == focus_path) focus = (int)i;
    }
    if (anchor >= (int)items.size()) anchor = -1;
    hover = drop_hl = -1;
    if (!keep_scroll) scroll_ = 0;
    layout();
    invalidate();
}

std::vector<int> FileView::selected() const {
    std::vector<int> out;
    for (size_t i = 0; i < sel.size(); i++) if (sel[i]) out.push_back((int)i);
    return out;
}

std::vector<EntryPtr> FileView::selected_entries() const {
    std::vector<EntryPtr> out;
    for (size_t i = 0; i < sel.size() && i < items.size(); i++) if (sel[i]) out.push_back(items[i]);
    return out;
}

int FileView::count_selected() const { return (int)std::count(sel.begin(), sel.end(), 1); }

void FileView::notify() {
    invalidate();
    if (host) host->fv_selection(this);
}

void FileView::select_only(int i, bool n) {
    std::fill(sel.begin(), sel.end(), 0);
    if (i >= 0 && i < (int)sel.size()) { sel[i] = 1; focus = anchor = i; }
    if (n) notify(); else invalidate();
}

void FileView::select_paths(const std::vector<wstr>& paths, bool by_name) {
    std::set<wstr> want;
    for (auto& p : paths) want.insert(lower(p));
    std::fill(sel.begin(), sel.end(), 0);
    int first = -1;
    for (size_t i = 0; i < items.size(); i++)
        if (want.count(lower(by_name ? items[i]->name : items[i]->path))) {
            sel[i] = 1;
            if (first < 0) first = (int)i;
        }
    if (first >= 0) { focus = anchor = first; ensure_visible(first); }
    notify();
}

void FileView::select_all() {
    std::fill(sel.begin(), sel.end(), 1);
    notify();
}

void FileView::clear_selection() {
    if (std::find(sel.begin(), sel.end(), 1) == sel.end()) return;
    std::fill(sel.begin(), sel.end(), 0);
    notify();
}

void FileView::set_focus_index(int i) {
    focus = i;
    invalidate();
}

int FileView::index_of(const wstr& path) const {
    wstr n = norm(path);
    for (size_t i = 0; i < items.size(); i++) if (norm(items[i]->path) == n) return (int)i;
    return -1;
}

void FileView::ensure_visible(int i) {
    if (i < 0 || i >= (int)items.size()) return;
    update_scrollbar();
    RECT r;
    item_rect(i, r);
    int top = header_h(), bottom = height();
    if (r.top < top) scroll_ -= top - r.top + (mode == VM_GRID ? S(8) : 0);
    else if (r.bottom > bottom) scroll_ += r.bottom - bottom + (mode == VM_GRID ? S(8) : 0);
    update_scrollbar();
    invalidate();
}

void FileView::scroll_by(int dy) {
    int old = scroll_;
    scroll_ += dy;
    update_scrollbar();
    if (scroll_ != old) invalidate();
}

void FileView::click_select(int i, WPARAM keys) {
    if (keys & MK_CONTROL) {
        sel[i] = !sel[i];
        anchor = i;
    } else if ((keys & MK_SHIFT) && anchor >= 0 && anchor < (int)items.size()) {
        std::fill(sel.begin(), sel.end(), 0);
        for (int k = std::min(anchor, i); k <= std::max(anchor, i); k++) sel[k] = 1;
    } else {
        std::fill(sel.begin(), sel.end(), 0);
        sel[i] = 1;
        anchor = i;
    }
    focus = i;
    notify();
}

void FileView::step(UINT vk, bool shift) {
    int n = (int)items.size();
    if (!n) return;
    int cur = focus >= 0 && focus < n ? focus : -1;
    int rows_vis = std::max(1, view_height() / (mode == VM_GRID ? ([this] { int tw, th; tile(tw, th); return th; })() : row_h()));
    int j = cur;
    int stepv = mode == VM_GRID ? cols_ : 1;
    switch (vk) {
        case VK_UP: j = cur < 0 ? 0 : cur - stepv; break;
        case VK_DOWN: j = cur < 0 ? 0 : cur + stepv; break;
        case VK_LEFT: if (mode != VM_GRID) return; j = cur < 0 ? 0 : cur - 1; break;
        case VK_RIGHT: if (mode != VM_GRID) return; j = cur < 0 ? 0 : cur + 1; break;
        case VK_HOME: j = 0; break;
        case VK_END: j = n - 1; break;
        case VK_PRIOR: j = cur - rows_vis * stepv; break;
        case VK_NEXT: j = cur + rows_vis * stepv; break;
        default: return;
    }
    if (mode == VM_GRID && (vk == VK_UP || vk == VK_DOWN) && (j < 0 || j >= n)) j = cur;  // stay in the column
    j = std::max(0, std::min(n - 1, j));
    if (shift) {
        if (anchor < 0) anchor = cur < 0 ? j : cur;
        std::fill(sel.begin(), sel.end(), 0);
        for (int k = std::min(anchor, j); k <= std::max(anchor, j); k++) sel[k] = 1;
    } else {
        std::fill(sel.begin(), sel.end(), 0);
        sel[j] = 1;
        anchor = j;
    }
    focus = j;
    ensure_visible(j);
    notify();
}

// ======================================================================
// painting
// ======================================================================
std::vector<wstr> FileView::wrap(HDC dc, const wstr& name, int width) const {
    std::vector<wstr> lines;
    wstr rest = name;
    HGDIOBJ old = SelectObject(dc, F.small);
    auto w = [&](const wstr& s) { SIZE sz; GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz); return (int)sz.cx; };
    while (!rest.empty() && lines.size() < 2) {
        if (w(rest) <= width) { lines.push_back(rest); rest.clear(); break; }
        int cut = (int)rest.size();
        while (cut > 1 && w(rest.substr(0, cut)) > width) cut--;
        if (lines.size() == 1) {
            while (cut > 1 && w(rest.substr(0, cut) + L"…") > width) cut--;
            lines.push_back(rest.substr(0, cut) + L"…");
            rest.clear();
            break;
        }
        int sp = (int)rest.rfind(L' ', cut);
        if (sp > cut / 2) cut = sp + 1;
        else {
            int soft = -1;
            for (wchar_t ch : wstr(L"._-(")) soft = std::max(soft, (int)rest.rfind(ch, cut - 1));
            if (soft > cut / 3) cut = soft + ((rest[soft] == L'.' || rest[soft] == L'(') ? 0 : 1);
        }
        if (cut <= 0) cut = 1;
        wstr line = rest.substr(0, cut);
        while (!line.empty() && line.back() == L' ') line.pop_back();
        lines.push_back(line);
        rest = rest.substr(cut);
    }
    SelectObject(dc, old);
    return lines;
}

void FileView::paint_dots(HDC dc, const RowInfo& ri, int x, int y, int size) {
    int d = std::max(S(7), (int)(size * 0.5)), gap = std::max(S(3), d / 2);
    int n = std::min<int>(3, (int)ri.tags.size());
    for (int k = n - 1; k >= 0; k--) {
        float cx = x + size - d / 2.0f - (n - 1 - k) * gap, cy = y + size - d / 2.0f;
        fill_circle(dc, cx, cy, d / 2.0f, ri.tags[k], T.surface, std::max(1.0f, S(1.5) * 1.0f));
    }
}

void FileView::paint_icon(HDC dc, Entry& e, const RowInfo& ri, int x, int y, int size) {
    if (e.dir && !e.drive) {
        folder_bitmap(ri.folder_color, size)->draw(dc, x, y);
        return;
    }
    if (mode == VM_GRID && !e.drive && (THUMB_EXTS.count(e.ext) || OWN_ICON_EXTS.count(e.ext))) {
        BitmapPtr b = host->fv_thumb(this, e, size);
        if (b && b->w > 0 && b->h > 0) {
            double k = std::min((double)size / b->w, (double)size / b->h);
            int w = std::max(1, (int)(b->w * k)), h = std::max(1, (int)(b->h * k));
            b->draw(dc, x + (size - w) / 2, y + (size - h) / 2, w, h);
            return;
        }
    }
    int idx = host->fv_icon(this, e);
    if (idx >= 0) draw_sys_icon(dc, idx, x, y, size);
    else {  // a plain page while Windows works out the icon
        RECT r{x + size / 5, y + size / 10, x + size * 4 / 5, y + size * 9 / 10};
        fill_round(dc, r, std::max(2, size / 10), mix(T.surface, T.fg, 0.75));
    }
}

void FileView::on_paint(HDC dc, const RECT& rc) {
    fill(dc, rc, T.surface);
    if (mode == VM_GRID) paint_grid(dc, rc);
    else paint_list(dc, rc);
    if (items.empty() && host) {
        RECT r{rc.left, header_h() + S(40), rc.right, header_h() + S(80)};
        text(dc, host->fv_empty(this), r, T.faint, F.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (drag_ == D_BAND && band_moved_) {
        RECT b{std::min(band0_.x, band1_.x), std::min(band0_.y, band1_.y) - scroll_, std::max(band0_.x, band1_.x),
               std::max(band0_.y, band1_.y) - scroll_};
        b.top = std::max(b.top, (LONG)header_h());
        fill_alpha(dc, b, T.accent, 36);
        HBRUSH br = CreateSolidBrush(T.accent);
        FrameRect(dc, &b, br);
        DeleteObject(br);
    }
    sb_.dragging = drag_ == D_SCROLL;
    sb_.paint(dc);
    if (edit_) {
        RECT nr = name_rect(edit_item_);
        frame_round(dc, nr, S(5), T.accent, 1.5f);
    }
}

void FileView::paint_list(HDC dc, const RECT& rc) {
    int hh = header_h(), rh = row_h(), isz = icon_px();
    auto xs = col_x();
    bool has_focus = GetFocus() == hwnd;
    HRGN clip = CreateRectRgn(0, hh, rc.right, rc.bottom);
    SelectClipRgn(dc, clip);
    int first = std::max(0, scroll_ / rh), n = (int)items.size();
    for (int i = first; i < n; i++) {
        int y = hh + i * rh - scroll_;
        if (y > rc.bottom) break;
        Entry& e = *items[i];
        RowInfo ri;
        host->fv_row(this, e, ri);
        RECT row{S(6), y + S(1), xs.back() > 0 ? std::min((int)rc.right - S(12), xs.back()) : rc.right - S(12), y + rh - S(1)};
        if (mode == VM_COLUMN) row.right = rc.right - S(12);
        COLORREF bg = sel[i] ? T.select : i == drop_hl ? T.drop : i == hover ? T.hover : CLR_INVALID;
        if (i == drop_hl) bg = T.drop;
        if (bg != CLR_INVALID) fill_round(dc, row, S(6), bg);
        if (sel[i] && i == focus && has_focus) {
            RECT pill{row.left + S(1), y + S(8), row.left + S(4), y + rh - S(8)};
            fill_round(dc, pill, S(2), T.accent);
        }
        int x = xs[0] + S(14);
        paint_icon(dc, e, ri, x, y + (rh - isz) / 2, isz);
        paint_dots(dc, ri, x, y + (rh - isz) / 2, isz);
        COLORREF fg = ri.cut ? T.faint : e.hidden ? T.muted : T.fg;
        int name_end = (mode == VM_COLUMN ? rc.right - S(34) : xs[1] - S(8));
        RECT tr{x + isz + S(10), y, name_end, y + rh};
        if (!(edit_ && edit_item_ == i)) text(dc, e.name, tr, fg, F.ui);
        if (mode == VM_COLUMN) {
            if (e.dir) {
                RECT cr{rc.right - S(36), y, rc.right - S(12), y + rh};
                draw_icon(dc, G_CHEVRON, cr, sel[i] ? T.fg : T.faint, 12);
            }
            continue;
        }
        for (size_t k = 1; k < columns.size() && k + 1 < xs.size(); k++) {
            const Column& c = columns[k];
            wstr v;
            if (c.key == "modified") v = fmt_time(e.mtime);
            else if (c.key == "type") v = e.drive ? (e.total > 0 ? fmt_size(e.free) + L" free of " + fmt_size(e.total) : L"Drive") : ri.type;
            else if (c.key == "size") v = e.drive ? L"" : e.size >= 0 ? fmt_size(e.size) : ri.pending_size ? L"…" : L"";
            else if (c.key == "tags") v = ri.tags_text;
            else if (c.key == "location") v = ri.location;
            RECT cr{xs[k] + S(10), y, xs[k + 1] - S(10), y + rh};
            text(dc, v, cr, sel[i] ? T.fg : T.muted, F.ui,
                 (c.right ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    if (hh) {
        RECT h{0, 0, rc.right, hh};
        fill(dc, h, T.surface);
        for (size_t k = 0; k < columns.size() && k + 1 < xs.size(); k++) {
            const Column& c = columns[k];
            wstr t = c.title;
            bool sorted = c.key == sort_key;
            RECT cr{xs[k] + (k == 0 ? S(14) : S(10)), 0, xs[k + 1] - S(10), hh};
            COLORREF col = sorted ? T.fg : T.head;
            if (c.right) {
                text(dc, t, cr, col, F.header, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                text(dc, t, cr, col, F.header);
            }
            if (sorted) {
                HDC tdc = dc;
                int tw = text_width(tdc, F.header, t);
                int ax = c.right ? cr.right - tw - S(14) : cr.left + tw + S(6);
                // a small triangle (drawn, so it never depends on the font having ▴ ▾)
                int cx = ax + S(4), cy = hh / 2, a = S(4);
                POINT tri[3];
                if (sort_desc) { tri[0] = {cx - a, cy - a / 2}; tri[1] = {cx + a, cy - a / 2}; tri[2] = {cx, cy + a / 2 + 1}; }
                else { tri[0] = {cx - a, cy + a / 2}; tri[1] = {cx + a, cy + a / 2}; tri[2] = {cx, cy - a / 2 - 1}; }
                HBRUSH br = CreateSolidBrush(T.accent);
                HPEN pen = CreatePen(PS_SOLID, 1, T.accent);
                HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
                Polygon(dc, tri, 3);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(br);
                DeleteObject(pen);
            }
            if (k > 0) vline(dc, xs[k], S(10), hh - S(10), T.border);
        }
        hline(dc, S(6), rc.right - S(6), hh - 1, T.border);
    }
}

void FileView::paint_grid(HDC dc, const RECT& rc) {
    int tw, th;
    tile(tw, th);
    int size = S(grid_size), n = (int)items.size();
    int first_row = std::max(0, (scroll_ - S(8)) / th);
    bool has_focus = GetFocus() == hwnd;
    for (int i = first_row * cols_; i < n; i++) {
        RECT r;
        item_rect(i, r);
        if (r.top > rc.bottom) break;
        if (r.bottom < 0) continue;
        Entry& e = *items[i];
        RowInfo ri;
        host->fv_row(this, e, ri);
        RECT t{r.left + S(4), r.top + S(2), r.right - S(4), r.bottom - S(4)};
        if (sel[i]) {
            fill_round(dc, t, S(8), T.select);
            if (i == focus && has_focus) frame_round(dc, t, S(8), T.accent, 1.2f);
        } else if (i == drop_hl) {
            fill_round(dc, t, S(8), T.drop);
        } else if (i == hover) {
            fill_round(dc, t, S(8), T.hover);
        }
        int ix = r.left + (tw - size) / 2, iy = r.top + S(8);
        paint_icon(dc, e, ri, ix, iy, size);
        if (!ri.tags.empty()) {
            int d = std::max(S(9), size / 6);
            for (int k = std::min<int>(3, (int)ri.tags.size()) - 1; k >= 0; k--)
                fill_circle(dc, ix + size - d / 2.0f - k * d * 0.6f, iy + size - d / 2.0f, d / 2.0f, ri.tags[k], T.surface, 2);
        }
        if (edit_ && edit_item_ == i) continue;
        auto lines = wrap(dc, e.name, tw - S(12));
        int y = r.top + size + S(14);
        COLORREF fg = ri.cut ? T.faint : e.hidden ? T.muted : T.fg;
        for (auto& l : lines) {
            RECT lr{r.left + S(4), y, r.right - S(4), y + line_h()};
            text(dc, l, lr, fg, F.small, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            y += line_h();
        }
    }
}

// ======================================================================
// mouse
// ======================================================================
LRESULT FileView::on_message(UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
        POINT p = cursor();
        if ((p.y < header_h() && header_hit_resize(p.x) >= 0) || drag_ == D_HEADER) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
    }
    return Control::on_message(m, w, l);
}

void FileView::on_mouse_move(int x, int y, WPARAM keys) {
    switch (drag_) {
        case D_SCROLL:
            if (sb_.drag_to(y)) { scroll_ = sb_.pos; invalidate(); }
            return;
        case D_HEADER: {
            int nw = std::max(S(50), resize_w0_ + (resize_col_ == 0 ? -(x - resize_x0_) : (x - resize_x0_)));
            int k = resize_col_ == 0 ? 1 : resize_col_;
            columns[k].width = nw * 96 / g_dpi;
            invalidate();
            return;
        }
        case D_PRESS:
            if (press_item_ >= 0 && std::abs(x - press_.x) + std::abs(y - press_.y) > S(6)) {
                drag_ = D_NONE;
                ReleaseCapture();
                std::vector<wstr> paths;
                for (auto& e : selected_entries()) if (!e->drive) paths.push_back(e->path);
                if (!paths.empty()) start_drag(hwnd, paths);
            }
            return;
        case D_BAND:
            band1_ = POINT{x, y + scroll_};
            if (!band_moved_ && std::abs(x - press_.x) + std::abs(y - press_.y) > S(5)) {
                band_moved_ = true;
                SetTimer(hwnd, TIMER_BAND, 40, nullptr);
            }
            if (band_moved_) band_update();
            return;
        default: break;
    }
    bool hot = sb_.hit(x, y);
    if (hot != sb_.hot) { sb_.hot = hot; invalidate(); }
    int h = hit(x, y);
    if (h != hover) { hover = h; invalidate(); }
}

void FileView::on_mouse_leave() {
    if (hover != -1 || sb_.hot) { hover = -1; sb_.hot = false; invalidate(); }
}

void FileView::on_lbutton_down(int x, int y, WPARAM keys) {
    if (edit_) finish_rename(true);
    SetFocus(hwnd);
    if (sb_.press(x, y)) {
        scroll_ = sb_.pos;
        if (sb_.dragging) { drag_ = D_SCROLL; SetCapture(hwnd); }
        invalidate();
        return;
    }
    if (y < header_h()) {
        int rc = header_hit_resize(x);
        if (rc >= 0) {
            drag_ = D_HEADER;
            resize_col_ = rc;
            resize_x0_ = x;
            resize_w0_ = S(columns[rc == 0 ? 1 : rc].width);
            SetCapture(hwnd);
        } else {
            std::string key = header_hit(x);
            if (!key.empty() && host) host->fv_sort(this, key);
        }
        return;
    }
    bool on_name = false;
    int i = hit(x, y, &on_name);
    press_ = POINT{x, y};
    press_keys_ = keys;
    if (i >= 0 && on_name) {
        deferred_ = sel[i] && count_selected() > 1 && !(keys & (MK_CONTROL | MK_SHIFT));
        if (!deferred_) click_select(i, keys);
        drag_ = D_PRESS;
        press_item_ = i;
        SetCapture(hwnd);
        return;
    }
    // empty space or beside a name: a selection box (a plain click selects that row)
    drag_ = D_BAND;
    band_moved_ = false;
    band0_ = band1_ = POINT{x, y + scroll_};
    band_base_ = (keys & (MK_CONTROL | MK_SHIFT)) ? sel : std::vector<char>(sel.size(), 0);
    if (mode == VM_GRID && !(keys & (MK_CONTROL | MK_SHIFT)) && i < 0) clear_selection();
    press_item_ = i;
    SetCapture(hwnd);
}

void FileView::on_lbutton_up(int x, int y, WPARAM keys) {
    Drag d = drag_;
    drag_ = D_NONE;
    if (GetCapture() == hwnd) ReleaseCapture();
    KillTimer(hwnd, TIMER_BAND);
    if (d == D_PRESS) {
        if (deferred_ && press_item_ >= 0) select_only(press_item_);
        deferred_ = false;
        if (host && press_item_ >= 0) host->fv_clicked(this, press_item_, press_keys_);
    } else if (d == D_BAND && !band_moved_) {
        int i = hit(x, y);
        if (i >= 0) {
            click_select(i, press_keys_);
            if (host) host->fv_clicked(this, i, press_keys_);
        } else if (!(press_keys_ & (MK_CONTROL | MK_SHIFT))) clear_selection();
    }
    band_moved_ = false;
    invalidate();
}

void FileView::on_capture_lost() {
    if (drag_ == D_BAND || drag_ == D_SCROLL || drag_ == D_HEADER) {
        drag_ = D_NONE;
        band_moved_ = false;
        KillTimer(hwnd, TIMER_BAND);
        invalidate();
    }
}

void FileView::on_lbutton_dbl(int x, int y, WPARAM keys) {
    if (y < header_h()) return;
    int i = hit(x, y);
    if (i >= 0 && host) {
        select_only(i);
        host->fv_open(this, i);
    }
}

void FileView::on_rbutton_up(int x, int y, WPARAM keys) {
    SetFocus(hwnd);
    if (y < header_h()) return;
    int i = hit(x, y);
    if (i >= 0 && !sel[i]) select_only(i);
    if (i < 0) clear_selection();
    POINT p{x, y};
    ClientToScreen(hwnd, &p);
    if (host) host->fv_menu(this, i, p);
}

void FileView::on_mbutton_down(int x, int y, WPARAM) {
    int i = hit(x, y);
    if (i >= 0 && host) host->fv_middle(this, i);
}

bool FileView::on_wheel(int delta, WPARAM keys, int x, int y) {
    if ((keys & MK_CONTROL) && mode == VM_GRID) {
        if (host) host->fv_zoom(this, delta > 0 ? 1 : -1);
        return true;
    }
    int step = mode == VM_GRID ? ([this] { int tw, th; tile(tw, th); return th / 2; })() : row_h();
    scroll_by(-delta * 3 * step / 120);
    POINT p = cursor();
    int h = hit(p.x, p.y);
    if (h != hover) { hover = h; invalidate(); }
    return true;
}

void FileView::band_update() {
    RECT b{std::min(band0_.x, band1_.x), std::min(band0_.y, band1_.y), std::max(band0_.x, band1_.x),
           std::max(band0_.y, band1_.y)};
    std::vector<char> hits(items.size(), 0);
    int n = (int)items.size();
    if (mode == VM_GRID) {
        for (int i = 0; i < n; i++) {
            RECT r;
            item_rect(i, r);
            OffsetRect(&r, 0, scroll_);
            if (r.left + S(4) < b.right && r.right - S(4) > b.left && r.top + S(2) < b.bottom && r.bottom - S(4) > b.top) hits[i] = 1;
        }
    } else {
        int rh = row_h(), hh = header_h();
        int a = std::max(0, (int)(b.top - hh) / rh), z = std::min(n - 1, (int)(b.bottom - hh) / rh);
        if (b.bottom >= hh) for (int i = a; i <= z; i++) hits[i] = 1;
    }
    bool toggle = (press_keys_ & MK_CONTROL) != 0;
    std::vector<char> want(items.size(), 0);
    int last = -1;
    for (int i = 0; i < n; i++) {
        bool base = i < (int)band_base_.size() && band_base_[i];
        want[i] = toggle ? (base != (bool)hits[i]) : (base || hits[i]);
        if (hits[i]) last = i;
    }
    if (want != sel) {
        sel = want;
        if (last >= 0) focus = last;
        notify();
    } else invalidate();
}

void FileView::on_timer(UINT_PTR id) {
    if (id != TIMER_BAND || drag_ != D_BAND) return;
    POINT p = cursor();
    int dy = p.y < header_h() + S(4) ? -S(18) : p.y > height() - S(4) ? S(18) : 0;
    if (dy) {
        scroll_by(dy);
        band1_ = POINT{p.x, p.y + scroll_};
        band_update();
    }
}

// ======================================================================
// keyboard
// ======================================================================
bool FileView::on_key(UINT vk, bool ctrl, bool shift, bool alt) {
    if (edit_) return false;
    if (vk == VK_SPACE && !ctrl) {
        if (!typed_.empty() && GetTickCount() - typed_at_ < 1000) return false;  // a space inside a name you're typing
        if (host && host->fv_key(this, vk, ctrl, shift, alt)) { typed_at_ = 0; MSG m; PeekMessageW(&m, hwnd, WM_CHAR, WM_CHAR, PM_REMOVE); return true; }
        return false;
    }
    bool nav = vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
               ((vk == VK_LEFT || vk == VK_RIGHT) && mode == VM_GRID);
    if (nav && !alt) { step(vk, shift); return true; }
    if (vk == 'A' && ctrl && !shift && !alt) { select_all(); return true; }
    if (vk == VK_APPS || (vk == VK_F10 && shift)) {
        int i = focus >= 0 && focus < (int)items.size() && sel[focus] ? focus : -1;
        RECT r{0, 0, S(40), S(40)};
        if (i >= 0) item_rect(i, r);
        POINT p{r.left + S(40), r.top + S(10)};
        ClientToScreen(hwnd, &p);
        if (host) host->fv_menu(this, i, p);
        return true;
    }
    if (host && host->fv_key(this, vk, ctrl, shift, alt)) {
        MSG m;
        PeekMessageW(&m, hwnd, WM_CHAR, WM_CHAR, PM_REMOVE);
        return true;
    }
    return false;
}

void FileView::on_char(wchar_t c) {
    if (c < 32 || edit_) return;
    DWORD now = GetTickCount();
    if (now - typed_at_ > 1000) typed_.clear();
    typed_ += towlower(c);
    typed_at_ = now;
    for (size_t i = 0; i < items.size(); i++)
        if (starts_with(lower(items[i]->name), typed_)) {
            select_only((int)i);
            ensure_visible((int)i);
            break;
        }
}

void FileView::on_focus(bool got) {
    invalidate();
    if (got && host) host->fv_focus(this);
}

// ======================================================================
// rename in place
// ======================================================================
LRESULT CALLBACK fv_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    FileView* fv = (FileView*)ref;
    switch (m) {
        case WM_KEYDOWN:
            if (w == VK_RETURN) { fv->finish_rename(true); return 0; }
            if (w == VK_ESCAPE) { fv->finish_rename(false); return 0; }
            if (w == 'A' && GetKeyState(VK_CONTROL) < 0) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
            break;
        case WM_CHAR:
            if (w == L'\r' || w == 27 || w == 1) return 0;
            break;
        case WM_KILLFOCUS:
            post_ui([fv] { if (fv->renaming()) fv->finish_rename(true); });
            break;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_HASSETSEL | DLGC_WANTCHARS;
        case WM_NCDESTROY: RemoveWindowSubclass(h, fv_edit_proc, 2); break;
    }
    return DefSubclassProc(h, m, w, l);
}

void FileView::start_rename(int i) {
    if (i < 0 || i >= (int)items.size() || items[i]->drive) return;
    if (edit_) cancel_rename();
    ensure_visible(i);
    edit_item_ = i;
    RECT r = name_rect(i);
    int fh = font_height(mode == VM_GRID ? F.small : F.ui);
    edit_ = CreateWindowExW(0, L"EDIT", items[i]->name.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (mode == VM_GRID ? ES_CENTER : 0),
                            r.left + S(6), r.top + (r.bottom - r.top - fh) / 2, r.right - r.left - S(12), fh, hwnd,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit_, WM_SETFONT, (WPARAM)(mode == VM_GRID ? F.small : F.ui), TRUE);
    SetWindowSubclass(edit_, fv_edit_proc, 2, (DWORD_PTR)this);
    solid_edit(edit_);
    wstr stem, ext;
    if (items[i]->dir) stem = items[i]->name;
    else split_ext(items[i]->name, stem, ext);
    SendMessageW(edit_, EM_SETSEL, 0, (LPARAM)stem.size());
    SetFocus(edit_);
    invalidate();
}

void FileView::finish_rename(bool keep) {
    if (!edit_) return;
    int n = GetWindowTextLengthW(edit_);
    wstr s(n, L'\0');
    if (n) GetWindowTextW(edit_, &s[0], n + 1);
    HWND e = edit_;
    int i = edit_item_;
    edit_ = nullptr;
    edit_item_ = -1;
    DestroyWindow(e);
    SetFocus(hwnd);
    invalidate();
    s = trim(s);
    if (keep && i >= 0 && i < (int)items.size() && s != items[i]->name && host) host->fv_rename(this, i, s);
}

void FileView::cancel_rename() {
    if (!edit_) return;
    HWND e = edit_;
    edit_ = nullptr;
    edit_item_ = -1;
    DestroyWindow(e);
    invalidate();
}
