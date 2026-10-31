#include "everything.h"
#include <cstring>

namespace {
// from voidtools' everything_ipc.h
const ULONG_PTR COPYDATA_QUERY2W = 18;
const DWORD REQ_NAME = 0x1, REQ_PATH = 0x2, REQ_FULL_PATH = 0x4, REQ_EXTENSION = 0x8, REQ_SIZE = 0x10,
            REQ_DATE_CREATED = 0x20, REQ_DATE_MODIFIED = 0x40, REQ_DATE_ACCESSED = 0x80, REQ_ATTRIBUTES = 0x100;
const DWORD ITEM_FOLDER = 0x1, ITEM_DRIVE = 0x2;
const DWORD SORT_NAME_ASCENDING = 1;
const DWORD REPLY_ID = 0x46455631;  // "FEV1"

#pragma pack(push, 1)
struct Query2 {
    DWORD reply_hwnd, reply_copydata_message, search_flags, offset, max_results, request_flags, sort_type;
    // then the search text (UTF-16, 0-terminated)
};
struct List2 {
    DWORD totitems, numitems, offset, request_flags, sort_type;
    // then Item2[numitems]
};
struct Item2 { DWORD flags, data_offset; };
#pragma pack(pop)

struct Reply {
    std::string data;
    bool got = false;
};

LRESULT CALLBACK reply_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA) {
        auto* cd = (COPYDATASTRUCT*)l;
        Reply* r = (Reply*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (r && cd && cd->dwData == REPLY_ID) {
            r->data.assign((const char*)cd->lpData, cd->cbData);
            r->got = true;
            return TRUE;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

HWND find_everything() {
    HWND h = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr);
    if (!h) h = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)", nullptr);  // Everything 1.5 alpha
    return h;
}

wstr quote(const wstr& s) { return s.find_first_of(L" |<>") != wstr::npos ? L"\"" + s + L"\"" : s; }
}  // namespace

bool everything_running() { return find_everything() != nullptr; }

bool everything_can_search(const wstr& root) {
    if (root.empty()) return true;
    if (starts_with(root, L"\\\\")) return false;  // network share
    wstr drive = root.substr(0, 3);
    if (drive.size() < 3 || drive[1] != L':') return false;
    UINT t = GetDriveTypeW(drive.c_str());
    if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) return false;
    wchar_t fs[32] = {};
    if (!GetVolumeInformationW(drive.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs, 32)) return false;
    return !_wcsicmp(fs, L"NTFS") || !_wcsicmp(fs, L"ReFS");
}

// what Everything should look for; size: and modified: are checked afterwards on our side
wstr everything_search_text(const Query& q, const wstr& root, bool hidden) {
    wstr t;
    auto add = [&](const wstr& s) { t += (t.empty() ? L"" : L" ") + s; };
    if (!root.empty()) {
        wstr r = clean_path(root);
        if (r.back() != L'\\') r += L'\\';
        add(L"\"" + r + L"\"");  // a term with \ matches the path: everything inside this folder
    }
    for (auto& w : q.words) add(quote(w));
    for (auto& g : q.globs) add(quote(g));
    if (!q.exts.empty()) {
        wstr e;
        for (auto& x : q.exts) e += (e.empty() ? L"" : L";") + (x[0] == L'.' ? x.substr(1) : x);
        add(L"ext:" + e);
    }
    if (!hidden) add(L"!attrib:h");
    if (q.kind == 1) add(L"folder:");
    else if (q.kind == 2) add(L"file:");
    return t;
}

bool everything_query(const wstr& text, DWORD max_results, std::vector<EvItem>& out, const std::function<bool()>& stop) {
    out.clear();
    HWND ev = find_everything();
    if (!ev) return false;
    static ATOM cls = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = reply_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FE_EverythingReply";
        return RegisterClassW(&wc);
    }();
    (void)cls;
    // a hidden window on this thread for the answer
    HWND me = CreateWindowExW(0, L"FE_EverythingReply", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!me) return false;
    Reply reply;
    SetWindowLongPtrW(me, GWLP_USERDATA, (LONG_PTR)&reply);

    std::string buf(sizeof(Query2) + (text.size() + 1) * sizeof(wchar_t), '\0');
    Query2* q = (Query2*)&buf[0];
    q->reply_hwnd = (DWORD)(ULONG_PTR)me;
    q->reply_copydata_message = REPLY_ID;
    q->search_flags = 0;
    q->offset = 0;
    q->max_results = max_results;
    q->request_flags = REQ_NAME | REQ_PATH | REQ_SIZE | REQ_DATE_MODIFIED | REQ_ATTRIBUTES;
    q->sort_type = SORT_NAME_ASCENDING;
    memcpy(&buf[sizeof(Query2)], text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT cd{COPYDATA_QUERY2W, (DWORD)buf.size(), &buf[0]};
    DWORD_PTR res = 0;
    // (not SMTO_BLOCK: Everything may answer while we wait, and that answer must get through)
    bool sent = SendMessageTimeoutW(ev, WM_COPYDATA, (WPARAM)me, (LPARAM)&cd, SMTO_ABORTIFHUNG | SMTO_NORMAL, 5000, &res) && res;
    // the answer comes back as a WM_COPYDATA to our window
    ULONGLONG until = GetTickCount64() + 15000;
    while (sent && !reply.got && GetTickCount64() < until && !stop()) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT | QS_SENDMESSAGE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(me);
    if (!reply.got || reply.data.size() < sizeof(List2)) return false;

    // read the answer: per item, the requested fields in a fixed order
    const char* base = reply.data.data();
    size_t n = reply.data.size();
    const List2* list = (const List2*)base;
    DWORD rf = list->request_flags;
    if (sizeof(List2) + (size_t)list->numitems * sizeof(Item2) > n) return false;
    const Item2* items = (const Item2*)(base + sizeof(List2));
    out.reserve(list->numitems);
    for (DWORD i = 0; i < list->numitems; i++) {
        size_t p = items[i].data_offset;
        bool ok = true;
        auto read_text = [&](wstr* into) {
            if (p + 4 > n) { ok = false; return; }
            DWORD len;
            memcpy(&len, base + p, 4);
            p += 4;
            if (p + (size_t)(len + 1) * 2 > n) { ok = false; return; }
            if (into) into->assign((const wchar_t*)(base + p), len);
            p += (size_t)(len + 1) * 2;
        };
        auto read_bytes = [&](void* into, size_t k) {
            if (p + k > n) { ok = false; return; }
            if (into) memcpy(into, base + p, k);
            p += k;
        };
        EvItem it;
        it.dir = (items[i].flags & (ITEM_FOLDER | ITEM_DRIVE)) != 0;
        if (rf & REQ_NAME) read_text(&it.name);
        if (rf & REQ_PATH) read_text(&it.folder);
        if (rf & REQ_FULL_PATH) read_text(nullptr);
        if (rf & REQ_EXTENSION) read_text(nullptr);
        if (rf & REQ_SIZE) {
            LARGE_INTEGER s{};
            read_bytes(&s, 8);
            it.size = it.dir || s.QuadPart < 0 ? -1 : s.QuadPart;
        }
        if (rf & REQ_DATE_CREATED) read_bytes(nullptr, 8);
        if (rf & REQ_DATE_MODIFIED) {
            FILETIME ft{};
            read_bytes(&ft, 8);
            if (ok && (ft.dwHighDateTime || ft.dwLowDateTime) && ft.dwHighDateTime != 0xFFFFFFFF) it.mtime = filetime_unix(ft);
        }
        if (rf & REQ_DATE_ACCESSED) read_bytes(nullptr, 8);
        if (rf & REQ_ATTRIBUTES) read_bytes(&it.attrs, 4);
        if (!ok) break;
        if (it.dir) it.attrs |= FILE_ATTRIBUTE_DIRECTORY;
        out.push_back(std::move(it));
    }
    return true;
}
