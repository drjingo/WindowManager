#include "util.h"
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cwctype>

// ======================================================================
// strings
// ======================================================================
std::string utf8(const wstr& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

wstr wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstr out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

wstr lower(wstr s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

wstr trim(const wstr& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool starts_with(const wstr& s, const wstr& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends_with(const wstr& s, const wstr& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool ends_with_ci(const wstr& s, const wstr& p) { return ends_with(lower(s), lower(p)); }
bool contains_ci(const wstr& hay, const wstr& needle) { return lower(hay).find(needle) != wstr::npos; }

std::vector<wstr> split(const wstr& s, wchar_t sep) {
    std::vector<wstr> out;
    size_t start = 0;
    for (;;) {
        size_t i = s.find(sep, start);
        out.push_back(s.substr(start, i == wstr::npos ? wstr::npos : i - start));
        if (i == wstr::npos) break;
        start = i + 1;
    }
    return out;
}

wstr replace_all(wstr s, const wstr& a, const wstr& b) {
    if (a.empty()) return s;
    size_t i = 0;
    while ((i = s.find(a, i)) != wstr::npos) { s.replace(i, a.size(), b); i += b.size(); }
    return s;
}

wstr fmt(const wchar_t* f, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf(buf, 2047, f, ap);
    va_end(ap);
    buf[2047] = 0;
    return buf;
}

int natural_cmp(const wstr& a, const wstr& b) { return StrCmpLogicalW(a.c_str(), b.c_str()); }

// ======================================================================
// paths
// ======================================================================
const wstr VIRTUAL = L"::";

bool is_virtual(const wstr& p) { return starts_with(p, VIRTUAL); }
wstr make_search_path(const wstr& root, const wstr& q) { return VIRTUAL + L"search\n" + root + L"\n" + q; }
wstr make_tag_path(const wstr& name) { return VIRTUAL + L"tag\n" + name; }

void parse_virtual(const wstr& p, wstr& kind, wstr& a, wstr& b) {
    kind.clear(); a.clear(); b.clear();
    if (!is_virtual(p)) return;
    wstr rest = p.substr(2);
    size_t i = rest.find(L'\n');
    kind = rest.substr(0, i);
    if (i == wstr::npos) return;
    rest = rest.substr(i + 1);
    size_t j = rest.find(L'\n');
    a = rest.substr(0, j);
    if (j != wstr::npos) b = rest.substr(j + 1);
}

wstr clean_path(const wstr& p0) {
    if (p0.empty() || is_virtual(p0)) return p0;
    wstr p = replace_all(p0, L"/", L"\\");
    // collapse \.\ and \..\ (only simple cases)
    std::vector<wstr> parts = split(p, L'\\');
    std::vector<wstr> out;
    bool unc = starts_with(p, L"\\\\");
    for (size_t i = 0; i < parts.size(); i++) {
        const wstr& s = parts[i];
        if (s == L"." ) continue;
        if (s.empty() && i > 0 && !(unc && i < 2)) continue;
        if (s == L".." && !out.empty() && out.back() != L".." && !(out.size() == 1 && out[0].size() == 2 && out[0][1] == L':')) {
            out.pop_back();
            continue;
        }
        out.push_back(s);
    }
    wstr r;
    for (size_t i = 0; i < out.size(); i++) {
        if (i) r += L"\\";
        r += out[i];
    }
    if (unc && !starts_with(r, L"\\\\")) r = L"\\" + r;
    if (r.size() == 2 && r[1] == L':') r += L"\\";
    if (r.empty()) r = L"\\";
    return r;
}

wstr norm(const wstr& p) {
    if (p.empty() || is_virtual(p)) return p;
    return lower(clean_path(p));
}

wstr path_join(const wstr& a, const wstr& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

wstr dir_name(const wstr& p0) {
    wstr p = clean_path(p0);
    if (is_drive_root(p)) return p;
    size_t i = p.find_last_of(L"\\/");
    if (i == wstr::npos) return L"";
    wstr d = p.substr(0, i);
    if (d.size() == 2 && d[1] == L':') d += L"\\";
    return d;
}

wstr base_name(const wstr& p0) {
    wstr p = clean_path(p0);
    if (is_drive_root(p)) return p;
    size_t i = p.find_last_of(L"\\/");
    return i == wstr::npos ? p : p.substr(i + 1);
}

wstr ext_lower(const wstr& name) {
    size_t i = name.find_last_of(L'.');
    if (i == wstr::npos || i == 0 || name.find_first_not_of(L'.') == wstr::npos) return L"";
    if (name.find_last_of(L"\\/") != wstr::npos && name.find_last_of(L"\\/") > i) return L"";
    return lower(name.substr(i));
}

void split_ext(const wstr& name, wstr& stem, wstr& ext) {
    size_t i = name.find_last_of(L'.');
    wstr t = name;
    size_t first = t.find_first_not_of(L'.');
    if (i == wstr::npos || first == wstr::npos || i <= first) { stem = name; ext.clear(); return; }
    stem = name.substr(0, i);
    ext = name.substr(i);
}

bool is_drive_root(const wstr& p) {
    return (p.size() == 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) || (p.size() == 2 && p[1] == L':');
}

wstr display_name(const wstr& p) {
    if (p.empty()) return L"This PC";
    if (is_virtual(p)) {
        wstr k, a, b;
        parse_virtual(p, k, a, b);
        return k == L"search" ? L"Search \u201c" + b + L"\u201d" : L"Tag: " + a;
    }
    wstr c = clean_path(p);
    if (is_drive_root(c)) return c;
    wstr b = base_name(c);
    return b.empty() ? c : b;
}

std::vector<wstr> ancestors(const wstr& p0) {
    std::vector<wstr> chain;
    if (p0.empty() || is_virtual(p0)) return {L""};
    wstr p = clean_path(p0);
    for (int guard = 0; guard < 256; guard++) {
        chain.push_back(p);
        wstr parent = dir_name(p);
        if (parent.empty() || norm(parent) == norm(p)) break;
        p = parent;
    }
    std::reverse(chain.begin(), chain.end());
    chain.insert(chain.begin(), L"");
    return chain;
}

bool is_dir(const wstr& p) {
    if (p.empty() || is_virtual(p)) return false;
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool exists(const wstr& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool same_drive(const wstr& a, const wstr& b) {
    auto root = [](const wstr& p) {
        wstr n = norm(p);
        if (starts_with(n, L"\\\\")) {  // \\server\share
            size_t i = n.find(L'\\', 2);
            i = i == wstr::npos ? wstr::npos : n.find(L'\\', i + 1);
            return n.substr(0, i);
        }
        return n.substr(0, 2);
    };
    return root(a) == root(b);
}

bool is_inside(const wstr& child, const wstr& parent) {
    wstr c = norm(child), p = norm(parent);
    if (c == p) return true;
    if (!p.empty() && p.back() != L'\\') p += L"\\";
    return starts_with(c, p);
}

wstr unique_name(const wstr& folder, const wstr& name, bool copy_suffix) {
    wstr base = name, ext;
    if (!is_dir(path_join(folder, name))) split_ext(name, base, ext);
    if (copy_suffix) base += L" - Copy";
    wstr cand = base + ext;
    for (int n = 2; exists(path_join(folder, cand)); n++) cand = base + fmt(L" (%d)", n) + ext;
    return path_join(folder, cand);
}

wstr check_name(const wstr& name0) {
    wstr name = trim(name0);
    if (name.empty()) return L"The name can't be empty.";
    wstr bad;
    for (wchar_t c : wstr(L"<>:\"/\\|?*"))
        if (name.find(c) != wstr::npos) { bad += bad.empty() ? L"" : L" "; bad += c; }
    if (!bad.empty()) return L"A name can't contain " + bad;
    for (wchar_t c : name)
        if (c < 32) return L"A name can't contain control characters.";
    if (name == L"." || name == L"..") return L"That name isn't allowed.";
    wstr up = name.substr(0, name.find(L'.'));
    CharUpperBuffW(&up[0], (DWORD)up.size());
    static const wchar_t* reserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5",
                                        L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4",
                                        L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (auto r : reserved)
        if (up == r) return L"Windows keeps that name for itself.";
    if (name.back() == L' ' || name.back() == L'.') return L"A name can't end with a space or a dot.";
    return L"";
}

wstr expand_env(const wstr& s) {
    wchar_t buf[4096];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 4096);
    return n && n < 4096 ? wstr(buf) : s;
}

wstr resolve_typed_path(const wstr& text0, const wstr& here) {
    wstr text = trim(text0);
    while (!text.empty() && text.front() == L'"') text.erase(0, 1);
    while (!text.empty() && text.back() == L'"') text.pop_back();
    text = trim(text);
    wstr l = lower(text);
    if (l.empty() || l == L"this pc" || l == L"computer" || l == L"my computer") return L"";
    text = expand_env(text);
    if (starts_with(text, L"~")) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
        if (n && n < MAX_PATH) text = wstr(buf) + text.substr(1);
    }
    if (text.size() == 2 && text[1] == L':') text += L"\\";
    bool abs = (text.size() >= 2 && text[1] == L':') || starts_with(text, L"\\\\") || starts_with(text, L"\\");
    if (!abs && !here.empty() && !is_virtual(here)) text = path_join(here, text);
    return clean_path(text);
}

wstr exe_path() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return wstr(buf, n);
}

wstr app_dir() { return dir_name(exe_path()); }

// ======================================================================
// time / numbers
// ======================================================================
double now_unix() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return filetime_unix(ft);
}

double filetime_unix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (!u.QuadPart) return 0;
    return (double)(u.QuadPart - 116444736000000000ULL) / 1e7;
}

static bool to_local(double t, SYSTEMTIME& st) {
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(t * 1e7) + 116444736000000000ULL;
    FILETIME ft{u.LowPart, u.HighPart}, lft;
    return FileTimeToLocalFileTime(&ft, &lft) && FileTimeToSystemTime(&lft, &st);
}

wstr fmt_size(i64 n) {
    if (n < 0) return L"";
    if (n < 1024) return fmt(L"%lld B", n);
    const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB"};
    double v = (double)n;
    for (int i = 0; i < 4; i++) {
        v /= 1024;
        if (v < 1024 || i == 3) {
            if (i == 0 || v >= 100) return fmt(L"%.0f %s", v, units[i]);
            return fmt(L"%.1f %s", v, units[i]);
        }
    }
    return L"";
}

wstr fmt_time(double t) {
    if (t <= 0) return L"";
    SYSTEMTIME st, now;
    if (!to_local(t, st)) return L"";
    to_local(now_unix(), now);
    if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay)
        return fmt(L"Today %02d:%02d", st.wHour, st.wMinute);
    SYSTEMTIME y;
    to_local(now_unix() - 86400, y);
    if (st.wYear == y.wYear && st.wMonth == y.wMonth && st.wDay == y.wDay)
        return fmt(L"Yesterday %02d:%02d", st.wHour, st.wMinute);
    return fmt(L"%02d/%02d/%04d %02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
}

wstr fmt_date_iso(double t) {
    SYSTEMTIME st;
    if (t <= 0 || !to_local(t, st)) to_local(now_unix(), st);
    return fmt(L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

wstr fmt_count(i64 n) {
    wstr s = fmt(L"%lld", n < 0 ? -n : n), out;
    int c = 0;
    for (size_t i = s.size(); i-- > 0;) {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i) out.insert(out.begin(), L',');
    }
    return n < 0 ? L"-" + out : out;
}

// ======================================================================
// logging
// ======================================================================
void log_line(const wstr& s) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string line = utf8(fmt(L"%04d-%02d-%02d %02d:%02d:%02d  ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                                st.wSecond) + s) + "\n";
    HANDLE h = CreateFileW(path_join(app_dir(), L"file_explorer.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
    CloseHandle(h);
}

// ======================================================================
// files
// ======================================================================
bool read_file(const wstr& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = sz.QuadPart == 0 || ReadFile(h, &out[0], (DWORD)sz.QuadPart, &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return ok;
}

bool write_file_atomic(const wstr& path, const std::string& data) {
    wstr tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(h);
    if (!ok) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// ======================================================================
// JSON
// ======================================================================
const Json* Json::get(const std::string& k) const {
    if (type != Obj) return nullptr;
    for (auto& kv : o)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

Json& Json::set(const std::string& k, const Json& v) {
    if (type != Obj) { type = Obj; o.clear(); }
    for (auto& kv : o)
        if (kv.first == k) { kv.second = v; return kv.second; }
    o.emplace_back(k, v);
    return o.back().second;
}

namespace {
struct Parser {
    const std::string& t;
    size_t i = 0;
    bool ok = true;
    explicit Parser(const std::string& s) : t(s) {}
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++; }
    bool eat(char c) { ws(); if (i < t.size() && t[i] == c) { i++; return true; } return false; }
    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4 && i < t.size(); k++, i++) {
            char c = t[i];
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
        }
        return v;
    }
    std::string str() {
        std::string out;
        i++;  // "
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) break;
            char e = t[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        i += 2;
                        unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        i++;
        return out;
    }
    Json value(int depth = 0) {
        ws();
        if (i >= t.size() || depth > 64) { ok = false; return {}; }
        char c = t[i];
        if (c == '{') {
            i++;
            Json j = Json::object();
            if (eat('}')) return j;
            do {
                ws();
                if (i >= t.size() || t[i] != '"') { ok = false; return j; }
                std::string k = str();
                if (!eat(':')) { ok = false; return j; }
                j.o.emplace_back(k, value(depth + 1));
            } while (eat(','));
            if (!eat('}')) ok = false;
            return j;
        }
        if (c == '[') {
            i++;
            Json j = Json::array();
            if (eat(']')) return j;
            do j.a.push_back(value(depth + 1));
            while (eat(','));
            if (!eat(']')) ok = false;
            return j;
        }
        if (c == '"') { Json j; j.type = Json::Str; j.s = str(); return j; }
        if (t.compare(i, 4, "true") == 0) { i += 4; return Json::boolean(true); }
        if (t.compare(i, 5, "false") == 0) { i += 5; return Json::boolean(false); }
        if (t.compare(i, 4, "null") == 0) { i += 4; return Json(); }
        size_t start = i;
        while (i < t.size() && (isdigit((unsigned char)t[i]) || t[i] == '-' || t[i] == '+' || t[i] == '.' || t[i] == 'e' || t[i] == 'E')) i++;
        if (start == i) { ok = false; i++; return {}; }
        return Json::number(atof(t.substr(start, i - start).c_str()));
    }
};

void dump_str(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    out += '"';
}

void dump(const Json& j, std::string& out, int indent, int level) {
    std::string pad(indent * (level + 1), ' '), pad0(indent * level, ' ');
    const char* nl = indent ? "\n" : "";
    switch (j.type) {
        case Json::Null: out += "null"; break;
        case Json::Bool: out += j.b ? "true" : "false"; break;
        case Json::Num: {
            char b[64];
            if (std::fabs(j.n - std::llround(j.n)) < 1e-9 && std::fabs(j.n) < 1e15) snprintf(b, 64, "%lld", (long long)std::llround(j.n));
            else snprintf(b, 64, "%.17g", j.n);
            out += b;
            break;
        }
        case Json::Str: dump_str(out, j.s); break;
        case Json::Arr:
            if (j.a.empty()) { out += "[]"; break; }
            out += "["; out += nl;
            for (size_t k = 0; k < j.a.size(); k++) {
                out += pad;
                dump(j.a[k], out, indent, level + 1);
                if (k + 1 < j.a.size()) out += ",";
                out += nl;
            }
            out += pad0 + "]";
            break;
        case Json::Obj:
            if (j.o.empty()) { out += "{}"; break; }
            out += "{"; out += nl;
            for (size_t k = 0; k < j.o.size(); k++) {
                out += pad;
                dump_str(out, j.o[k].first);
                out += indent ? ": " : ":";
                dump(j.o[k].second, out, indent, level + 1);
                if (k + 1 < j.o.size()) out += ",";
                out += nl;
            }
            out += pad0 + "}";
            break;
    }
}
}  // namespace

Json Json::parse(const std::string& text, bool* ok) {
    Parser p(text);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB) p.i = 3;
    Json j = p.value();
    if (ok) *ok = p.ok;
    return j;
}

std::string Json::dump(int indent) const {
    std::string out;
    ::dump(*this, out, indent, 0);
    return out;
}
