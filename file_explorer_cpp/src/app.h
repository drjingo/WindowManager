// The main window and everything it knows: tabs, favourites, tags, recent folders, folder colours, the listing,
// search, folder sizes, copy / move / delete / rename, zip, two panes, Quick Look, tab sets, themes.
#pragma once
#include "chrome.h"
#include <atomic>
#include <deque>
#include <set>

enum View { V_LIST = 0, V_GRID = 1, V_COLUMNS = 2 };
extern const char* VIEW_KEYS[3];

struct Tab {
    wstr path;
    int view = -1;  // -1: the default view
    std::vector<wstr> back, fwd, select;
    double scroll = 0;
    wstr filter;
};
struct Fav { wstr name, path; };
struct TagDef { wstr name; std::string color; };
struct Visit { wstr path; double n = 0, t = 0; };
struct TabSet { wstr name; std::vector<std::pair<wstr, int>> tabs; int active = 0; };
struct ColorRec { wstr path; std::string color; bool sub = false; };  // sub: subfolders get it too
struct TagRec { wstr path; std::vector<wstr> tags; };
struct SearchState {
    int token = -1, found = 0, dirs = 0;
    bool done = true, stopped = false, full = false, everything = false;
    wstr query, root;
    double start = 0;
};

// something Quick Look can show (the main view or the right-hand pane)
struct QLSource {
    virtual ~QLSource() = default;
    virtual EntryPtr ql_entry() = 0;
    virtual void ql_step(UINT vk) = 0;
    virtual void ql_focus() = 0;
    virtual void ql_open(EntryPtr e) = 0;
};

class ColumnsView;
class SecondPane;
class QuickLook;

class App : public Control, public FileViewHost, public QLSource {
  public:
    App();
    ~App() override;
    bool start(const wstr& start_path);

    // ---- saved state
    std::vector<Tab> tabs;
    int active = 0;
    std::vector<Fav> favorites;
    std::map<wstr, Visit> visits;          // norm path →
    std::vector<TagDef> tag_defs;
    std::map<wstr, TagRec> file_tags;      // norm path →
    std::map<wstr, ColorRec> folder_colors;
    int colors_with_sub = 0;  // how many have sub (0: no need to look at parent folders)
    void count_color_subs();
    std::string default_folder_color = "yellow";
    bool color_in_explorer = false, show_hidden = false, calc_sizes = true;
    std::string sort_key = "name";
    bool sort_desc = false;
    int default_view = V_LIST, grid_size = 64;
    wstr theme_name;
    std::vector<TabSet> tab_sets;
    wstr current_set;
    int sidebar_w = 230;
    std::set<wstr> collapsed;
    bool preview_open = false;
    bool use_everything = true;  // search with voidtools Everything when it is running
    int preview_w = 340;  // at 96 dpi  // sidebar sections you folded ("TAGS" …)
    bool pane2_open = false;
    wstr pane2_saved_path;
    bool pane2_saved_valid = false;
    RECT ql_rect{};
    Json extra_state;  // keys we don't use (kept as they are)

    // ---- the listing
    Entries entries;
    std::atomic<int> token{0};
    std::atomic<int> search_cancel{-1};
    bool loading = false;
    SearchState search;
    std::map<wstr, std::pair<i64, double>> dir_sizes;  // norm path → (bytes, when)
    std::set<wstr> sizing;
    std::set<wstr> cut_paths;
    std::vector<std::pair<wstr, wstr>> undo;  // last rename(s): (new, old)

    // ---- widgets
    TabBar tabbar;
    Toolbar toolbar;
    Sidebar sidebar;
    FileView view;
    StatusBar status;
    ColumnsView* columns = nullptr;
    SecondPane* pane2 = nullptr;
    QuickLook* ql = nullptr;
    QuickLook* preview = nullptr;  // the preview panel (Alt+P)
    Watcher watcher;
    FileView* active_view = nullptr;  // the side you're on (two panes)

    Tab& cur() { return tabs[active]; }
    FileView* main_fv();  // the view that holds the selection (List / Grid, or the last column)
    int view_mode();
    wstr real_folder();  // the folder you're in (not This PC / search / tag)

    // ---- layout & window
    LRESULT on_message(UINT m, WPARAM w, LPARAM l) override;
    void on_size(int w, int h) override;
    void on_paint(HDC dc, const RECT& rc) override;
    void on_close() override;
    void on_mouse_move(int x, int y, WPARAM keys) override;
    void on_lbutton_down(int x, int y, WPARAM keys) override;
    void on_lbutton_up(int x, int y, WPARAM keys) override;
    void on_timer(UINT_PTR id) override;
    void layout();
    void open_from_other(const wstr& path);  // a second FileExplorer.exe handed us a folder
    bool pre_key(MSG& msg);  // shortcuts, before the focused control sees the key
    void say(const wstr& text, int seconds = 6);
    void update_status();
    void redraw_all();

    // ---- state
    void load_state();
    void save_state();

    // ---- navigation
    void navigate(const wstr& path, const std::vector<wstr>& select = {});
    void load(const wstr& path, const std::vector<wstr>& select = {}, bool keep_scroll = false);
    void show_listing(int tok, const wstr& path, Entries list, DWORD error, double mtime);
    void fill(bool keep_scroll = true);
    void go_back();
    void go_forward();
    void go_up();
    void refresh();
    void new_tab(const wstr& path);
    void switch_tab(int i);
    void close_tab(int i);
    void close_others(int i);
    void remember_view();
    void set_view(int v);
    void show_view();
    void focus_view();
    Entries sorted(const Entries& in);
    void sort_by(const std::string& key);

    // ---- filter & search
    void apply_filter();
    void start_search(const wstr& text = L"\x01");
    void stop_search_cmd();
    bool stop_search();
    void filter_escape();
    void search_found(int tok, Entries batch, int dirs, bool done);
    wstr location_of(const Entry& e);
    wstr empty_text();

    // ---- folder sizes
    void start_sizes(int tok);
    void toggle_sizes();

    // ---- favourites, recent, tags, colours
    bool is_favorite(const wstr& p);
    void add_favorite(const wstr& p);
    void remove_favorite(int i);
    void rename_favorite(int i);
    void move_favorite(int i, int j);
    void toggle_favorite();
    void favorites_changed(const wstr& msg = L"");
    void record_visit(const wstr& p);
    static double frecency(const Visit& v, double now);
    std::vector<wstr> recent_folders(size_t k = 7);
    void forget_recent(const wstr& p);  // "" = all
    std::vector<wstr> tags_of(const wstr& p);
    COLORREF tag_color(const wstr& name);
    std::map<wstr, int> tag_counts();
    void tag_menu(Menu& m, const std::vector<wstr>& paths);
    void toggle_tag(const std::vector<wstr>& paths, const wstr& name);
    void add_tag(const std::vector<wstr>& paths, const wstr& name, bool quiet = false);
    void clear_tags(const std::vector<wstr>& paths);
    void new_tag(const std::vector<wstr>& paths = {});
    void edit_tag(int i);
    void move_tag(int i, int d);
    void delete_tag(int i);
    void tags_changed();
    std::string folder_color(const wstr& p);
    bool inherits_color(const wstr& p);  // gets a parent folder's colour
    void color_menu(Menu& m, const std::vector<wstr>& paths);
    void default_color_menu(Menu& m);
    void set_folder_color(const std::vector<wstr>& paths, const std::string& color, int sub = -1);  // sub -1: keep
    void set_color_subfolders(const std::vector<wstr>& paths, bool on);
    void folder_color_more(const std::vector<wstr>& paths);
    void toggle_color_in_explorer();
    void colors_changed();

    // ---- files
    std::vector<EntryPtr> selection();
    std::vector<wstr> selected_paths();
    void open_entry(EntryPtr e);
    void open_selected();
    void copy_paths(const std::vector<wstr>& paths, bool cut);
    void paste(const wstr& into = L"");
    wstr transfer_problem(const std::vector<wstr>& paths, const wstr& dest, bool move);
    bool transfer(const std::vector<wstr>& paths, const wstr& dest, bool move);
    void copy_across(bool move, bool from_right);
    void delete_selected(bool permanent);
    void delete_paths(const std::vector<wstr>& paths, bool permanent, const std::vector<wstr>& select = {},
                      const wstr& select_in = L"");
    void run_op(FileOp op, std::vector<wstr> paths, wstr dest, unsigned flags, wstr done_text,
                std::vector<wstr> select, wstr select_in, std::vector<std::pair<wstr, wstr>> remap);
    void reload_panes(const std::vector<wstr>& select = {}, const wstr& select_in = L"");
    void remap_paths(const std::vector<std::pair<wstr, wstr>>& pairs);
    void new_folder(const wstr& in = L"");
    void start_rename();
    bool rename_paths(std::vector<std::pair<wstr, wstr>> pairs, bool is_undo = false);
    void bulk_rename(const std::vector<EntryPtr>& entries);
    void undo_rename();
    void add_archive_items(Menu& m, const std::vector<EntryPtr>& sel, const wstr& folder);
    void compress(const std::vector<wstr>& paths, const wstr& folder, const wstr& name);
    void extract(const wstr& archive, const wstr& folder, bool own_folder);
    void copy_text(const wstr& t);

    // ---- menus
    void item_menu(int i, POINT pt);
    void background_menu(POINT pt);
    void theme_menu();
    void set_app_theme(const wstr& name);

    // ---- two panes
    void toggle_dual();
    void set_active(FileView* v);

    // ---- preview panel
    void toggle_preview();
    void update_preview(QLSource* src);

    // ---- tab sets
    void sets_menu(POINT pt);
    void save_tab_set(const wstr& name = L"");
    void open_tab_set(int i, bool replace = true);
    void rename_tab_set(int i);
    void delete_tab_set(int i);

    // ---- Quick Look
    void quick_look(QLSource* src);
    EntryPtr ql_entry() override;
    void ql_step(UINT vk) override;
    void ql_focus() override { focus_view(); }
    void ql_open(EntryPtr e) override { open_entry(e); }

    // ---- FileViewHost (the main view)
    void fv_open(FileView*, int i) override;
    void fv_menu(FileView*, int i, POINT screen) override;
    void fv_selection(FileView*) override;
    void fv_middle(FileView*, int i) override;
    bool fv_key(FileView*, UINT vk, bool ctrl, bool shift, bool alt) override;
    void fv_sort(FileView*, const std::string& key) override { sort_by(key); }
    void fv_rename(FileView*, int i, const wstr& name) override;
    void fv_row(FileView*, const Entry& e, RowInfo& out) override;
    BitmapPtr fv_thumb(FileView*, const Entry& e, int size) override;
    int fv_icon(FileView*, Entry& e) override;
    wstr fv_folder(FileView*) override { return real_folder(); }
    bool fv_can_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) override;
    void fv_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) override;
    void fv_zoom(FileView*, int dir) override;
    void fv_focus(FileView* v) override { set_active(nullptr); }
    wstr fv_empty(FileView*) override { return empty_text(); }

    // shared helpers for the other views
    void row_info(const Entry& e, RowInfo& out, bool with_location);
    BitmapPtr thumb(const wstr& path, int size, std::function<void()> arrived);
    int icon_index(Entry& e, std::function<void()> arrived);

  private:
    std::map<wstr, BitmapPtr> thumbs_;
    std::deque<wstr> thumb_order_;
    std::set<wstr> thumb_pending_;
    std::map<wstr, int> icon_ext_;  // ".pdf" → index
    std::map<wstr, int> icon_path_;
    std::set<wstr> icon_pending_;
    int split_drag_ = 0;            // 1 sidebar, 2 the two panes
    int split_x0_ = 0, split_w0_ = 0;
    double pane2_frac_ = 0.5;
    RECT body_{}, area_{}, preview_rc_{};
    UINT_PTR msg_timer_ = 0;
    ULONGLONG msg_hold_ = 0;  // a message stays at least until then (a reload right after won't wipe it)
    std::vector<wstr> pending_select_;
    wstr pending_rename_;
};

extern App* g_app;
constexpr ULONG_PTR COPYDATA_OPEN = 0x46454F50;  // "FEOP"
extern const wchar_t* INSTANCE_PROP;
void pane2_navigate(App* a, const wstr& path);
