// Dialogs: a folder colour, a tag (name + colour) and Rename many at once (with a live preview).
#pragma once
#include "app.h"

// a folder colour: palette, custom colour, gradient (two colours or a ready-made one); subfolders = null hides that
// option. spec in = the colour now.
bool folder_color_dialog(HWND owner, const wstr& title, std::string& spec, bool* subfolders);
bool tag_dialog(HWND owner, const wstr& title, wstr& name, std::string& color);
void bulk_rename_dialog(App* app, const std::vector<EntryPtr>& entries);
