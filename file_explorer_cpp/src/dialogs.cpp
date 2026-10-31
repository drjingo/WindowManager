#include "dialogs.h"
#include <algorithm>

namespace {

// a row of rounded "chips": pick one (choice) or click one (action)
class Chips : public Control {
  public:
    std::vector<wstr> labels;
    int selected = -1;     // -1: action chips (nothing stays selected)
    std::function<void(int)> picked;
    void on_paint(HDC dc, const RECT& rc) override {
        fill(dc, rc, T.bg);
        rects_.clear();
        int x = 0;
        for (size_t i = 0; i < labels.size(); i++) {
            int w = text_width(dc, F.small, labels[i]) + S(20);
            RECT r{x, (rc.bottom - S(26)) / 2, x + w, (rc.bottom + S(26)) / 2};
            bool on = (int)i == selected, hot = (int)i == hot_;
            fill_round(dc, r, S(13), on ? T.accent : hot ? mix(T.field, T.accent, 0.18) : T.field);
            text(dc, labels[i], r, on ? T.on_accent : T.fg, F.small, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            rects_.push_back(r);
            x += w + S(6);
        }
    }
    int hit(int x, int y) {
        for (size_t i = 0; i < rects_.size(); i++) if (PtInRect(&rects_[i], POINT{x, y})) return (int)i;
        return -1;
    }
    void on_mouse_move(int x, int y, WPARAM) override {
        int h = hit(x, y);
        if (h != hot_) { hot_ = h; invalidate(); }
        SetCursor(LoadCursor(nullptr, h >= 0 ? IDC_HAND : IDC_ARROW));
    }
    void on_mouse_leave() override { hot_ = -1; invalidate(); }
    void on_lbutton_down(int x, int y, WPARAM) override {
        int h = hit(x, y);
        if (h < 0) return;
        if (selected >= 0) selected = h;
        invalidate();
        if (picked) picked(h);
    }

  private:
    std::vector<RECT> rects_;
    int hot_ = -1;
};

// colour dots for a tag
class Swatches : public Control {
  public:
    std::string color;
    void on_paint(HDC dc, const RECT& rc) override {
        fill(dc, rc, T.bg);
        int d = S(26), gap = S(10);
        rects_.clear();
        for (size_t i = 0; i < FOLDER_COLOR_NAMES.size(); i++) {
            int x = (int)i * (d + gap) + S(4);
            RECT r{x, (rc.bottom - d) / 2, x + d, (rc.bottom + d) / 2};
            rects_.push_back(r);
            float cx = r.left + d / 2.0f, cy = r.top + d / 2.0f;
            bool on = FOLDER_COLOR_NAMES[i] == color;
            if (on) fill_circle(dc, cx, cy, d / 2.0f + S(3), T.bg, T.fg, (float)S(2));
            fill_circle(dc, cx, cy, d / 2.0f - (on ? S(1) : 0), folder_rgb(FOLDER_COLOR_NAMES[i]));
        }
    }
    void on_lbutton_down(int x, int y, WPARAM) override {
        for (size_t i = 0; i < rects_.size(); i++)
            if (PtInRect(&rects_[i], POINT{x, y})) { color = FOLDER_COLOR_NAMES[i]; invalidate(); if (changed) changed(); }
    }
    std::function<void()> changed;

  private:
    std::vector<RECT> rects_;
};

struct TagDialog : Dialog {
    Label l_name, l_color;
    TextBox box;
    Swatches sw;
    Button ok, cancel;
};

struct BulkDialog : Dialog {
    App* app = nullptr;
    std::vector<Entry> entries;
    std::vector<wstr> names;
    std::map<int, wstr> problems;
    bool ready = false;
    Label l_tpl, l_help, l_find, l_repl, l_nums, l_start, l_step, l_digits, l_case, msg;
    TextBox tpl, find, repl, start, step, digits;
    Chips presets, cases;
    Check regex, match_case, keep_ext;
    SimpleList list;
    Button ok, cancel;

    void on_size(int w, int h) override { place(w, h); }
    LRESULT on_message(UINT m, WPARAM wp, LPARAM lp) override {
        if (m == WM_GETMINMAXINFO) { ((MINMAXINFO*)lp)->ptMinTrackSize = POINT{S(640), S(520)}; return 0; }
        if (m == WM_TIMER && wp == 7) { KillTimer(hwnd, 7); update(); return 0; }
        return Dialog::on_message(m, wp, lp);
    }
    void changed() { SetTimer(hwnd, 7, 120, nullptr); }

    void place(int w, int h) {
        if (!ok.hwnd) return;
        int L = S(18), lw = S(92), x = L + lw, right = w - L, fw = right - x, rh = S(34);
        int y = S(16);
        l_tpl.move(L, y, lw, rh);
        tpl.move(x, y, fw, rh);
        y += rh + S(2);
        l_help.move(x, y, fw, S(20));
        y += S(22);
        presets.move(x, y, fw, S(30));
        y += S(40);
        int half = (fw - S(100)) / 2;
        l_find.move(L, y, lw, rh);
        find.move(x, y, half, rh);
        l_repl.move(x + half, y, S(100), rh);
        repl.move(x + half + S(100), y, fw - half - S(100), rh);
        y += rh + S(6);
        int cx = x;
        for (Check* c : {&regex, &match_case, &keep_ext}) {
            int cw = c->ideal_width();
            c->move(cx, y, cw, S(26));
            cx += cw + S(18);
        }
        y += S(34);
        l_nums.move(L, y, lw, rh);
        cx = x;
        auto pair = [&](Label& l, TextBox& b, int lw2) {
            l.move(cx, y, lw2, rh);
            cx += lw2;
            b.move(cx, y, S(64), rh);
            cx += S(64) + S(16);
        };
        pair(l_start, start, S(60));
        pair(l_step, step, S(40));
        pair(l_digits, digits, S(150));
        y += rh + S(8);
        l_case.move(L, y, lw, rh);
        cases.move(x, y, fw, rh);
        y += rh + S(12);
        int by = h - S(52);
        list.move(L, y, w - 2 * L, by - S(10) - y);
        cancel.move(right - S(100), by, S(100), S(36));
        ok.move(right - S(208), by, S(100), S(36));
        msg.move(L, by, right - S(220) - L, S(36));
    }

    BulkOpts options() {
        BulkOpts o;
        o.tpl = tpl.get();
        o.find = find.get();
        o.repl = repl.get();
        o.regex = regex.checked;
        o.match_case = match_case.checked;
        o.keep_ext = keep_ext.checked;
        o.case_mode = std::max(0, cases.selected);
        auto num = [](const wstr& s, int d) {
            try { size_t k; int v = std::stoi(trim(s), &k); return v; } catch (...) { return d; }
        };
        o.start = num(start.get(), 1);
        o.step = num(step.get(), 1);
        o.digits = std::max(1, std::min(9, num(digits.get(), 1)));
        return o;
    }

    void update() {
        KillTimer(hwnd, 7);
        wstr error;
        names = bulk_names(entries, options(), error);
        if (!error.empty() || names.size() != entries.size()) {
            names.clear();
            for (auto& e : entries) names.push_back(e.name);
            problems.clear();
            if (error.empty()) error = L"something is wrong with the pattern";
        } else {
            problems = bulk_problems(entries, names);
        }
        std::vector<SimpleList::Row> rows;
        int n_changed = 0;
        for (size_t i = 0; i < entries.size(); i++) {
            SimpleList::Row r;
            auto it = problems.find((int)i);
            wstr shown = names[i];
            if (it != problems.end()) { shown += L"    ⚠ " + it->second; r.color = T.bad; }
            else if (names[i] == entries[i].name) r.color = T.faint;
            else { r.color = T.good; n_changed++; }
            r.cells = {entries[i].name, shown};
            if (entries[i].dir && !entries[i].drive) r.icon = folder_bitmap(app->folder_color(entries[i].path), S(16));
            else r.sys_icon = entries[i].icon >= 0 ? entries[i].icon : -1;
            rows.push_back(std::move(r));
        }
        list.set_rows(std::move(rows));
        if (!error.empty()) msg.set(L"Pattern problem: " + error, T.bad);
        else if (!problems.empty()) msg.set(fmt(L"%d name%s can't be used — fix them to rename", (int)problems.size(), problems.size() > 1 ? L"s" : L""), T.bad);
        else msg.set(n_changed ? fmt(L"%d of %d will be renamed", n_changed, (int)entries.size()) : L"Nothing changes yet", T.muted);
        ready = n_changed && problems.empty() && error.empty();
        ok.set_enabled(ready);
    }

    void apply() {
        update();
        if (!ready) return;
        finish(1);
    }
};

void label(Label& l, HWND parent, const wstr& t, HFONT f = nullptr, COLORREF c = CLR_INVALID) {
    l.create(parent);
    l.text_ = t;
    l.font = f ? f : F.ui;
    l.color = c;
}

}  // namespace

// ======================================================================
bool tag_dialog(HWND owner, const wstr& title, wstr& name, std::string& color) {
    TagDialog d;
    int w = S(440), h = S(230);
    RECT wr{0, 0, w, h};
    AdjustWindowRectEx(&wr, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    d.create_top(owner, title, WS_CAPTION | WS_SYSMENU | WS_POPUP, WS_EX_DLGMODALFRAME, 0, 0, wr.right - wr.left, wr.bottom - wr.top);
    label(d.l_name, d.hwnd, L"Name", F.small);
    d.l_name.move(S(18), S(12), w - S(36), S(22));
    d.box.create_in(d.hwnd);
    d.box.hint = L"e.g. Important";
    d.box.move(S(18), S(36), w - S(36), S(34));
    d.box.set(name, false);
    label(d.l_color, d.hwnd, L"Colour", F.small);
    d.l_color.move(S(18), S(82), w - S(36), S(22));
    d.sw.create(d.hwnd);
    d.sw.color = is_folder_color(color) ? color : FOLDER_COLOR_NAMES[0];
    d.sw.move(S(18), S(106), w - S(36), S(40));
    d.ok.create(d.hwnd);
    d.ok.label = L"Save";
    d.ok.primary = true;
    d.cancel.create(d.hwnd);
    d.cancel.label = L"Cancel";
    d.cancel.move(w - S(18) - S(96), h - S(52), S(96), S(34));
    d.ok.move(w - S(18) - S(96) * 2 - S(8), h - S(52), S(96), S(34));
    auto save = [&] {
        if (trim(d.box.get()).empty()) { d.box.focus(); return; }
        d.finish(1);
    };
    d.ok.click = save;
    d.enter = save;
    d.cancel.click = [&] { d.finish(0); };
    d.box.select_all();
    d.run(owner);
    bool ok = d.result == 1;
    if (ok) { name = trim(d.box.get()); color = d.sw.color; }
    DestroyWindow(d.hwnd);
    return ok;
}

// ======================================================================
void bulk_rename_dialog(App* app, const std::vector<EntryPtr>& list) {
    BulkDialog d;
    d.app = app;
    for (auto& e : list) {
        Entry c = *e;
        if (c.icon < 0) c.icon = app->icon_index(*e, [] {});
        d.entries.push_back(c);
    }
    int w = S(760), h = S(640);
    RECT ar;
    GetWindowRect(app->hwnd, &ar);
    h = std::min(h, std::max(S(520), (int)(ar.bottom - ar.top) - S(40)));
    RECT wr{0, 0, w, h};
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_THICKFRAME;
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    d.create_top(app->hwnd, fmt(L"Rename %d items", (int)list.size()), style, 0, 0, 0, wr.right - wr.left, wr.bottom - wr.top);
    HWND p = d.hwnd;
    label(d.l_tpl, p, L"New name");
    d.tpl.create_in(p);
    d.tpl.set(L"{name}", false);
    label(d.l_help, p, L"{name} the old name   {n} a number   {date} date modified   {today}   {parent} the folder's name — any other text is kept", F.small, T.faint);
    d.presets.create(p);
    d.presets.labels = {L"Number them", L"Name + number", L"Date first", L"Folder name + number", L"Reset"};
    static const wchar_t* PRESET[] = {L"{name} ({n})", L"Photo {n}", L"{date} {name}", L"{parent} {n}", L"{name}"};
    d.presets.picked = [&](int i) {
        d.tpl.set(PRESET[i]);
        d.tpl.focus();
        SendMessageW(d.tpl.edit, EM_SETSEL, (WPARAM)-1, -1);
    };
    label(d.l_find, p, L"Find");
    d.find.create_in(p);
    label(d.l_repl, p, L"replace with", nullptr, T.muted);
    d.l_repl.flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
    d.repl.create_in(p);
    auto check = [&](Check& c, const wstr& t, bool on) {
        c.create(p);
        c.label = t;
        c.checked = on;
        c.changed = [&] { d.changed(); };
    };
    check(d.regex, L"Regular expression", false);
    check(d.match_case, L"Match case", false);
    check(d.keep_ext, L"Keep extensions (.jpg, .pdf …)", true);
    label(d.l_nums, p, L"Numbers");
    label(d.l_start, p, L"start at", F.small);
    label(d.l_step, p, L"step", F.small);
    label(d.l_digits, p, L"digits (1 → 1, 3 → 001)", F.small);
    d.start.create_in(p);
    d.start.set(L"1", false);
    d.step.create_in(p);
    d.step.set(L"1", false);
    d.digits.create_in(p);
    d.digits.set(L"1", false);
    label(d.l_case, p, L"Case");
    d.cases.create(p);
    for (auto c : CASE_NAMES) d.cases.labels.push_back(c);
    d.cases.selected = 0;
    d.cases.picked = [&](int) { d.changed(); };
    d.list.create(p);
    d.list.headers = {L"Now", L"Becomes"};
    d.list.widths = {280, 0};
    label(d.msg, p, L"", F.small);
    d.msg.flags = DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX;
    d.ok.create(p);
    d.ok.label = L"Rename";
    d.ok.primary = true;
    d.cancel.create(p);
    d.cancel.label = L"Cancel";
    d.ok.click = [&] { d.apply(); };
    d.cancel.click = [&] { d.finish(0); };
    d.enter = [&] { d.apply(); };
    for (TextBox* t : {&d.tpl, &d.find, &d.repl, &d.start, &d.step, &d.digits}) t->changed = [&] { d.changed(); };
    RECT cr;
    GetClientRect(d.hwnd, &cr);
    d.place(cr.right, cr.bottom);
    d.update();
    d.tpl.focus();
    SendMessageW(d.tpl.edit, EM_SETSEL, (WPARAM)-1, -1);
    d.run(app->hwnd);
    std::vector<std::pair<wstr, wstr>> pairs;
    if (d.result == 1)
        for (size_t i = 0; i < d.entries.size(); i++)
            if (d.names[i] != d.entries[i].name) pairs.push_back({d.entries[i].path, path_join(dir_name(d.entries[i].path), d.names[i])});
    DestroyWindow(d.hwnd);
    if (!pairs.empty()) app->rename_paths(pairs);
}
