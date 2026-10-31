// Search through "Everything" (voidtools), when it's running: it keeps an index of every file on NTFS drives, so a
// search over a whole drive takes milliseconds instead of walking every folder. Talks to it directly (its IPC
// messages), so no DLL is needed. When it isn't running, the normal folder-by-folder search is used.
#pragma once
#include "fs.h"

struct EvItem {
    wstr name, folder;
    bool dir = false;
    i64 size = -1;
    double mtime = 0;
    DWORD attrs = 0;
};

bool everything_running();
bool everything_can_search(const wstr& root);  // "" = all drives; false for network / non-NTFS (not indexed)
wstr everything_search_text(const Query& q, const wstr& root, bool hidden);
// false if Everything didn't answer (then search the normal way). stop() is checked while waiting.
bool everything_query(const wstr& text, DWORD max_results, std::vector<EvItem>& out, const std::function<bool()>& stop);
