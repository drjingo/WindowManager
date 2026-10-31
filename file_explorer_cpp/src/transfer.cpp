#include "transfer.h"
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

enum Choice { CH_REPLACE = 0, CH_SKIP = 1, CH_BOTH = 2, CH_CANCEL = 3 };

struct Job {
    std::vector<wstr> paths;
    wstr dest;
    bool move = false;
    std::atomic<bool> paused{false}, cancel{false}, skip{false}, scanning{true}, finished{false};
    std::atomic<i64> bytes_done{0}, bytes_total{0};
    std::atomic<int> files_done{0}, files_total{0};
    std::mutex mu;
    wstr current;                   // the file being copied
    std::vector<wstr> errors;
    std::vector<wstr> landed;       // names in dest (to select afterwards)
    std::vector<std::pair<wstr, wstr>> moved;
    int skipped = 0;
    // a question for the UI thread
    std::condition_variable cv;
    bool answered = false;
    int answer = CH_SKIP;
    bool answer_all = false;
};
using JobPtr = std::shared_ptr<Job>;

std::atomic<int> g_running{0};
std::set<Job*> g_jobs;  // UI thread only

wstr last_error_text(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   (LPWSTR)&msg, 0, nullptr);
    wstr t = msg ? trim(msg) : fmt(L"error %lu", e);
    if (msg) LocalFree(msg);
    return t;
}

bool is_link_dir(DWORD attrs) { return (attrs & FILE_ATTRIBUTE_DIRECTORY) && (attrs & FILE_ATTRIBUTE_REPARSE_POINT); }

// how much is there (files, bytes); links to folders are not followed
void measure(const wstr& path, int& files, i64& bytes, const std::atomic<bool>& cancel) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) bytes += ((i64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        files++;
        return;
    }
    if (is_link_dir(a)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (cancel) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!is_link_dir(fd.dwFileAttributes)) measure(path_join(path, fd.cFileName), files, bytes, cancel);
        } else {
            files++;
            bytes += ((i64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

struct CopyCtx {
    Job* job;
    i64 last = 0;
};

DWORD CALLBACK progress_cb(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                           LPVOID data) {
    CopyCtx* c = (CopyCtx*)data;
    Job* j = c->job;
    j->bytes_done += done.QuadPart - c->last;
    c->last = done.QuadPart;
    while (j->paused && !j->cancel && !j->skip) Sleep(60);
    return j->cancel || j->skip ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

// one file; false = a real error (recorded) or cancelled
bool copy_file(Job& j, const wstr& src, const wstr& dst) {
    while (j.paused && !j.cancel) Sleep(60);  // paused between files too (small files may never call back)
    if (j.cancel) return false;
    {
        std::lock_guard<std::mutex> l(j.mu);
        j.current = base_name(src);
    }
    // a read-only file in the way can't be overwritten
    DWORD da = GetFileAttributesW(dst.c_str());
    if (da != INVALID_FILE_ATTRIBUTES && (da & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(dst.c_str(), da & ~FILE_ATTRIBUTE_READONLY);
    WIN32_FILE_ATTRIBUTE_DATA sd{};
    GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &sd);
    i64 size = ((i64)sd.nFileSizeHigh << 32) | sd.nFileSizeLow;
    CopyCtx ctx{&j};
    BOOL stop = FALSE;
    BOOL ok = CopyFileExW(src.c_str(), dst.c_str(), progress_cb, &ctx, &stop, 0);
    DWORD err = ok ? 0 : GetLastError();
    if (ok) {
        j.bytes_done += size - ctx.last;  // the callback may not see the last bytes of small files
        j.files_done++;
        return true;
    }
    if (j.skip || j.cancel) {
        DeleteFileW(dst.c_str());  // the half-copied file
        j.bytes_done += size - ctx.last;
        if (j.skip) {
            j.skip = false;
            j.files_done++;
            std::lock_guard<std::mutex> l(j.mu);
            j.skipped++;
            return true;
        }
        return false;
    }
    j.bytes_done += size - ctx.last;
    j.files_done++;
    std::lock_guard<std::mutex> l(j.mu);
    j.errors.push_back(base_name(src) + L": " + last_error_text(err));
    return false;
}

bool copy_tree(Job& j, const wstr& src, const wstr& dst) {
    if (j.cancel) return false;
    DWORD a = GetFileAttributesW(src.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        std::lock_guard<std::mutex> l(j.mu);
        j.errors.push_back(base_name(src) + L": " + last_error_text(GetLastError()));
        return false;
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return copy_file(j, src, dst);
    if (is_link_dir(a)) {
        std::lock_guard<std::mutex> l(j.mu);
        j.errors.push_back(base_name(src) + L": a link to another folder (not copied)");
        return false;
    }
    if (!CreateDirectoryW(dst.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        std::lock_guard<std::mutex> l(j.mu);
        j.errors.push_back(base_name(dst) + L": " + last_error_text(GetLastError()));
        return false;
    }
    SetFileAttributesW(dst.c_str(), a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY));
    bool ok = true;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((src + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return true;  // empty
    do {
        if (j.cancel) { ok = false; break; }
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!copy_tree(j, path_join(src, fd.cFileName), path_join(dst, fd.cFileName))) ok = false;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool delete_tree(const wstr& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(p.c_str()) != 0;
    if (!is_link_dir(a)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((p + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                delete_tree(path_join(p, fd.cFileName));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return RemoveDirectoryW(p.c_str()) != 0;
}

// ======================================================================
// "Already there" question
// ======================================================================
struct ConflictDialog : Dialog {
    Label what, info_new, info_old;
    Check all;
    Button replace, both, skip;
};

wstr describe(const wstr& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return L"";
    ULARGE_INTEGER u;
    u.LowPart = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    double t = (double)(u.QuadPart / 10000000ULL) - 11644473600.0;
    wstr s = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L"Folder" : fmt_size(((i64)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    return s + L"   ·   modified " + fmt_time(t);
}

int ask_conflict(HWND owner, const wstr& src, const wstr& dst, bool& for_all, int remaining) {
    ConflictDialog d;
    bool folder = is_dir(dst) && is_dir(src);
    int w = S(500), h = S(240);
    RECT wr{0, 0, w, h};
    AdjustWindowRectEx(&wr, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    d.create_top(owner, folder ? L"Folder already there" : L"File already there", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                 WS_EX_DLGMODALFRAME, 0, 0, wr.right - wr.left, wr.bottom - wr.top);
    auto lab = [&](Label& l, const wstr& t, HFONT f, COLORREF c, int y, int hh) {
        l.create(d.hwnd);
        l.text_ = t;
        l.font = f;
        l.color = c;
        l.flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
        l.move(S(20), y, w - S(40), hh);
    };
    lab(d.what, L"“" + base_name(dst) + L"” is already in “" + display_name(dir_name(dst)) + L"”", F.ui_bold, T.fg, S(16), S(26));
    lab(d.info_new, L"Coming in:   " + describe(src), F.small, T.muted, S(50), S(22));
    lab(d.info_old, L"Already there:   " + describe(dst), F.small, T.muted, S(74), S(22));
    d.all.create(d.hwnd);
    d.all.label = fmt(L"Do the same for the next %d", remaining);
    d.all.move(S(20), S(112), d.all.ideal_width(), S(26));
    d.all.show(remaining > 0);
    int bw = S(140), by = h - S(56);
    for (auto [b, t, i] : {std::tuple<Button*, const wchar_t*, int>{&d.replace, folder ? L"Merge" : L"Replace", 0},
                           {&d.both, L"Keep both", 1}, {&d.skip, L"Skip", 2}}) {
        b->create(d.hwnd);
        b->label = t;
        b->move(S(20) + i * (bw + S(10)), by, bw, S(36));
    }
    d.replace.primary = true;
    d.replace.click = [&] { d.finish(1 + CH_REPLACE); };
    d.both.click = [&] { d.finish(1 + CH_BOTH); };
    d.skip.click = [&] { d.finish(1 + CH_SKIP); };
    d.enter = [&] { d.finish(1 + CH_REPLACE); };
    d.run(owner);
    for_all = d.all.checked;
    int r = d.result == 0 ? CH_CANCEL : d.result - 1;
    DestroyWindow(d.hwnd);
    return r;
}

// ======================================================================
// the progress window
// ======================================================================
class TransferWindow : public Control {
  public:
    App* app;
    JobPtr job;
    Button pause, skip, cancel;
    std::deque<std::pair<double, i64>> samples;  // (seconds, bytes) for the speed
    double started = 0;
    static int open_count;
    ITaskbarList3* taskbar = nullptr;

    TransferWindow(App* a, JobPtr j) : app(a), job(j) {}
    ~TransferWindow() override {
        if (taskbar) {
            taskbar->SetProgressState(app->hwnd, TBPF_NOPROGRESS);
            taskbar->Release();
        }
    }

    static double now() { return GetTickCount64() / 1000.0; }

    void open() {
        int w = S(520), h = S(214);
        RECT wr{0, 0, w, h};
        DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_POPUP;
        AdjustWindowRectEx(&wr, style, FALSE, 0);
        create_top(app->hwnd, job->move ? L"Moving" : L"Copying", style, 0, 0, 0, wr.right - wr.left, wr.bottom - wr.top);
        style_window(hwnd);
        // stacked a little lower-right of the main window's centre, one below the other
        RECT ar, me;
        GetWindowRect(app->hwnd, &ar);
        GetWindowRect(hwnd, &me);
        int k = open_count++ % 5;
        SetWindowPos(hwnd, nullptr, ar.right - (me.right - me.left) - S(30) - k * S(24), ar.bottom - (me.bottom - me.top) - S(50) - k * S(24), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        int bw = S(100), by = h - S(52);
        pause.create(hwnd);
        pause.label = L"Pause";
        pause.move(w - S(20) - 3 * bw - 2 * S(8), by, bw, S(34));
        skip.create(hwnd);
        skip.label = L"Skip file";
        skip.move(w - S(20) - 2 * bw - S(8), by, bw, S(34));
        cancel.create(hwnd);
        cancel.label = L"Cancel";
        cancel.move(w - S(20) - bw, by, bw, S(34));
        pause.click = [this] {
            job->paused = !job->paused;
            pause.label = job->paused ? L"Resume" : L"Pause";
            pause.primary = job->paused;
            pause.invalidate();
            if (taskbar) taskbar->SetProgressState(app->hwnd, job->paused ? TBPF_PAUSED : TBPF_NORMAL);
            invalidate();
        };
        skip.click = [this] { if (!job->scanning) job->skip = true; };
        cancel.click = [this] { stop(); };
        started = now();
        SetTimer(hwnd, 1, 450, nullptr);  // only show up when it takes a while
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, (void**)&taskbar)))
            taskbar->HrInit();
        else taskbar = nullptr;
    }

    void stop() {
        job->cancel = true;
        job->paused = false;
        cancel.set_enabled(false);
        cancel.label = L"Stopping…";
        invalidate();
    }

    void on_close() override { stop(); }

    void on_timer(UINT_PTR id) override {
        if (id == 1) {
            KillTimer(hwnd, 1);
            if (!job->finished) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                SetTimer(hwnd, 2, 120, nullptr);
            }
            return;
        }
        double t = now();
        samples.push_back({t, job->bytes_done.load()});
        while (samples.size() > 2 && t - samples.front().first > 4) samples.pop_front();
        if (taskbar && job->bytes_total > 0)
            taskbar->SetProgressValue(app->hwnd, (ULONGLONG)job->bytes_done.load(), (ULONGLONG)job->bytes_total.load());
        else if (taskbar) taskbar->SetProgressState(app->hwnd, TBPF_INDETERMINATE);
        invalidate();
    }

    void on_paint(HDC dc, const RECT& rc) override {
        fill(dc, rc, T.bg);
        Job& j = *job;
        int n = (int)j.paths.size();
        wstr what = n == 1 ? L"“" + base_name(j.paths[0]) + L"”" : fmt(L"%d items", n);
        wstr title = (j.move ? L"Moving " : L"Copying ") + what + L" to “" + display_name(j.dest) + L"”";
        text(dc, title, RECT{S(20), S(14), rc.right - S(20), S(40)}, T.fg, F.ui_bold);
        i64 done = j.bytes_done, total = j.bytes_total;
        double frac = total > 0 ? std::min(1.0, (double)done / total) : 0;
        RECT bar{S(20), S(52), rc.right - S(20), S(62)};
        fill_round(dc, bar, S(5), T.field);
        if (j.scanning) {  // a sliding block while counting
            int bw = (bar.right - bar.left) / 4;
            int x = bar.left + (int)(fmod(now() * 0.8, 1.0) * (bar.right - bar.left - bw));
            fill_round(dc, RECT{x, bar.top, x + bw, bar.bottom}, S(5), T.accent);
        } else if (frac > 0) {
            RECT f = bar;
            f.right = bar.left + std::max(S(10), (int)((bar.right - bar.left) * frac));
            fill_round(dc, f, S(5), j.paused ? T.star : T.accent);
        }
        wstr line;
        if (j.scanning) line = L"Getting ready…  " + fmt_count(j.files_total) + L" files so far";
        else {
            line = fmt(L"%d%%", (int)(frac * 100)) + L"   ·   " + fmt_size(done) + L" of " + fmt_size(total);
            if (j.paused) line += L"   ·   paused";
            else if (samples.size() >= 2 && samples.back().first - samples.front().first > 0.5) {
                double speed = (samples.back().second - samples.front().second) / (samples.back().first - samples.front().first);
                if (speed > 1) {
                    line += L"   ·   " + fmt_size((i64)speed) + L"/s";
                    double left = (total - done) / speed;
                    wstr l = left < 5 ? L"a few seconds" : left < 90 ? fmt(L"%d s", (int)left)
                             : left < 5400 ? fmt(L"%d min", (int)(left / 60 + 0.5)) : fmt(L"%.1f h", left / 3600);
                    line += L"   ·   about " + l + L" left";
                }
            }
        }
        text(dc, line, RECT{S(20), S(72), rc.right - S(20), S(96)}, T.muted, F.small);
        wstr cur;
        int skipped, errs;
        {
            std::lock_guard<std::mutex> l(j.mu);
            cur = j.current;
            skipped = j.skipped;
            errs = (int)j.errors.size();
        }
        wstr files = fmt(L"%s of %s files", fmt_count(j.files_done).c_str(), fmt_count(j.files_total).c_str());
        if (skipped) files += fmt(L"  ·  %d skipped", skipped);
        if (errs) files += fmt(L"  ·  %d problem%s", errs, errs > 1 ? L"s" : L"");
        text(dc, files, RECT{S(20), S(98), rc.right - S(20), S(120)}, errs ? T.bad : T.faint, F.small);
        if (!cur.empty()) text(dc, cur, RECT{S(20), S(120), rc.right - S(20), S(142)}, T.faint, F.small);
    }
};
int TransferWindow::open_count = 0;

void work(JobPtr jp, HWND owner, TransferWindow* win) {
    Job& j = *jp;
    // 1. how much
    for (auto& p : j.paths) {
        int f = 0;
        i64 b = 0;
        measure(p, f, b, j.cancel);
        j.files_total += f;
        j.bytes_total += b;
    }
    j.scanning = false;
    // 2. one item at a time
    int remember = -1;
    for (size_t i = 0; i < j.paths.size() && !j.cancel; i++) {
        const wstr& p = j.paths[i];
        wstr name = base_name(p);
        wstr target = path_join(j.dest, name);
        int item_files = 0;
        i64 item_bytes = 0;
        measure(p, item_files, item_bytes, j.cancel);
        bool replace = false;
        if (norm(dir_name(p)) == norm(j.dest)) {
            if (j.move) continue;  // already here
            target = unique_name(j.dest, name, true);  // "name - Copy"
        } else if (exists(target)) {
            int c = remember;
            if (c < 0) {
                bool all = false;
                int left = (int)(j.paths.size() - i - 1);
                // ask on the UI thread and wait for the answer
                std::unique_lock<std::mutex> l(j.mu);
                j.answered = false;
                post_ui([jp, owner, p, target, left] {
                    bool a = false;
                    // ask over the progress window when it's showing, else over the main window
                    HWND o = IsWindowVisible(owner) ? owner : GetWindow(owner, GW_OWNER);
                    int r = ask_conflict(o, p, target, a, left);
                    std::lock_guard<std::mutex> g(jp->mu);
                    jp->answer = r;
                    jp->answer_all = a;
                    jp->answered = true;
                    jp->cv.notify_all();
                });
                j.cv.wait(l, [&] { return j.answered; });
                c = j.answer;
                all = j.answer_all;
                if (all) remember = c;
            }
            if (c == CH_CANCEL) { j.cancel = true; break; }
            if (c == CH_SKIP) {
                j.files_done += item_files;
                j.bytes_done += item_bytes;
                std::lock_guard<std::mutex> l(j.mu);
                j.skipped += std::max(1, item_files);
                continue;
            }
            if (c == CH_BOTH) target = unique_name(j.dest, name, false);
            else replace = true;
        }
        bool merge_dirs = replace && is_dir(target) && is_dir(p);
        if (j.move && same_drive(p, j.dest) && !merge_dirs) {
            // same drive: just a rename, instant
            {
                std::lock_guard<std::mutex> l(j.mu);
                j.current = name;
            }
            if (replace && !is_dir(target)) {
                DWORD a = GetFileAttributesW(target.c_str());
                if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(target.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
            }
            if (MoveFileExW(p.c_str(), target.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0)) {
                std::lock_guard<std::mutex> l(j.mu);
                j.moved.push_back({p, target});
                j.landed.push_back(base_name(target));
            } else {
                std::lock_guard<std::mutex> l(j.mu);
                j.errors.push_back(name + L": " + last_error_text(GetLastError()));
            }
            j.files_done += item_files;
            j.bytes_done += item_bytes;
            continue;
        }
        bool ok = copy_tree(j, p, target);
        {
            std::lock_guard<std::mutex> l(j.mu);
            if (exists(target)) j.landed.push_back(base_name(target));
        }
        if (ok && j.move && !j.cancel) {
            if (delete_tree(p)) {
                std::lock_guard<std::mutex> l(j.mu);
                j.moved.push_back({p, target});
            } else {
                std::lock_guard<std::mutex> l(j.mu);
                j.errors.push_back(name + L": copied, but the original couldn't be removed");
            }
        }
    }
    j.finished = true;
    post_ui([jp, win] {
        App* app = win->app;
        Job& j = *jp;
        int n = (int)j.paths.size();
        wstr items = n == 1 ? L"“" + base_name(j.paths[0]) + L"”" : fmt(L"%d items", n);
        wstr msg;
        if (j.cancel) msg = fmt(L"Stopped — %s of %s files ", fmt_count(j.files_done).c_str(), fmt_count(j.files_total).c_str()) + (j.move ? L"moved." : L"copied.");
        else if (!j.errors.empty()) msg = (j.move ? L"Moved " : L"Copied ") + items + fmt(L", but %d couldn't be", (int)j.errors.size());
        else msg = (j.move ? L"Moved " : L"Copied ") + items + L" to " + display_name(j.dest) + L" ✓";
        if (j.skipped && !j.cancel) msg += fmt(L"  ·  %d skipped", j.skipped);
        for (auto& n : j.landed) app->dir_sizes.erase(norm(path_join(j.dest, n)));  // measured while still copying
        if (!j.moved.empty()) app->remap_paths(j.moved);
        app->reload_panes(j.landed, j.dest);
        app->say(msg, 10);
        win->show(false);
        std::vector<wstr> errs = j.errors;
        HWND owner = app->hwnd;
        delete win;
        g_running--;
        g_jobs.erase(jp.get());
        if (!errs.empty()) {
            wstr list;
            for (size_t i = 0; i < errs.size() && i < 12; i++) list += L"• " + errs[i] + L"\n";
            if (errs.size() > 12) list += fmt(L"… and %d more", (int)errs.size() - 12);
            confirm(owner, L"Some files had problems", list, L"OK");
        }
    });
}

}  // namespace

void start_transfer(App* app, const std::vector<wstr>& paths, const wstr& dest, bool move) {
    auto job = std::make_shared<Job>();
    job->paths = paths;
    job->dest = dest;
    job->move = move;
    auto* win = new TransferWindow(app, job);
    win->open();
    g_running++;
    g_jobs.insert(job.get());
    HWND owner = win->hwnd;
    std::thread([job, owner, win] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        work(job, owner, win);
        CoUninitialize();
    }).detach();
}

bool transfers_running() { return g_running > 0; }

void cancel_transfers() {
    for (Job* j : g_jobs) { j->cancel = true; j->paused = false; }
}
