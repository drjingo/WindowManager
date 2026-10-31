// Window chrome: tab bar, toolbar (with the clickable address bar and the search box), sidebar, status bar.
#pragma once
#include "fileview.h"

class App;

class TabBar : public Control {
  public:
    COLORREF glass_color() const override { return T.panel; }
    App* app = nullptr;
    ~TabBar() override;
    void init(App* a, HWND parent);
    void on_paint(HDC dc, const RECT& rc) override;
    void on_mouse_move(int x, int y, WPARAM) override;
    void on_mouse_leave() override;
    void on_lbutton_down(int x, int y, WPARAM) override;
    void on_lbutton_dbl(int x, int y, WPARAM) override;
    void on_rbutton_up(int x, int y, WPARAM) override;
    void on_mbutton_down(int x, int y, WPARAM) override;
    void on_timer(UINT_PTR id) override;
    int ideal_height() const { return S(38); }
  private:
    struct Hit { int tab = -1; bool close = false, plus = false, sets = false; };
    std::vector<RECT> rects_;
    RECT plus_{}, sets_{};
    int hover_tab_ = -1;
    bool hover_close_ = false, hover_plus_ = false, hover_sets_ = false;
    int drop_tab_ = -1;
    DropTarget* drop_ = nullptr;
    void compute(HDC dc);
    Hit hit(int x, int y);
};

class Breadcrumb : public Control {
  public:
    COLORREF glass_color() const override { return T.bg; }
    std::function<void(const wstr&)> navigate;
    std::function<void()> escape;
    std::function<void(const wstr&)> middle;
    std::function<bool()> show_hidden;
    std::function<bool(const wstr& dest, const std::vector<wstr>& paths, DWORD effect)> can_drop;
    std::function<void(const wstr& dest, const std::vector<wstr>& paths, DWORD effect)> dropped;
    wstr path;
    ~Breadcrumb() override;
    void init(HWND parent);
    void set_path(const wstr& p);
    void edit();
    void stop_edit();
    bool editing() const { return editing_; }
    void on_paint(HDC dc, const RECT& rc) override;
    void on_size(int w, int h) override;
    void on_mouse_move(int x, int y, WPARAM) override;
    void on_mouse_leave() override { hot_ = -1; invalidate(); }
    void on_lbutton_down(int x, int y, WPARAM) override;
    void on_mbutton_down(int x, int y, WPARAM) override;
  private:
    struct Seg { wstr label, path; bool arrow; RECT r{}, ar{}; bool hidden_menu = false; };
    std::vector<Seg> segs_;
    std::vector<Seg> folded_;
    int hot_ = -1;  // index*2 (+1 for the arrow)
    bool editing_ = false;
    TextBox box_;
    DropTarget* drop_ = nullptr;
    int drop_seg_ = -1;
    void compute(HDC dc);
    void menu_for(int i);
};

class SearchBox : public TextBox {
  public:
    std::function<void()> search;  // the ⌕ was clicked
    void on_paint(HDC dc, const RECT& rc) override;
    void on_size(int w, int h) override;
    void on_lbutton_down(int x, int y, WPARAM) override;
    void on_mouse_move(int x, int y, WPARAM) override;
    void on_mouse_leave() override { invalidate(); }
};

class Toolbar : public Control {
  public:
    COLORREF glass_color() const override { return T.bg; }
    enum Btn { B_BACK, B_FWD, B_UP, B_REFRESH, B_STAR, B_LIST, B_GRID, B_COLUMNS, B_DUAL, B_PREVIEW, B_THEME, B_COUNT };
    App* app = nullptr;
    Breadcrumb crumb;
    SearchBox search;
    bool enabled[B_COUNT];
    bool active[B_COUNT];
    bool star_on = false;
    void init(App* a, HWND parent);
    void on_paint(HDC dc, const RECT& rc) override;
    void on_size(int w, int h) override;
    void on_mouse_move(int x, int y, WPARAM) override;
    void on_mouse_leave() override;
    void on_lbutton_down(int x, int y, WPARAM) override;
    void on_timer(UINT_PTR id) override;
    int ideal_height() const { return S(52); }
    RECT button_rect(Btn b) const { return rects_[b]; }
  private:
    RECT rects_[B_COUNT]{};
    std::vector<RECT> groups_;
    int hot_ = -1, tip_for_ = -1;
    int hit(int x, int y) const;
};

class Sidebar : public Control {
  public:
    COLORREF glass_color() const override { return T.panel; }
    App* app = nullptr;
    ~Sidebar() override;
    void init(App* a, HWND parent);
    void rebuild();          // all sections
    void toggle_section(const wstr& name);
    void set_all_sections(bool fold);
    void set_current(const wstr& path) { current_ = path; invalidate(); }
    void on_paint(HDC dc, const RECT& rc) override;
    void on_size(int w, int h) override;
    void on_mouse_move(int x, int y, WPARAM keys) override;
    void on_mouse_leave() override;
    void on_lbutton_down(int x, int y, WPARAM keys) override;
    void on_lbutton_up(int x, int y, WPARAM keys) override;
    void on_rbutton_up(int x, int y, WPARAM keys) override;
    void on_mbutton_down(int x, int y, WPARAM keys) override;
    bool on_wheel(int delta, WPARAM keys, int x, int y) override;
    void on_capture_lost() override;
  private:
    enum Kind { K_HEADER, K_FAV, K_RECENT, K_TAG, K_NEWTAG, K_FOLDER, K_DRIVE, K_HINT };
    struct Row { Kind kind; wstr text, path, extra; int index = -1; COLORREF dot = 0; };
    std::vector<Row> rows_;
    wstr current_;
    int scroll_ = 0, hover_ = -1, drop_row_ = -1;
    ScrollBar sb_;
    bool sb_drag_ = false;
    int drag_fav_ = -1, drag_over_ = -1;
    POINT press_{};
    bool drag_moved_ = false;
    DropTarget* drop_ = nullptr;
    int row_h(const Row& r) const;
    int row_at(int y) const;
    int row_top(int i) const;
    int total_h() const;
};

class StatusBar : public Control {
  public:
    COLORREF glass_color() const override { return T.panel; }
    wstr text_;
    bool message_ = false;
    void set(const wstr& t, bool message = false) { text_ = t; message_ = message; invalidate(); }
    void on_paint(HDC dc, const RECT& rc) override;
    int ideal_height() const { return S(28); }
};
