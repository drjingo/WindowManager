#include "fs.h"
#include "gfx.h"
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>
#include <regex>
#include <thread>

// ======================================================================
// listing
// ======================================================================
bool hidden_attrs(DWORD attrs, const wstr& name) {
    return (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

static EntryPtr make_entry(const WIN32_FIND_DATAW& fd, const wstr& folder) {
    auto e = std::make_shared<Entry>();
    e->name = fd.cFileName;
    e->path = path_join(folder, e->name);
    e->dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e->hidden = hidden_attrs(fd.dwFileAttributes, e->name);
    e->mtime = filetime_unix(fd.ftLastWriteTime);
    if (!e->dir) {
        e->size = ((i64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e->ext = ext_lower(e->name);
    }
    return e;
}

Entries list_folder(const wstr& path, bool show_hidden, DWORD* error) {
    Entries out;
    if (error) *error = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(path_join(path, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (error && e != ERROR_FILE_NOT_FOUND) *error = e ? e : ERROR_ACCESS_DENIED;
        return out;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!show_hidden && hidden_attrs(fd.dwFileAttributes, fd.cFileName)) continue;
        out.push_back(make_entry(fd, path));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

EntryPtr entry_for(const wstr& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return nullptr;
    auto e = std::make_shared<Entry>();
    e->path = clean_path(path);
    e->name = base_name(e->path);
    e->dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e->hidden = false;
    e->mtime = filetime_unix(a.ftLastWriteTime);
    if (!e->dir) {
        e->size = ((i64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        e->ext = ext_lower(e->name);
    }
    e->folder = dir_name(e->path);
    return e;
}

double folder_mtime(const wstr& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return -1;
    return filetime_unix(a.ftLastWriteTime);
}

std::vector<Drive> list_drives() {
    std::vector<Drive> out;
    DWORD mask = GetLogicalDrives();
    UINT old = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        Drive d;
        d.root = wstr(1, (wchar_t)(L'A' + i)) + L":\\";
        d.type = GetDriveTypeW(d.root.c_str());
        if (d.type == DRIVE_UNKNOWN || d.type == DRIVE_NO_ROOT_DIR) continue;
        wchar_t name[MAX_PATH + 1] = {};
        bool ok = GetVolumeInformationW(d.root.c_str(), name, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        ULARGE_INTEGER fr, tot;
        if (GetDiskFreeSpaceExW(d.root.c_str(), &fr, &tot, nullptr)) { d.free = (i64)fr.QuadPart; d.total = (i64)tot.QuadPart; }
        d.label = ok && name[0] ? name
                  : d.type == DRIVE_REMOVABLE ? L"Removable" : d.type == DRIVE_REMOTE ? L"Network"
                  : d.type == DRIVE_CDROM ? L"CD/DVD" : L"Local Disk";
        out.push_back(d);
    }
    SetErrorMode(old);
    return out;
}

const std::vector<Drive>& cached_drives(double max_age) {
    static std::vector<Drive> cache;
    static double at = 0;
    if (cache.empty() || now_unix() - at > max_age) { cache = list_drives(); at = now_unix(); }
    return cache;
}

Entries drive_entries(const std::vector<Drive>& drives) {
    Entries out;
    for (auto& d : drives) {
        auto e = std::make_shared<Entry>();
        e->name = d.label + L" (" + d.root.substr(0, 2) + L")";
        e->path = d.root;
        e->dir = e->drive = true;
        e->free = d.free;
        e->total = d.total;
        out.push_back(e);
    }
    return out;
}

static wstr known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    wstr out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

wstr home_dir() {
    wstr h = known(FOLDERID_Profile);
    if (h.empty()) {
        wchar_t buf[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) h = buf;
    }
    return h.empty() ? L"C:\\" : h;
}

std::vector<std::pair<wstr, wstr>> quick_access() {
    std::vector<std::pair<wstr, wstr>> out;
    out.push_back({L"Home", home_dir()});
    const std::pair<const wchar_t*, const KNOWNFOLDERID*> ids[] = {
        {L"Desktop", &FOLDERID_Desktop}, {L"Downloads", &FOLDERID_Downloads}, {L"Documents", &FOLDERID_Documents},
        {L"Pictures", &FOLDERID_Pictures}, {L"Music", &FOLDERID_Music}, {L"Videos", &FOLDERID_Videos}};
    for (auto& [name, id] : ids) {
        wstr p = known(*id);
        if (p.empty()) p = path_join(home_dir(), name);
        if (is_dir(p)) out.push_back({name, p});
    }
    return out;
}

bool dir_size(const wstr& path, const std::function<bool()>& stop, i64& bytes, i64& files) {
    bytes = files = 0;
    std::vector<wstr> stack{path};
    int n = 0;
    while (!stack.empty()) {
        wstr p = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(path_join(p, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(path_join(p, fd.cFileName));
                } else {
                    bytes += ((i64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    files++;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (++n % 32 == 0 && stop()) return false;
    }
    return true;
}

// ======================================================================
// search
// ======================================================================
const int SEARCH_LIMIT = 5000;
const wchar_t* SEARCH_HELP =
    L"Enter searches all subfolders: words, *.pdf, ext:pdf,docx, type:image|video|audio|doc|code|archive|folder, "
    L"size:>10mb, size:large, modified:today|this week|7d|2024-05|>2024-01-01";

static const std::map<wstr, std::vector<wstr>>& type_exts() {
    static std::map<wstr, std::vector<wstr>> m = {
        {L"image", {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp", L".heic", L".heif", L".tif", L".tiff", L".ico",
                    L".svg", L".raw", L".cr2", L".nef", L".psd", L".avif"}},
        {L"video", {L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".webm", L".m4v", L".mpg", L".mpeg", L".flv", L".3gp"}},
        {L"audio", {L".mp3", L".wav", L".flac", L".aac", L".ogg", L".m4a", L".wma", L".opus", L".mid"}},
        {L"doc", {L".pdf", L".doc", L".docx", L".odt", L".rtf", L".txt", L".md", L".xls", L".xlsx", L".ods", L".csv",
                  L".ppt", L".pptx", L".odp", L".epub", L".one"}},
        {L"code", {L".py", L".pyw", L".js", L".ts", L".jsx", L".tsx", L".json", L".html", L".htm", L".css", L".c", L".h",
                   L".cpp", L".hpp", L".cs", L".java", L".go", L".rs", L".php", L".rb", L".sh", L".bat", L".cmd",
                   L".ps1", L".sql", L".xml", L".yml", L".yaml", L".toml", L".ini", L".kt", L".swift", L".lua", L".ipynb"}},
        {L"archive", {L".zip", L".7z", L".rar", L".tar", L".gz", L".tgz", L".bz2", L".xz", L".iso", L".cab"}},
    };
    return m;
}

bool is_type(const wstr& kind, const wstr& ext) {
    auto& m = type_exts();
    auto it = m.find(kind);
    return it != m.end() && std::find(it->second.begin(), it->second.end(), ext) != it->second.end();
}

static bool parse_size(const wstr& t0, double& v) {
    wstr t = lower(trim(t0));
    size_t i = 0;
    while (i < t.size() && (iswdigit(t[i]) || t[i] == L'.' || t[i] == L',')) i++;
    if (!i) return false;
    v = _wtof(replace_all(t.substr(0, i), L",", L".").c_str());
    wstr u = trim(t.substr(i));
    if (u == L"" || u == L"b") return true;
    const wchar_t* units[] = {L"k", L"m", L"g", L"t"};
    for (int k = 0; k < 4; k++)
        if (u == units[k] || u == wstr(units[k]) + L"b" || u == wstr(units[k]) + L"ib") {
            v *= std::pow(1024.0, k + 1);
            return true;
        }
    return false;
}

static double local_ts(int y, int m, int d) {
    SYSTEMTIME lt{}, ut{};
    lt.wYear = (WORD)y; lt.wMonth = (WORD)m; lt.wDay = (WORD)d;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &lt, &ut)) ut = lt;
    FILETIME ft;
    SystemTimeToFileTime(&ut, &ft);
    return filetime_unix(ft);
}

static bool parse_when(const wstr& t0, double& lo, double& hi) {
    wstr t = replace_all(replace_all(lower(trim(t0)), L" ", L""), L"_", L"");
    SYSTEMTIME now;
    GetLocalTime(&now);
    double today = local_ts(now.wYear, now.wMonth, now.wDay), day = 86400;
    int dow = (now.wDayOfWeek + 6) % 7;  // Monday = 0
    double monday = today - dow * day;
    double first = local_ts(now.wYear, now.wMonth, 1);
    int pm = now.wMonth == 1 ? 12 : now.wMonth - 1, py = now.wMonth == 1 ? now.wYear - 1 : now.wYear;
    double jan = local_ts(now.wYear, 1, 1);
    hi = -1;
    if (t == L"today") { lo = today; return true; }
    if (t == L"yesterday") { lo = today - day; hi = today; return true; }
    if (t == L"thisweek" || t == L"week") { lo = monday; return true; }
    if (t == L"lastweek") { lo = monday - 7 * day; hi = monday; return true; }
    if (t == L"thismonth" || t == L"month") { lo = first; return true; }
    if (t == L"lastmonth") { lo = local_ts(py, pm, 1); hi = first; return true; }
    if (t == L"thisyear" || t == L"year") { lo = jan; return true; }
    if (t == L"lastyear") { lo = local_ts(now.wYear - 1, 1, 1); hi = jan; return true; }
    // 7d, 3w, 2m, 1y, 12h
    if (t.size() >= 2 && iswdigit(t[0])) {
        size_t i = 0;
        while (i < t.size() && iswdigit(t[i])) i++;
        if (i == t.size() - 1) {
            double n = _wtof(t.substr(0, i).c_str());
            wchar_t u = t.back();
            double secs = u == L'h' ? 3600 : u == L'd' ? 86400 : u == L'w' ? 604800 : u == L'm' ? 2629800 : u == L'y' ? 31557600 : -1;
            if (secs > 0) { lo = now_unix() - n * secs; return true; }
        }
    }
    int y = 0, m = 0, d = 0;
    wchar_t c;
    if (swscanf(t.c_str(), L"%d/%d/%d%lc", &d, &m, &y, &c) == 3 && y > 1900) {
    } else {
        y = m = d = 0;
        int got = swscanf(t.c_str(), L"%d-%d-%d%lc", &y, &m, &d, &c);
        if (got < 1 || got > 3 || y < 1900) return false;
        if (got == 1 && t.size() != 4) return false;
    }
    if (d) { lo = local_ts(y, m, d); hi = lo + day; }
    else if (m) { lo = local_ts(y, m, 1); hi = local_ts(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1); }
    else { lo = local_ts(y, 1, 1); hi = local_ts(y + 1, 1, 1); }
    return true;
}

static std::vector<wstr> tokens(const wstr& s) {
    std::vector<wstr> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && iswspace(s[i])) i++;
        if (i >= s.size()) break;
        size_t j = i;
        if (s[i] == L'"') {
            j = s.find(L'"', i + 1);
            j = j == wstr::npos ? s.size() : j + 1;
        } else
            while (j < s.size() && !iswspace(s[j])) j++;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool parse_query(const wstr& text0, Query& q, wstr& error) {
    q = Query();
    error.clear();
    // "modified:this week" → "modified:thisweek"
    wstr text = text0;
    std::wregex rx(LR"(\b(modified|date|changed):\s*(this|last)\s+(week|month|year)\b)", std::regex::icase);
    text = std::regex_replace(text, rx, L"$1:$2$3");
    for (const wstr& tok : tokens(text)) {
        if (tok.front() == L'"') {
            wstr w = lower(replace_all(tok, L"\"", L""));
            if (!w.empty()) q.words.push_back(w);
            continue;
        }
        size_t colon = tok.find(L':');
        wstr key = colon == wstr::npos ? L"" : lower(tok.substr(0, colon));
        wstr val = colon == wstr::npos ? L"" : tok.substr(colon + 1);
        if (!val.empty() && (key == L"ext" || key == L"extension")) {
            for (auto x : split(val, L',')) {
                x = lower(trim(x));
                while (!x.empty() && (x[0] == L'*' || x[0] == L'.')) x.erase(0, 1);
                if (!x.empty()) q.exts.push_back(L"." + x);
            }
        } else if (!val.empty() && (key == L"type" || key == L"kind")) {
            static const std::map<wstr, wstr> alias = {
                {L"picture", L"image"}, {L"pictures", L"image"}, {L"photo", L"image"}, {L"photos", L"image"},
                {L"images", L"image"}, {L"img", L"image"}, {L"videos", L"video"}, {L"movie", L"video"},
                {L"movies", L"video"}, {L"music", L"audio"}, {L"sound", L"audio"}, {L"song", L"audio"},
                {L"songs", L"audio"}, {L"docs", L"doc"}, {L"document", L"doc"}, {L"documents", L"doc"},
                {L"archives", L"archive"}, {L"zip", L"archive"}, {L"compressed", L"archive"}};
            wstr v = lower(val);
            auto a = alias.find(v);
            if (a != alias.end()) v = a->second;
            if (v == L"folder" || v == L"folders" || v == L"dir" || v == L"dirs" || v == L"directory") q.kind = 1;
            else if (v == L"file" || v == L"files") q.kind = 2;
            else if (type_exts().count(v)) for (auto& x : type_exts().at(v)) q.exts.push_back(x);
            else { error = L"Unknown type \u201c" + val + L"\u201d \u2014 try folder, file, image, video, audio, doc, code, archive"; return false; }
        } else if (!val.empty() && key == L"size") {
            static const std::map<wstr, std::pair<double, double>> words = {
                {L"empty", {0, 0}}, {L"tiny", {0, 16 * 1024.0}}, {L"small", {16 * 1024.0, 1048576.0}},
                {L"medium", {1048576.0, 128 * 1048576.0}}, {L"large", {128 * 1048576.0, 1073741824.0}},
                {L"huge", {1073741824.0, 4 * 1073741824.0}}, {L"gigantic", {4 * 1073741824.0, -1}}};
            wstr v = lower(replace_all(replace_all(val, L">=", L">"), L"<=", L"<"));
            double a = -1, b = -1, x;
            bool ok = true;
            if (words.count(v)) { a = words.at(v).first; b = words.at(v).second; }
            else if (v[0] == L'>') ok = parse_size(v.substr(1), a);
            else if (v[0] == L'<') ok = parse_size(v.substr(1), b);
            else if (v.find(L"..") != wstr::npos) {
                size_t k = v.find(L"..");
                ok = parse_size(v.substr(0, k), a) && parse_size(v.substr(k + 2), b);
            } else { ok = parse_size(v, x); a = x; }  // size:10mb = at least 10 MB
            if (!ok) { error = L"Can't read the size \u201c" + val + L"\u201d \u2014 e.g. size:>10mb, size:<500kb, size:1mb..1gb, size:large"; return false; }
            q.has_size = true; q.size_lo = a; q.size_hi = b;
        } else if (!val.empty() && (key == L"modified" || key == L"date" || key == L"changed")) {
            wstr v = lower(replace_all(replace_all(val, L">=", L">"), L"<=", L"<"));
            double a = -1, b = -1, x, y;
            bool ok;
            if (v[0] == L'>') { ok = parse_when(v.substr(1), a, y); }
            else if (v[0] == L'<') { ok = parse_when(v.substr(1), b, y); }
            else if (v.find(L"..") != wstr::npos) {
                size_t k = v.find(L"..");
                ok = parse_when(v.substr(0, k), a, y) && parse_when(v.substr(k + 2), x, b);
                if (ok && b < 0) b = -1;
            } else ok = parse_when(v, a, b);
            if (!ok) { error = L"Can't read the date \u201c" + val + L"\u201d \u2014 e.g. modified:today, modified:this week, modified:7d, modified:2024-05, modified:>2024-01-01"; return false; }
            q.has_when = true; q.when_lo = a; q.when_hi = b;
        } else if (tok.find_first_of(L"*?") != wstr::npos) {
            if (tok.size() > 2 && tok[0] == L'*' && tok[1] == L'.' && tok.find_first_of(L"*?", 1) == wstr::npos)
                q.exts.push_back(lower(tok.substr(1)));
            else q.globs.push_back(lower(tok));
        } else q.words.push_back(lower(tok));
    }
    if (q.words.empty() && q.globs.empty() && q.exts.empty() && !q.kind && !q.has_size && !q.has_when) {
        error = L"Type something to search for.";
        return false;
    }
    return true;
}

bool query_match(const Query& q, const wstr& name, bool dir, i64 size, double mtime) {
    if ((q.kind == 1 && !dir) || (q.kind == 2 && dir)) return false;
    wstr n;  // lower-cased only when needed
    auto ln = [&]() -> const wstr& { if (n.empty()) n = lower(name); return n; };
    if (!q.exts.empty()) {
        if (dir) return false;
        bool any = false;
        for (auto& x : q.exts) if (ends_with(ln(), x)) { any = true; break; }
        if (!any) return false;
    }
    for (auto& w : q.words) if (ln().find(w) == wstr::npos) return false;
    for (auto& g : q.globs) if (!PathMatchSpecW(ln().c_str(), g.c_str())) return false;
    if (q.has_size) {
        if (dir || size < 0) return false;
        if ((q.size_lo >= 0 && size < q.size_lo) || (q.size_hi >= 0 && size > q.size_hi)) return false;
    }
    if (q.has_when) {
        if (mtime <= 0 || (q.when_lo >= 0 && mtime < q.when_lo) || (q.when_hi >= 0 && mtime >= q.when_hi)) return false;
    }
    return true;
}

// ======================================================================
// archives: Windows' own tar.exe (zip, tar, gz …) or 7-Zip
// ======================================================================
static const wchar_t* ARCH_SUFFIXES[] = {L".tar.gz", L".tar.bz2", L".tar.xz", L".tgz", L".tbz2", L".tbz", L".txz",
                                         L".tar", L".zip", L".7z", L".rar", L".gz", L".bz2", L".xz", L".cab", L".iso"};

wstr archive_kind(const wstr& name) {
    wstr l = lower(name);
    if (ends_with(l, L".zip")) return L"zip";
    for (auto s : {L".tar", L".tar.gz", L".tgz", L".tar.bz2", L".tbz2", L".tbz", L".tar.xz", L".txz"})
        if (ends_with(l, s)) return L"tar";
    for (auto s : {L".7z", L".rar", L".gz", L".bz2", L".xz", L".cab", L".iso"})
        if (ends_with(l, s)) return (!find_7zip().empty() || !find_tar().empty()) ? L"7z" : L"";
    return L"";
}

wstr archive_stem(const wstr& name) {
    wstr l = lower(name);
    for (auto s : ARCH_SUFFIXES)
        if (ends_with(l, s) && name.size() > wcslen(s)) return name.substr(0, name.size() - wcslen(s));
    wstr st, ex;
    split_ext(name, st, ex);
    return st;
}

wstr find_7zip() {
    static wstr cache = L"?";
    if (cache != L"?") return cache;
    cache.clear();
    for (auto env : {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)"}) {
        wchar_t buf[MAX_PATH];
        if (GetEnvironmentVariableW(env, buf, MAX_PATH)) {
            wstr p = path_join(path_join(buf, L"7-Zip"), L"7z.exe");
            if (exists(p)) { cache = p; break; }
        }
    }
    return cache;
}

wstr find_tar() {
    static wstr cache = L"?";
    if (cache != L"?") return cache;
    wchar_t buf[MAX_PATH];
    GetSystemDirectoryW(buf, MAX_PATH);
    wstr p = path_join(buf, L"tar.exe");
    cache = exists(p) ? p : L"";
    return cache;
}

static wstr quote(const wstr& s) { return L"\"" + s + L"\""; }

bool run_process(const wstr& cmdline, const wstr& cwd, DWORD& exit_code, std::string& output,
                 const std::atomic<bool>* stop) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    wstr cl = cmdline;
    BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return false; }
    std::thread reader([rd, &output] {
        char buf[4096];
        DWORD n;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
            if (output.size() < 1 << 20) output.append(buf, n);
        }
    });
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
        if (stop && stop->load()) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 2000); break; }
    }
    GetExitCodeProcess(pi.hProcess, &exit_code);
    reader.join();
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

static wstr last_line(const std::string& out) {
    wstr t = trim(wide(out));
    size_t i = t.find_last_of(L'\n');
    return trim(i == wstr::npos ? t : t.substr(i + 1));
}

bool compress_zip(const std::vector<wstr>& paths, const wstr& target, wstr& error, const std::atomic<bool>* stop) {
    if (paths.empty()) return false;
    wstr folder = dir_name(paths[0]);
    wstr tar = find_tar(), sz = find_7zip(), cmd;
    if (!tar.empty()) {
        cmd = quote(tar) + L" -a -c -f " + quote(target) + L" -C " + quote(folder);
        for (auto& p : paths) cmd += L" " + quote(base_name(p));
    } else if (!sz.empty()) {
        cmd = quote(sz) + L" a -tzip -y " + quote(target);
        for (auto& p : paths) cmd += L" " + quote(base_name(p));
    } else {
        error = L"needs tar.exe (Windows 10 1803 or newer) or 7-Zip";
        return false;
    }
    DWORD code = 1;
    std::string out;
    if (!run_process(cmd, folder, code, out, stop)) { error = L"couldn't start the zip tool"; return false; }
    if (stop && stop->load()) { DeleteFileW(target.c_str()); error = L"stopped"; return false; }
    if (code != 0 && !(code == 1 && !sz.empty() && tar.empty())) {
        DeleteFileW(target.c_str());
        error = last_line(out);
        if (error.empty()) error = fmt(L"the zip tool stopped (code %lu)", code);
        return false;
    }
    return true;
}

bool extract_archive(const wstr& archive, const wstr& dest, wstr& error, const std::atomic<bool>* stop) {
    CreateDirectoryW(dest.c_str(), nullptr);
    wstr tar = find_tar(), sz = find_7zip();
    wstr kind = archive_kind(base_name(archive));
    std::string out;
    DWORD code = 1;
    // tar refuses ../ and absolute paths by itself (no -P)
    if (!tar.empty() && (kind != L"7z" || sz.empty())) {
        wstr cmd = quote(tar) + L" -x -f " + quote(archive) + L" -C " + quote(dest);
        if (run_process(cmd, dest, code, out, stop) && code == 0) return true;
        if (!sz.empty() && !(stop && stop->load())) out.clear();
        else {
            wstr l = lower(wide(out));
            error = l.find(L"passphrase") != wstr::npos || l.find(L"encrypt") != wstr::npos
                        ? L"it has a password \u2014 open it with 7-Zip" : last_line(out);
            if (error.empty()) error = fmt(L"tar stopped (code %lu)", code);
            return false;
        }
    }
    if (!sz.empty()) {
        wstr cmd = quote(sz) + L" x -y -bso0 -bsp0 -o" + quote(dest) + L" " + quote(archive);
        if (run_process(cmd, dest, code, out, stop) && (code == 0 || code == 1)) return true;
        error = last_line(out);
        if (error.empty()) error = fmt(L"7-Zip stopped (code %lu)", code);
        return false;
    }
    error = L"needs tar.exe (Windows 10 1803 or newer) or 7-Zip for this kind of archive";
    return false;
}

// a zip's table of contents (read directly, it's fast)
static bool zip_list(const wstr& path, std::vector<ArchItem>& out, wstr& error, size_t limit) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error = L"can't open it"; return false; }
    LARGE_INTEGER size;
    GetFileSizeEx(h, &size);
    auto read_at = [&](i64 off, void* buf, DWORD n) {
        LARGE_INTEGER li;
        li.QuadPart = off;
        DWORD got = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, buf, n, &got, nullptr) && got == n;
    };
    i64 tail = std::min<i64>(size.QuadPart, 65557);
    std::string buf((size_t)tail, '\0');
    bool ok = read_at(size.QuadPart - tail, &buf[0], (DWORD)tail);
    i64 eocd = -1;
    for (i64 i = tail - 22; ok && i >= 0; i--)
        if (*(uint32_t*)&buf[(size_t)i] == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { CloseHandle(h); error = L"not a zip file (or damaged)"; return false; }
    uint64_t count = *(uint16_t*)&buf[(size_t)eocd + 10];
    uint64_t cd_size = *(uint32_t*)&buf[(size_t)eocd + 12];
    uint64_t cd_off = *(uint32_t*)&buf[(size_t)eocd + 16];
    if (cd_off == 0xFFFFFFFF && eocd >= 20 && *(uint32_t*)&buf[(size_t)eocd - 20] == 0x07064b50) {  // zip64
        uint64_t z64 = *(uint64_t*)&buf[(size_t)eocd - 20 + 8];
        char rec[56];
        if (read_at((i64)z64, rec, 56) && *(uint32_t*)rec == 0x06064b50) {
            count = *(uint64_t*)&rec[32];
            cd_size = *(uint64_t*)&rec[40];
            cd_off = *(uint64_t*)&rec[48];
        }
    }
    if (cd_size > 256u * 1024 * 1024) { CloseHandle(h); error = L"too big to list"; return false; }
    std::string cd((size_t)cd_size, '\0');
    if (!read_at((i64)cd_off, &cd[0], (DWORD)cd_size)) { CloseHandle(h); error = L"damaged zip"; return false; }
    CloseHandle(h);
    size_t p = 0;
    for (uint64_t k = 0; k < count && p + 46 <= cd.size() && out.size() < limit; k++) {
        if (*(uint32_t*)&cd[p] != 0x02014b50) break;
        uint16_t flags = *(uint16_t*)&cd[p + 8];
        uint64_t usize = *(uint32_t*)&cd[p + 24];
        uint16_t nlen = *(uint16_t*)&cd[p + 28], xlen = *(uint16_t*)&cd[p + 30], clen = *(uint16_t*)&cd[p + 32];
        if (p + 46 + nlen > cd.size()) break;
        std::string raw = cd.substr(p + 46, nlen);
        if (usize == 0xFFFFFFFF) {  // zip64 extra field
            size_t x = p + 46 + nlen, end = x + xlen;
            while (x + 4 <= end && x + 4 <= cd.size()) {
                uint16_t id = *(uint16_t*)&cd[x], sz = *(uint16_t*)&cd[x + 2];
                if (id == 1 && x + 12 <= cd.size()) { usize = *(uint64_t*)&cd[x + 4]; break; }
                x += 4 + sz;
            }
        }
        wstr name;
        if (flags & 0x800) name = wide(raw);
        else {
            int n = MultiByteToWideChar(437, 0, raw.data(), (int)raw.size(), nullptr, 0);
            name.resize(n);
            MultiByteToWideChar(437, 0, raw.data(), (int)raw.size(), &name[0], n);
        }
        bool dir = !name.empty() && (name.back() == L'/' || name.back() == L'\\');
        out.push_back({name, (i64)usize, dir});
        p += 46 + nlen + xlen + clen;
    }
    return true;
}

bool archive_list(const wstr& path, std::vector<ArchItem>& out, wstr& error, size_t limit) {
    if (archive_kind(base_name(path)) == L"zip") return zip_list(path, out, error, limit);
    wstr tar = find_tar(), sz = find_7zip();
    std::string o;
    DWORD code = 1;
    if (!tar.empty() && run_process(quote(tar) + L" -t -f " + quote(path), dir_name(path), code, o) && code == 0) {
        for (auto& line : split(wide(o), L'\n')) {
            wstr l = trim(line);
            if (l.empty()) continue;
            out.push_back({l, -1, l.back() == L'/'});
            if (out.size() >= limit) break;
        }
        return true;
    }
    error = sz.empty() && tar.empty() ? L"needs tar.exe or 7-Zip to look inside" : L"can't read what's inside";
    return false;
}

// ======================================================================
// text preview
// ======================================================================
const std::set<wstr> TEXT_EXTS = {
    L".txt", L".md", L".markdown", L".log", L".csv", L".tsv", L".json", L".xml", L".html", L".htm", L".css", L".js",
    L".mjs", L".ts", L".jsx", L".tsx", L".py", L".pyw", L".c", L".h", L".cpp", L".hpp", L".cc", L".cs", L".java",
    L".go", L".rs", L".php", L".rb", L".kt", L".swift", L".lua", L".sh", L".bash", L".bat", L".cmd", L".ps1",
    L".psm1", L".sql", L".ini", L".cfg", L".conf", L".toml", L".yml", L".yaml", L".env", L".gitignore", L".srt",
    L".reg", L".tex", L".rst", L".vue", L".svelte", L".r", L".m", L".pl", L".scss", L".less", L".properties",
    L".nfo", L".diff", L".patch"};
const std::set<wstr> THUMB_EXTS = {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp", L".heic", L".heif",
                                   L".tif", L".tiff", L".ico", L".svg", L".mp4", L".mkv", L".avi", L".mov", L".wmv",
                                   L".webm", L".m4v", L".pdf", L".psd", L".raw", L".cr2", L".nef"};
const std::set<wstr> OWN_ICON_EXTS = {L".exe", L".lnk", L".ico", L".url", L".appref-ms", L".msc"};

bool read_text(const wstr& path, size_t limit, wstr& text, bool& cut, bool sniff) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string raw(limit + 1, '\0');
    DWORD got = 0;
    ReadFile(h, &raw[0], (DWORD)raw.size(), &got, nullptr);
    CloseHandle(h);
    raw.resize(got);
    cut = raw.size() > limit;
    if (cut) raw.resize(limit);
    auto u = [&](size_t i) { return (unsigned char)raw[i]; };
    if (raw.size() >= 2 && ((u(0) == 0xFF && u(1) == 0xFE) || (u(0) == 0xFE && u(1) == 0xFF))) {
        bool be = u(0) == 0xFE;
        text.clear();
        for (size_t i = 2; i + 1 < raw.size(); i += 2)
            text += (wchar_t)(be ? (u(i) << 8 | u(i + 1)) : (u(i + 1) << 8 | u(i)));
    } else {
        if (raw.substr(0, 8192).find('\0') != std::string::npos) return false;
        size_t start = raw.size() >= 3 && u(0) == 0xEF && u(1) == 0xBB && u(2) == 0xBF ? 3 : 0;
        const char* p = raw.data() + start;
        int n = (int)(raw.size() - start);
        int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, n, nullptr, 0);
        if (!wn && cut) {  // cut in the middle of a character
            for (int k = 1; k <= 3; k++)
                if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, n - k, nullptr, 0)) { n -= k; wn = 1; break; }
        }
        UINT cp = wn ? CP_UTF8 : CP_ACP;
        wn = MultiByteToWideChar(cp, 0, p, n, nullptr, 0);
        text.assign(wn, L'\0');
        MultiByteToWideChar(cp, 0, p, n, &text[0], wn);
    }
    text = replace_all(text, L"\r\n", L"\n");
    if (sniff) {
        size_t m = std::min<size_t>(text.size(), 4000), good = 0;
        for (size_t i = 0; i < m; i++) {
            wchar_t c = text[i];
            if (c == L'\n' || c == L'\r' || c == L'\t' || (c >= 32 && c != 0xFFFD)) good++;
        }
        if (m && good * 100 / m < 95) return false;
    }
    return true;
}

namespace {
const std::map<wstr, wstr>& lang_of() {
    static std::map<wstr, wstr> m = {
        {L".py", L"py"}, {L".pyw", L"py"}, {L".rb", L"py"}, {L".r", L"py"}, {L".js", L"js"}, {L".mjs", L"js"},
        {L".ts", L"js"}, {L".jsx", L"js"}, {L".tsx", L"js"}, {L".php", L"js"}, {L".vue", L"js"}, {L".svelte", L"js"},
        {L".c", L"c"}, {L".h", L"c"}, {L".cpp", L"c"}, {L".hpp", L"c"}, {L".cc", L"c"}, {L".cs", L"c"},
        {L".java", L"c"}, {L".go", L"c"}, {L".rs", L"c"}, {L".kt", L"c"}, {L".swift", L"c"}, {L".m", L"c"},
        {L".json", L"json"}, {L".sh", L"sh"}, {L".bash", L"sh"}, {L".pl", L"sh"}, {L".bat", L"bat"}, {L".cmd", L"bat"},
        {L".ps1", L"ps"}, {L".psm1", L"ps"}, {L".sql", L"sql"}, {L".lua", L"sql"}, {L".html", L"html"},
        {L".htm", L"html"}, {L".xml", L"html"}, {L".svg", L"html"}, {L".css", L"css"}, {L".scss", L"css"},
        {L".less", L"css"}, {L".md", L"md"}, {L".markdown", L"md"}, {L".ini", L"ini"}, {L".cfg", L"ini"},
        {L".conf", L"ini"}, {L".toml", L"ini"}, {L".reg", L"ini"}, {L".properties", L"ini"}, {L".env", L"ini"},
        {L".yml", L"yaml"}, {L".yaml", L"yaml"}};
    return m;
}

std::set<wstr> words_of(const wchar_t* s) {
    std::set<wstr> out;
    for (auto& w : split(s, L' ')) if (!w.empty()) out.insert(w);
    return out;
}

const std::set<wstr>& keywords(const wstr& lang) {
    static std::map<wstr, std::set<wstr>> m = {
        {L"py", words_of(L"False None True and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield self print end require module")},
        {L"js", words_of(L"break case catch class const continue debugger default delete do else export extends finally for function if import in instanceof let new return super switch this throw try typeof var void while with yield async await of null undefined true false interface type enum implements public private protected static readonly from as echo")},
        {L"c", words_of(L"auto break case char const continue default do double else enum extern float for goto if int long register return short signed sizeof static struct switch typedef union unsigned void volatile while class public private protected namespace using template typename new delete true false nullptr bool include define string var func package import fn let mut impl pub use match override virtual this null val")},
        {L"sh", words_of(L"if then else elif fi for while do done case esac in function return local export echo exit source set unset read shift my sub use")},
        {L"bat", words_of(L"echo set if else for in do goto call exit not exist errorlevel setlocal endlocal defined pause start cd copy del move")},
        {L"ps", words_of(L"function param if else elseif foreach for while do switch return begin process end try catch finally throw break continue in")},
        {L"sql", words_of(L"select from where insert into values update set delete create table drop alter join left right inner outer on group by order having limit and or not null as distinct union all primary key index view local function end then if else elseif return for in do while nil true false")},
    };
    static std::set<wstr> none;
    auto it = m.find(lang);
    return it == m.end() ? none : it->second;
}

bool word_char(wchar_t c) { return iswalnum(c) || c == L'_'; }
}  // namespace

bool is_code_ext(const wstr& ext) {
    auto& m = lang_of();
    auto it = m.find(ext);
    return it != m.end() && it->second != L"md";
}

std::vector<Span> highlight(const wstr& text0, const wstr& ext) {
    std::vector<Span> out;
    auto it = lang_of().find(ext);
    if (it == lang_of().end()) return out;
    const wstr lang = it->second;
    const wstr& t = text0;
    const int n = (int)std::min<size_t>(t.size(), 300000);
    auto add = [&](int a, int b, TokKind k) { if (b > a && out.size() < 40000) out.push_back({a, b - a, k}); };
    auto line_start = [&](int i) { int j = i; while (j > 0 && t[j - 1] != L'\n') j--; for (int k = j; k < i; k++) if (!iswspace(t[k])) return false; return true; };
    auto line_end = [&](int i) { while (i < n && t[i] != L'\n') i++; return i; };
    bool ci = lang == L"bat" || lang == L"ps" || lang == L"sql";
    const std::set<wstr>& kw = keywords(lang);

    if (lang == L"md") {
        for (int i = 0; i < n;) {
            int e = line_end(i);
            if (t[i] == L'#') { add(i, e, T_HEADING); i = e + 1; continue; }
            for (int k = i; k < e; k++) {
                if (t[k] == L'`') {
                    int j = (int)t.find(L'`', k + 1);
                    if (j > 0 && j <= e) { add(k, j + 1, T_CODE); k = j; }
                } else if (k + 1 < e && t[k] == L'*' && t[k + 1] == L'*') {
                    int j = (int)t.find(L"**", k + 2);
                    if (j > 0 && j < e) { add(k, j + 2, T_BOLD); k = j + 1; }
                } else if (t[k] == L'[') {
                    int j = (int)t.find(L"](", k);
                    int z = j > 0 ? (int)t.find(L')', j) : -1;
                    if (j > 0 && z > 0 && z < e) { add(k, z + 1, T_KEY); k = z; }
                }
            }
            i = e + 1;
        }
        return out;
    }
    if (lang == L"ini" || lang == L"yaml") {
        for (int i = 0; i < n;) {
            int e = line_end(i), s = i;
            while (s < e && iswspace(t[s])) s++;
            if (s < e && (t[s] == L'#' || (lang == L"ini" && t[s] == L';'))) add(s, e, T_COMMENT);
            else if (lang == L"ini" && s < e && t[s] == L'[') add(s, e, T_HEADING);
            else {
                wchar_t sep = lang == L"ini" ? L'=' : L':';
                int k = s;
                if (lang == L"yaml" && k < e && t[k] == L'-') k++;
                int j = k;
                while (j < e && t[j] != sep && t[j] != L'"' && t[j] != L'\'') j++;
                if (j < e && t[j] == sep && j > k) {
                    add(k, j, T_KEY);
                    int v = j + 1;
                    while (v < e && iswspace(t[v])) v++;
                    if (v < e && (t[v] == L'"' || t[v] == L'\'')) add(v, e, T_STRING);
                    else if (v < e && (iswdigit(t[v]) || t[v] == L'-')) add(v, e, T_NUMBER);
                    if (lang == L"yaml") {
                        int c = (int)t.find(L" #", v);
                        if (c > 0 && c < e) add(c + 1, e, T_COMMENT);
                    }
                }
            }
            i = e + 1;
        }
        return out;
    }
    if (lang == L"html") {
        for (int i = 0; i < n;) {
            if (t.compare(i, 4, L"<!--") == 0) {
                int j = (int)t.find(L"-->", i);
                j = j < 0 ? n : j + 3;
                add(i, j, T_COMMENT);
                i = j;
            } else if (t[i] == L'<' && i + 1 < n && (iswalpha(t[i + 1]) || t[i + 1] == L'/' || t[i + 1] == L'!' || t[i + 1] == L'?')) {
                int j = i + 1;
                while (j < n && (word_char(t[j]) || t[j] == L'/' || t[j] == L':' || t[j] == L'-' || t[j] == L'!' || t[j] == L'?')) j++;
                add(i, j, T_TAG);
                while (j < n && t[j] != L'>') {  // attributes
                    if (t[j] == L'"' || t[j] == L'\'') {
                        int z = (int)t.find(t[j], j + 1);
                        z = z < 0 ? n : z + 1;
                        add(j, z, T_STRING);
                        j = z;
                    } else if (word_char(t[j])) {
                        int z = j;
                        while (z < n && (word_char(t[z]) || t[z] == L'-' || t[z] == L':')) z++;
                        if (z < n && t[z] == L'=') add(j, z, T_KEY);
                        j = z;
                    } else j++;
                }
                if (j < n) { add(j - (t[j - 1] == L'/' ? 1 : 0), j + 1, T_TAG); j++; }
                i = j;
            } else i++;
        }
        return out;
    }
    // code-like languages
    bool brace = false;  // css: inside { }
    for (int i = 0; i < n;) {
        wchar_t c = t[i];
        // block comments
        if ((lang == L"js" || lang == L"c" || lang == L"css" || lang == L"sql") && c == L'/' && i + 1 < n && t[i + 1] == L'*') {
            int j = (int)t.find(L"*/", i + 2);
            j = j < 0 ? n : j + 2;
            add(i, j, T_COMMENT); i = j; continue;
        }
        if (lang == L"ps" && c == L'<' && i + 1 < n && t[i + 1] == L'#') {
            int j = (int)t.find(L"#>", i + 2);
            j = j < 0 ? n : j + 2;
            add(i, j, T_COMMENT); i = j; continue;
        }
        // line comments
        bool lc = ((lang == L"py" || lang == L"sh" || lang == L"ps") && c == L'#') ||
                  ((lang == L"js" || lang == L"c") && c == L'/' && i + 1 < n && t[i + 1] == L'/') ||
                  (lang == L"sql" && c == L'-' && i + 1 < n && t[i + 1] == L'-') ||
                  (lang == L"bat" && line_start(i) && (t.compare(i, 2, L"::") == 0 || (i + 3 <= n && lower(t.substr(i, 3)) == L"rem" && (i + 3 == n || !word_char(t[i + 3])))));
        if (lang == L"c" && c == L'#' && line_start(i)) {  // #include
            int j = i + 1;
            while (j < n && word_char(t[j])) j++;
            add(i, j, T_KEY); i = j; continue;
        }
        if (lc) { int j = line_end(i); add(i, j, T_COMMENT); i = j; continue; }
        // strings
        if (c == L'"' || c == L'\'' || (c == L'`' && lang == L"js")) {
            int j;
            if (lang == L"py" && i + 2 < n && t[i + 1] == c && t[i + 2] == c) {
                int z = (int)t.find(wstr(3, c), i + 3);
                j = z < 0 ? n : z + 3;
            } else {
                j = i + 1;
                while (j < n && t[j] != c && (t[j] != L'\n' || c == L'`')) {
                    if (t[j] == L'\\' && lang != L"bat") j++;
                    j++;
                }
                j = std::min(n, j + 1);
            }
            if (lang == L"json") {  // a string followed by ':' is a key
                int k = j;
                while (k < n && (t[k] == L' ' || t[k] == L'\t')) k++;
                add(i, j, k < n && t[k] == L':' ? T_KEY : T_STRING);
            } else add(i, j, T_STRING);
            i = j; continue;
        }
        // variables
        if ((lang == L"sh" || lang == L"ps") && c == L'$') {
            int j = i + 1;
            if (j < n && t[j] == L'{') { int z = (int)t.find(L'}', j); j = z < 0 ? n : z + 1; }
            else while (j < n && word_char(t[j])) j++;
            add(i, j, T_KEY); i = j; continue;
        }
        if (lang == L"bat" && (c == L'%' || c == L'!')) {
            int j = i + 1;
            if (j < n && t[j] == L'~') j++;
            while (j < n && word_char(t[j])) j++;
            if (j < n && t[j] == c) j++;
            if (j > i + 1) { add(i, j, T_KEY); i = j; continue; }
        }
        if (lang == L"css") {
            if (c == L'{') brace = true;
            if (c == L'}') brace = false;
        }
        // numbers
        if (iswdigit(c) && (i == 0 || !word_char(t[i - 1]))) {
            int j = i + 1;
            if (c == L'0' && j < n && (t[j] == L'x' || t[j] == L'X')) { j++; while (j < n && iswxdigit(t[j])) j++; }
            else {
                while (j < n && (iswdigit(t[j]) || t[j] == L'.')) j++;
                if (j < n && (t[j] == L'e' || t[j] == L'E')) { j++; if (j < n && (t[j] == L'+' || t[j] == L'-')) j++; while (j < n && iswdigit(t[j])) j++; }
            }
            if (j < n && word_char(t[j]) && lang != L"css") { while (j < n && word_char(t[j])) j++; i = j; continue; }
            add(i, j, T_NUMBER); i = j; continue;
        }
        // words
        if (word_char(c) || (lang == L"css" && (c == L'-' || c == L'.' || c == L'#'))) {
            int j = i + 1;
            while (j < n && (word_char(t[j]) || (lang == L"css" && t[j] == L'-'))) j++;
            if (lang == L"css") {
                int k = j;
                while (k < n && (t[k] == L' ' || t[k] == L'\t')) k++;
                if (brace && k < n && t[k] == L':') add(i, j, T_KEY);
                else if (!brace) add(i, j, T_TAG);
            } else if (lang == L"json") {
                wstr w = t.substr(i, j - i);
                if (w == L"true" || w == L"false" || w == L"null") add(i, j, T_KEYWORD);
            } else {
                wstr w = t.substr(i, j - i);
                if (kw.count(ci ? lower(w) : w)) add(i, j, T_KEYWORD);
            }
            i = j; continue;
        }
        i++;
    }
    return out;
}

// ======================================================================
// rename
// ======================================================================
const wchar_t* CASE_NAMES[5] = {L"Keep", L"lowercase", L"UPPERCASE", L"Title Case", L"Sentence case"};

std::vector<wstr> bulk_names(const std::vector<Entry>& entries, const BulkOpts& o, wstr& error) {
    std::vector<wstr> out;
    error.clear();
    std::unique_ptr<std::wregex> rx;
    wstr repl = o.repl;
    if (!o.find.empty() && o.regex) {
        try {
            auto flags = std::regex::ECMAScript | (o.match_case ? std::regex::flag_type(0) : std::regex::icase);
            rx.reset(new std::wregex(o.find, flags));
        } catch (std::regex_error& e) {
            error = L"Pattern problem: " + wide(e.what());
            for (auto& e2 : entries) out.push_back(e2.name);
            return out;
        }
        repl = std::regex_replace(repl, std::wregex(LR"(\\(\d))"), L"$$$1");  // \1 works like $1
    }
    wstr today = fmt_date_iso(now_unix());
    for (size_t i = 0; i < entries.size(); i++) {
        const Entry& e = entries[i];
        wstr stem = e.name, ext;
        if (o.keep_ext && !e.dir) split_ext(e.name, stem, ext);
        long n = o.start + (long)i * o.step;
        wstr num = fmt(L"%0*ld", std::max(1, o.digits), n < 0 ? -n : n);
        if (n < 0) num = L"-" + num;
        wstr parent = display_name(dir_name(e.path));
        wstr date = e.mtime > 0 ? fmt_date_iso(e.mtime) : today;
        wstr tpl = o.tpl.empty() ? L"{name}" : o.tpl, s;
        for (size_t k = 0; k < tpl.size(); k++) {
            if (tpl[k] == L'{') {
                size_t z = tpl.find(L'}', k);
                if (z != wstr::npos) {
                    wstr key = lower(tpl.substr(k + 1, z - k - 1));
                    wstr v = key == L"name" ? stem : key == L"n" ? num : key == L"date" ? date : key == L"today" ? today
                             : key == L"parent" ? parent : key == L"ext" ? (ext.empty() ? L"" : ext.substr(1)) : L"\x01";
                    if (v != L"\x01") { s += v; k = z; continue; }
                }
            }
            s += tpl[k];
        }
        if (!o.find.empty()) {
            if (rx) {
                try { s = std::regex_replace(s, *rx, repl); } catch (std::regex_error& ex) { error = L"Pattern problem: " + wide(ex.what()); }
            } else {
                wstr hay = o.match_case ? s : lower(s), nd = o.match_case ? o.find : lower(o.find), r;
                size_t pos = 0, f;
                while ((f = hay.find(nd, pos)) != wstr::npos) { r += s.substr(pos, f - pos) + o.repl; pos = f + nd.size(); }
                s = r + s.substr(pos);
            }
        }
        switch (o.case_mode) {
            case 1: s = lower(s); break;
            case 2: if (!s.empty()) CharUpperBuffW(&s[0], (DWORD)s.size()); break;
            case 3: {
                s = lower(s);
                for (size_t k = 0; k < s.size(); k++)
                    if (k == 0 || wcschr(L" \t_-([.", s[k - 1])) s[k] = towupper(s[k]);
                break;
            }
            case 4: s = lower(s); if (!s.empty()) s[0] = towupper(s[0]); break;
        }
        out.push_back(trim(s) + ext);
    }
    return out;
}

std::map<int, wstr> bulk_problems(const std::vector<Entry>& entries, const std::vector<wstr>& names) {
    std::map<int, wstr> problems;
    std::map<wstr, int> seen;
    std::set<wstr> sources;
    for (auto& e : entries) sources.insert(norm(e.path));
    for (size_t i = 0; i < entries.size() && i < names.size(); i++) {
        const Entry& e = entries[i];
        const wstr& nw = names[i];
        wstr key = norm(dir_name(e.path)) + L"|" + lower(nw);
        if (seen.count(key)) {
            problems[(int)i] = L"Two items would get this name";
            if (!problems.count(seen[key])) problems[seen[key]] = L"Two items would get this name";
            continue;
        }
        seen[key] = (int)i;
        if (nw == e.name) continue;
        wstr why = check_name(nw);
        if (why.empty()) {
            wstr target = path_join(dir_name(e.path), nw);
            if (exists(target) && !sources.count(norm(target))) why = L"Something with this name is already there";
        }
        if (!why.empty()) problems[(int)i] = why;
    }
    return problems;
}

bool apply_renames(std::vector<std::pair<wstr, wstr>> pairs, std::vector<std::pair<wstr, wstr>>& done_out, wstr& error) {
    std::vector<std::pair<wstr, wstr>> todo;
    for (auto& p : pairs)
        if (p.first != p.second) todo.push_back(p);
    struct St { wstr o, t, n; };
    std::vector<St> staged, done;
    bool ok = true;
    for (auto& [o, n] : todo) {
        wstr t = path_join(dir_name(o), fmt(L".~rename-%08x%04x", GetTickCount(), (unsigned)staged.size()));
        if (!MoveFileW(o.c_str(), t.c_str())) { ok = false; break; }
        staged.push_back({o, t, n});
    }
    if (ok)
        for (auto& s : staged) {
            if (!MoveFileW(s.t.c_str(), s.n.c_str())) { ok = false; break; }
            done.push_back(s);
        }
    if (!ok) {
        DWORD e = GetLastError();
        wchar_t* msg = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       e, 0, (LPWSTR)&msg, 0, nullptr);
        error = msg ? trim(msg) : fmt(L"error %lu", e);
        if (msg) LocalFree(msg);
        for (auto it = done.rbegin(); it != done.rend(); ++it) MoveFileW(it->n.c_str(), it->t.c_str());
        for (auto it = staged.rbegin(); it != staged.rend(); ++it) MoveFileW(it->t.c_str(), it->o.c_str());
        return false;
    }
    done_out = todo;
    return true;
}

// ======================================================================
// Windows shell
// ======================================================================
static wstr multi_sz(const std::vector<wstr>& paths) {
    wstr s;
    for (auto& p : paths) { s += clean_path(p); s += L'\0'; }
    s += L'\0';
    return s;
}

bool shell_file_op(FileOp op, const std::vector<wstr>& paths, const wstr& dest, unsigned flags, HWND owner, bool& aborted) {
    wstr from = multi_sz(paths), to;
    SHFILEOPSTRUCTW s{};
    s.hwnd = owner;
    s.wFunc = op == OP_MOVE ? FO_MOVE : op == OP_COPY ? FO_COPY : FO_DELETE;
    s.pFrom = from.c_str();
    if (!dest.empty()) { to = clean_path(dest); to += L'\0'; to += L'\0'; s.pTo = to.c_str(); }
    s.fFlags = (FILEOP_FLAGS)flags;
    int r = SHFileOperationW(&s);
    aborted = s.fAnyOperationsAborted != 0;
    return r == 0;
}

static UINT cf_effect() { static UINT f = RegisterClipboardFormatW(L"Preferred DropEffect"); return f; }

static bool open_clip(HWND owner) {
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(owner)) return true;
        Sleep(30);
    }
    return false;
}

bool clipboard_set_files(const std::vector<wstr>& paths, bool cut, HWND owner) {
    wstr files = multi_sz(paths);
    size_t bytes = sizeof(DROPFILES) + files.size() * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    auto* df = (DROPFILES*)GlobalLock(h);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy((char*)df + sizeof(DROPFILES), files.data(), files.size() * sizeof(wchar_t));
    GlobalUnlock(h);
    HGLOBAL e = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 4);
    *(DWORD*)GlobalLock(e) = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY | DROPEFFECT_LINK;
    GlobalUnlock(e);
    if (!open_clip(owner)) { GlobalFree(h); GlobalFree(e); return false; }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, h);
    SetClipboardData(cf_effect(), e);
    CloseClipboard();
    return true;
}

bool clipboard_get_files(HWND owner, std::vector<wstr>& paths, bool& cut) {
    paths.clear();
    cut = false;
    if (!IsClipboardFormatAvailable(CF_HDROP) || !open_clip(owner)) return false;
    HDROP h = (HDROP)GetClipboardData(CF_HDROP);
    if (h) {
        UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(h, i, nullptr, 0);
            wstr p(len, L'\0');
            DragQueryFileW(h, i, &p[0], len + 1);
            paths.push_back(p);
        }
        HANDLE eff = GetClipboardData(cf_effect());
        if (eff) {
            DWORD* d = (DWORD*)GlobalLock(eff);
            if (d) { cut = (*d & DROPEFFECT_MOVE) != 0; GlobalUnlock(eff); }
        }
    }
    CloseClipboard();
    return !paths.empty();
}

void clipboard_set_text(HWND owner, const wstr& t) {
    if (!open_clip(owner)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    memcpy(GlobalLock(h), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
}

bool open_file(const wstr& path, wstr& error) {
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = path.c_str();
    sei.lpDirectory = nullptr;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return true;
    DWORD e = GetLastError();
    if (e == ERROR_NO_ASSOCIATION) { open_with(path); return true; }
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   (LPWSTR)&msg, 0, nullptr);
    error = msg ? trim(msg) : fmt(L"error %lu", e);
    if (msg) LocalFree(msg);
    return false;
}

void open_with(const wstr& path) {
    OPENASINFO oi{};
    oi.pcszFile = path.c_str();
    oi.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_EXEC;
    if (FAILED(SHOpenWithDialog(nullptr, &oi))) {
        wstr args = L"shell32.dll,OpenAs_RunDLL " + path;
        ShellExecuteW(nullptr, nullptr, L"rundll32.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void show_properties(const wstr& path, HWND owner) { SHObjectProperties(owner, SHOP_FILEPATH, path.c_str(), nullptr); }

void open_in_explorer(const wstr& path, bool select) {
    wstr args = select ? L"/select,\"" + path + L"\"" : L"\"" + path + L"\"";
    ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void open_terminal(const wstr& folder) {
    wchar_t wt[MAX_PATH];
    if (SearchPathW(nullptr, L"wt.exe", nullptr, MAX_PATH, wt, nullptr)) {
        wstr args = L"-d \"" + folder + L"\"";
        ShellExecuteW(nullptr, nullptr, wt, args.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    wstr args = L"/K cd /d \"" + folder + L"\"";
    ShellExecuteW(nullptr, nullptr, L"cmd.exe", args.c_str(), folder.c_str(), SW_SHOWNORMAL);
}

// ---- folder colours in Windows Explorer: a desktop.ini with our icon (the folder's own icon is kept)
wstr folder_icons_dir() { return path_join(app_dir(), L"folder_icons"); }

static wstr read_ini_text(const wstr& path) {
    std::string raw;
    if (!read_file(path, raw)) return L"";
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE)
        return wstr((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF) return wide(raw.substr(3));
    int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
    wstr w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), &w[0], n);
    return w;
}

static wstr ini_with_icon(const wstr& text, const wstr& ico) {
    std::vector<wstr> out;
    bool in_sec = false, found = false;
    wstr ours = lower(folder_icons_dir());
    for (auto line : split(replace_all(text, L"\r\n", L"\n"), L'\n')) {
        wstr st = trim(line);
        if (!st.empty() && st.front() == L'[' && st.back() == L']') {
            if (in_sec && !ico.empty() && !found) { out.push_back(L"IconResource=" + ico + L",0"); found = true; }
            in_sec = lower(st) == L"[.shellclassinfo]";
            out.push_back(line);
            continue;
        }
        if (in_sec && starts_with(st, L";Original-")) { out.push_back(ico.empty() ? st.substr(10) : line); continue; }
        wstr l = lower(st);
        if (in_sec && (starts_with(l, L"iconresource") || starts_with(l, L"iconfile") || starts_with(l, L"iconindex"))) {
            if (l.find(ours) == wstr::npos) out.push_back(!ico.empty() ? L";Original-" + st : line);
            continue;
        }
        out.push_back(line);
    }
    if (!ico.empty() && !found) {
        if (in_sec) out.push_back(L"IconResource=" + ico + L",0");
        else {
            if (!out.empty() && !trim(out.back()).empty()) out.push_back(L"");
            out.push_back(L"[.ShellClassInfo]");
            out.push_back(L"IconResource=" + ico + L",0");
        }
    }
    wstr r;
    for (auto& l : out) r += l + L"\r\n";
    r = trim(r);
    if (lower(r) == L"[.shellclassinfo]") return L"";
    return r.empty() ? L"" : r + L"\r\n";
}

bool set_explorer_folder_color(const wstr& folder, const std::string& color, const std::string& ico_data, wstr& error) {
    wstr ini = path_join(folder, L"desktop.ini"), ico;
    if (!color.empty()) {
        CreateDirectoryW(folder_icons_dir().c_str(), nullptr);
        ico = path_join(folder_icons_dir(), wide(color) + L".ico");
        if (!write_file_atomic(ico, ico_data)) { error = L"can't write the icon"; return false; }
    }
    wstr text = exists(ini) ? read_ini_text(ini) : L"";
    wstr nw = ini_with_icon(text, ico);
    if (exists(ini)) SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!nw.empty()) {
        std::string data = "\xFF\xFE";
        data.append((const char*)nw.data(), nw.size() * 2);
        if (!write_file_atomic(ini, data)) { error = L"can't write desktop.ini (no permission?)"; return false; }
        SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        if (!PathMakeSystemFolderW(folder.c_str())) {
            DWORD a = GetFileAttributesW(folder.c_str());
            if (a != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(folder.c_str(), a | FILE_ATTRIBUTE_READONLY);
        }
    } else if (exists(ini)) {
        DeleteFileW(ini.c_str());
        PathUnmakeSystemFolderW(folder.c_str());
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, folder.c_str(), nullptr);
    wstr parent = dir_name(folder);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, parent.c_str(), nullptr);
    return true;
}

// ======================================================================
// watcher
// ======================================================================
void Watcher::watch(const wstr& path, std::function<void()> changed) {
    stop();
    if (path.empty() || is_virtual(path)) return;
    HANDLE ch = FindFirstChangeNotificationW(path.c_str(), FALSE,
                                             FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                 FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (ch == INVALID_HANDLE_VALUE) return;
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    alive_ = std::make_shared<std::atomic<bool>>(true);
    HANDLE stop_ev = stop_event_;
    auto alive = alive_;
    std::thread([ch, stop_ev, alive, changed] {
        HANDLE hs[2] = {stop_ev, ch};
        for (;;) {
            DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0 + 1) break;
            // wait for things to calm down (a copy makes many changes)
            while (WaitForSingleObject(stop_ev, 250) == WAIT_TIMEOUT) {
                if (!FindNextChangeNotification(ch)) break;
                if (WaitForSingleObject(ch, 0) != WAIT_OBJECT_0) break;
            }
            if (WaitForSingleObject(stop_ev, 0) == WAIT_OBJECT_0) break;
            post_ui([alive, changed] { if (alive->load()) changed(); });
            if (!FindNextChangeNotification(ch)) break;
        }
        FindCloseChangeNotification(ch);
        CloseHandle(stop_ev);
    }).detach();
}

void Watcher::stop() {
    if (alive_) alive_->store(false);
    if (stop_event_) SetEvent(stop_event_);
    stop_event_ = nullptr;
    alive_.reset();
}
