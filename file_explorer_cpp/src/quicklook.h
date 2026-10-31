// Quick Look (Space): pictures, videos, PDFs (Windows thumbnails), text and code with colours, what's inside a
// zip, folders. Space / Esc close, arrows go to the next item, Enter opens.
#pragma once
#include "app.h"

class QuickLook : public Control {
  public:
    App* app = nullptr;
    QLSource* source = nullptr;
    EntryPtr entry;

    bool embedded = false;  // the preview panel inside the main window (not a window of its own)
    void init(App* a);
    void init_child(App* a, HWND parent);
    void clear(const wstr& msg);
    void show_for(QLSource* src);
    void close_ql();
    bool is_open() const { return hwnd && IsWindowVisible(hwnd); }
    void render(EntryPtr e);
    void selection_changed();  // the source's selection moved

    void on_paint(HDC dc, const RECT& rc) override;
    void on_size(int w, int h) override;
    bool on_key(UINT vk, bool ctrl, bool shift, bool alt) override;
    void on_close() override { close_ql(); }
    bool on_wheel(int delta, WPARAM keys, int x, int y) override;
    void on_lbutton_down(int x, int y, WPARAM) override;
    void on_mouse_move(int x, int y, WPARAM) override;
    void on_lbutton_up(int, int, WPARAM) override;
    void on_timer(UINT_PTR id) override;
    LRESULT on_message(UINT m, WPARAM w, LPARAM l) override;

  private:
    enum Mode { M_NONE, M_PICTURE, M_TEXT, M_ARCHIVE, M_FOLDER, M_MESSAGE };
    Mode mode_ = M_NONE;
    int token_ = 0;
    wstr info_, message_, folder_info_;
    BitmapPtr picture_;
    bool picture_is_icon_ = false;
    // text
    std::vector<wstr> lines_;
    std::vector<std::vector<Span>> line_spans_;  // per line, start relative to the line
    wstr text_;
    bool wrap_ = false, code_ = false, cut_ = false;
    int vscroll_ = 0, hscroll_ = 0;
    std::vector<std::pair<int, int>> wrapped_;  // (line, start) of each visual line when wrapping
    int wrap_width_ = -1;
    // archive
    SimpleList list_;
    RECT body_{}, open_btn_{}, close_btn_{};
    int hot_ = 0;
    bool sb_drag_ = false;
    ScrollBar sb_;
    void set_text(const wstr& t, const wstr& ext);
    void rewrap(HDC dc);
    int text_line_h() const;
    int visual_lines() const;
    void copy_text();
};
