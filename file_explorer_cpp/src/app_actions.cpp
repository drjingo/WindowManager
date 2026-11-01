// What you can do: favourites, recent folders, tags, folder colours, copy / move / delete / rename, zip,
// the right-click menus, two panes, tab sets, Quick Look, colours.
#include "app.h"
#include "dialogs.h"
#include "transfer.h"
#include "everything.h"
#include "panes.h"
#include "quicklook.h"
#include <algorithm>
#include <set>

enum { TIMER_RECENT = 12 };

// ======================================================================
// favourites
// ======================================================================
bool App::is_favorite(const wstr& p) {
    if (p.empty() || is_virtual(p)) return false;
    for (auto& f : favorites) if (norm(f.path) == norm(p)) return true;
    return false;
}

void App::add_favorite(const wstr& p) {
    if (p.empty() || is_virtual(p) || is_favorite(p)) return;
    favorites.push_back({display_name(p), clean_path(p)});
    favorites_changed(L"★ Added “" + display_name(p) + L"” to favorites");
}

void App::remove_favorite(int i) {
    if (i < 0 || i >= (int)favorites.size()) return;
    wstr name = favorites[i].name;
    favorites.erase(favorites.begin() + i);
    favorites_changed(L"Removed “" + name + L"” from favorites");
}

void App::rename_favorite(int i) {
    if (i < 0 || i >= (int)favorites.size()) return;
    wstr name = favorites[i].name;
    if (ask_text(hwnd, L"Rename favorite", L"Name shown in the sidebar:", name) && !trim(name).empty()) {
        favorites[i].name = trim(name);
        favorites_changed();
    }
}

void App::move_favorite(int i, int j) {
    j = std::max(0, std::min(j, (int)favorites.size() - 1));
    if (i < 0 || i >= (int)favorites.size()) return;
    if (i != j) {
        Fav f = favorites[i];
        favorites.erase(favorites.begin() + i);
        favorites.insert(favorites.begin() + j, f);
    }
    favorites_changed();
}

void App::toggle_favorite() {
    wstr p = real_folder();
    if (p.empty()) return;
    for (size_t i = 0; i < favorites.size(); i++)
        if (norm(favorites[i].path) == norm(p)) { remove_favorite((int)i); return; }
    add_favorite(p);
}

void App::favorites_changed(const wstr& msg) {
    sidebar.rebuild();
    toolbar.star_on = is_favorite(cur().path);
    toolbar.invalidate();
    save_state();
    if (!msg.empty()) say(msg);
}

// ======================================================================
// recent & frequent folders
// ======================================================================
void App::record_visit(const wstr& p) {
    if (p.empty() || is_virtual(p)) return;
    Visit& v = visits[norm(p)];
    v.path = clean_path(p);
    v.n += 1;
    v.t = now_unix();
    if (visits.size() > 300) {
        std::vector<std::pair<double, wstr>> scored;
        double now = now_unix();
        for (auto& [k, x] : visits) scored.push_back({frecency(x, now), k});
        std::sort(scored.begin(), scored.end());
        for (size_t i = 0; i < 50 && i < scored.size(); i++) visits.erase(scored[i].second);
    }
    SetTimer(hwnd, TIMER_RECENT, 400, nullptr);
}

double App::frecency(const Visit& v, double now) {
    double age = now - v.t;
    double w = age < 3600 ? 4 : age < 86400 ? 2 : age < 7 * 86400 ? 1 : age < 30 * 86400 ? 0.5 : 0.2;
    return v.n * w;
}

std::vector<wstr> App::recent_folders(size_t k) {
    std::set<wstr> skip;
    for (auto& f : favorites) skip.insert(norm(f.path));
    for (auto& [n, p] : quick_access()) skip.insert(norm(p));
    double now = now_unix();
    std::vector<std::pair<double, wstr>> scored;
    for (auto& [key, v] : visits) scored.push_back({-frecency(v, now), key});
    std::sort(scored.begin(), scored.end());
    std::vector<wstr> out;
    for (auto& [s, key] : scored) {
        const Visit& v = visits[key];
        if (skip.count(key) || is_drive_root(v.path) || !is_dir(v.path)) continue;
        out.push_back(v.path);
        if (out.size() >= k) break;
    }
    return out;
}

void App::forget_recent(const wstr& p) {
    if (p.empty()) visits.clear();
    else visits.erase(norm(p));
    sidebar.rebuild();
    save_state();
}

// ======================================================================
// tags
// ======================================================================
std::vector<wstr> App::tags_of(const wstr& p) {
    std::vector<wstr> out;
    if (p.empty() || file_tags.empty()) return out;
    auto it = file_tags.find(norm(p));
    if (it == file_tags.end()) return out;
    for (auto& t : it->second.tags)
        for (auto& d : tag_defs) if (d.name == t) { out.push_back(t); break; }
    return out;
}

COLORREF App::tag_color(const wstr& name) {
    for (auto& d : tag_defs) if (d.name == name) return folder_rgb(d.color);
    return folder_rgb("grey");
}

std::map<wstr, int> App::tag_counts() {
    std::map<wstr, int> out;
    for (auto& [k, r] : file_tags) for (auto& t : r.tags) out[t]++;
    return out;
}

void App::tag_menu(Menu& m, const std::vector<wstr>& paths) {
    std::vector<std::vector<wstr>> have;
    for (auto& p : paths) have.push_back(tags_of(p));
    bool any = false;
    for (auto& h : have) if (!h.empty()) any = true;
    for (auto& t : tag_defs) {
        size_t n = 0;
        for (auto& h : have) if (std::find(h.begin(), h.end(), t.name) != h.end()) n++;
        wstr name = t.name;
        HBITMAP bmp = menu_bitmap(*tag_dot(folder_rgb(t.color), S(12)));
        m.item((n && n < paths.size() ? L"– " : L"") + t.name, [this, paths, name] { toggle_tag(paths, name); }, true,
               n == paths.size() && n > 0, bmp);
    }
    m.sep();
    m.item(L"New tag…", [this, paths] { new_tag(paths); });
    if (any) m.item(L"Remove all tags", [this, paths] { clear_tags(paths); });
}

void App::toggle_tag(const std::vector<wstr>& paths, const wstr& name) {
    bool all = !paths.empty();
    for (auto& p : paths) {
        auto t = tags_of(p);
        if (std::find(t.begin(), t.end(), name) == t.end()) all = false;
    }
    wstr msg;
    if (all) {
        for (auto& p : paths) {
            auto it = file_tags.find(norm(p));
            if (it == file_tags.end()) continue;
            auto& v = it->second.tags;
            v.erase(std::remove(v.begin(), v.end(), name), v.end());
            if (v.empty()) file_tags.erase(it);
        }
        msg = L"Removed “" + name + L"”";
    } else {
        add_tag(paths, name, true);
        msg = L"Tagged “" + name + L"”";
    }
    tags_changed();
    say(msg + L": " + (paths.size() == 1 ? display_name(paths[0]) : fmt(L"%d items", (int)paths.size())), 4);
}

void App::add_tag(const std::vector<wstr>& paths, const wstr& name, bool quiet) {
    for (auto& p : paths) {
        TagRec& r = file_tags[norm(p)];
        r.path = clean_path(p);
        if (std::find(r.tags.begin(), r.tags.end(), name) == r.tags.end()) r.tags.push_back(name);
    }
    if (!quiet) {
        tags_changed();
        say(L"Tagged “" + name + L"”: " + (paths.size() == 1 ? display_name(paths[0]) : fmt(L"%d items", (int)paths.size())), 4);
    }
}

void App::clear_tags(const std::vector<wstr>& paths) {
    for (auto& p : paths) file_tags.erase(norm(p));
    tags_changed();
}

void App::new_tag(const std::vector<wstr>& paths) {
    std::set<std::string> used;
    for (auto& t : tag_defs) used.insert(t.color);
    std::string color = "blue";
    for (auto& c : FOLDER_COLOR_NAMES) if (!used.count(c)) { color = c; break; }
    wstr name;
    if (!tag_dialog(hwnd, L"New tag", name, color)) return;
    bool exists_already = false;
    for (auto& t : tag_defs)
        if (lower(t.name) == lower(name)) { name = t.name; exists_already = true; }
    if (!exists_already) tag_defs.push_back({name, color});
    if (!paths.empty()) add_tag(paths, name, true);
    tags_changed();
}

void App::edit_tag(int i) {
    if (i < 0 || i >= (int)tag_defs.size()) return;
    TagDef t = tag_defs[i];
    wstr name = t.name;
    std::string color = t.color;
    if (!tag_dialog(hwnd, L"Edit tag", name, color)) return;
    if (name != t.name) {
        for (size_t j = 0; j < tag_defs.size(); j++)
            if ((int)j != i && lower(tag_defs[j].name) == lower(name)) { say(L"There's already a tag called “" + name + L"”.", 6); return; }
        for (auto& [k, r] : file_tags) for (auto& x : r.tags) if (x == t.name) x = name;
        wstr kk, a, b;
        parse_virtual(cur().path, kk, a, b);
        if (kk == L"tag" && a == t.name) cur().path = make_tag_path(name);
    }
    tag_defs[i] = {name, color};
    tags_changed();
}

void App::move_tag(int i, int d) {
    int j = i + d;
    if (i < 0 || j < 0 || i >= (int)tag_defs.size() || j >= (int)tag_defs.size()) return;
    std::swap(tag_defs[i], tag_defs[j]);
    tags_changed();
}

void App::delete_tag(int i) {
    if (i < 0 || i >= (int)tag_defs.size()) return;
    TagDef t = tag_defs[i];
    int n = tag_counts()[t.name];
    if (n && !confirm(hwnd, L"Delete tag", L"Delete the tag “" + t.name + L"”?\n\nIt's removed from " +
                                               fmt(L"%d item%s", n, n > 1 ? L"s" : L"") + L" (the files themselves stay).",
                      L"Delete"))
        return;
    tag_defs.erase(tag_defs.begin() + i);
    for (auto it = file_tags.begin(); it != file_tags.end();) {
        auto& v = it->second.tags;
        v.erase(std::remove(v.begin(), v.end(), t.name), v.end());
        it = v.empty() ? file_tags.erase(it) : std::next(it);
    }
    tags_changed();
}

void App::tags_changed() {
    sidebar.rebuild();
    wstr k, a, b;
    parse_virtual(cur().path, k, a, b);
    if (k == L"tag") refresh();
    else fill(true);
    if (pane2_open) pane2->view.invalidate();
    if (ql && ql->is_open() && ql->entry) ql->render(ql->entry);
    save_state();
}

// ======================================================================
// folder colours
// ======================================================================
void App::count_color_subs() {
    colors_with_sub = 0;
    for (auto& [k, v] : folder_colors) if (v.sub) colors_with_sub++;
}

bool App::inherits_color(const wstr& p) {
    if (!colors_with_sub) return false;
    wstr k = norm(p);
    for (size_t cut = k.rfind(L'\\'); cut != wstr::npos && cut > 0; cut = k.rfind(L'\\', cut - 1)) {
        auto it = folder_colors.find(k.substr(0, cut));
        if (it != folder_colors.end() && it->second.sub) return true;
    }
    return false;
}

// its own colour; else the colour of the nearest parent folder that gives its colour to its subfolders; else the default
std::string App::folder_color(const wstr& p) {
    wstr k = norm(p);
    auto it = folder_colors.find(k);
    if (it != folder_colors.end()) return it->second.color;
    if (colors_with_sub)
        for (size_t cut = k.rfind(L'\\'); cut != wstr::npos && cut > 0; cut = k.rfind(L'\\', cut - 1)) {
            auto pit = folder_colors.find(k.substr(0, cut));
            if (pit != folder_colors.end() && pit->second.sub) return pit->second.color;
        }
    return default_folder_color;
}

void App::color_menu(Menu& m, const std::vector<wstr>& paths) {
    std::set<std::string> cur_colors;
    int own = 0, subs = 0;
    for (auto& p : paths) {
        auto it = folder_colors.find(norm(p));
        cur_colors.insert(it != folder_colors.end() ? it->second.color : "");
        if (it != folder_colors.end()) { own++; if (it->second.sub) subs++; }
    }
    std::string cur = cur_colors.size() == 1 ? *cur_colors.begin() : "-";
    // no colour of its own: it shows its parent's (when that one gives it to subfolders) or the default
    std::string base = paths.size() == 1 && cur.empty() ? folder_color(paths[0]) : default_folder_color;
    wstr base_label = base != default_folder_color || (paths.size() == 1 && cur.empty() && inherits_color(paths[0]))
                          ? L"Same as parent folder (" + color_label(base) + L")"
                          : L"Default (" + color_label(default_folder_color) + L")";
    m.item(base_label, [this, paths] { set_folder_color(paths, ""); }, true, cur.empty(),
           menu_bitmap(*gradient_dot(base, S(12))));
    m.sep();
    for (auto& c : FOLDER_COLOR_NAMES) {
        std::string col = c;
        m.item(color_label(c), [this, paths, col] { set_folder_color(paths, col); }, true, cur == c,
               menu_bitmap(*gradient_dot(c, S(12))));
    }
    Menu gm(m, 0);
    for (auto& [name, spec] : GRADIENT_PRESETS) {
        std::string sp = spec;
        gm.item(name + L"  (" + color_label(spec.substr(0, spec.find('>'))) + L" → " +
                    color_label(spec.substr(spec.find('>') + 1)) + L")",
                [this, paths, sp] { set_folder_color(paths, sp); }, true, cur == spec, menu_bitmap(*gradient_dot(spec, S(12))));
    }
    m.sub(L"Gradients", gm);
    m.item(L"More colours, custom, gradients…", [this, paths] { folder_color_more(paths); });
    m.sep();
    bool all_sub = own == (int)paths.size() && subs == own;
    m.item(L"Same colour for all subfolders", [this, paths, all_sub] { set_color_subfolders(paths, !all_sub); },
           own == (int)paths.size(), all_sub);
    m.item(L"Show colours in Windows Explorer too", [this] { toggle_color_in_explorer(); }, true, color_in_explorer);
}

void App::folder_color_more(const std::vector<wstr>& paths) {
    std::string spec = folder_color(paths[0]);
    auto it = folder_colors.find(norm(paths[0]));
    bool sub = it != folder_colors.end() && it->second.sub;
    wstr title = paths.size() == 1 ? L"Colour of " + display_name(paths[0]) : fmt(L"Colour of %d folders", (int)paths.size());
    if (folder_color_dialog(hwnd, title, spec, &sub)) set_folder_color(paths, spec, sub ? 1 : 0);
}

void App::default_color_menu(Menu& m) {
    auto set_default = [this](const std::string& col) {
        default_folder_color = col;
        colors_changed();
        say(L"Folders are " + color_label(col) + L" now (folders you coloured keep their colour).", 6);
    };
    for (auto& c : FOLDER_COLOR_NAMES) {
        std::string col = c;
        m.item(color_label(c), [set_default, col] { set_default(col); }, true, c == default_folder_color,
               menu_bitmap(*gradient_dot(c, S(12))));
    }
    Menu gm(m, 0);
    for (auto& [name, spec] : GRADIENT_PRESETS) {
        std::string sp = spec;
        gm.item(name, [set_default, sp] { set_default(sp); }, true, sp == default_folder_color,
                menu_bitmap(*gradient_dot(spec, S(12))));
    }
    m.sub(L"Gradients", gm);
    m.item(L"More colours, custom, gradients…", [this, set_default] {
        std::string spec = default_folder_color;
        if (folder_color_dialog(hwnd, L"Default folder colour", spec, nullptr)) set_default(spec);
    });
}

void App::set_folder_color(const std::vector<wstr>& paths, const std::string& color, int sub) {
    std::vector<wstr> errors;
    for (auto& p : paths) {
        auto it = folder_colors.find(norm(p));
        bool keep_sub = it != folder_colors.end() && it->second.sub;
        if (color.empty()) folder_colors.erase(norm(p));
        else folder_colors[norm(p)] = {clean_path(p), color, sub < 0 ? keep_sub : sub == 1};
        if (color_in_explorer) {
            wstr err;
            if (!set_explorer_folder_color(p, color, color.empty() ? "" : make_folder_ico(color), err))
                errors.push_back(display_name(p) + L": " + err);
        }
    }
    count_color_subs();
    colors_changed();
    wstr what = paths.size() == 1 ? display_name(paths[0]) : fmt(L"%d folders", (int)paths.size());
    bool subs = !color.empty() && folder_colors.count(norm(paths[0])) && folder_colors[norm(paths[0])].sub;
    if (!errors.empty()) say(L"Couldn't change it in Windows Explorer: " + errors[0], 8);
    else say(what + L": " + (color.empty() ? L"default colour" : color_label(color)) +
             (subs ? L", subfolders too" : L"") +
             (color_in_explorer ? L" — Windows Explorer may take a moment to show it" : L""), 8);
}

void App::set_color_subfolders(const std::vector<wstr>& paths, bool on) {
    for (auto& p : paths) {
        auto it = folder_colors.find(norm(p));
        if (it != folder_colors.end()) it->second.sub = on;
    }
    count_color_subs();
    colors_changed();
    say(on ? L"Subfolders get the same colour now (unless they have their own)." : L"Subfolders use their own / the default colour again.", 6);
}

void App::toggle_color_in_explorer() {
    bool on = !color_in_explorer;
    if (on && !confirm(hwnd, L"Folder colours in Windows Explorer",
                       L"Show the colours you give folders in Windows Explorer too?\n\nThis writes a small hidden "
                       L"desktop.ini into each coloured folder (the way Windows customizes folders) and keeps the icons in "
                       L"the folder_icons folder next to this app — don't move or delete it. Turning it off puts the "
                       L"normal folders back.", L"Show them"))
        return;
    color_in_explorer = on;
    int errors = 0;
    for (auto& [k, v] : folder_colors) {
        wstr err;
        if (!set_explorer_folder_color(v.path, on ? v.color : "", on ? make_folder_ico(v.color) : "", err)) errors++;
    }
    save_state();
    say(wstr(on ? L"Colours shown in Windows Explorer too" : L"Windows Explorer shows normal folders again") +
        (errors ? fmt(L" (%d folder%s couldn't be changed)", errors, errors > 1 ? L"s" : L"") : L""), 8);
}

void App::colors_changed() {
    sidebar.rebuild();
    redraw_all();
    save_state();
}

// ======================================================================
// files
// ======================================================================
std::vector<EntryPtr> App::selection() { return main_fv()->selected_entries(); }

std::vector<wstr> App::selected_paths() {
    std::vector<wstr> out;
    for (auto& e : selection()) if (!e->drive) out.push_back(e->path);
    return out;
}

void App::open_entry(EntryPtr e) {
    if (!e) return;
    if (e->dir) { navigate(e->path); return; }
    wstr err;
    if (open_file(e->path, err)) say(L"Opening " + e->name + L"…", 3);
    else say(L"Can't open " + e->name + L": " + err, 10);
}

void App::open_selected() {
    auto sel = selection();
    if (sel.size() == 1) open_entry(sel[0]);
    else for (auto& e : sel) if (!e->dir) open_entry(e);
}

void App::copy_text(const wstr& t) {
    clipboard_set_text(hwnd, t);
    say(L"Copied ✓", 2);
}

void App::copy_paths(const std::vector<wstr>& paths, bool cut) {
    if (paths.empty()) return;
    if (!clipboard_set_files(paths, cut, hwnd)) { say(L"Couldn't use the clipboard — try again.", 6); return; }
    cut_paths.clear();
    if (cut) for (auto& p : paths) cut_paths.insert(norm(p));
    main_fv()->invalidate();
    if (pane2_open) pane2->view.invalidate();
    say(fmt(L"%s %d item%s — paste with Ctrl+V", cut ? L"Cut" : L"Copied", (int)paths.size(), paths.size() > 1 ? L"s" : L""), 4);
}

void App::paste(const wstr& into) {
    wstr dest = into.empty() ? (active_view && pane2_open ? pane2->path : real_folder()) : into;
    if (dest.empty() || is_virtual(dest)) { say(L"Open a folder to paste into.", 4); return; }
    std::vector<wstr> paths;
    bool cut = false;
    clipboard_get_files(hwnd, paths, cut);
    paths.erase(std::remove_if(paths.begin(), paths.end(), [](const wstr& p) { return !exists(p); }), paths.end());
    if (paths.empty()) { say(L"Nothing to paste — copy some files first.", 4); return; }
    if (transfer(paths, dest, cut) && cut) cut_paths.clear();
}

wstr App::transfer_problem(const std::vector<wstr>& paths, const wstr& dest, bool move) {
    if (dest.empty() || is_virtual(dest)) return L"That's not a folder";
    if (!is_dir(dest)) return L"That folder isn't there any more";
    if (move) {
        bool all_here = true;
        for (auto& p : paths) if (norm(dir_name(p)) != norm(dest)) all_here = false;
        if (all_here) return L"They're already in this folder";
    }
    for (auto& p : paths) if (is_inside(dest, p)) return L"Can't put a folder inside itself";
    return L"";
}

bool App::transfer(const std::vector<wstr>& paths, const wstr& dest, bool move) {
    wstr problem = transfer_problem(paths, dest, move);
    if (!problem.empty()) { say(problem + L".", 5); return false; }
    start_transfer(this, paths, dest, move);  // our own engine + progress window (Pause / Skip / Cancel)
    return true;
}

void App::copy_across(bool move, bool from_right) {
    if (!pane2_open) { say(L"Open the second pane first (F3).", 5); return; }
    std::vector<wstr> paths;
    wstr dest;
    if (from_right) {
        for (auto& e : pane2->selection()) if (!e->drive) paths.push_back(e->path);
        dest = real_folder();
    } else {
        paths = selected_paths();
        dest = pane2->has_path ? pane2->path : L"";
    }
    if (paths.empty()) { say(L"Select something first.", 4); return; }
    transfer(paths, dest, move);
}

void App::delete_selected(bool permanent) {
    FileView* fv = main_fv();
    auto sel = fv->selected();
    std::vector<wstr> paths;
    for (int i : sel) if (!fv->items[i]->drive) paths.push_back(fv->items[i]->path);
    if (paths.empty()) return;
    std::vector<wstr> after;  // what to select afterwards
    int first = sel.empty() ? 0 : sel[0];
    for (int i = first; i < (int)fv->items.size(); i++) if (!fv->sel[i]) { after.push_back(fv->items[i]->name); break; }
    if (after.empty()) for (int i = first - 1; i >= 0; i--) if (!fv->sel[i]) { after.push_back(fv->items[i]->name); break; }
    delete_paths(paths, permanent, after, real_folder());
}

void App::delete_paths(const std::vector<wstr>& paths, bool permanent, const std::vector<wstr>& select, const wstr& select_in) {
    size_t n = paths.size();
    wstr what = n == 1 ? L"“" + base_name(paths[0]) + L"”" : fmt(L"these %d items", (int)n);
    if (permanent && !confirm(hwnd, L"Delete for good", L"Delete " + what + L" for good?\n\nThey don't go to the Recycle Bin and can't be brought back.", L"Delete", true))
        return;
    unsigned flags = FOF_NOCONFIRMATION | (permanent ? 0 : FOF_ALLOWUNDO | FOF_WANTNUKEWARNING);
    wstr text = (permanent ? L"Deleted " : L"Moved to the Recycle Bin: ") + (n == 1 ? base_name(paths[0]) : fmt(L"%d items", (int)n));
    run_op(OP_DELETE, paths, L"", flags, text, select, select_in, {});
}

void App::run_op(FileOp op, std::vector<wstr> paths, wstr dest, unsigned flags, wstr done_text, std::vector<wstr> select,
                 wstr select_in, std::vector<std::pair<wstr, wstr>> remap) {
    say(op == OP_DELETE ? L"Deleting…" : op == OP_MOVE ? L"Moving…" : L"Copying…", 60);
    HWND owner = hwnd;
    run_bg([=] {
        bool aborted = false;
        bool ok = shell_file_op(op, paths, dest, flags, owner, aborted);
        std::vector<std::pair<wstr, wstr>> moved;
        if (ok) for (auto& [o, n] : remap) if (!exists(o) && exists(n)) moved.push_back({o, n});
        post_ui([=] {
            if (!moved.empty()) remap_paths(moved);
            reload_panes(select, select_in);
            say(ok && !aborted ? done_text + L" ✓"
                : aborted ? wstr(L"Stopped.")
                          : wstr(L"Some files couldn't be changed — they may be open in another program, or need administrator rights."), 8);
        });
    });
}

void App::reload_panes(const std::vector<wstr>& select, const wstr& select_in) {
    wstr here = real_folder();
    wstr k, a, b;
    parse_virtual(cur().path, k, a, b);
    if (k == L"search") {  // search results: drop what's gone (the search isn't run again)
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const EntryPtr& e) { return !exists(e->path); }), entries.end());
        fill(true);
    } else {
        remember_view();
        if (!here.empty() && !select.empty() && !select_in.empty() && norm(select_in) == norm(here)) cur().select = select;
        load(cur().path, cur().select, true);
    }
    if (pane2_open) {
        if (!select.empty() && !select_in.empty() && pane2->has_path && norm(select_in) == norm(pane2->path)) {
            pane2->select_names = select;
            pane2->load(pane2->path, select);
        } else pane2->reload();
    }
}

void App::remap_paths(const std::vector<std::pair<wstr, wstr>>& pairs) {
    bool changed = false;
    for (auto& [old_p, new_p] : pairs) {
        wstr o = norm(old_p), nn = clean_path(new_p);
        size_t olen = clean_path(old_p).size();
        auto inside = [&](const wstr& k) { return k == o || starts_with(k, o + L"\\"); };
        auto remap_map = [&](auto& m) {
            std::vector<wstr> keys;
            for (auto& [k, v] : m) if (inside(k)) keys.push_back(k);
            for (auto& k : keys) {
                auto v = m[k];
                m.erase(k);
                v.path = nn + v.path.substr(std::min(olen, v.path.size()));
                m[norm(v.path)] = v;
                changed = true;
            }
        };
        remap_map(folder_colors);
        count_color_subs();
        remap_map(file_tags);
        remap_map(visits);
        for (auto& f : favorites) {
            wstr fn = norm(f.path);
            if (inside(fn)) {
                if (fn == o && f.name == display_name(f.path)) f.name = display_name(nn);
                f.path = nn + f.path.substr(std::min(olen, f.path.size()));
                changed = true;
            }
        }
        for (auto& e : entries)
            if (norm(e->path) == o) { e->path = nn; e->name = base_name(nn); }
    }
    if (changed) { sidebar.rebuild(); save_state(); }
}

void App::new_folder(const wstr& in) {
    wstr folder = !in.empty() ? in : (active_view && pane2_open ? pane2->path : real_folder());
    if (folder.empty() || is_virtual(folder)) return;
    wstr target = unique_name(folder, L"New folder");
    if (!CreateDirectoryW(target.c_str(), nullptr)) {
        say(L"Couldn't create a folder here.", 8);
        return;
    }
    wstr name = base_name(target);
    if (norm(folder) == norm(real_folder())) {
        pending_rename_ = name;
        load(cur().path, {name}, true);
    }
    if (pane2_open && pane2->has_path && norm(folder) == norm(pane2->path)) pane2->load(folder, {name});
}

void App::start_rename() {
    FileView* fv = main_fv();
    auto sel = fv->selected();
    std::vector<EntryPtr> es;
    for (int i : sel) if (!fv->items[i]->drive) es.push_back(fv->items[i]);
    if (es.size() > 1) { bulk_rename(es); return; }
    if (es.size() == 1) fv->start_rename(fv->index_of(es[0]->path));
}

bool App::rename_paths(std::vector<std::pair<wstr, wstr>> pairs, bool is_undo) {
    std::vector<Entry> es;
    std::vector<wstr> names;
    for (auto& [o, n] : pairs) {
        EntryPtr e = entry_for(o);
        Entry x = e ? *e : Entry();
        if (!e) { x.name = base_name(o); x.path = o; }
        es.push_back(x);
        names.push_back(base_name(n));
    }
    auto problems = bulk_problems(es, names);
    if (!problems.empty()) {
        auto [i, why] = *problems.begin();
        say(L"Can't rename to “" + names[i] + L"”: " + why, 8);
        return false;
    }
    std::vector<std::pair<wstr, wstr>> done;
    wstr err;
    if (!apply_renames(pairs, done, err)) { say(L"Couldn't rename: " + err + L" — nothing was changed.", 10); return false; }
    if (done.empty()) return false;
    remap_paths(done);
    undo.clear();
    if (!is_undo) for (auto& [o, n] : done) undo.push_back({n, o});
    std::vector<wstr> new_names;
    for (auto& [o, n] : done) new_names.push_back(base_name(n));
    wstr folder = dir_name(done[0].second);
    wstr msg = is_undo ? L"Rename undone ✓"
               : done.size() == 1 ? L"Renamed to “" + new_names[0] + L"”  ·  Ctrl+Z undoes it"
                                  : fmt(L"Renamed %d items ✓  ·  Ctrl+Z undoes it", (int)done.size());
    reload_panes(new_names, folder);
    post_ui([this, msg] { say(msg, 8); });
    return true;
}

void App::bulk_rename(const std::vector<EntryPtr>& es) {
    std::vector<EntryPtr> list;
    for (auto& e : es) if (!e->drive) list.push_back(e);
    if (!list.empty()) bulk_rename_dialog(this, list);
}

void App::undo_rename() {
    if (undo.empty()) { say(L"Nothing to undo (Ctrl+Z undoes the last rename).", 4); return; }
    auto pairs = undo;
    undo.clear();
    rename_paths(pairs, true);
}

// ======================================================================
// zip / unzip
// ======================================================================
void App::add_archive_items(Menu& m, const std::vector<EntryPtr>& sel, const wstr& folder) {
    std::vector<EntryPtr> files;
    for (auto& e : sel) if (!e->drive) files.push_back(e);
    if (files.empty() || folder.empty()) return;
    wstr name = files.size() == 1 && !files[0]->dir ? archive_stem(files[0]->name) : files.size() == 1 ? files[0]->name : display_name(folder);
    std::vector<wstr> paths;
    for (auto& e : files) paths.push_back(e->path);
    m.item(L"Compress to “" + name + L".zip”", [this, paths, folder, name] { compress(paths, folder, name); });
    std::vector<EntryPtr> arch;
    for (auto& e : files) if (!e->dir && !archive_kind(e->name).empty()) arch.push_back(e);
    if (!arch.empty() && arch.size() == files.size()) {
        if (arch.size() == 1) {
            wstr a = arch[0]->path, stem = archive_stem(arch[0]->name);
            m.item(L"Extract here", [this, a, folder] { extract(a, folder, false); });
            m.item(L"Extract to “" + stem + L"\\”", [this, a, folder] { extract(a, folder, true); });
        } else {
            m.item(fmt(L"Extract each to its own folder (%d)", (int)arch.size()), [this, arch, folder] {
                for (auto& e : arch) extract(e->path, folder, true);
            });
        }
    }
}

void App::compress(const std::vector<wstr>& paths, const wstr& folder, const wstr& name) {
    wstr target = unique_name(folder, name + L".zip");
    wstr tname = base_name(target);
    say(L"Compressing to " + tname + L"…", 600);
    run_bg([=] {
        wstr err;
        bool ok = compress_zip(paths, target, err, nullptr);
        post_ui([=] {
            reload_panes(ok ? std::vector<wstr>{tname} : std::vector<wstr>{}, folder);
            say(ok ? L"Compressed to " + tname + L" ✓" : L"Couldn't compress: " + err, 10);
        });
    });
}

void App::extract(const wstr& archive, const wstr& folder, bool own_folder) {
    wstr name = base_name(archive), stem = archive_stem(name);
    say(L"Extracting " + name + L"…", 600);
    run_bg([=] {
        wstr tmp = path_join(folder, fmt(L".extracting-%08x", GetTickCount()));
        wstr err;
        std::vector<wstr> shown;
        bool ok = extract_archive(archive, tmp, err, nullptr);
        if (ok) {
            if (own_folder) {
                wstr target = unique_name(folder, stem);
                ok = MoveFileW(tmp.c_str(), target.c_str()) != 0;
                if (ok) shown.push_back(base_name(target));
                else err = L"couldn't name the new folder";
            } else {  // straight here; what's already there is kept (new items get "(2)")
                Entries inside = list_folder(tmp, true, nullptr);
                for (auto& e : inside) {
                    wstr target = path_join(folder, e->name);
                    if (exists(target)) target = unique_name(folder, e->name);
                    if (MoveFileW(e->path.c_str(), target.c_str())) shown.push_back(base_name(target));
                }
                RemoveDirectoryW(tmp.c_str());
            }
        }
        if (!ok) {
            std::vector<wstr> del{tmp};
            bool ab;
            if (exists(tmp)) shell_file_op(OP_DELETE, del, L"", FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI, nullptr, ab);
        }
        post_ui([=] {
            reload_panes(shown, folder);
            say(ok ? L"Extracted " + name + L" ✓" : L"Couldn't extract " + name + L": " + err, 10);
        });
    });
}

// ======================================================================
// right-click menus
// ======================================================================
void App::item_menu(int i, POINT pt) {
    FileView* fv = main_fv();
    if (i < 0 || i >= (int)fv->items.size()) return;
    EntryPtr e = fv->items[i];
    auto sel = fv->selected_entries();
    bool one = sel.size() == 1;
    std::vector<EntryPtr> files;
    std::vector<wstr> paths;
    for (auto& x : sel) if (!x->drive) { files.push_back(x); paths.push_back(x->path); }
    wstr folder = real_folder();
    bool virt = is_virtual(cur().path);
    Menu m;
    m.item(L"Open", [this] { open_selected(); });
    if (one && e->dir) {
        m.item(L"Open in new tab", [this, e] { new_tab(e->path); });
        if (pane2_open) m.item(L"Open on the right", [this, e] { pane2->navigate(e->path); });
        if (!e->drive) {
            bool fav = is_favorite(e->path);
            m.item(fav ? L"★ Remove from favorites" : L"★ Add to favorites", [this, e, fav] {
                if (fav) { for (size_t k = 0; k < favorites.size(); k++) if (norm(favorites[k].path) == norm(e->path)) { remove_favorite((int)k); break; } }
                else add_favorite(e->path);
            });
        }
    } else if (one) {
        m.item(L"Open with…", [e] { open_with(e->path); });
    }
    m.item(L"Quick Look\tSpace", [this] { quick_look(this); });
    if (virt && one) m.item(L"Open file location", [this, e] { navigate(e->path); });
    if (!files.empty()) {
        m.sep();
        m.item(L"Cut\tCtrl+X", [this, paths] { copy_paths(paths, true); });
        m.item(L"Copy\tCtrl+C", [this, paths] { copy_paths(paths, false); });
        if (one && e->dir) m.item(L"Paste into this folder", [this, e] { paste(e->path); });
        if (pane2_open) {
            m.item(L"Copy to the right\tF5", [this] { copy_across(false, false); });
            m.item(L"Move to the right\tF6", [this] { copy_across(true, false); });
        }
        m.sep();
        m.item(files.size() == 1 ? L"Rename\tF2" : fmt(L"Rename %d items…\tF2", (int)files.size()), [this] { start_rename(); });
        m.item(L"Delete\tDel", [this] { delete_selected(false); });
        m.sep();
        wstr zf = folder;
        if (zf.empty()) {
            std::set<wstr> parents;
            for (auto& p : paths) parents.insert(norm(dir_name(p)));
            if (parents.size() == 1) zf = dir_name(paths[0]);
        }
        add_archive_items(m, files, zf);
        Menu tm(m, 0);
        tag_menu(tm, paths);
        m.sub(L"Tags", tm);
    }
    std::vector<wstr> dirs;
    for (auto& x : sel) if (x->dir && !x->drive) dirs.push_back(x->path);
    if (!dirs.empty() && dirs.size() == sel.size()) {
        Menu cm(m, 0);
        color_menu(cm, dirs);
        m.sub(L"Folder colour", cm);
    }
    m.sep();
    std::vector<wstr> all;
    for (auto& x : sel) all.push_back(x->path);
    m.item(L"Copy path", [this, all] {
        wstr t;
        for (auto& p : all) t += (t.empty() ? L"" : L"\r\n") + p;
        copy_text(t);
    });
    m.item(L"Show in Windows Explorer", [e] { open_in_explorer(e->path, true); });
    if (one) m.item(L"Properties", [this, e] { show_properties(e->path, hwnd); });
    m.show(hwnd, pt);
}

void App::background_menu(POINT pt) {
    wstr folder = real_folder();
    wstr k, a, b;
    parse_virtual(cur().path, k, a, b);
    Menu m;
    if (!folder.empty()) {
        m.item(L"New folder\tCtrl+Shift+N", [this] { new_folder(); });
        m.item(L"Paste\tCtrl+V", [this] { paste(); });
        m.sep();
        bool fav = is_favorite(folder);
        m.item(fav ? L"★ Remove from favorites" : L"★ Add to favorites", [this] { toggle_favorite(); });
    }
    if (k == L"search") {
        if (!search.done) m.item(L"Stop searching\tEsc", [this] { stop_search(); });
        m.item(L"Search again", [this] { refresh(); });
        m.sep();
    }
    Menu vm(m, 0);
    int mode = view_mode();
    vm.item(L"List\tCtrl+Shift+1", [this] { set_view(V_LIST); }, true, mode == V_LIST);
    vm.item(L"Grid\tCtrl+Shift+2", [this] { set_view(V_GRID); }, true, mode == V_GRID);
    vm.item(L"Columns\tCtrl+Shift+3", [this] { set_view(V_COLUMNS); }, k.empty(), mode == V_COLUMNS);
    vm.sep();
    static const int sizes[] = {48, 64, 96, 128, 192};
    for (int s : sizes)
        vm.item(fmt(L"Grid icons %d px", s), [this, s] { set_view(V_GRID); grid_size = s; view.grid_size = s; SendMessageW(view.hwnd, WM_SIZE, 0, 0); view.invalidate(); save_state(); }, true,
                mode == V_GRID && grid_size == s);
    vm.sep();
    vm.item(L"Two panes side by side\tF3", [this] { toggle_dual(); }, true, pane2_open);
    vm.item(L"Show folder sizes", [this] { toggle_sizes(); }, true, calc_sizes);
    vm.item(everything_running() ? L"Search with Everything (running)" : L"Search with Everything (not running)", [this] {
        use_everything = !use_everything;
        say(use_everything ? (everything_running() ? L"Searches use Everything — instant on NTFS drives."
                                                   : L"Searches will use Everything once it's running (voidtools.com).")
                           : L"Searches go folder by folder.", 6);
        save_state();
    }, true, use_everything);
    m.sub(L"View", vm);
    Menu sm(m, 0);
    std::vector<std::pair<std::string, wstr>> keys = {{"name", L"Name"}, {"modified", L"Date modified"}, {"type", L"Type"}, {"size", L"Size"}, {"tags", L"Tags"}};
    if (!k.empty()) keys.push_back({"location", L"Folder"});
    for (auto& [key, label] : keys) {
        std::string kk = key;
        sm.item(label + (sort_key == key ? (sort_desc ? L"  ▾" : L"  ▴") : L""), [this, kk] { sort_by(kk); }, true, sort_key == key);
    }
    m.sub(L"Sort by", sm);
    if (!folder.empty()) {
        Menu cm(m, 0);
        color_menu(cm, {folder});
        m.sub(L"Colour of this folder", cm);
    }
    Menu dm(m, 0);
    default_color_menu(dm);
    m.sub(L"Default folder colour", dm);
    m.sep();
    m.item(L"Refresh\tCtrl+R", [this] { refresh(); });
    m.item(L"Show hidden files\tCtrl+H", [this] { show_hidden = !show_hidden; refresh(); save_state(); }, true, show_hidden);
    if (!undo.empty()) m.item(L"Undo rename\tCtrl+Z", [this] { undo_rename(); });
    if (!folder.empty()) {
        m.sep();
        m.item(L"Open terminal here", [folder] { open_terminal(folder); });
        m.item(L"Open in Windows Explorer", [folder] { open_in_explorer(folder, false); });
        m.item(L"Properties", [this, folder] { show_properties(folder, hwnd); });
    }
    m.show(hwnd, pt);
}

// ======================================================================
// colours (themes): switched right away, nothing restarts
// ======================================================================
void App::theme_menu() {
    Menu m;
    for (auto& name : theme_names()) {
        Theme t = theme_by_name(name);
        int w = S(34), h = S(14);
        Bitmap sw(w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                COLORREF c = (y < S(3) || y >= h - S(3) || x < S(3) || x >= w - S(3)) ? t.bg : mix(t.accent, t.accent2, (double)(x - S(3)) / std::max(1, w - S(7)));
                sw.bits[y * w + x] = 0xFF000000u | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
            }
        wstr n = name;
        m.item(name, [this, n] { set_app_theme(n); }, true, name == theme_name, menu_bitmap(sw));
    }
    RECT r = toolbar.button_rect(Toolbar::B_THEME);
    POINT p{r.right, r.bottom};
    ClientToScreen(toolbar.hwnd, &p);
    m.show(hwnd, p, true);
}


void App::set_app_theme(const wstr& name) {
    if (name == theme_name) return;
    theme_name = name;
    set_theme(name);
    clear_art_cache();
    reset_brushes();
    use_dark_menus(!T.light);
    redraw_all();
    save_state();
    say(L"Colours: " + name, 3);
}

// ======================================================================
// two panes
// ======================================================================
void App::toggle_dual() {
    if (pane2_open) {
        pane2_open = false;
        pane2->show(false);
        set_active(nullptr);
        layout();
        fill(true);
        focus_view();
    } else {
        wstr start = pane2->has_path ? pane2->path : pane2_saved_valid ? pane2_saved_path : real_folder();
        if (start.empty() && !pane2_saved_valid) start = home_dir();
        pane2_open = true;
        layout();
        pane2->show(true);
        pane2->load(start);
        fill(true);
        set_active(nullptr);
        say(L"Two panes: F5 copies to the other side, F6 moves, Tab switches, F3 closes.", 6);
    }
    show_view();
    save_state();
}

void App::set_active(FileView* v) {
    if (active_view == v) return;
    active_view = v;
    invalidate();
    if (pane2) pane2->invalidate();
    update_preview(v && pane2 ? (QLSource*)pane2 : (QLSource*)this);
}

// ======================================================================
// tab sets
// ======================================================================
void App::sets_menu(POINT pt) {
    Menu m;
    if (tab_sets.empty()) m.disabled(L"No tab sets yet — save the tabs you have open to make one");
    for (size_t i = 0; i < tab_sets.size(); i++) {
        auto& s = tab_sets[i];
        int n = (int)s.tabs.size(), idx = (int)i;
        m.item(s.name + fmt(L"   (%d tab%s)", n, n == 1 ? L"" : L"s"), [this, idx] { open_tab_set(idx); }, true, s.name == current_set);
    }
    m.sep();
    m.item(L"Save these tabs as a new set…\tCtrl+Shift+S", [this] { save_tab_set(); });
    if (!tab_sets.empty()) {
        Menu add(m, 0), upd(m, 0), ren(m, 0), del(m, 0);
        for (size_t i = 0; i < tab_sets.size(); i++) {
            int idx = (int)i;
            wstr n = tab_sets[i].name;
            add.item(n, [this, idx] { open_tab_set(idx, false); });
            upd.item(n, [this, n] { save_tab_set(n); });
            ren.item(n, [this, idx] { rename_tab_set(idx); });
            del.item(n, [this, idx] { delete_tab_set(idx); });
        }
        m.sub(L"Add a set to these tabs", add);
        m.sub(L"Update a set with these tabs", upd);
        m.sub(L"Rename a set", ren);
        m.sub(L"Delete a set", del);
    }
    m.show(hwnd, pt);
}

void App::save_tab_set(const wstr& name0) {
    wstr name = name0;
    if (name.empty()) {
        name = current_set;
        if (!ask_text(hwnd, L"Save tab set", fmt(L"A name for these %d tab%s (e.g. School, Project):", (int)tabs.size(), tabs.size() == 1 ? L"" : L"s"), name)) return;
    }
    name = trim(name).substr(0, 40);
    if (name.empty()) return;
    TabSet s;
    s.name = name;
    s.active = active;
    for (auto& t : tabs) {
        wstr p = t.path, k, a, b;
        parse_virtual(p, k, a, b);
        if (k == L"search") p = a;
        s.tabs.push_back({p, t.view < 0 ? default_view : t.view});
    }
    bool updated = false;
    for (auto& x : tab_sets) if (lower(x.name) == lower(name)) { x = s; updated = true; }
    if (!updated) tab_sets.push_back(s);
    current_set = name;
    tabbar.invalidate();
    save_state();
    say((updated ? L"Updated the tab set “" + name + L"”" : fmt(L"Saved the tab set “%s” (%d tabs)", name.c_str(), (int)tabs.size())) +
        L" — open it from Tab sets in the tab bar", 6);
}

void App::open_tab_set(int i, bool replace) {
    if (i < 0 || i >= (int)tab_sets.size()) return;
    TabSet s = tab_sets[i];
    std::vector<Tab> list;
    int missing = 0;
    for (auto& [p, v] : s.tabs) {
        if (p.empty() || is_virtual(p) || is_dir(p)) {
            Tab t;
            t.path = p;
            t.view = v;
            list.push_back(t);
        } else missing++;
    }
    if (list.empty()) { say(L"None of the folders in “" + s.name + L"” are there any more.", 8); return; }
    remember_view();
    if (replace) {
        tabs = list;
        active = std::max(0, std::min(s.active, (int)tabs.size() - 1));
        current_set = s.name;
    } else {
        active = (int)tabs.size();
        tabs.insert(tabs.end(), list.begin(), list.end());
    }
    load(cur().path);
    save_state();
    say(wstr(replace ? L"Opened “" : L"Added “") + s.name + fmt(L"” — %d tab%s", (int)list.size(), list.size() == 1 ? L"" : L"s") +
        (missing ? fmt(L" (%d folder%s not found)", missing, missing > 1 ? L"s" : L"") : L""), 6);
}

void App::rename_tab_set(int i) {
    if (i < 0 || i >= (int)tab_sets.size()) return;
    wstr name = tab_sets[i].name;
    if (!ask_text(hwnd, L"Rename tab set", L"New name:", name)) return;
    name = trim(name).substr(0, 40);
    if (name.empty() || name == tab_sets[i].name) return;
    for (size_t j = 0; j < tab_sets.size(); j++)
        if ((int)j != i && lower(tab_sets[j].name) == lower(name)) { say(L"There's already a tab set called “" + name + L"”.", 6); return; }
    if (current_set == tab_sets[i].name) current_set = name;
    tab_sets[i].name = name;
    tabbar.invalidate();
    save_state();
}

void App::delete_tab_set(int i) {
    if (i < 0 || i >= (int)tab_sets.size()) return;
    wstr name = tab_sets[i].name;
    if (!confirm(hwnd, L"Delete tab set", L"Delete the tab set “" + name + L"”?\n\nOnly the saved list goes — your folders and open tabs stay.", L"Delete")) return;
    tab_sets.erase(tab_sets.begin() + i);
    if (current_set == name) current_set.clear();
    tabbar.invalidate();
    save_state();
}

// ======================================================================
// Quick Look
// ======================================================================
void App::quick_look(QLSource* src) {
    if (!ql) { ql = new QuickLook(); ql->init(this); }
    if (ql->is_open() && ql->source == src) { ql->close_ql(); return; }
    if (!src->ql_entry()) { say(L"Select something to preview.", 3); return; }
    ql->show_for(src);
}
