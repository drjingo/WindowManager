# File Explorer — C++ / Win32 version

The same app as `file_explorer.py` (v3.4), written in C++ on top of plain Win32. It has no Python, no Tk and no runtime DLLs.
The result is a single `FileExplorer.exe` of about 2.4 MB that runs on Windows 10 and 11.

## Build (MinGW / MSYS2)

1. Install MSYS2 from https://www.msys2.org.
2. Open the **MSYS2 UCRT64** terminal and run `pacman -S mingw-w64-ucrt-x86_64-gcc make`.
3. Build it one of these two ways:
   - From the UCRT64 terminal: `cd` to this folder and run `make`.
   - From cmd: add `C:\msys64\ucrt64\bin` to PATH and run `build.bat`.

To cross-compile from Linux, run `make CROSS=x86_64-w64-mingw32- CXX=x86_64-w64-mingw32-g++-posix`.

## Run

    FileExplorer.exe                  opens your last tabs
    FileExplorer.exe "D:\Projects"    opens (or switches to) that folder
    FileExplorer.exe icon out.ico     writes the app icon
    FileExplorer.exe --new-window     a second window (normally a second start opens a tab in the running one)

Settings live in `file_explorer.json` next to the exe. It uses the same format as the Python version, so you can copy the
Python app's file across to keep your favourites, tags, colours, tab sets and theme.

## What's in it

All the Python version's features are included:
- Tabs and tab sets
- Breadcrumbs with folder menus
- Favourites, Recent folders and Tags (each sidebar section folds: click its header)
- Coloured folders (right-click a folder → *Folder colour*), optionally applied in Windows Explorer too:
  - 20 deep, saturated colours, a custom colour (Windows' colour picker), or a **gradient**: 10 ready-made ones
    (*Sunset*, *Ocean*, *Aurora* …) or any two colours (*More colours, custom, gradients…*, with a live preview)
  - **Same colour for all subfolders**: every folder inside gets it too, unless it has its own colour
  - **Default folder colour** for all other folders (right-click empty space), also any colour or gradient
- List, Grid and Columns views
- Filtering, and search through subfolders (`*.pdf size:>1mb modified:this week` …)
- Folder sizes
- Instant search through [Everything](https://www.voidtools.com) when it is running. It talks to Everything directly, so
  no DLL is needed. `size:` and `modified:` filters still work. Without Everything, or on network and non-NTFS drives, it
  searches folder by folder as before. Turn it off with right-click → View → Search with Everything.
- Two panes (F3; F5 copies to the other pane, F6 moves)
- Quick Look (Space)
- Preview panel (Alt+P or the toolbar button). It sits on the right, follows the selection and can be resized by dragging its edge.
- A progress window for copy / move, showing speed and time left, with Pause, Skip file and Cancel. If a name is already
  taken it asks Replace (Merge for folders) / Keep both / Skip, optionally for all. Progress also shows on the taskbar button.
- Single instance: starting it again, or opening a folder with it, adds a tab to the window that is already open
- Bulk rename with a live preview and Ctrl+Z undo
- Zip and extract (built-in `tar.exe`, or 7-Zip if installed)
- Drag and drop with Windows Explorer (real OLE)
- Rubber-band selection
- The 8 themes (Tokyo Night, Nord, Catppuccin), which switch instantly with no restart

## Source layout

| file | what |
|---|---|
| `src/util.*` | strings, paths, formatting, JSON |
| `src/theme.*` | the themes, folder colours, fonts, DPI |
| `src/gfx.*` | drawing, folder and app icon art, Windows icons and thumbnails, background work |
| `src/fs.*` | listing, drives, search, archives, text colouring, rename, copy/move/delete, folder watching |
| `src/ui.*` | base control, menus, drag and drop, buttons, text boxes, dialogs |
| `src/fileview.*` | the file list / grid / column view |
| `src/chrome.*` | tab bar, breadcrumb, search box, toolbar, sidebar, status bar |
| `src/app*.cpp` | the main window and every action |
| `src/panes.*` | the right-hand pane, Columns view |
| `src/quicklook.*`, `src/dialogs.*` | Quick Look and the preview panel, the tag dialog, bulk rename |
| `src/everything.*` | search through voidtools Everything (its IPC) |
| `src/transfer.*` | the copy / move engine and its progress window |
