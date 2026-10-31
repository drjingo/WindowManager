// The file view: one custom-drawn control for List, Grid and the columns of Columns view.
// Selection, keyboard, type-to-jump, hover, rubber-band selection, inline rename, and real drag & drop.
#pragma once
#include "fs.h"
#include "ui.h"

enum ViewMode { VM_LIST, VM_GRID, VM_COLUMN };

struct RowInfo {
    std::vector<COLORREF> tags;  // dot colours
    wstr tags_text, type, location;
    std::string folder_color = "yellow";
    bool cut = false, pending_size = false;
};

struct Column {
    std::string key;  // name, modified, type, size, tags, location
    wstr title;
    int width;        // at 96 dpi (name: minimum)
    bool right = false;
};

class FileView;
struct FileViewHost {
    virtual ~FileViewHost() = default;
    virtual void fv_open(FileView*, int i) = 0;
    virtual void fv_menu(FileView*, int i, POINT screen) = 0;
    virtual void fv_selection(FileView*) {}
    virtual void fv_middle(FileView*, int i) {}
    virtual bool fv_key(FileView*, UINT vk, bool ctrl, bool shift, bool alt) { return false; }
    virtual void fv_sort(FileView*, const std::string& key) {}
    virtual void fv_rename(FileView*, int i, const wstr& name) {}
    virtual void fv_row(FileView*, const Entry& e, RowInfo& out) {}
    virtual BitmapPtr fv_thumb(FileView*, const Entry& e, int size) { return nullptr; }
    virtual int fv_icon(FileView*, Entry& e) { return -1; }
    virtual wstr fv_folder(FileView*) { return L""; }  // dropping on empty space goes here
    virtual bool fv_can_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) { return true; }
    virtual void fv_drop(FileView*, const wstr& dest, const std::vector<wstr>& paths, DWORD effect) {}
    virtual void fv_zoom(FileView*, int dir) {}
    virtual void fv_focus(FileView*) {}
    virtual wstr fv_empty(FileView*) { return L""; }
    virtual void fv_clicked(FileView*, int i, WPARAM keys) {}  // a plain click finished (Columns view)
};

class FileView : public Control {
  public:
    FileViewHost* host = nullptr;
    ViewMode mode = VM_LIST;
    int grid_size = 64;
    std::vector<Column> columns;
    std::string sort_key = "name";
    bool sort_desc = false;
    bool show_header = true;
    bool active_mark = false;  // draw the "this side is active" line (two panes)

    Entries items;
    std::vector<char> sel;
    int focus = -1, anchor = -1, hover = -1, drop_hl = -1;

    ~FileView() override;
    bool create_view(HWND parent, FileViewHost* h, ViewMode m);
    // new contents; keeps the selection (by path) unless told otherwise
    void set_items(Entries e, bool keep_selection = true, bool keep_scroll = true);
    std::vector<int> selected() const;
    std::vector<EntryPtr> selected_entries() const;
    int count_selected() const;
    void select_only(int i, bool notify = true);
    void select_paths(const std::vector<wstr>& paths, bool by_name = false);
    void select_all();
    void clear_selection();
    void set_focus_index(int i);
    void ensure_visible(int i);
    int index_of(const wstr& path) const;
    void start_rename(int i);
    bool renaming() const { return edit_ != nullptr; }
    void cancel_rename();
    void step(UINT vk, bool shift);  // keyboard moves (also used by Quick Look)
    void scroll_to_top() { scroll_ = 0; invalidate(); }
    double scroll_fraction() const;
    void set_scroll_fraction(double f);
    int cols() const { return cols_; }
    RECT name_rect(int i);  // where the name is (client coordinates)
    int hit(int x, int y, bool* on_name = nullptr) const;

    void on_paint(HDC dc, const RECT& rc) override;
    void on_size(int w, int h) override;
    void on_mouse_move(int x, int y, WPARAM keys) override;
    void on_mouse_leave() override;
    void on_lbutton_down(int x, int y, WPARAM keys) override;
    void on_lbutton_up(int x, int y, WPARAM keys) override;
    void on_lbutton_dbl(int x, int y, WPARAM keys) override;
    void on_rbutton_up(int x, int y, WPARAM keys) override;
    void on_mbutton_down(int x, int y, WPARAM keys) override;
    bool on_wheel(int delta, WPARAM keys, int x, int y) override;
    bool on_key(UINT vk, bool ctrl, bool shift, bool alt) override;
    void on_char(wchar_t c) override;
    void on_timer(UINT_PTR id) override;
    void on_focus(bool got) override;
    void on_capture_lost() override;
    LRESULT on_message(UINT m, WPARAM w, LPARAM l) override;

  private:
    int scroll_ = 0;
    int cols_ = 1;
    ScrollBar sb_;
    DropTarget* drop_ = nullptr;
    // mouse
    enum Drag { D_NONE, D_PRESS, D_BAND, D_HEADER, D_SCROLL };
    Drag drag_ = D_NONE;
    POINT press_{};
    int press_item_ = -1;
    bool deferred_ = false;
    WPARAM press_keys_ = 0;
    // rubber band (content coordinates)
    POINT band0_{}, band1_{};
    std::vector<char> band_base_;
    bool band_moved_ = false;
    int resize_col_ = -1, resize_x0_ = 0, resize_w0_ = 0;
    // type to jump
    wstr typed_;
    DWORD typed_at_ = 0;
    // rename
    HWND edit_ = nullptr;
    int edit_item_ = -1;

    int header_h() const;
    int row_h() const;
    int icon_px() const;
    void tile(int& tw, int& th) const;
    int line_h() const;
    int content_height() const;
    int view_height() const;
    void item_rect(int i, RECT& r) const;  // client coordinates
    void layout();
    void update_scrollbar();
    std::vector<int> col_x() const;  // list column left edges (+ the end)
    int header_hit_resize(int x) const;
    std::string header_hit(int x) const;
    void paint_list(HDC dc, const RECT& rc);
    void paint_grid(HDC dc, const RECT& rc);
    void paint_icon(HDC dc, Entry& e, const RowInfo& ri, int x, int y, int size);
    void paint_dots(HDC dc, const RowInfo& ri, int x, int y, int size);
    void click_select(int i, WPARAM keys);
    void band_update();
    void notify();
    void finish_rename(bool keep);
    void scroll_by(int dy);
    std::vector<wstr> wrap(HDC dc, const wstr& name, int width) const;
    friend LRESULT CALLBACK fv_edit_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
};
