// The right-hand pane (F3) and the Columns view (like macOS Finder).
#pragma once
#include "app.h"

class SecondPane : public Control, public FileViewHost, public QLSource {
  public:
    App* app = nullptr;
    wstr path;
    bool has_path = false;
    std::vector<wstr> back, fwd;
    Entries entries;
    FileView view;
    Breadcrumb crumb;
    Watcher watcher;
    int token = 0;
    std::vector<wstr> select_names;

    void init(App* a, HWND parent);
    void navigate(const wstr& p, const std::vector<wstr>& select = {});
    void load(const wstr& p, const std::vector<wstr>& select = {});
    void reload();
    void fill();
    void go_back();
    void go_up();
    std::vector<EntryPtr> selection() { return view.selected_entries(); }
    void update_info();

    void on_paint(HDC dc, const RECT& rc) override;
    void on_size(int w, int h) override;
    void on_mouse_move(int x, int y, WPARAM) override;
    void on_mouse_leave() override { hot_ = -1; invalidate(); }
    void on_lbutton_down(int x, int y, WPARAM) override;

    // FileViewHost
    void fv_open(FileView*, int i) override;
    void fv_menu(FileView*, int i, POINT screen) override;
    void fv_selection(FileView*) override;
    void fv_middle(FileView*, int i) override;
    bool fv_key(FileView*, UINT vk, bool ctrl, bool shift, bool alt) override;
    void fv_sort(FileView*, const std::string& key) override { app->sort_by(key); }
    void fv_rename(FileView*, int i, const wstr& name) override;
    void fv_row(FileView*, const Entry& e, RowInfo& out) override { app->row_info(e, out, false); }
    int fv_icon(FileView*, Entry& e) override;
    wstr fv_folder(FileView*) override { return has_path ? path : L""; }
    bool fv_can_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) override;
    void fv_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) override;
    void fv_focus(FileView*) override { app->set_active(&view); }
    wstr fv_empty(FileView*) override { return path.empty() ? L"" : L"This folder is empty"; }

    // Quick Look
    EntryPtr ql_entry() override;
    void ql_step(UINT vk) override { view.step(vk == VK_LEFT ? VK_UP : vk == VK_RIGHT ? VK_DOWN : vk, false); }
    void ql_focus() override { SetFocus(view.hwnd); }
    void ql_open(EntryPtr e) override { if (e->dir) navigate(e->path); else app->open_entry(e); }

    wstr info_;
  private:
    RECT b_back_{}, b_up_{}, b_close_{};
    int hot_ = -1;
};

class ColumnsView : public Control, public FileViewHost {
  public:
    App* app = nullptr;
    struct Col {
        wstr folder;
        bool mirror = false;
        std::unique_ptr<FileView> fv;
        double mtime = -2;
    };
    std::vector<Col> cols;
    FileView* mirror() { return cols.empty() ? nullptr : cols.back().fv.get(); }

    void init(App* a, HWND parent);
    void rebuild();          // after the folder or its listing changed
    void sync_selection();   // the main selection changed
    void refresh_preview() { invalidate(); }

    void on_paint(HDC dc, const RECT& rc) override;
    void on_size(int w, int h) override { offset_ = std::min(max_offset(), std::max(0, (int)cols.size() - 1) * (col_w() + 1)); place(); }
    bool on_wheel(int delta, WPARAM keys, int x, int y) override;
    void on_lbutton_down(int x, int y, WPARAM) override;
    void on_mouse_move(int x, int y, WPARAM) override;
    void on_lbutton_up(int, int, WPARAM) override;
    void on_timer(UINT_PTR id) override;

    // FileViewHost (all the columns)
    void fv_open(FileView*, int i) override;
    void fv_menu(FileView*, int i, POINT screen) override;
    void fv_selection(FileView*) override;
    void fv_middle(FileView*, int i) override;
    bool fv_key(FileView*, UINT vk, bool ctrl, bool shift, bool alt) override;
    void fv_rename(FileView* v, int i, const wstr& name) override { app->fv_rename(v, i, name); }
    void fv_row(FileView*, const Entry& e, RowInfo& out) override { app->row_info(e, out, false); }
    int fv_icon(FileView* v, Entry& e) override { return app->fv_icon(v, e); }
    wstr fv_folder(FileView* v) override;
    bool fv_can_drop(FileView* v, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) override { return app->fv_can_drop(v, dest, paths, effect); }
    void fv_drop(FileView* v, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) override { app->fv_drop(v, dest, paths, effect); }
    void fv_focus(FileView*) override { app->set_active(nullptr); }
    void fv_clicked(FileView* v, int i, WPARAM keys) override;
    wstr fv_empty(FileView* v) override;

  private:
    int offset_ = 0;
    std::map<wstr, std::pair<double, Entries>> cache_;
    std::vector<wstr> chain_;
    wstr pending_open_;
    ScrollBar hsb_;
    bool hdrag_ = false;
    int hdrag_x_ = 0, hdrag_off_ = 0;
    int content_w() const;
    int max_offset() const;
    void place();
    Entries listing(const wstr& folder, double& mt);
    int col_w() const { return S(250); }
    int preview_w() const { return S(300); }
};
