// Dialogs: a tag (name + colour) and Rename many at once (with a live preview).
#pragma once
#include "app.h"

bool tag_dialog(HWND owner, const wstr& title, wstr& name, std::string& color);
void bulk_rename_dialog(App* app, const std::vector<EntryPtr>& entries);
