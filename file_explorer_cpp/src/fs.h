// Files and Windows shell: listing, drives, sizes, search, archives, text preview, rename, copy/move/delete,
// clipboard, folder colours in Windows Explorer, folder watching.
#pragma once
#include "util.h"
#include <atomic>
#include <set>

struct Entry {
    wstr name, path, ext;
    wstr folder;            // where it is (search results, tag lists)
    bool dir = false, hidden = false, drive = false;
    i64 size = -1;          // files; folders once their size is known
    double mtime = 0;
    i64 free = -1, total = -1;  // drives
    int icon = -2;          // system icon index (-2: not asked yet)
};
using EntryPtr = std::shared_ptr<Entry>;
using Entries = std::vector<EntryPtr>;

Entries list_folder(const wstr& path, bool show_hidden, DWORD* error);
EntryPtr entry_for(const wstr& path);
bool hidden_attrs(DWORD attrs, const wstr& name);
double folder_mtime(const wstr& path);  // changes when files are added / removed / renamed

struct Drive { wstr root, label; i64 free = -1, total = -1; UINT type = 0; };
std::vector<Drive> list_drives();
const std::vector<Drive>& cached_drives(double max_age = 10);
Entries drive_entries(const std::vector<Drive>& drives);
std::vector<std::pair<wstr, wstr>> quick_access();  // (name, path): Home, Desktop, Downloads …
wstr home_dir();

bool dir_size(const wstr& path, const std::function<bool()>& stop, i64& bytes, i64& files);

// ---- search: "report *.pdf size:>1mb modified:this week"
struct Query {
    std::vector<wstr> words, globs;
    std::vector<wstr> exts;
    int kind = 0;  // 1 folders, 2 files
    bool has_size = false, has_when = false;
    double size_lo = -1, size_hi = -1, when_lo = -1, when_hi = -1;
};
bool parse_query(const wstr& text, Query& q, wstr& error);
bool query_match(const Query& q, const wstr& name, bool dir, i64 size, double mtime);
extern const int SEARCH_LIMIT;
extern const wchar_t* SEARCH_HELP;

// ---- archives
wstr archive_kind(const wstr& name);  // "zip", "tar", "7z" or ""
wstr archive_stem(const wstr& name);
wstr find_7zip();
wstr find_tar();
struct ArchItem { wstr name; i64 size; bool dir; };
bool archive_list(const wstr& path, std::vector<ArchItem>& out, wstr& error, size_t limit = 5000);
bool compress_zip(const std::vector<wstr>& paths, const wstr& target_zip, wstr& error, const std::atomic<bool>* stop);
bool extract_archive(const wstr& archive, const wstr& dest_folder, wstr& error, const std::atomic<bool>* stop);
bool run_process(const wstr& cmdline, const wstr& cwd, DWORD& exit_code, std::string& output,
                 const std::atomic<bool>* stop = nullptr);

// ---- text preview with colours
enum TokKind { T_KEYWORD = 1, T_STRING, T_COMMENT, T_NUMBER, T_KEY, T_TAG, T_HEADING, T_BOLD, T_CODE };
struct Span { int start, len; TokKind kind; };
extern const std::set<wstr> TEXT_EXTS;
bool read_text(const wstr& path, size_t limit, wstr& text, bool& cut, bool sniff);
std::vector<Span> highlight(const wstr& text, const wstr& ext);
bool is_code_ext(const wstr& ext);
extern const std::set<wstr> THUMB_EXTS;
extern const std::set<wstr> OWN_ICON_EXTS;
bool is_type(const wstr& kind, const wstr& ext);  // "image", "video", "audio", "doc", "code", "archive"

// ---- rename
struct BulkOpts {
    wstr tpl = L"{name}", find, repl;
    bool regex = false, match_case = false, keep_ext = true;
    int case_mode = 0;  // 0 keep, 1 lower, 2 UPPER, 3 Title, 4 Sentence
    int start = 1, step = 1, digits = 1;
};
extern const wchar_t* CASE_NAMES[5];
std::vector<wstr> bulk_names(const std::vector<Entry>& entries, const BulkOpts& o, wstr& error);
std::map<int, wstr> bulk_problems(const std::vector<Entry>& entries, const std::vector<wstr>& names);
// rename all (swaps work); all or nothing. Returns the pairs that changed.
bool apply_renames(std::vector<std::pair<wstr, wstr>> pairs, std::vector<std::pair<wstr, wstr>>& done, wstr& error);

// ---- Windows shell
enum FileOp { OP_MOVE = 1, OP_COPY = 2, OP_DELETE = 3 };
bool shell_file_op(FileOp op, const std::vector<wstr>& paths, const wstr& dest, unsigned flags, HWND owner,
                   bool& aborted);
bool clipboard_set_files(const std::vector<wstr>& paths, bool cut, HWND owner);
bool clipboard_get_files(HWND owner, std::vector<wstr>& paths, bool& cut);
void clipboard_set_text(HWND owner, const wstr& text);
bool open_file(const wstr& path, wstr& error);
void open_with(const wstr& path);
void show_properties(const wstr& path, HWND owner);
void open_in_explorer(const wstr& path, bool select);
void open_terminal(const wstr& folder);

// folder colour in Windows Explorer (desktop.ini); color empty → the normal folder back
bool set_explorer_folder_color(const wstr& folder, const std::string& color, const std::string& ico_data, wstr& error);
wstr folder_icons_dir();

// ---- watch a folder for changes (no polling): calls back on the UI thread
class Watcher {
  public:
    ~Watcher() { stop(); }
    void watch(const wstr& path, std::function<void()> changed);
    void stop();
  private:
    HANDLE stop_event_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
};
