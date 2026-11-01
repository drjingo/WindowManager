#include "dialogs.h"
#include <algorithm>
#include <commdlg.h>

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

// colour dots to pick from (they wrap into rows); gradients show as gradient dots. With allow_custom, a last "+" dot
// opens Windows' colour picker, and then shows the colour picked.
class Swatches : public Control {
  public:
    std::vector<std::string> specs = FOLDER_COLOR_NAMES;
    std::string color;
    bool allow_custom = false, enabled = true;
    std::function<void()> changed;
    static int dot() { return S(26); }
    static int step() { return S(34); }
    int count() const { return (int)specs.size() + (allow_custom ? 1 : 0); }
    int height_for(int w) const {
        int per = std::max(1, (w - S(4)) / step());
        return ((count() + per - 1) / per) * step();
    }
    bool is_custom() const {
        return !color.empty() && std::find(specs.begin(), specs.end(), color) == specs.end();
    }
    void on_paint(HDC dc, const RECT& rc) override {
        fill(dc, rc, T.bg);
        int d = dot(), st = step(), per = std::max(1, (int)(rc.right - S(4)) / st);
        rects_.clear();
        for (int i = 0; i < count(); i++) {
            int x = (i % per) * st + S(4), y = (i / per) * st + (st - d) / 2;
            RECT r{x, y, x + d, y + d};
            rects_.push_back(r);
            float cx = r.left + d / 2.0f, cy = r.top + d / 2.0f;
            bool custom_slot = i == (int)specs.size();
            bool on = custom_slot ? is_custom() : specs[i] == color;
            if (on) fill_circle(dc, cx, cy, d / 2.0f + S(3), T.bg, T.fg, (float)S(2));
            int dd = d - (on ? S(2) : 0), off = (d - dd) / 2;
            if (custom_slot && !is_custom()) {
                fill_circle(dc, cx, cy, d / 2.0f, T.field, T.border, (float)S(1));
                text(dc, L"+", r, T.muted, F.ui_bold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                gradient_dot(custom_slot ? color : specs[i], dd)->draw(dc, r.left + off, r.top + off, -1, -1, enabled ? 255 : 90);
            }
        }
    }
    void on_mouse_move(int x, int y, WPARAM) override {
        bool hit = false;
        for (auto& r : rects_) if (PtInRect(&r, POINT{x, y})) hit = true;
        SetCursor(LoadCursor(nullptr, hit && enabled ? IDC_HAND : IDC_ARROW));
    }
    void on_lbutton_down(int x, int y, WPARAM) override {
        if (!enabled) return;
        for (size_t i = 0; i < rects_.size(); i++) {
            if (!PtInRect(&rects_[i], POINT{x, y})) continue;
            if (i == specs.size()) {
                if (!pick_custom()) return;
            } else color = specs[i];
            invalidate();
            if (changed) changed();
            return;
        }
    }
    bool pick_custom() {
        static COLORREF custom[16] = {};
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof cc;
        cc.hwndOwner = GetAncestor(hwnd, GA_ROOT);
        cc.rgbResult = color.empty() ? folder_rgb("blue") : folder_rgb(color);
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (!ChooseColorW(&cc)) return false;
        color = hex_of(cc.rgbResult);
        return true;
    }

  private:
    std::vector<RECT> rects_;
};

// a big folder in a colour, to see the result before saving
class FolderPreview : public Control {
  public:
    std::string spec;
    void on_paint(HDC dc, const RECT& rc) override {
        fill(dc, rc, T.bg);
        int n = std::min((int)rc.bottom, S(80));
        folder_bitmap(spec, n)->draw(dc, (rc.right - n) / 2, 0);
        RECT t{0, n, rc.right, rc.bottom};
        text(dc, color_label(spec), t, T.muted, F.small, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
};

struct ColorDialog : Dialog {
    Label l_color, l_to, l_presets;
    Swatches first, second, presets;
    Check gradient, subfolders;
    FolderPreview preview;
    Button ok, cancel;
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
    int w = S(480), h = S(272);
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
    d.sw.allow_custom = true;
    d.sw.color = is_folder_color(color) && !is_gradient(color) ? color : FOLDER_COLOR_NAMES[0];
    d.sw.move(S(18), S(104), w - S(36), d.sw.height_for(w - S(36)));
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
bool folder_color_dialog(HWND owner, const wstr& title, std::string& spec, bool* subfolders) {
    ColorDialog d;
    int L = S(18), w = S(560), fw = w - 2 * L;
    std::string a = "yellow", b = "red";
    if (is_folder_color(spec)) {
        size_t gt = spec.find('>');
        a = gt == std::string::npos ? spec : spec.substr(0, gt);
        if (gt != std::string::npos) b = spec.substr(gt + 1);
    }
    d.create_top(owner, title, WS_CAPTION | WS_SYSMENU | WS_POPUP, WS_EX_DLGMODALFRAME, 0, 0, 100, 100);
    d.first.create(d.hwnd);
    d.first.allow_custom = true;
    d.first.color = a;
    d.second.create(d.hwnd);
    d.second.allow_custom = true;
    d.second.color = b;
    d.presets.create(d.hwnd);
    d.presets.specs.clear();
    for (auto& g : GRADIENT_PRESETS) d.presets.specs.push_back(g.second);
    d.presets.color = is_gradient(spec) ? spec : "";
    d.gradient.create(d.hwnd);
    d.gradient.label = L"Gradient (two colours)";
    d.gradient.checked = is_gradient(spec);
    label(d.l_color, d.hwnd, L"Colour", F.small);
    label(d.l_to, d.hwnd, L"Gradient to", F.small);
    label(d.l_presets, d.hwnd, L"Ready-made gradients", F.small);
    d.preview.create(d.hwnd);
    if (subfolders) {
        d.subfolders.create(d.hwnd);
        d.subfolders.label = L"Same colour for all subfolders (unless they have their own)";
        d.subfolders.checked = *subfolders;
    }
    d.ok.create(d.hwnd);
    d.ok.label = L"Apply";
    d.ok.primary = true;
    d.cancel.create(d.hwnd);
    d.cancel.label = L"Cancel";

    int pw = S(120), sw = fw - pw - S(12), y = S(12);
    int h1 = d.first.height_for(sw);
    d.l_color.move(L, y, sw, S(22));
    d.preview.move(L + sw + S(12), y + S(22), pw, S(100));
    y += S(24);
    d.first.move(L, y, sw, h1);
    y += h1 + S(10);
    d.gradient.move(L, y, d.gradient.ideal_width(), S(26));
    y += S(34);
    d.l_to.move(L, y, sw, S(22));
    y += S(24);
    d.second.move(L, y, sw, d.second.height_for(sw));
    y += d.second.height_for(sw) + S(10);
    d.l_presets.move(L, y, fw, S(22));
    y += S(24);
    d.presets.move(L, y, fw, d.presets.height_for(fw));
    y += d.presets.height_for(fw) + S(10);
    if (subfolders) {
        d.subfolders.move(L, y, d.subfolders.ideal_width(), S(26));
        y += S(38);
    }
    y += S(6);
    d.cancel.move(w - L - S(96), y, S(96), S(34));
    d.ok.move(w - L - S(96) * 2 - S(8), y, S(96), S(34));
    int h = y + S(34) + S(16);
    RECT wr{0, 0, w, h};
    AdjustWindowRectEx(&wr, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    SetWindowPos(d.hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    center_on(d.hwnd, owner);

    auto result = [&] {
        return d.gradient.checked && d.second.color != d.first.color ? d.first.color + ">" + d.second.color : d.first.color;
    };
    auto update = [&] {
        d.second.enabled = d.gradient.checked;
        d.l_to.set(L"Gradient to", d.gradient.checked ? CLR_INVALID : T.faint);
        d.presets.color = d.gradient.checked ? result() : "";
        d.preview.spec = result();
        for (Control* c : std::initializer_list<Control*>{&d.second, &d.presets, &d.preview}) c->invalidate();
    };
    d.first.changed = update;
    d.second.changed = [&] {
        if (!d.gradient.checked) d.gradient.checked = true, d.gradient.invalidate();
        update();
    };
    d.gradient.changed = update;
    d.presets.changed = [&] {
        std::string g = d.presets.color;
        size_t gt = g.find('>');
        d.first.color = g.substr(0, gt);
        d.second.color = g.substr(gt + 1);
        d.gradient.checked = true;
        d.first.invalidate();
        d.gradient.invalidate();
        update();
    };
    update();
    d.ok.click = [&] { d.finish(1); };
    d.enter = [&] { d.finish(1); };
    d.cancel.click = [&] { d.finish(0); };
    d.run(owner);
    bool ok = d.result == 1;
    if (ok) {
        spec = result();
        if (subfolders) *subfolders = d.subfolders.checked;
    }
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
