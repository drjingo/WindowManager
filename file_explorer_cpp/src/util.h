// File Explorer (C++ / Win32) — small helpers: strings, paths, formatting, JSON, logging.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using wstr = std::wstring;
using i64 = long long;

// ---- strings
std::string utf8(const wstr& s);
wstr wide(const std::string& s);
wstr lower(wstr s);
wstr trim(const wstr& s);
bool starts_with(const wstr& s, const wstr& p);
bool ends_with(const wstr& s, const wstr& p);
bool ends_with_ci(const wstr& s, const wstr& p);
bool contains_ci(const wstr& hay, const wstr& needle);  // needle already lower-case
std::vector<wstr> split(const wstr& s, wchar_t sep);
wstr replace_all(wstr s, const wstr& a, const wstr& b);
wstr fmt(const wchar_t* f, ...);
int natural_cmp(const wstr& a, const wstr& b);  // like Explorer: file2 before file10

// ---- paths
extern const wstr VIRTUAL;  // "::" — search results and tag listings
bool is_virtual(const wstr& p);
wstr make_search_path(const wstr& root, const wstr& query);
wstr make_tag_path(const wstr& name);
void parse_virtual(const wstr& p, wstr& kind, wstr& a, wstr& b);
wstr norm(const wstr& p);           // compare key: \\, no trailing \\, lower case
wstr clean_path(const wstr& p);     // \\, no trailing \\ (except C:\)
wstr path_join(const wstr& a, const wstr& b);
wstr dir_name(const wstr& p);
wstr base_name(const wstr& p);
wstr ext_lower(const wstr& name);   // ".pdf" (or "")
void split_ext(const wstr& name, wstr& stem, wstr& ext);
wstr display_name(const wstr& p);
std::vector<wstr> ancestors(const wstr& p);  // "" (This PC), C:\, C:\Users, …
bool is_dir(const wstr& p);
bool exists(const wstr& p);
bool is_drive_root(const wstr& p);
bool same_drive(const wstr& a, const wstr& b);
bool is_inside(const wstr& child, const wstr& parent);  // child == parent or below it
wstr unique_name(const wstr& folder, const wstr& name, bool copy_suffix = false);
wstr check_name(const wstr& name);  // "" if fine, else why not
wstr expand_env(const wstr& s);
wstr resolve_typed_path(const wstr& text, const wstr& here);
wstr app_dir();
wstr exe_path();

// ---- time / numbers
double now_unix();
double filetime_unix(const FILETIME& ft);
wstr fmt_size(i64 n);  // n < 0 → ""
wstr fmt_time(double t);
wstr fmt_date_iso(double t);
wstr fmt_count(i64 n);  // 12,345

// ---- logging
void log_line(const wstr& s);

// ---- JSON (enough for file_explorer.json)
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;  // UTF-8
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    Json() = default;
    static Json null() { return Json(); }
    static Json boolean(bool v) { Json j; j.type = Bool; j.b = v; return j; }
    static Json number(double v) { Json j; j.type = Num; j.n = v; return j; }
    static Json string(const wstr& v) { Json j; j.type = Str; j.s = utf8(v); return j; }
    static Json array() { Json j; j.type = Arr; return j; }
    static Json object() { Json j; j.type = Obj; return j; }

    bool is_obj() const { return type == Obj; }
    bool is_arr() const { return type == Arr; }
    bool is_str() const { return type == Str; }
    bool is_num() const { return type == Num; }
    bool is_bool() const { return type == Bool; }
    const Json* get(const std::string& k) const;
    Json& set(const std::string& k, const Json& v);
    void push(const Json& v) { a.push_back(v); }
    wstr str(const wstr& def = L"") const { return type == Str ? wide(s) : def; }
    double num(double def = 0) const { return type == Num ? n : def; }
    bool truthy(bool def = false) const { return type == Bool ? b : type == Num ? n != 0 : def; }
    // shortcuts on objects
    wstr gstr(const std::string& k, const wstr& def = L"") const { auto p = get(k); return p ? p->str(def) : def; }
    double gnum(const std::string& k, double def = 0) const { auto p = get(k); return p ? p->num(def) : def; }
    bool gbool(const std::string& k, bool def = false) const { auto p = get(k); return p ? p->truthy(def) : def; }

    static Json parse(const std::string& text, bool* ok = nullptr);
    std::string dump(int indent = 2) const;
};
bool read_file(const wstr& path, std::string& out);
bool write_file_atomic(const wstr& path, const std::string& data);
