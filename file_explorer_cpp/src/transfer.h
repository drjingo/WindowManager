// Copy / move with our own progress window: speed, time left, Pause, Skip, Cancel, and a question when a name is
// already taken (Replace / Keep both / Skip, optionally for all). Also shows progress on the taskbar button.
#pragma once
#include "app.h"

void start_transfer(App* app, const std::vector<wstr>& paths, const wstr& dest, bool move);
bool transfers_running();  // something is still being copied (asked before closing)
void cancel_transfers();
