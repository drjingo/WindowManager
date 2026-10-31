"""
Window Manager for Windows — launcher, window switcher, workspaces, clipboard history and encrypted notes
- OPEN: your open windows, first. Alt+1 … Alt+0 (then Alt+Q … Alt+P for windows 11-20)
  switch to them from anywhere
  (press again on the active one to minimize it).
- FIXED SHORTCUTS: right-click any app → Fixed shortcut → e.g. Alt+P. That key then always
  switches to the app (cycling its windows, minimizing on a repeat press), or starts it if closed.
- Alt+.  shows / hides the launcher, Alt+F  opens it with the search bar focused.
- BROWSER PROFILES: each profile acts like its own app — its windows are recognised, it can have
  its own fixed shortcut, and double-click switches to it or opens it.
- Alt+N: encrypted notes with tabs (AES-256-GCM, password via scrypt; needs `pip install cryptography`).
- Click a section header to fold / unfold it.
- MY APPS: apps you add yourself ("+ Add app", or right-click → Add to My apps).
- BROWSER PROFILES: profiles you add with "+ Browser profile".
- APPS: everything in the Start menu.
Double-click (or Enter) opens / switches to the selected entry.

- WORKSPACES: groups of apps / profiles / links / folders opened together, each window placed
  where you want it. Give one a key: right-click → Fixed shortcut.
- Alt+V: clipboard history (encrypted with your notes password).
- Search bar: "= 12*7.5" calculates, "g text" searches Google, "yt", "w", "gh"… (settings.ini),
  links open directly, and commands like lock, sleep, restart, settings.
- Lives in the tray: closing the window keeps it running. Tray menu → Start with Windows.
- Alt+Shift+T: active window always on top. Alt+Shift+O: see-through. Del / M in Alt+Space and Alt+Enter:
  close / minimize the window you see. Right-click a window: Always on top, See-through, Force quit.
- Text snippets: type ;mail anywhere → replaced by its text (tray menu → Snippets…).
- Timers: "timer 10m tea", "remind 18:30 call prof", "pomodoro" in the search bar.
- Session restore: after a restart, offers to reopen the apps you had open, where they were.
- Hold Alt: cheat sheet. Alt+Shift+M: mute the active app. Alt+Shift+N: quick note into your notes.
- Window groups: right-click a window → Group with… — switching to one brings the others up behind it.

Run:      python window_manager.py     (python window_manager.py decrypt  — decrypt notes to .txt)
Requires: Windows 10/11, Python 3.8+, and `pip install cryptography` for notes and clipboard history
Settings: settings.ini next to this file. Data: the data folder next to this file.
"""

import base64
import glob
import datetime
import configparser
import ctypes
import json
import os
import hashlib
import hmac
import math
import ast
import urllib.parse
from xml.sax.saxutils import escape as xml_escape
import queue
import signal
import shutil
import re
import struct
import subprocess
import sys
import threading
import time
import traceback
import uuid
import zlib
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog
import tkinter.font as tkfont

IS_WIN = sys.platform == "win32"
if IS_WIN:
    import winreg
    from ctypes import wintypes

CREATE_NO_WINDOW = 0x08000000
LOCAL = os.environ.get("LOCALAPPDATA", "")
ROAMING = os.environ.get("APPDATA", os.path.expanduser("~"))
PF = os.environ.get("ProgramFiles", r"C:\Program Files")
PF86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
# Folder of the script — or of the .exe when built with PyInstaller (a one-file .exe runs from a temp folder).
APP_DIR = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))
SETTINGS_FILE = os.path.join(APP_DIR, "settings.ini")
IMPORT_MESSAGE = None  # set when old data was imported at start-up


def set_data_dir(path):
    """Where config, notes and clipboard history live ([data] data_folder in settings.ini)."""
    global DATA_DIR, CONFIG_DIR, CONFIG_FILE, UI_FILE, NOTES_FILE, NOTES_DIR, USAGE_FILE, CLIP_FILE, CLIP_KEY_FILE
    DATA_DIR = CONFIG_DIR = path
    CONFIG_FILE = os.path.join(path, "config.json")
    UI_FILE = os.path.join(path, "ui.json")
    NOTES_FILE = os.path.join(path, "notes.enc")         # single-file notes from v13/v14 (split on unlock)
    NOTES_DIR = os.path.join(path, "notes")               # one encrypted file per tab
    USAGE_FILE = os.path.join(path, "usage.json")         # how often / how recently things were opened
    CLIP_FILE = os.path.join(path, "clipboard.json")      # clipboard history, each item encrypted
    CLIP_KEY_FILE = os.path.join(path, "clipboard.key")   # password mode: public key + locked private key
    global SNIPPETS_FILE, TIMERS_FILE, SESSION_FILE, SESSION_PREV_FILE
    SNIPPETS_FILE = os.path.join(path, "snippets.json")   # encrypted with your Windows account
    TIMERS_FILE = os.path.join(path, "timers.json")       # timers and reminders (encrypted the same way)
    SESSION_FILE = os.path.join(path, "session.json")     # apps open right now (updated every 30 s)
    SESSION_PREV_FILE = os.path.join(path, "session-previous.json")  # apps open before the last restart
    global QUICK_PENDING_FILE
    QUICK_PENDING_FILE = os.path.join(path, "quick-notes-waiting.json")  # quick notes waiting for an unlock


set_data_dir(os.path.join(APP_DIR, "data"))
VERSION = "v32"

# Dark command-palette theme
T = {
    "bg": "#131418", "surface": "#1b1d23", "field": "#22252d", "border": "#2b2e38",
    "fg": "#e8e9ed", "muted": "#9095a1", "faint": "#5f6470",
    "accent": "#8aa4ff", "select": "#2a3456", "hover": "#1d2028",
}
UI_FONT = "Segoe UI"
SECTION_IDS = ("h:timers", "h:open", "h:fixed", "h:ws", "h:prof", "h:pins", "h:apps")
LOG_FILE = os.path.join(APP_DIR, "window_manager.log")
APP_PATHS = r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths"
LINK_DIR = os.path.join(APP_DIR, ".launcher-links")
LAUNCHER_ELEVATED = False   # set at startup
PENDING_LAUNCHES = {}       # profile key -> time it was started (to recognise its windows)
# ----------------------------------------------------------------------
# Shortcut settings (settings.ini). The tables below are rebuilt by apply_settings().
# ----------------------------------------------------------------------

# Rows of keys by physical position (scan code), so they work on any keyboard layout.
KEY_ROWS = {
    "digits": [(0x02 + i, "1234567890"[i]) for i in range(10)],
    "top": [(0x10 + i, "QWERTYUIOP"[i]) for i in range(10)],
    "home": [(0x1E + i, "ASDFGHJKL"[i]) for i in range(9)],
    "bottom": [(0x2C + i, "ZXCVBNM"[i]) for i in range(7)],
}
MOD_NAMES = ("ctrl", "win", "alt", "shift")

DEFAULT_SETTINGS = """\
; ======================================================================
;  Window Manager settings
;  Edit and save this file: the launcher applies changes within a few seconds.
; ======================================================================

[shortcuts]
; Key(s) held for every launcher shortcut. Combine with "+":
;   alt  |  ctrl+alt  |  alt+shift  |  win+alt  |  ctrl+shift
; Note: ctrl+alt is the same as AltGr on many keyboards (it would block typing @ # { on AZERTY).
modifier = alt

; Rows of keys that switch between your open windows, in this order.
; Pick one or more, separated by commas:
;   digits = 1 2 3 4 5 6 7 8 9 0     (numpad digits work too)
;   top    = Q W E R T Y U I O P     (the row under the digits)
;   home   = A S D F G H J K L       (the middle letter row)
;   bottom = Z X C V B N M           (the bottom letter row)
; Examples:   rows = digits           -> 10 shortcuts
;             rows = digits, top      -> 20 shortcuts (default)
;             rows = digits, top, home
; Letter rows only take over a key while a window is on it, so the key keeps
; working normally in your apps the rest of the time.
rows = digits, top

; One key for each launcher action: a letter, a digit or "." (leave empty to turn it off).
; If one of these keys is also in a row above, the action wins and that row skips the key.
show_hide = .
search = F
notes = N
clipboard = V

; Switch to the next / previous open window, in the order of the OPEN list (numpad + and - work too).
next_window = +
prev_window = -

; Show the list of your open windows with their numbers. In the list: Space or ↓ next, ↑ previous,
; Enter switches (or press a number, or click), Del closes the window, M minimizes it, Esc closes the list.
switcher = space

; Cycle: really switches to each open window in turn (speed: cycle_step_ms). Enter stays on the one you see,
; Esc goes back to where you started. (Many apps use Alt+Enter for full screen — change it if needed.)
cycle = enter

; The active window: keep it above all other windows (press again to undo).
; "shift+T" means your modifier + Shift + T (Alt+Shift+T by default).
always_on_top = shift+T

; The active window: make it see-through, one step per press, then solid again (see see_through_levels).
see_through = shift+O

; The active app: mute / unmute its sound (only that app — e.g. Spotify, or your browser).
mute = shift+M

; A small box to jot one line into your encrypted notes (the tab named by quick_note_tab).
quick_note = shift+N

; Open File Explorer (file_explorer\\file_explorer.py: tabs and favourite folders).
file_explorer = shift+E

[launcher]
; Hide the launcher after you open something, when you opened it with show_hide or search.
hide_after_launch = yes

; Minutes before notes that are hidden lock themselves (0 = only lock when you close them).
notes_autolock_minutes = 10

; Shortest notes password allowed when you create or change it.
notes_min_password = 4

; Closing the launcher window keeps it running in the tray (quit from the tray icon).
close_to_tray = yes

; Cycle (Alt+Enter): milliseconds before it switches to the next window.
cycle_step_ms = 500

; Animation when the cycle (Alt+Enter) moves to the next window:
;   fade      = the screen dims, then the next window appears (default)
;   wipe      = a panel sweeps across the screen
;   zoom      = a glowing outline grows into the next window
;   highlight = the next window flashes with a coloured outline
;   none      = switch instantly
cycle_transition = fade

; How long the animation lasts, in milliseconds (it always fits inside cycle_step_ms).
transition_ms = 260

; See-through steps, in % of opacity (100 = solid), for the see_through key and the right-click menu.
see_through_levels = 85, 70, 55

; Hold the modifier (Alt) alone this long to see every shortcut and which window is on each key.
; 0 = off.
cheat_sheet_hold_ms = 800

; Quick note: the notes tab it goes into (created if missing), and whether each line starts with the time.
quick_note_tab = Quick notes
quick_note_time = yes

; Window groups (right-click a window → Group with…): when you minimize one with its shortcut,
; minimize the others of its group too.
group_minimize_together = yes

[clipboard]
; Keep a history of the text you copy (open it with the clipboard key, default Alt+V).
; Text copied from password managers that mark it as private is never kept.
enabled = yes

; How many items to keep. 0 = keep everything until you delete it (pinned items are always kept).
max_items = 0

; Keep the history after a restart (no = forget it when the launcher quits).
save_history = yes

; How the saved history is encrypted:
;   windows  = with your Windows account: readable right after you sign in, only by you (default)
;   password = with your notes password: after a restart, older items stay locked until you enter it
encryption = windows

; Paste right away when you pick an item (no = only copy it).
paste_on_select = yes

[search]
; Type a prefix and a space in the search bar, e.g. "g best laptops" or "yt lofi".
; {} is replaced by what you type. Add your own:  prefix = https://site/search?q={}
g = https://www.google.com/search?q={}
yt = https://www.youtube.com/results?search_query={}
w = https://en.wikipedia.org/w/index.php?search={}
gh = https://github.com/search?q={}
maps = https://www.google.com/maps/search/{}
tr = https://translate.google.com/?sl=auto&tl=en&text={}

; The prefix used for the "Search the web for …" line at the end of the results.
default = g

; Open searches and links in one of your browser profiles (part of its name, e.g. Work),
; or leave empty to use your default browser.
browser_profile =

[snippets]
; Type a trigger anywhere, e.g. ;mail — it's replaced by its text. Edit your snippets from the tray menu
; (Snippets…) or search "snippets". They're saved encrypted with your Windows account.
enabled = yes

[timers]
; In the search bar: "timer 10m tea", "remind 18:30 call prof", "remind in 2h stretch", "pomodoro", "timers".
; Play a sound when a timer or reminder goes off.
sound = yes

; Snooze button: minutes.
snooze_minutes = 5

; Pomodoro: minutes of work, of short break, of long break, and work sessions before a long break.
pomodoro_work = 25
pomodoro_break = 5
pomodoro_long_break = 15
pomodoro_rounds = 4

[session]
; Remember which apps are open (and where their windows are), to open them again after a restart:
;   ask = after a restart, show the list and let you pick (default)
;   yes = reopen them all by themselves
;   no  = off (nothing is remembered)
; Tray menu → Restore last session… works any time.
restore = ask

[data]
; Folder for your data: config, notes and clipboard history.
; Empty = the "data" folder next to this app. You can use %USERPROFILE%, %APPDATA%, … and relative paths.
; Restart the app after changing it (move your old data folder there first to keep it).
data_folder =

; Older versions kept their data in this folder. If it exists, it's imported ONCE into the data folder
; (only while that one is still empty), then renamed "… (imported <date>)" so it's never copied again.
; Leave empty to turn this off.
import_from = %APPDATA%\\AppLauncher
"""


def _detect_old_data_folder():
    """Microsoft Store Python keeps %APPDATA% in a private folder; point to it when it's there."""
    for path in glob.glob(os.path.join(LOCAL, "Packages", "PythonSoftwareFoundation.Python.*", "LocalCache",
                                       "Roaming", "AppLauncher")):
        if os.path.isdir(path):
            return "%LOCALAPPDATA%" + path[len(LOCAL):] if LOCAL and path.startswith(LOCAL) else path
    return "%APPDATA%\\AppLauncher"


def settings_defaults():
    return DEFAULT_SETTINGS.replace("import_from = %APPDATA%\\AppLauncher",
                                    "import_from = " + _detect_old_data_folder())

SETTINGS = {}
MODIFIERS = {"alt"}
MOD_LABEL = "Alt+"
SLOTS = []            # [{"id": "top:9", "sc": 0x19, "char": "P", "row": "top"}, ...]
SLOT_INDEX = {}       # slot id -> index
SLOT_BY_SC = {}       # scan code -> index
NUMPAD_SLOT = {}      # numpad vk -> index
ALWAYS_SLOTS = set()  # digit slots: always taken, even when empty
CMD_BY_VK = {}        # vk -> "toggle" | "search" | "notes"
CMD_BY_VK_SHIFT = {}  # vk -> command, for keys written "shift+X" (modifier + Shift + X)
CMD_KEYS = {}         # command -> key text from settings
HOTKEY_COUNT = 0
SETTINGS_WARNINGS = []


TRANSITIONS = ("fade", "wipe", "zoom", "highlight", "none")
KEY_NAMES = {".": ".", "+": "Plus", "-": "Minus", "space": "Space", "enter": "Enter"}


def _key_to_vks(text):
    text = text.strip()
    if not text:
        return []
    if text == ".":
        return [0xBE, 0x6E]  # main keyboard and numpad
    if text == "+":
        return [0xBB, 0x6B]  # the =/+ key and numpad +
    if text == "-":
        return [0xBD, 0x6D]  # the -/_ key and numpad -
    if text.lower() == "space":
        return [0x20]
    if text.lower() == "enter":
        return [0x0D]
    if len(text) == 1 and text.isascii() and text.isalpha():
        return [ord(text.upper())]
    if len(text) == 1 and text.isdigit():
        return [0x30 + int(text), 0x60 + int(text)]
    return None


def _vk_to_scancode(vk):
    if IS_WIN:
        return ctypes.windll.user32.MapVirtualKeyW(vk, 0)  # MAPVK_VK_TO_VSC
    for row in KEY_ROWS.values():
        for sc, ch in row:
            if ord(ch) == vk:
                return sc
    return 0


def load_settings():
    """Read settings.ini (creating it with defaults if missing) and rebuild the shortcut tables."""
    global SETTINGS, MODIFIERS, MOD_LABEL, SLOTS, SLOT_INDEX, SLOT_BY_SC, NUMPAD_SLOT, ALWAYS_SLOTS
    global CMD_BY_VK, CMD_BY_VK_SHIFT, CMD_KEYS, HOTKEY_COUNT, SETTINGS_WARNINGS
    warnings = []
    if not os.path.isfile(SETTINGS_FILE):
        try:
            with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
                f.write(settings_defaults())
        except OSError:
            warnings.append("couldn't create settings.ini")
    else:
        upgrade_settings_file()
    cp = configparser.ConfigParser(inline_comment_prefixes=(";", "#"), interpolation=None)
    try:
        cp.read_string(settings_defaults())
        cp.read(SETTINGS_FILE, encoding="utf-8")
    except configparser.Error as e:
        warnings.append(f"settings.ini has an error ({e}); using defaults")
        cp = configparser.ConfigParser(inline_comment_prefixes=(";", "#"), interpolation=None)
        cp.read_string(settings_defaults())
    sh, la, cl, da = cp["shortcuts"], cp["launcher"], cp["clipboard"], cp["data"]
    user_cp = configparser.ConfigParser(inline_comment_prefixes=(";", "#"), interpolation=None)
    try:
        user_cp.read(SETTINGS_FILE, encoding="utf-8")
    except configparser.Error:
        user_cp = configparser.ConfigParser()
    # your own [search] list replaces the default one, so engines you delete stay deleted
    se = user_cp["search"] if user_cp.has_section("search") else cp["search"]

    # modifier
    mods = {m.strip().lower() for m in sh.get("modifier", "alt").split("+") if m.strip()}
    if not mods or not mods <= set(MOD_NAMES) or not mods & {"alt", "ctrl", "win"}:
        warnings.append(f"modifier “{sh.get('modifier')}” isn't valid; using alt")
        mods = {"alt"}
    MODIFIERS = mods
    MOD_LABEL = "+".join(m.capitalize() for m in MOD_NAMES if m in mods) + "+"

    # action keys
    CMD_BY_VK, CMD_BY_VK_SHIFT, CMD_KEYS = {}, {}, {}
    taken_sc = set()
    for cmd, opt in (("toggle", "show_hide"), ("search", "search"), ("notes", "notes"), ("clipboard", "clipboard"),
                     ("next", "next_window"), ("prev", "prev_window"), ("switcher", "switcher"),
                     ("cycle", "cycle"), ("topmost", "always_on_top"), ("opacity", "see_through"),
                     ("mute", "mute"), ("quicknote", "quick_note"), ("explorer", "file_explorer")):
        text = sh.get(opt, "").strip()
        shifted = text.lower().startswith("shift+") and len(text) > 6
        if shifted:
            text = text[6:].strip()
            if "shift" in mods:
                warnings.append(f"{opt} = shift+{text} can't work while the modifier already has shift; turned off")
                continue
        vks = _key_to_vks(text)
        if vks is None:
            warnings.append(f"{opt} = “{text}” isn't a letter, a digit, '.', '+', '-', 'space' or 'enter' "
                            "(optionally after shift+); turned off")
            continue
        if not vks:
            continue
        table = CMD_BY_VK_SHIFT if shifted else CMD_BY_VK
        if vks[0] in table:
            warnings.append(f"{opt} uses the same key as {table[vks[0]]}; turned off")
            continue
        for vk in vks:
            table[vk] = cmd
        CMD_KEYS[cmd] = ("Shift+" if shifted else "") + KEY_NAMES.get(text.lower(), text.upper())
        if not shifted:
            taken_sc.add(_vk_to_scancode(vks[0]))

    # rows
    rows = [r.strip().lower() for r in sh.get("rows", "digits, top").split(",") if r.strip()]
    slots = []
    for r in rows:
        if r not in KEY_ROWS:
            warnings.append(f"unknown row “{r}” (use digits, top, home, bottom)")
            continue
        if any(x["row"] == r for x in slots):
            continue
        for i, (sc, ch) in enumerate(KEY_ROWS[r]):
            if sc in taken_sc:
                continue  # an action uses this key
            slots.append({"id": f"{r}:{i}", "sc": sc, "char": ch, "row": r})
    SLOTS = slots
    SLOT_INDEX = {x["id"]: i for i, x in enumerate(slots)}
    SLOT_BY_SC = {x["sc"]: i for i, x in enumerate(slots)}
    NUMPAD_SLOT = {0x60 + int(x["char"]): i for i, x in enumerate(slots) if x["row"] == "digits"}
    ALWAYS_SLOTS = {i for i, x in enumerate(slots) if x["row"] == "digits"}
    HOTKEY_COUNT = len(slots)

    def yes(v):
        return str(v).strip().lower() in ("1", "yes", "true", "on")
    try:
        autolock = max(0, int(float(la.get("notes_autolock_minutes", "10"))))
    except ValueError:
        warnings.append("notes_autolock_minutes must be a number; using 10")
        autolock = 10
    def number(section, key, default, lo, hi):
        try:
            return min(hi, max(lo, int(float(section.get(key, str(default))))))
        except ValueError:
            warnings.append(f"{key} must be a number; using {default}")
            return default

    transition = la.get("cycle_transition", "fade").strip().lower()
    if transition not in TRANSITIONS:
        warnings.append(f"cycle_transition = “{transition}” isn't one of {', '.join(TRANSITIONS)}; using fade")
        transition = "fade"
    levels = []
    for part in la.get("see_through_levels", "85, 70, 55").replace(";", ",").split(","):
        try:
            v = int(float(part.strip().rstrip("%")))
        except ValueError:
            continue
        if 10 <= v <= 99 and v not in levels:
            levels.append(v)
    if not levels:
        warnings.append("see_through_levels needs numbers between 10 and 99; using 85, 70, 55")
        levels = [85, 70, 55]
    levels.sort(reverse=True)
    sn = cp["snippets"] if cp.has_section("snippets") else {}
    tm = cp["timers"] if cp.has_section("timers") else {}
    ss = cp["session"] if cp.has_section("session") else {}
    restore = str(ss.get("restore", "ask")).strip().lower()
    restore = {"yes": "yes", "auto": "yes", "true": "yes", "on": "yes", "no": "no", "off": "no",
               "false": "no"}.get(restore, restore)
    if restore not in ("ask", "yes", "no"):
        warnings.append(f"[session] restore = “{restore}” isn't ask, yes or no; using ask")
        restore = "ask"
    clip_mode = cl.get("encryption", "windows").strip().lower()
    if clip_mode not in ("windows", "password"):
        warnings.append(f"[clipboard] encryption = “{clip_mode}” isn't valid (windows or password); using windows")
        clip_mode = "windows"

    engines = {}
    for k, v in se.items():
        if k in ("default", "browser_profile"):
            continue
        v = v.strip()
        if " " in k or not re.match(r"^https?://", v, re.I) or "{}" not in v:
            warnings.append(f"[search] {k} isn't a valid search (needs http(s)://… with {{}}); skipped")
            continue
        engines[k.lower()] = v
    default_engine = se.get("default", "g").strip().lower()
    if engines and default_engine not in engines:
        default_engine = next(iter(engines))

    SETTINGS = {"hide_after_launch": yes(la.get("hide_after_launch", "yes")), "autolock_min": autolock,
                "close_to_tray": yes(la.get("close_to_tray", "yes")),
                "cycle_step_ms": number(la, "cycle_step_ms", 500, 150, 5000),
                "transition": transition,
                "transition_ms": number(la, "transition_ms", 260, 60, 2000),
                "notes_min_pw": number(la, "notes_min_password", 4, 1, 256),
                "data_folder": resolve_folder(da.get("data_folder", "")) or os.path.join(APP_DIR, "data"),
                "import_from": resolve_folder(da.get("import_from", "")),
                "clip_enabled": yes(cl.get("enabled", "yes")),
                "clip_max": number(cl, "max_items", 0, 0, 100000),
                "clip_encryption": clip_mode,
                "clip_save": yes(cl.get("save_history", "yes")),
                "clip_paste": yes(cl.get("paste_on_select", "yes")),
                "engines": engines, "default_engine": default_engine,
                "browser_profile": se.get("browser_profile", "").strip(),
                "opacity_levels": levels,
                "snippets_on": yes(sn.get("enabled", "yes")),
                "timer_sound": yes(tm.get("sound", "yes")),
                "snooze_min": number(tm, "snooze_minutes", 5, 1, 240),
                "pomo_work": number(tm, "pomodoro_work", 25, 1, 240),
                "pomo_break": number(tm, "pomodoro_break", 5, 1, 120),
                "pomo_long": number(tm, "pomodoro_long_break", 15, 1, 240),
                "pomo_rounds": number(tm, "pomodoro_rounds", 4, 1, 20),
                "session_restore": restore,
                "cheat_ms": number(la, "cheat_sheet_hold_ms", 800, 0, 5000),
                "quick_tab": (la.get("quick_note_tab", "Quick notes") or "").strip() or "Quick notes",
                "quick_time": yes(la.get("quick_note_time", "yes")),
                "group_min": yes(la.get("group_minimize_together", "yes"))}
    if not _DATA_DIR_APPLIED:
        apply_data_folder()
    elif os.path.normcase(SETTINGS["data_folder"]) != os.path.normcase(DATA_DIR):
        warnings.append("data_folder changed — restart Window Manager to use it")
    SETTINGS_WARNINGS = warnings
    for w in warnings:
        log("settings: " + w)
    return warnings


_DATA_DIR_APPLIED = False


def apply_data_folder():
    global _DATA_DIR_APPLIED
    _DATA_DIR_APPLIED = True
    set_data_dir(SETTINGS["data_folder"])


def slot_label(idx):
    """Shortcut name for window slot idx, using the current keyboard layout's letters."""
    if not 0 <= idx < len(SLOTS):
        return "?"
    x = SLOTS[idx]
    ch = x["char"]
    if IS_WIN and x["row"] != "digits":
        vk = ctypes.windll.user32.MapVirtualKeyW(x["sc"], 1)  # MAPVK_VSC_TO_VK
        if 0x41 <= vk <= 0x5A:
            ch = chr(vk)
    return MOD_LABEL + ch


def hints_text():
    parts = []
    for cmd, label in (("toggle", "show/hide"), ("search", "search"), ("notes", "notes"),
                       ("clipboard", "clipboard"), ("switcher", "switcher"), ("cycle", "cycle")):
        if cmd in CMD_KEYS:
            parts.append(f"{MOD_LABEL}{CMD_KEYS[cmd]} {label}")
    if "next" in CMD_KEYS and "prev" in CMD_KEYS:
        parts.append(f"{MOD_LABEL}{CMD_KEYS['next']}/{CMD_KEYS['prev']} next/prev")
    if SLOTS:
        parts.append(f"{slot_label(0)}…{slot_label(len(SLOTS) - 1).split('+')[-1]} switch")
    return "   ".join(parts)


def _default_blocks():
    """{section: (section text, {key: key line with the comments above it})} from DEFAULT_SETTINGS."""
    blocks, cur, lines = {}, None, []
    for line in settings_defaults().splitlines():
        m = re.match(r"\[(\w+)\]\s*$", line)
        if m:
            if cur:
                blocks[cur] = lines
            cur, lines = m.group(1), [line]
        elif cur:
            lines.append(line)
    if cur:
        blocks[cur] = lines
    out = {}
    for sec, ls in blocks.items():
        keys, buf = {}, []
        for line in ls[1:]:
            t = line.strip()
            if not t:
                buf = []
            elif t.startswith(";"):
                buf.append(line)
            else:
                m = re.match(r"([\w.]+)\s*=", t)
                if m:
                    keys[m.group(1).lower()] = "\n".join(buf + [line])
                buf = []
        out[sec] = ("\n".join(ls).rstrip() + "\n", keys)
    return out


SETTINGS_REWRITES = [  # old default text → new default text (only if you never changed it)
    ("; Shortest notes password allowed when you create or change it.\nnotes_min_password = 6",
     "; Shortest notes password allowed when you create or change it.\nnotes_min_password = 4"),
    ("; Open File Explorer (file_explorer.py next to this app: tabs and favourite folders).",
     "; Open File Explorer (file_explorer\\file_explorer.py: tabs and favourite folders)."),
    ("; How many items to keep (pinned items are kept on top of this).\nmax_items = 50",
     "; How many items to keep. 0 = keep everything until you delete it (pinned items are always kept).\n"
     "max_items = 0"),
    ("; Keep the history after a restart, encrypted so only your notes password can read it.\n"
     "; This needs a notes password: open Notes once to create it. no = forget it when the launcher quits.\n"
     "save_history = yes",
     "; Keep the history after a restart (no = forget it when the launcher quits).\nsave_history = yes\n\n"
     "; How the saved history is encrypted:\n"
     ";   windows  = with your Windows account: readable right after you sign in, only by you (default)\n"
     ";   password = with your notes password: after a restart, older items stay locked until you enter it\n"
     "encryption = windows"),
    ("; (see switcher_step_ms) — press Enter, or this key again, to switch to it.",
     "; (speed: switcher_step_ms) — press Enter, or this key again, to switch to it."),
    ("; Cycle: really switches to each open window in turn (same speed). Enter stays on the one you see,",
     "; Cycle: really switches to each open window in turn (speed: cycle_step_ms). Enter stays on the one you see,"),
    ("; Switcher and cycle: milliseconds before moving on to the next window.",
     "; Switcher (Alt+Space): milliseconds before the highlight moves to the next window."),
    ("(it always fits inside switcher_step_ms).", "(it always fits inside cycle_step_ms)."),
    ("; Show your open windows with their numbers; the highlight moves to the next one by itself\n"
     "; (speed: switcher_step_ms) — press Enter, or this key again, to switch to it.\nswitcher = space",
     """; Show the list of your open windows with their numbers. In the list: Space or ↓ next, ↑ previous,\n; Enter switches (or press a number, or click), Del closes the window, M minimizes it, Esc closes the list.\nswitcher = space"""),
    ("; Switcher (Alt+Space): milliseconds before the highlight moves to the next window.\nswitcher_step_ms = 500\n\n",
     ""),
    ("""; Show your open windows with their numbers. Press this key again to move to the next one, Enter to switch\n; (or a number, or click). With switcher_step_ms the highlight also moves on by itself.\nswitcher = space""", """; Show the list of your open windows with their numbers. In the list: Space or ↓ next, ↑ previous,\n; Enter switches (or press a number, or click), Del closes the window, M minimizes it, Esc closes the list.\nswitcher = space"""),
    ("""; Switcher (Alt+Space): 0 = the highlight only moves when you press (default);\n; or milliseconds before it moves to the next window by itself (then this key again switches to it).\nswitcher_step_ms = 0\n\n""", ""),
]


def resolve_folder(text):
    """'%APPDATA%\\x', '~\\x', 'data2' (relative to the app folder) → full path; '' → ''."""
    text = (text or "").strip().strip('"')
    if not text:
        return ""
    text = re.sub(r"%(\w+)%", lambda m: os.environ.get(m.group(1), m.group(0)), text)  # %APPDATA% etc.
    path = os.path.expanduser(os.path.expandvars(text))
    return os.path.normpath(path if os.path.isabs(path) else os.path.join(APP_DIR, path))


def upgrade_settings_file():
    """Add options that are new in this version to an existing settings.ini, keeping your values."""
    try:
        with open(SETTINGS_FILE, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return []
    original = text
    for old, new in SETTINGS_REWRITES:
        text = text.replace(old, new)
    cp = configparser.ConfigParser(inline_comment_prefixes=(";", "#"), interpolation=None)
    try:
        cp.read_string(text)
    except configparser.Error:
        return []
    if not cp.has_option("launcher", "cycle_step_ms") and cp.has_option("launcher", "switcher_step_ms"):
        # new in v25: the cycle gets its own speed — start it at your current switcher speed, right below it
        m = re.search(r"^switcher_step_ms\s*=\s*([^\n;#]*)[^\n]*$", text, re.M)
        if m:
            value = m.group(1).strip()
            value = value if value and value not in ("0", "0.0") else "500"
            text = (text[:m.end()] + "\n\n; Cycle (Alt+Enter): milliseconds before it switches to the next window.\n"
                    f"cycle_step_ms = {value}" + text[m.end():])
            cp.set("launcher", "cycle_step_ms", value)
    new, added = text, []
    if text != original:
        added.append("updated comments / new speed option")
    for sec, (block, keys) in _default_blocks().items():
        if not cp.has_section(sec):
            new = new.rstrip("\n") + "\n\n" + block
            added.append(f"[{sec}]")
            continue
        if sec == "search":
            continue  # your list of search sites is yours
        missing = [k for k in keys if not cp.has_option(sec, k)]
        if not missing:
            continue
        m = re.search(rf"^\[{sec}\]\s*$", new, re.M)
        nxt = re.search(r"^\[", new[m.end():], re.M)
        pos = m.end() + nxt.start() if nxt else len(new)
        insert = "\n" + "\n\n".join(keys[k] for k in missing) + "\n"
        new = new[:pos].rstrip("\n") + "\n" + insert + ("\n" + new[pos:] if nxt else "")
        added += missing
    if added:
        try:
            with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
                f.write(new)
            log("settings.ini: added new options " + ", ".join(added))
        except OSError:
            return []
    return added


IMPORT_MARKER = "IMPORTED-by-Window-Manager.txt"


def migrate_old_data():
    """Import the old data folder ([data] import_from) ONCE, then rename it so it's never copied again."""
    global IMPORT_MESSAGE
    src = SETTINGS.get("import_from")
    if not src or not os.path.isdir(src) or os.path.isfile(os.path.join(src, IMPORT_MARKER)):
        return
    if os.path.normcase(os.path.abspath(src)) == os.path.normcase(os.path.abspath(DATA_DIR)):
        return
    in_use = os.path.isfile(CONFIG_FILE) or bool(notes_tab_files()) or os.path.isfile(NOTES_FILE)
    copied = []
    if not in_use:
        os.makedirs(DATA_DIR, exist_ok=True)
        for name in ("config.json", "ui.json", "notes.enc"):
            path = os.path.join(src, name)
            if os.path.isfile(path) and not os.path.exists(os.path.join(DATA_DIR, name)):
                shutil.copy2(path, os.path.join(DATA_DIR, name))
                copied.append(name)
        old_profiles = _read_json(os.path.join(src, "profiles.json"))  # the very first versions
        if isinstance(old_profiles, list) and not os.path.exists(CONFIG_FILE):
            write_json_atomic(CONFIG_FILE, {"profiles": old_profiles, "pins": []}, indent=2)
            copied.append("profiles.json")
    new_name = f"{src} (imported {datetime.date.today().isoformat()})"
    try:
        os.rename(src, new_name)
        where = f"renamed to “{os.path.basename(new_name)}”"
    except OSError:
        try:
            with open(os.path.join(src, IMPORT_MARKER), "w", encoding="utf-8") as f:
                f.write(f"Imported into {DATA_DIR} on {datetime.date.today()}. You can delete this folder.\n")
        except OSError:
            log(f"couldn't mark {src} as imported")
            return
        where = "marked as imported"
    if copied:
        IMPORT_MESSAGE = f"Imported your old data ({', '.join(copied)}). The old folder was {where}."
    else:
        IMPORT_MESSAGE = f"Old data folder not imported (this data folder is already in use); it was {where}."
    log(f"old data: {src} → {DATA_DIR}: copied {copied or 'nothing'}; {where}")


WINDOW_SCAN_MS = 1000
SESSION_SAVE_MS = 30000


def log(text):
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')}  {text}"
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


# ======================================================================
# Installed apps (everything in the Start menu: desktop + Store apps)
# ======================================================================

def get_start_apps():
    if not IS_WIN:
        return []
    cmd = ("[Console]::OutputEncoding=[Text.Encoding]::UTF8;"
           "Get-StartApps | Select-Object Name,AppID | ConvertTo-Json -Compress")
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd],
            capture_output=True, timeout=60, creationflags=CREATE_NO_WINDOW,
        ).stdout.decode("utf-8-sig", errors="replace").strip()
        data = json.loads(out or "[]")
    except (OSError, subprocess.SubprocessError, ValueError):
        return []

    if isinstance(data, dict):
        data = [data]

    apps, seen = [], set()
    for d in data:
        name = (d.get("Name") or "").strip()
        app_id = (d.get("AppID") or "").strip()
        if not name or not app_id or app_id in seen:
            continue
        if name.lower().startswith(("uninstall", "désinstaller")):
            continue
        seen.add(app_id)
        apps.append({
            "kind": "app",
            "name": name,
            "app_id": app_id,
            "icon_key": "app:" + app_id,
            "icon_src": "shell:AppsFolder\\" + app_id,
        })
    apps.sort(key=lambda a: a["name"].lower())
    return apps


# ======================================================================
# Browser profiles
# ======================================================================

# (label, user-data folder under %LOCALAPPDATA%, App Paths exe name, fallback exe paths)
CHROMIUM_BROWSERS = [
    ("Chrome", r"Google\Chrome\User Data", "chrome.exe",
     [r"Google\Chrome\Application\chrome.exe"]),
    ("Edge", r"Microsoft\Edge\User Data", "msedge.exe",
     [r"Microsoft\Edge\Application\msedge.exe"]),
    ("Brave", r"BraveSoftware\Brave-Browser\User Data", "brave.exe",
     [r"BraveSoftware\Brave-Browser\Application\brave.exe"]),
    ("Vivaldi", r"Vivaldi\User Data", "vivaldi.exe",
     [r"Vivaldi\Application\vivaldi.exe"]),
    ("Chromium", r"Chromium\User Data", None,
     [r"Chromium\Application\chrome.exe"]),
]


def find_exe(app_path_name, relative_candidates):
    if IS_WIN and app_path_name:
        for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
            try:
                with winreg.OpenKey(hive, f"{APP_PATHS}\\{app_path_name}") as k:
                    path = str(winreg.QueryValueEx(k, "")[0]).strip().strip('"')
                    if os.path.isfile(path):
                        return path
            except OSError:
                pass
    for base in (PF, PF86, LOCAL):
        for rel in relative_candidates:
            path = os.path.join(base, rel)
            if os.path.isfile(path):
                return path
    return None


def make_profile(name, exe, args):
    """args: list (detected profiles) or str (custom, passed to the command line as typed)."""
    return {
        "kind": "profile",
        "name": name,
        "exe": exe,
        "args": args,
        "icon_key": "exe:" + exe.lower(),
        "icon_src": exe,
    }


def profile_id(p):
    return p["exe"].lower() + "|" + json.dumps(p["args"])


def chromium_profile_dir(p):
    """'Profile 1' for a Chromium profile entry, None for other browsers."""
    args = p["args"]
    if isinstance(args, list):
        for a in args:
            if a.startswith("--profile-directory="):
                return a.split("=", 1)[1].strip('"') or None
        return None
    m = re.search(r'--profile-directory=(?:"([^"]+)"|(\S+))', args or "")
    return (m.group(1) or m.group(2)) if m else None


def chromium_app_id_suffix(folder):
    """Chromium tags each window with an app id ending in '.UserData.<folder without spaces>'."""
    return ".UserData." + "".join(c for c in folder if (c.isascii() and c.isalnum()) or c == ".")


def short_profile_name(p):
    name = p["name"].split(" — ", 1)[-1]
    return re.sub(r"\s*\([^)]*@[^)]*\)\s*$", "", name).strip() or p["name"]


def detect_browser_profiles():
    found = []

    for browser, data_rel, app_name, rels in CHROMIUM_BROWSERS:
        state = os.path.join(LOCAL, data_rel, "Local State")
        if not os.path.isfile(state):
            continue
        exe = find_exe(app_name, rels)
        log(f"profile scan: {browser} data found, exe={exe}")
        if not exe:
            continue
        try:
            with open(state, encoding="utf-8") as f:
                cache = json.load(f).get("profile", {}).get("info_cache", {})
        except (OSError, ValueError):
            continue
        for folder, info in sorted(cache.items()):
            label = (info.get("name") or folder).strip()
            email = (info.get("user_name") or "").strip()
            if email and email not in label:
                label += f" ({email})"
            found.append(make_profile(f"{browser} — {label}", exe,
                                      [f"--profile-directory={folder}"]))

    ini = os.path.join(ROAMING, r"Mozilla\Firefox\profiles.ini")
    exe = find_exe("firefox.exe", [r"Mozilla Firefox\firefox.exe"])
    if exe and os.path.isfile(ini):
        cp = configparser.ConfigParser(interpolation=None)
        try:
            cp.read(ini, encoding="utf-8")
            for sec in cp.sections():
                if sec.startswith("Profile") and cp.has_option(sec, "Name"):
                    name = cp.get(sec, "Name")
                    found.append(make_profile(f"Firefox — {name}", exe,
                                              ["-P", name, "-no-remote"]))
        except configparser.Error:
            pass

    log(f"profile scan: {len(found)} profiles: {[p['name'] for p in found]}")
    return found


# ======================================================================
# "My apps" (added manually) + saved config
# ======================================================================

def make_pin_app(name, app_id):
    return {"kind": "pin", "target": "app", "name": name, "app_id": app_id,
            "icon_key": "app:" + app_id, "icon_src": "shell:AppsFolder\\" + app_id}


def make_pin_file(name, path):
    return {"kind": "pin", "target": "file", "name": name, "path": path,
            "icon_key": "exe:" + path.lower(), "icon_src": path}


def pin_id(p):
    return "app|" + p["app_id"] if p["target"] == "app" else "file|" + p["path"].lower()


def _read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def load_config():
    data = _read_json(CONFIG_FILE)
    if not isinstance(data, dict):
        data = {"profiles": [], "pins": []}

    profiles = [make_profile(d["name"], d["exe"], d.get("args", []))
                for d in data.get("profiles", [])
                if isinstance(d, dict) and d.get("name") and d.get("exe")]
    pins = []
    for d in data.get("pins", []):
        if not isinstance(d, dict) or not d.get("name"):
            continue
        if d.get("app_id"):
            pins.append(make_pin_app(d["name"], d["app_id"]))
        elif d.get("path"):
            pins.append(make_pin_file(d["name"], d["path"]))
    bindings = []
    for d in data.get("bindings", []):
        if not (isinstance(d, dict) and d.get("name")
                and (d.get("exe") or d.get("aumid") or d.get("app_id") or d.get("workspace"))):
            continue
        key = d.get("key")
        if not key and isinstance(d.get("slot"), int):  # older versions stored a number
            key = f"digits:{d['slot']}" if d["slot"] < 10 else f"top:{d['slot'] - 10}"
        if not key:
            continue
        b = make_binding(None, d["name"], d.get("exe"), d.get("aumid"), d.get("app_id"), d.get("profile"),
                         d.get("args"), d.get("workspace"))
        b["slot_id"], b["slot"] = key, SLOT_INDEX.get(key)
        bindings.append(b)
    workspaces = [make_workspace(d["name"], d.get("items", []), d.get("id"))
                  for d in data.get("workspaces", []) if isinstance(d, dict) and d.get("name")]
    return profiles, pins, bindings, workspaces


WS_ITEM_TYPES = ("app", "file", "profile", "url")
WS_POSITIONS = [("none", "Don't move"), ("max", "Maximized"), ("left", "Left half"), ("right", "Right half"),
                ("top", "Top half"), ("bottom", "Bottom half"), ("top-left", "Top-left quarter"),
                ("top-right", "Top-right quarter"), ("bottom-left", "Bottom-left quarter"),
                ("bottom-right", "Bottom-right quarter"), ("left-third", "Left third"),
                ("center-third", "Middle third"), ("right-third", "Right third"),
                ("left-2/3", "Left two thirds"), ("right-2/3", "Right two thirds"), ("center", "Centered"),
                ("saved", "Saved position")]
WS_POSITION_RECTS = {  # fractions of the monitor's work area: x, y, width, height
    "left": (0, 0, .5, 1), "right": (.5, 0, .5, 1), "top": (0, 0, 1, .5), "bottom": (0, .5, 1, .5),
    "top-left": (0, 0, .5, .5), "top-right": (.5, 0, .5, .5), "bottom-left": (0, .5, .5, .5),
    "bottom-right": (.5, .5, .5, .5), "left-third": (0, 0, 1 / 3, 1), "center-third": (1 / 3, 0, 1 / 3, 1),
    "right-third": (2 / 3, 0, 1 / 3, 1), "left-2/3": (0, 0, 2 / 3, 1), "right-2/3": (1 / 3, 0, 2 / 3, 1),
    "center": (.15, .08, .7, .84),
}


def clean_ws_item(d):
    """Validate one workspace entry from config.json."""
    if not isinstance(d, dict) or d.get("type") not in WS_ITEM_TYPES or not d.get("name"):
        return None
    need = {"app": "app_id", "file": "path", "profile": "exe", "url": "url"}[d["type"]]
    if not d.get(need):
        return None
    item = {k: d[k] for k in ("type", "name", "app_id", "path", "exe", "args", "url", "profile") if k in d}
    place = d.get("place") if isinstance(d.get("place"), dict) else {}
    pos = place.get("pos") if place.get("pos") in dict(WS_POSITIONS) else "none"
    rect = place.get("rect") if isinstance(place.get("rect"), list) and len(place.get("rect")) == 4 else None
    item["place"] = {"pos": pos if pos != "saved" or rect else "none",
                     "monitor": place.get("monitor") if isinstance(place.get("monitor"), int) else 0,
                     "rect": rect}
    return item


def make_workspace(name, items=(), ws_id=None):
    return {"kind": "workspace", "id": ws_id or "ws-" + os.urandom(4).hex(), "name": name,
            "items": [x for x in (clean_ws_item(i) for i in items) if x],
            "icon_key": "logo", "icon_src": None}


def ws_item_to_item(it):
    """A workspace entry as a launchable list item."""
    t = it["type"]
    if t == "app":
        return {"kind": "app", "name": it["name"], "app_id": it["app_id"],
                "icon_key": "app:" + it["app_id"], "icon_src": "shell:AppsFolder\\" + it["app_id"]}
    if t == "file":
        return make_pin_file(it["name"], it["path"])
    if t == "profile":
        return make_profile(it["name"], it["exe"], it.get("args") or [])
    return {"kind": "url", "name": it["name"], "url": it["url"], "profile": it.get("profile"),
            "icon_key": "url", "icon_src": None}


def ws_item_label(it):
    return {"app": "App", "file": "Program / file" if it.get("path", "").lower().endswith(".exe") else "File / folder",
            "profile": "Browser profile", "url": "Link"}[it["type"]]


def make_binding(slot, name, exe=None, aumid=None, app_id=None, profile=None, args=None, workspace=None):
    """A fixed shortcut: Alt+<slot> always means this app, browser profile or workspace."""
    app_id = None if profile or workspace else (app_id or aumid)
    slot_id = SLOTS[slot]["id"] if isinstance(slot, int) and 0 <= slot < len(SLOTS) else None
    if workspace:
        exe = aumid = profile = None
    return {"kind": "binding", "slot": slot, "slot_id": slot_id, "name": name,
            "exe": exe.lower() if exe else None, "aumid": aumid, "app_id": app_id,
            "profile": profile, "args": args, "workspace": workspace,
            "icon_key": "logo" if workspace else ("app:" + app_id) if app_id else "exe:" + (exe or "").lower(),
            "icon_src": None if workspace else ("shell:AppsFolder\\" + app_id) if app_id else exe}


def binding_matches(b, ident):
    exe, aumid, profile = ident
    if b["profile"]:
        return profile == b["profile"]
    return bool((b["aumid"] and aumid == b["aumid"]) or (b["exe"] and exe == b["exe"]))


def binding_key(b):
    if b.get("workspace"):
        return ("workspace", b["workspace"])
    return ("profile", b["profile"]) if b["profile"] else ("app", b["exe"] or "", b["aumid"] or "")


def save_config(profiles, pins, bindings=(), workspaces=()):
    os.makedirs(CONFIG_DIR, exist_ok=True)
    data = {
        "profiles": [{"name": p["name"], "exe": p["exe"], "args": p["args"]} for p in profiles],
        "pins": [{"name": p["name"], "app_id": p["app_id"]} if p["target"] == "app"
                 else {"name": p["name"], "path": p["path"]} for p in pins],
        "bindings": [{"key": b["slot_id"], "name": b["name"], "exe": b["exe"], "aumid": b["aumid"],
                      "app_id": b["app_id"], "profile": b["profile"], "args": b["args"],
                      "workspace": b.get("workspace")} for b in bindings],
        "workspaces": [{"id": w["id"], "name": w["name"], "items": w["items"]} for w in workspaces],
    }
    write_json_atomic(CONFIG_FILE, data, indent=2)


# ======================================================================
# Usage ("frecency"): things you open often and recently rank higher in search
# ======================================================================

USAGE_HALF_LIFE = 14 * 24 * 3600  # a use counts half as much after two weeks


class Usage:
    def __init__(self):
        data = _read_json(USAGE_FILE)
        self.data = {k: v for k, v in (data or {}).items() if isinstance(v, list) and len(v) == 2}

    def score(self, key, now=None):
        v = self.data.get(key)
        if not v:
            return 0.0
        now = now or time.time()
        return v[0] * 0.5 ** (max(0.0, now - v[1]) / USAGE_HALF_LIFE)

    def bump(self, key):
        if not key:
            return
        now = time.time()
        self.data[key] = [self.score(key, now) + 1.0, now]
        if len(self.data) > 2000:  # forget the least used
            for k in sorted(self.data, key=lambda k: self.score(k, now))[:500]:
                del self.data[k]
        try:
            write_json_atomic(USAGE_FILE, self.data)
        except OSError:
            log("could not save usage.json")

    def bonus(self, key):
        s = self.score(key)
        return min(30.0, 12.0 * math.log2(1.0 + s)) if s else 0.0


# ======================================================================
# Search helpers: matching, calculator, links, web searches
# ======================================================================

_WORD_SPLIT = re.compile(r"[\s\-_.·—–/\\()\[\]:,|]+")


def _subsequence(q, text):
    """Letters of q in order, close together (e.g. 'dstd' in 'android studio', not spread over a long title)."""
    limit = len(q) * 2 + 1
    for start, ch in enumerate(text):
        if ch != q[0]:
            continue
        i, pos = 1, start
        while i < len(q):
            pos = text.find(q[i], pos + 1)
            if pos < 0 or pos - start >= limit:
                break
            i += 1
        if i == len(q):
            return True
    return False


def match_score(q, name, keywords=()):
    """How well the query matches a name (0 = no match). Exact > prefix > word start > initials > inside."""
    best = 0
    for text in (name, *keywords):
        n = text.lower()
        if not n:
            continue
        if n == q:
            return 100
        if n.startswith(q):
            best = max(best, 90)
            continue
        words = [w for w in _WORD_SPLIT.split(n) if w]
        if " " in q:
            parts = q.split()
            if all(any(w.startswith(p) for w in words) for p in parts):
                best = max(best, 72)
            elif all(p in n for p in parts):
                best = max(best, 45)
            continue
        if any(w.startswith(q) for w in words):
            best = max(best, 75)
        elif len(q) >= 2 and "".join(w[0] for w in words).startswith(q):
            best = max(best, 65)
        elif q in n:
            best = max(best, 50)
        elif len(q) >= 3 and _subsequence(q, n):
            best = max(best, 20)
    return best


_CALC_FUNCS = {
    "sqrt": math.sqrt, "abs": abs, "round": round, "floor": math.floor, "ceil": math.ceil,
    "sin": math.sin, "cos": math.cos, "tan": math.tan, "asin": math.asin, "acos": math.acos, "atan": math.atan,
    "ln": math.log, "log": math.log10, "log10": math.log10, "log2": math.log2, "exp": math.exp,
    "rad": math.radians, "deg": math.degrees,
}
_CALC_CONSTS = {"pi": math.pi, "e": math.e, "tau": math.tau}
_CALC_OPS = {ast.Add: lambda a, b: a + b, ast.Sub: lambda a, b: a - b, ast.Mult: lambda a, b: a * b,
             ast.Div: lambda a, b: a / b, ast.FloorDiv: lambda a, b: a // b, ast.Mod: lambda a, b: a % b}


def _calc_eval(node):
    if isinstance(node, ast.Expression):
        return _calc_eval(node.body)
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)) and not isinstance(node.value, bool):
        return node.value
    if isinstance(node, ast.BinOp):
        a, b = _calc_eval(node.left), _calc_eval(node.right)
        if isinstance(node.op, ast.Pow):
            if abs(b) > 1000 or abs(a) > 1e100:
                raise ValueError("number too big")
            return a ** b
        op = _CALC_OPS.get(type(node.op))
        if op is None:
            raise ValueError("unsupported operator")
        return op(a, b)
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
        v = _calc_eval(node.operand)
        return v if isinstance(node.op, ast.UAdd) else -v
    if isinstance(node, ast.Name) and node.id in _CALC_CONSTS:
        return _CALC_CONSTS[node.id]
    if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in _CALC_FUNCS
            and len(node.args) == 1 and not node.keywords):
        return _CALC_FUNCS[node.func.id](_calc_eval(node.args[0]))
    raise ValueError("not a calculation")


def try_calc(q):
    """'= 12*7.5', or plain arithmetic like '12*7,5' → (expression, result text) or None."""
    explicit = q.startswith("=")
    expr = q[1:].strip() if explicit else q.strip()
    if not expr or len(expr) > 200:
        return None
    if not explicit:
        if not re.fullmatch(r"[\d\s.,+\-*/%^×÷()]+", expr) or not re.search(r"\d\s*[-+*/%^×÷]\s*[\d(.]", expr):
            return None
    expr2 = re.sub(r"(?<=\d),(?=\d)", ".", expr).replace("^", "**").replace("×", "*").replace("÷", "/")
    try:
        value = _calc_eval(ast.parse(expr2, mode="eval"))
    except (SyntaxError, ValueError, TypeError, ZeroDivisionError, OverflowError, RecursionError):
        return None
    if isinstance(value, float):
        if math.isnan(value) or math.isinf(value):
            return None
        text = f"{value:.12g}"
        if "e" not in text and "." in text:
            text = text.rstrip("0").rstrip(".")
    else:
        text = str(value)
        if len(text) > 60:
            text = f"{float(value):.12g}"
    return expr, text


_URL_TLDS = ("com org net io dev ai app co ma fr edu gov me info uk de us ca tv gg xyz ly so be ch nl es it "
             "eu ru jp cn in br au tech site online blog cloud page").split()


def looks_like_url(q):
    """'github.com/x', 'https://…', 'localhost:3000' → full URL, else None."""
    q = q.strip()
    if " " in q or len(q) < 4:
        return None
    if re.match(r"^https?://\S+$", q, re.I):
        return q
    if re.match(r"^(localhost|\d{1,3}(\.\d{1,3}){3})(:\d+)?(/\S*)?$", q, re.I):
        return "http://" + q
    m = re.match(r"^(www\.)?([a-z0-9-]+\.)+([a-z]{2,})(:\d+)?(/\S*)?$", q, re.I)
    if m and (m.group(1) or m.group(3).lower() in _URL_TLDS):
        return "https://" + q
    return None


def engine_name(url):
    host = urllib.parse.urlsplit(url).netloc.lower()
    known = {"www.google.com/maps": "Google Maps", "translate.google.com": "Google Translate",
             "www.google.com": "Google", "www.youtube.com": "YouTube", "en.wikipedia.org": "Wikipedia",
             "github.com": "GitHub", "duckduckgo.com": "DuckDuckGo", "www.bing.com": "Bing"}
    path = urllib.parse.urlsplit(url).path
    if host == "www.google.com" and path.startswith("/maps"):
        return "Google Maps"
    if host in known:
        return known[host]
    parts = [p for p in host.split(".") if p not in ("www", "com", "org", "net")]
    return (parts[0] if parts else host).capitalize()


def engine_url(template, text):
    return template.replace("{}", urllib.parse.quote_plus(text))


def load_ui():
    data = _read_json(UI_FILE)
    return data if isinstance(data, dict) else {}


def save_ui(data):
    try:
        os.makedirs(CONFIG_DIR, exist_ok=True)
        with open(UI_FILE, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
    except OSError:
        log("could not save ui.json")


# ======================================================================
# Encrypted notes: AES-256-GCM, key from the password with scrypt
# ======================================================================

NOTES_KDF = {"name": "scrypt", "n": 2 ** 15, "r": 8, "p": 1}
NOTES_AAD = b"applauncher-notes-v1"
def notes_min_password():
    return SETTINGS.get("notes_min_pw", 4)


def notes_crypto_available():
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM  # noqa: F401
        return True
    except ImportError:
        return False


def notes_key(password, salt, kdf):
    return hashlib.scrypt(password.encode("utf-8"), salt=salt, n=kdf["n"], r=kdf["r"], p=kdf["p"],
                          maxmem=2 ** 27, dklen=32)


def _notes_aad(salt, kdf):
    # Binds the settings to the ciphertext, so they can't be swapped without detection.
    return NOTES_AAD + b"|" + json.dumps(kdf, sort_keys=True).encode("ascii") + b"|" + salt


def _b64(b):
    return base64.b64encode(b).decode("ascii")


def notes_encrypt(payload, key, salt, kdf):
    """Encrypt a payload into a self-contained blob (fresh random nonce each time)."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    nonce = os.urandom(12)
    data = AESGCM(key).encrypt(nonce, json.dumps(payload, ensure_ascii=False).encode("utf-8"), _notes_aad(salt, kdf))
    return {"v": 2, "cipher": "AES-256-GCM", "kdf": kdf, "salt": _b64(salt), "nonce": _b64(nonce), "data": _b64(data)}


def notes_read_blob(path):
    with open(path, encoding="utf-8") as f:
        blob = json.load(f)
    if not isinstance(blob, dict) or not {"kdf", "salt", "nonce", "data"} <= set(blob):
        raise ValueError("not an encrypted notes file")
    return blob


def notes_blob_key(blob, password, cache=None):
    """Derive the key for a blob (scrypt is slow, so keys are cached per salt)."""
    salt, kdf = base64.b64decode(blob["salt"]), blob["kdf"]
    ck = (blob["salt"], json.dumps(kdf, sort_keys=True))
    if cache is not None and ck in cache:
        return cache[ck], salt, kdf
    key = notes_key(password, salt, kdf)
    if cache is not None:
        cache[ck] = key
    return key, salt, kdf


def notes_decrypt_blob(blob, key):
    from cryptography.exceptions import InvalidTag
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    salt, kdf = base64.b64decode(blob["salt"]), blob["kdf"]
    try:
        plain = AESGCM(key).decrypt(base64.b64decode(blob["nonce"]), base64.b64decode(blob["data"]),
                                    _notes_aad(salt, kdf))
    except InvalidTag:
        raise ValueError("wrong password or damaged file") from None
    return json.loads(plain.decode("utf-8"))


def notes_decrypt_file(path, password, cache=None):
    """Decrypt any notes file with a password → (payload, key, salt, kdf). ValueError if it doesn't open."""
    blob = notes_read_blob(path)
    key, salt, kdf = notes_blob_key(blob, password, cache)
    return notes_decrypt_blob(blob, key), key, salt, kdf


def payload_tabs(payload):
    """A tab file holds one tab; the older single notes file holds {"tabs": [...]}."""
    if isinstance(payload.get("tabs"), list):
        return [t for t in payload["tabs"] if isinstance(t, dict)]
    return [payload]


def write_json_atomic(path, obj, indent=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=indent, ensure_ascii=False)
    os.replace(tmp, path)


def notes_tab_files(folder=None):
    folder = folder or NOTES_DIR
    try:
        return sorted(os.path.join(folder, n) for n in os.listdir(folder) if n.lower().endswith(".enc"))
    except OSError:
        return []


def notes_exist():
    return bool(notes_tab_files()) or os.path.isfile(NOTES_FILE)


def new_tab_id():
    return "tab-" + os.urandom(5).hex()


def notes_load_all(password):
    """Open every tab file in data/notes. Returns (tabs, key, salt, kdf, failed_paths, ids_to_delete)
    or None when there are no tab files. Raises ValueError if the password opens none of them."""
    files = notes_tab_files()
    if not files:
        return None
    cache, tabs, failed, drop, vault = {}, [], [], [], None
    for path in files:
        tid = os.path.splitext(os.path.basename(path))[0]
        try:
            payload, key, salt, kdf = notes_decrypt_file(path, password, cache)
        except (ValueError, KeyError, OSError):
            failed.append(path)
            continue
        if vault is None:
            vault = (key, salt, kdf)
        other_key = salt != vault[1]  # copied in from elsewhere: re-encrypt with this vault's key
        items = payload_tabs(payload)
        if len(items) > 1:
            drop.append(tid)  # a whole older notes file dropped in: split it into tab files
        for t in items:
            tabs.append({"id": None if len(items) > 1 else tid, "title": t.get("title") or "Notes",
                         "text": t.get("text", ""), "order": t.get("order"), "closed": bool(t.get("closed")),
                         "dirty": other_key or len(items) > 1})
    if vault is None:
        raise ValueError("wrong password")
    tabs.sort(key=lambda t: (t["order"] is None, t["order"] or 0))
    return tabs, vault[0], vault[1], vault[2], failed, drop


def notes_open(password, path=None):
    """Decrypt one notes file (kept for compatibility)."""
    return notes_decrypt_file(path or NOTES_FILE, password)


# ======================================================================
# Clipboard history, encrypted on disk.
# Each item is sealed with a public key (X25519 + AES-256-GCM), so copying never needs your password.
# The matching private key is stored locked by your notes password, so only you can read old items.
# ======================================================================

CLIP_INFO = b"applauncher-clipboard-v1"
CLIP_MAX_CHARS = 100_000


def clip_create_keys(key, salt, kdf):
    """Make the clipboard key pair; the private half is encrypted like a notes tab."""
    from cryptography.hazmat.primitives import serialization as ser
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
    priv = X25519PrivateKey.generate()
    raw = priv.private_bytes(ser.Encoding.Raw, ser.PrivateFormat.Raw, ser.NoEncryption())
    pub = priv.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    write_json_atomic(CLIP_KEY_FILE, {"v": 1, "public": _b64(pub),
                                      "private": notes_encrypt({"x25519": _b64(raw)}, key, salt, kdf)})


def clip_public_key():
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PublicKey
    data = _read_json(CLIP_KEY_FILE)
    if not isinstance(data, dict) or "public" not in data:
        return None
    return X25519PublicKey.from_public_bytes(base64.b64decode(data["public"]))


def clip_private_key(password=None, vault=None):
    """Unlock the private key with the notes password, or with an unlocked notes vault (key, salt, kdf)."""
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
    data = _read_json(CLIP_KEY_FILE)
    if not isinstance(data, dict) or "private" not in data:
        raise ValueError("no clipboard key")
    blob = data["private"]
    if vault and blob.get("salt") == _b64(vault[1]):
        key = vault[0]
    elif password is not None:
        key = notes_blob_key(blob, password)[0]
    else:
        raise ValueError("password needed")
    raw = base64.b64decode(notes_decrypt_blob(blob, key)["x25519"])
    return X25519PrivateKey.from_private_bytes(raw)


def clip_rewrap(old_vault, new_vault):
    """Notes password changed: lock the clipboard private key with the new one."""
    if not os.path.isfile(CLIP_KEY_FILE):
        return
    data = _read_json(CLIP_KEY_FILE)
    payload = notes_decrypt_blob(data["private"], old_vault[0])
    data["private"] = notes_encrypt(payload, *new_vault)
    write_json_atomic(CLIP_KEY_FILE, data)


def _clip_key(shared, epk, pub):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    return HKDF(algorithm=hashes.SHA256(), length=32, salt=None, info=CLIP_INFO + epk + pub).derive(shared)


def clip_seal(pub, text):
    from cryptography.hazmat.primitives import serialization as ser
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    eph = X25519PrivateKey.generate()
    epk = eph.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    pub_raw = pub.public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    key = _clip_key(eph.exchange(pub), epk, pub_raw)
    nonce = os.urandom(12)
    return {"epk": _b64(epk), "nonce": _b64(nonce),
            "data": _b64(AESGCM(key).encrypt(nonce, text.encode("utf-8"), CLIP_INFO))}


def clip_open(priv, sealed):
    from cryptography.exceptions import InvalidTag
    from cryptography.hazmat.primitives import serialization as ser
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PublicKey
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    epk = base64.b64decode(sealed["epk"])
    pub_raw = priv.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    key = _clip_key(priv.exchange(X25519PublicKey.from_public_bytes(epk)), epk, pub_raw)
    try:
        return AESGCM(key).decrypt(base64.b64decode(sealed["nonce"]), base64.b64decode(sealed["data"]),
                                   CLIP_INFO).decode("utf-8")
    except InvalidTag:
        raise ValueError("damaged clipboard item") from None


CLIP_HARD_CAP = 5000  # safety limit when max_items = 0 ("keep everything")


def dpapi_available():
    return IS_WIN


class ClipboardStore:
    """Clipboard items, newest first.
    encryption = windows: each item is encrypted with your Windows account (readable after you sign in).
    encryption = password: each item is sealed with a public key; reading old ones needs the notes password."""

    def __init__(self):
        self.items = []    # {"text": str | None, "sealed": dict | None, "t": float, "pinned": bool, "old": bool}
        self.pub = None
        self.ignore = None  # text we put on the clipboard ourselves and don't want as a new item
        self.reload_settings()

    def reload_settings(self):
        self.mode = SETTINGS.get("clip_encryption", "windows")
        self.max_items = SETTINGS.get("clip_max", 0)
        self.pub = None
        if not SETTINGS.get("clip_save", True):
            self.save_enabled = False
        elif self.mode == "windows":
            self.save_enabled = dpapi_available()
        else:
            self.save_enabled = notes_crypto_available()
            if self.save_enabled:
                try:
                    self.pub = clip_public_key()
                except Exception:
                    log("clipboard key error:\n" + traceback.format_exc())

    @property
    def can_save(self):
        return self.save_enabled and (self.mode == "windows" or self.pub is not None)

    def _seal(self, text):
        if self.mode == "windows":
            return {"dpapi": _b64(dpapi_protect(text.encode("utf-8")))}
        return clip_seal(self.pub, text)

    def _matches_mode(self, sealed):
        return ("dpapi" in sealed) == (self.mode == "windows")

    def load(self):
        if not self.save_enabled:
            return
        data = _read_json(CLIP_FILE)
        for d in (data or {}).get("items", []):
            sealed = d.get("sealed") if isinstance(d, dict) else None
            if not isinstance(sealed, dict):
                continue
            text = None
            if "dpapi" in sealed:
                try:
                    text = dpapi_unprotect(base64.b64decode(sealed["dpapi"])).decode("utf-8")
                except (ValueError, OSError, UnicodeDecodeError):
                    log("a saved clipboard item couldn't be read (saved by another Windows account?)")
                    continue
            self.items.append({"text": text, "sealed": sealed, "t": float(d.get("t") or 0),
                               "pinned": bool(d.get("pinned")), "old": text is None})

    def locked_count(self):
        return sum(1 for it in self.items if it["text"] is None)

    def visible(self):
        """Readable items: pinned first, then newest first."""
        shown = [it for it in self.items if it["text"] is not None]
        return [it for it in shown if it["pinned"]] + [it for it in shown if not it["pinned"]]

    def add(self, text):
        if not text or not text.strip() or len(text) > CLIP_MAX_CHARS:
            return False
        if self.ignore is not None and text == self.ignore:
            self.ignore = None
            return False
        if self.items and self.items[0]["text"] == text:
            return False
        same = next((it for it in self.items if it["text"] == text), None)
        if same:
            self.items.remove(same)
            same["t"] = time.time()
            self.items.insert(0, same)
        else:
            self.items.insert(0, {"text": text, "sealed": None, "t": time.time(), "pinned": False, "old": False})
        self._trim()
        self.save()
        return True

    def _trim(self):
        limit = self.max_items or CLIP_HARD_CAP
        kept, n = [], 0
        for it in self.items:
            if not it["pinned"]:
                n += 1
                if n > limit:
                    continue
            kept.append(it)
        self.items = kept

    def save(self):
        if not self.can_save:
            return
        for it in self.items:  # encrypt new items, and re-encrypt readable ones if the encryption setting changed
            if it["text"] is not None and (not it["sealed"] or not self._matches_mode(it["sealed"])):
                try:
                    it["sealed"] = self._seal(it["text"])
                    it["old"] = False
                except Exception:
                    log("clipboard encrypt error:\n" + traceback.format_exc())
        try:
            write_json_atomic(CLIP_FILE, {"v": 2, "items": [
                {"t": round(it["t"]), "pinned": it["pinned"], "sealed": it["sealed"]}
                for it in self.items if it["sealed"]]})
        except OSError:
            log("could not save clipboard history")

    def keys_created(self):
        """The password key pair now exists: save what's in memory (password mode)."""
        self.reload_settings()
        self.save()

    def unlock(self, priv):
        bad = 0
        for it in list(self.items):
            if it["text"] is None:
                try:
                    it["text"] = clip_open(priv, it["sealed"])
                except (ValueError, KeyError):
                    self.items.remove(it)
                    bad += 1
        seen, kept = set(), []  # drop duplicates now that everything is readable
        for it in self.items:
            if it["text"] in seen and not it["pinned"]:
                continue
            seen.add(it["text"])
            kept.append(it)
        self.items = kept
        self.save()  # with encryption = windows this converts them, so they never need the password again
        return bad

    def lock(self):
        for it in self.items:
            if it["old"] and it["sealed"] and "dpapi" not in it["sealed"]:
                it["text"] = None

    def remove(self, items):
        drop = {id(i) for i in (items if isinstance(items, (list, tuple)) else [items])}
        self.items = [it for it in self.items if id(it) not in drop]
        self.save()

    def toggle_pin(self, items):
        items = items if isinstance(items, (list, tuple)) else [items]
        pin = not all(it["pinned"] for it in items)
        for it in items:
            it["pinned"] = pin
        self._trim()
        self.save()

    def clear(self):
        """Delete everything that isn't pinned (including encrypted older items)."""
        self.items = [it for it in self.items if it["pinned"]]
        self.save()


# ======================================================================
# PNG encoder (for Tk images, no Pillow needed)
# ======================================================================

def encode_png(w, h, rgba):
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


def make_logo_rgba(size, accent=(0x8a, 0xa4, 0xff), light=(0xe8, 0xe9, 0xed)):
    """App icon: four rounded tiles (three accent, one light), anti-aliased. Returns RGBA bytes."""
    pad, gap = size * 0.06, size * 0.11
    cell = (size - 2 * pad - gap) / 2
    rad = cell * 0.3
    tiles = []
    for row in range(2):
        for col in range(2):
            x0, y0 = pad + col * (cell + gap), pad + row * (cell + gap)
            tiles.append((x0, y0, x0 + cell, y0 + cell, light if (row, col) == (1, 1) else accent))
    out = bytearray(size * size * 4)
    ss = 4
    for y in range(size):
        for x in range(size):
            cov, rgb = 0, (0, 0, 0)
            for sy in range(ss):
                for sx in range(ss):
                    px, py = x + (sx + 0.5) / ss, y + (sy + 0.5) / ss
                    for x0, y0, x1, y1, color in tiles:
                        if x0 <= px <= x1 and y0 <= py <= y1:
                            dx = max(x0 + rad - px, 0, px - (x1 - rad))
                            dy = max(y0 + rad - py, 0, py - (y1 - rad))
                            if dx * dx + dy * dy <= rad * rad:
                                cov += 1
                                rgb = color
                            break
            if cov:
                i = (y * size + x) * 4
                out[i:i + 4] = bytes((*rgb, round(255 * cov / (ss * ss))))
    return bytes(out)


def make_checkbox_rgba(size, checked, accent=(0x8a, 0xa4, 0xff), border=(0x6a, 0x6f, 0x7c),
                       mark=(0x0b, 0x0d, 0x12)):
    """A rounded checkbox (empty, or filled with a check mark), anti-aliased RGBA bytes."""
    pad, rad = size * 0.12, size * 0.22
    x0, y0, x1, y1 = pad, pad, size - pad, size - pad
    lw = max(1.2, size / 14)
    # check mark: two segments
    segs = [((size * .30, size * .52), (size * .44, size * .66)), ((size * .44, size * .66), (size * .72, size * .36))]

    def inside(px, py, shrink=0.0):
        a, b, c, d, r = x0 + shrink, y0 + shrink, x1 - shrink, y1 - shrink, max(0.0, rad - shrink)
        if not (a <= px <= c and b <= py <= d):
            return False
        dx = max(a + r - px, 0, px - (c - r))
        dy = max(b + r - py, 0, py - (d - r))
        return dx * dx + dy * dy <= r * r

    def near_seg(px, py, w):
        for (ax, ay), (bx, by) in segs:
            vx, vy = bx - ax, by - ay
            t = max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
            qx, qy = ax + t * vx - px, ay + t * vy - py
            if qx * qx + qy * qy <= w * w:
                return True
        return False

    out = bytearray(size * size * 4)
    ss = 4
    for y in range(size):
        for x in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    px, py = x + (sx + .5) / ss, y + (sy + .5) / ss
                    col = None
                    if checked:
                        if inside(px, py):
                            col = mark if near_seg(px, py, lw) else accent
                    elif inside(px, py) and not inside(px, py, lw):
                        col = border
                    if col:
                        acc[0] += col[0]
                        acc[1] += col[1]
                        acc[2] += col[2]
                        acc[3] += 1
            if acc[3]:
                i = (y * size + x) * 4
                n = acc[3]
                out[i:i + 4] = bytes((acc[0] // n, acc[1] // n, acc[2] // n, round(255 * n / (ss * ss))))
    return bytes(out)


def make_ico(sizes=(16, 20, 24, 32, 40, 48, 64, 128, 256)):
    """A Windows .ico with PNG images inside (for the .exe icon)."""
    pngs = [encode_png(n, n, make_logo_rgba(n)) for n in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset, entries, data = 6 + 16 * len(sizes), b"", b""
    for n, png in zip(sizes, pngs):
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    return head + entries + data


# ======================================================================
# Win32 (ctypes)
# ======================================================================

if IS_WIN:
    c_void_p = ctypes.c_void_p
    c_uint32 = ctypes.c_uint32

    class GUID(ctypes.Structure):
        _fields_ = [("Data1", ctypes.c_uint32), ("Data2", ctypes.c_uint16),
                    ("Data3", ctypes.c_uint16), ("Data4", ctypes.c_ubyte * 8)]

        def __init__(self, text):
            super().__init__()
            u = uuid.UUID(text)
            self.Data1, self.Data2, self.Data3 = u.fields[0], u.fields[1], u.fields[2]
            for i, b in enumerate(u.bytes[8:]):
                self.Data4[i] = b

    class SIZE(ctypes.Structure):
        _fields_ = [("cx", ctypes.c_long), ("cy", ctypes.c_long)]

    class BITMAP(ctypes.Structure):
        _fields_ = [("bmType", ctypes.c_long), ("bmWidth", ctypes.c_long),
                    ("bmHeight", ctypes.c_long), ("bmWidthBytes", ctypes.c_long),
                    ("bmPlanes", ctypes.c_uint16), ("bmBitsPixel", ctypes.c_uint16),
                    ("bmBits", c_void_p)]

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_long),
                    ("biHeight", ctypes.c_long), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_long),
                    ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", ctypes.c_uint32),
                    ("biClrImportant", ctypes.c_uint32)]

    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long), ("mouseData", ctypes.c_uint32),
                    ("dwFlags", ctypes.c_uint32), ("time", ctypes.c_uint32), ("dwExtraInfo", ctypes.c_size_t)]

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", ctypes.c_uint16), ("wScan", ctypes.c_uint16), ("dwFlags", ctypes.c_uint32),
                    ("time", ctypes.c_uint32), ("dwExtraInfo", ctypes.c_size_t)]

    class _INPUTUNION(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]

    class INPUT(ctypes.Structure):
        _fields_ = [("type", ctypes.c_uint32), ("u", _INPUTUNION)]

    IID_IShellItemImageFactory = GUID("bcc18b79-ba16-442f-80c4-8a59c30c463b")
    SIIGBF_ICONONLY = 0x4
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, c_void_p, c_void_p)

    ole32 = ctypes.windll.ole32
    shell32 = ctypes.windll.shell32
    gdi32 = ctypes.windll.gdi32
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    dwmapi = ctypes.windll.dwmapi

    def _sig(fn, argtypes, restype=ctypes.c_int):
        fn.argtypes, fn.restype = argtypes, restype

    _sig(ole32.CoInitializeEx, [c_void_p, c_uint32], ctypes.c_long)
    _sig(shell32.SHCreateItemFromParsingName,
         [ctypes.c_wchar_p, c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(c_void_p)], ctypes.c_long)

    _sig(gdi32.GetObjectW, [c_void_p, ctypes.c_int, c_void_p])
    _sig(gdi32.GetDIBits, [c_void_p, c_void_p, ctypes.c_uint, ctypes.c_uint, c_void_p, c_void_p, ctypes.c_uint])
    _sig(gdi32.DeleteObject, [c_void_p])
    _sig(gdi32.CreateCompatibleDC, [c_void_p], c_void_p)
    _sig(gdi32.CreateDIBSection,
         [c_void_p, c_void_p, ctypes.c_uint, ctypes.POINTER(c_void_p), c_void_p, c_uint32], c_void_p)
    _sig(gdi32.SelectObject, [c_void_p, c_void_p], c_void_p)
    _sig(gdi32.DeleteDC, [c_void_p])

    _sig(user32.GetDC, [c_void_p], c_void_p)
    _sig(user32.ReleaseDC, [c_void_p, c_void_p])
    _sig(user32.DrawIconEx, [c_void_p, ctypes.c_int, ctypes.c_int, c_void_p, ctypes.c_int,
                             ctypes.c_int, ctypes.c_uint, c_void_p, ctypes.c_uint])
    _sig(user32.EnumWindows, [WNDENUMPROC, c_void_p])
    _sig(user32.EnumChildWindows, [c_void_p, WNDENUMPROC, c_void_p])
    _sig(user32.IsWindow, [c_void_p])
    _sig(user32.IsWindowVisible, [c_void_p])
    _sig(user32.IsIconic, [c_void_p])
    _sig(user32.GetWindow, [c_void_p, ctypes.c_uint], c_void_p)
    _sig(user32.GetWindowLongW, [c_void_p, ctypes.c_int], ctypes.c_long)
    _sig(user32.SetWindowLongW, [c_void_p, ctypes.c_int, ctypes.c_long], ctypes.c_long)
    _sig(user32.GetWindowTextLengthW, [c_void_p])
    _sig(user32.GetWindowTextW, [c_void_p, ctypes.c_wchar_p, ctypes.c_int])
    _sig(user32.GetClassNameW, [c_void_p, ctypes.c_wchar_p, ctypes.c_int])
    _sig(user32.GetWindowThreadProcessId, [c_void_p, ctypes.POINTER(c_uint32)], c_uint32)
    _sig(user32.ShowWindow, [c_void_p, ctypes.c_int])
    _sig(user32.SetForegroundWindow, [c_void_p])
    _sig(user32.GetForegroundWindow, [], c_void_p)
    _sig(user32.BringWindowToTop, [c_void_p])
    _sig(user32.AttachThreadInput, [c_uint32, c_uint32, ctypes.c_bool])
    _sig(user32.PostMessageW, [c_void_p, ctypes.c_uint, c_void_p, c_void_p])
    _sig(user32.SendInput, [ctypes.c_uint, ctypes.POINTER(INPUT), ctypes.c_int], ctypes.c_uint)
    _sig(user32.SwitchToThisWindow, [c_void_p, ctypes.c_bool], None)
    _sig(user32.SendMessageTimeoutW, [c_void_p, ctypes.c_uint, c_void_p, c_void_p, ctypes.c_uint,
                                      ctypes.c_uint, ctypes.POINTER(c_void_p)], c_void_p)
    _get_class_long = getattr(user32, "GetClassLongPtrW", None) or user32.GetClassLongW
    _sig(_get_class_long, [c_void_p, ctypes.c_int], c_void_p)
    _sig(user32.RegisterHotKey, [c_void_p, ctypes.c_int, ctypes.c_uint, ctypes.c_uint])
    _sig(user32.UnregisterHotKey, [c_void_p, ctypes.c_int])
    _sig(user32.GetMessageW, [ctypes.POINTER(wintypes.MSG), c_void_p, ctypes.c_uint, ctypes.c_uint])
    _sig(user32.SetWindowsHookExW, [ctypes.c_int, c_void_p, c_void_p, c_uint32], c_void_p)
    _sig(user32.CallNextHookEx, [c_void_p, ctypes.c_int, ctypes.c_size_t, c_void_p], ctypes.c_ssize_t)
    _sig(user32.UnhookWindowsHookEx, [c_void_p])
    _sig(user32.GetAsyncKeyState, [ctypes.c_int], ctypes.c_short)
    _sig(user32.TranslateMessage, [ctypes.POINTER(wintypes.MSG)])
    _sig(user32.DispatchMessageW, [ctypes.POINTER(wintypes.MSG)], ctypes.c_ssize_t)
    _sig(kernel32.GetModuleHandleW, [ctypes.c_wchar_p], c_void_p)
    _sig(shell32.SHGetKnownFolderPath, [ctypes.POINTER(GUID), c_uint32, c_void_p, ctypes.POINTER(c_void_p)],
         ctypes.c_long)
    _sig(ole32.CoTaskMemFree, [c_void_p], None)
    _sig(user32.PeekMessageW, [ctypes.POINTER(wintypes.MSG), c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint])
    _sig(user32.PostThreadMessageW, [c_uint32, ctypes.c_uint, c_void_p, c_void_p])

    _sig(kernel32.OpenProcess, [c_uint32, ctypes.c_bool, c_uint32], c_void_p)
    _sig(kernel32.CloseHandle, [c_void_p])
    _sig(kernel32.QueryFullProcessImageNameW,
         [c_void_p, c_uint32, ctypes.c_wchar_p, ctypes.POINTER(c_uint32)])
    _sig(kernel32.GetApplicationUserModelId,
         [c_void_p, ctypes.POINTER(c_uint32), ctypes.c_wchar_p], ctypes.c_long)
    _sig(kernel32.GetCurrentThreadId, [], c_uint32)
    advapi32 = ctypes.windll.advapi32
    _sig(advapi32.OpenProcessToken, [c_void_p, c_uint32, ctypes.POINTER(c_void_p)])
    _sig(advapi32.GetTokenInformation, [c_void_p, ctypes.c_int, c_void_p, c_uint32, ctypes.POINTER(c_uint32)])
    _sig(dwmapi.DwmGetWindowAttribute, [c_void_p, c_uint32, c_void_p, c_uint32], ctypes.c_long)
    _sig(dwmapi.DwmSetWindowAttribute, [c_void_p, c_uint32, c_void_p, c_uint32], ctypes.c_long)

    # ---------------------------------------------------------- icons

    def _vcall(this, index, restype, argtypes, *args):
        vtbl = ctypes.cast(this, ctypes.POINTER(ctypes.POINTER(c_void_p)))[0]
        fn = ctypes.WINFUNCTYPE(restype, c_void_p, *argtypes)(vtbl[index])
        return fn(this, *args)

    def _hbitmap_to_png(hbm):
        bm = BITMAP()
        if not gdi32.GetObjectW(hbm, ctypes.sizeof(bm), ctypes.byref(bm)):
            return None
        w, h = bm.bmWidth, abs(bm.bmHeight)
        bi = BITMAPINFOHEADER(biSize=ctypes.sizeof(BITMAPINFOHEADER), biWidth=w,
                              biHeight=-h, biPlanes=1, biBitCount=32, biCompression=0)
        buf = ctypes.create_string_buffer(w * h * 4)
        hdc = user32.GetDC(None)
        try:
            lines = gdi32.GetDIBits(hdc, hbm, 0, h, buf, ctypes.byref(bi), 0)
        finally:
            user32.ReleaseDC(None, hdc)
        if lines != h:
            return None

        px = bytearray(buf.raw)  # BGRA, premultiplied
        has_alpha = any(px[3::4])
        for i in range(0, len(px), 4):
            b, g, r, a = px[i], px[i + 1], px[i + 2], px[i + 3]
            if not has_alpha:
                a = 255
            elif 0 < a < 255:
                r, g, b = min(255, r * 255 // a), min(255, g * 255 // a), min(255, b * 255 // a)
            px[i], px[i + 1], px[i + 2], px[i + 3] = r, g, b, a
        return encode_png(w, h, bytes(px))

    def extract_icon_png(parsing_name, size):
        """Icon for a file path or shell:AppsFolder\\<AppID>, as PNG bytes."""
        item = c_void_p()
        hr = shell32.SHCreateItemFromParsingName(
            parsing_name, None, ctypes.byref(IID_IShellItemImageFactory), ctypes.byref(item))
        if hr != 0 or not item.value:
            return None
        hbm = c_void_p()
        try:
            hr = _vcall(item.value, 3, ctypes.c_long,
                        [SIZE, ctypes.c_int, ctypes.POINTER(c_void_p)],
                        SIZE(size, size), SIIGBF_ICONONLY, ctypes.byref(hbm))
        finally:
            _vcall(item.value, 2, ctypes.c_ulong, [])
        if hr != 0 or not hbm.value:
            return None
        try:
            return _hbitmap_to_png(hbm)
        finally:
            gdi32.DeleteObject(hbm)

    def _hicon_to_png(hicon, s):
        """Draw the icon on black and on white; the difference gives the alpha channel."""
        hdc = gdi32.CreateCompatibleDC(None)
        if not hdc:
            return None
        layers = []
        try:
            for fill in (0x00, 0xFF):
                bi = BITMAPINFOHEADER(biSize=ctypes.sizeof(BITMAPINFOHEADER), biWidth=s,
                                      biHeight=-s, biPlanes=1, biBitCount=32)
                bits = c_void_p()
                hbm = gdi32.CreateDIBSection(hdc, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
                if not hbm or not bits.value:
                    return None
                old = gdi32.SelectObject(hdc, hbm)
                ctypes.memset(bits.value, fill, s * s * 4)
                user32.DrawIconEx(hdc, 0, 0, hicon, s, s, 0, None, 3)  # DI_NORMAL
                gdi32.GdiFlush()
                layers.append(ctypes.string_at(bits.value, s * s * 4))
                gdi32.SelectObject(hdc, old)
                gdi32.DeleteObject(hbm)
        finally:
            gdi32.DeleteDC(hdc)

        black, white = layers
        out = bytearray(s * s * 4)
        for i in range(0, len(out), 4):
            a = max(0, min(255, 255 - (white[i + 1] - black[i + 1])))
            if a:
                out[i] = min(255, black[i + 2] * 255 // a)
                out[i + 1] = min(255, black[i + 1] * 255 // a)
                out[i + 2] = min(255, black[i] * 255 // a)
            out[i + 3] = a
        return encode_png(s, s, bytes(out))

    # ---------------------------------------------------------- windows

    def window_pid(hwnd):
        pid = c_uint32()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        return pid.value

    def process_info(pid):
        """(exe path, AppUserModelID or None) for a process."""
        h = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return None, None
        try:
            buf, n = ctypes.create_unicode_buffer(1024), c_uint32(1024)
            exe = buf.value if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) else None
            abuf, an = ctypes.create_unicode_buffer(512), c_uint32(512)
            aumid = abuf.value if kernel32.GetApplicationUserModelId(h, ctypes.byref(an), abuf) == 0 else None
            return exe, aumid
        finally:
            kernel32.CloseHandle(h)

    def _real_pid(hwnd):
        """Store apps run inside ApplicationFrameHost; find the real app process."""
        pid = window_pid(hwnd)
        exe, _ = process_info(pid)
        if exe and os.path.basename(exe).lower() == "applicationframehost.exe":
            found = []

            def cb(child, _):
                cpid = window_pid(child)
                if cpid != pid:
                    found.append(cpid)
                    return False
                return True

            user32.EnumChildWindows(hwnd, WNDENUMPROC(cb), None)
            if found:
                return found[0]
        return pid

    def _window_hicon(hwnd):
        res = c_void_p()
        for which in (1, 2, 0):  # ICON_BIG, ICON_SMALL2, ICON_SMALL
            if user32.SendMessageTimeoutW(hwnd, 0x7F, which, None, 0x2, 150, ctypes.byref(res)) and res.value:
                return res.value
        for idx in (-14, -34):   # GCLP_HICON, GCLP_HICONSM
            h = _get_class_long(hwnd, idx)
            if h:
                return h
        return None

    def window_icon_png(hwnd, size):
        exe, aumid = process_info(_real_pid(hwnd))
        if aumid:
            png = extract_icon_png("shell:AppsFolder\\" + aumid, size)
            if png:
                return png
        hicon = _window_hicon(hwnd)
        if hicon:
            png = _hicon_to_png(hicon, size)
            if png:
                return png
        return extract_icon_png(exe, size) if exe else None

    def list_open_windows():
        """Alt+Tab-style list of (hwnd, title), most recently used first."""
        own_pid = os.getpid()
        out = []

        def cb(hwnd, _):
            try:
                if not hwnd or not user32.IsWindowVisible(hwnd) or user32.GetWindow(hwnd, 4):  # GW_OWNER
                    return True
                ex = user32.GetWindowLongW(hwnd, -20)  # GWL_EXSTYLE
                if (ex & 0x80 and not ex & 0x40000) or ex & 0x08000000:  # tool window / no-activate
                    return True
                cloaked = c_uint32(0)
                if dwmapi.DwmGetWindowAttribute(hwnd, 14, ctypes.byref(cloaked), 4) == 0 and cloaked.value:
                    return True
                n = user32.GetWindowTextLengthW(hwnd)
                if n <= 0 or window_pid(hwnd) == own_pid:
                    return True
                cls = ctypes.create_unicode_buffer(64)
                user32.GetClassNameW(hwnd, cls, 64)
                if cls.value in ("Progman", "WorkerW", "Shell_TrayWnd"):
                    return True
                title = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, title, n + 1)
                out.append((hwnd, title.value))
            except OSError:
                pass
            return True

        user32.EnumWindows(WNDENUMPROC(cb), None)
        return out

    def _is_front(hwnd, wait=0.15):
        """Foreground changes can lag a little, so check for a short moment."""
        deadline = time.monotonic() + wait
        while True:
            if user32.GetForegroundWindow() == hwnd:
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.015)

    def focus_window(hwnd):
        """Bring a window to the front. Returns the method that worked, or None if blocked."""
        if not user32.IsWindow(hwnd):
            return None
        if user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, 9)  # SW_RESTORE

        user32.SetForegroundWindow(hwnd)
        if _is_front(hwnd):
            return "direct"

        # A zero-size mouse input makes us the last input source, lifting the focus lock.
        dummy = INPUT(type=0)  # INPUT_MOUSE, all zero
        user32.SendInput(1, ctypes.byref(dummy), ctypes.sizeof(INPUT))
        user32.SetForegroundWindow(hwnd)
        if _is_front(hwnd):
            return "input"

        # Borrow the foreground window's input queue.
        cur = kernel32.GetCurrentThreadId()
        fg = user32.GetForegroundWindow()
        fg_thread = user32.GetWindowThreadProcessId(fg, None) if fg else 0
        attached = bool(fg_thread and fg_thread != cur and user32.AttachThreadInput(cur, fg_thread, True))
        try:
            user32.BringWindowToTop(hwnd)
            user32.SetForegroundWindow(hwnd)
        finally:
            if attached:
                user32.AttachThreadInput(cur, fg_thread, False)
        if _is_front(hwnd):
            return "attach"

        # The call Alt+Tab itself uses.
        user32.SwitchToThisWindow(hwnd, True)
        if _is_front(hwnd, 0.3):
            return "alt-tab"
        return None

    def toggle_window(hwnd):
        """'gone', 'minimized', a method name (switched), or None (blocked)."""
        if not user32.IsWindow(hwnd):
            return "gone"
        if user32.GetForegroundWindow() == hwnd and not user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, 6)  # SW_MINIMIZE
            return "minimized"
        return focus_window(hwnd)

    class PROPERTYKEY(ctypes.Structure):
        _fields_ = [("fmtid", GUID), ("pid", ctypes.c_uint32)]

    class PROPVARIANT(ctypes.Structure):
        _fields_ = [("vt", ctypes.c_ushort), ("r1", ctypes.c_ushort), ("r2", ctypes.c_ushort),
                    ("r3", ctypes.c_ushort), ("ptr", c_void_p), ("pad", c_void_p)]

    IID_IPropertyStore = GUID("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")
    PKEY_AppUserModel_ID = PROPERTYKEY(fmtid=GUID("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), pid=5)
    CLSID_ShellLink = GUID("00021401-0000-0000-C000-000000000046")
    IID_IShellLinkW = GUID("000214F9-0000-0000-C000-000000000046")
    IID_IPersistFile = GUID("0000010B-0000-0000-C000-000000000046")
    _sig(shell32.SHGetPropertyStoreForWindow, [c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(c_void_p)],
         ctypes.c_long)
    _sig(ole32.PropVariantClear, [ctypes.POINTER(PROPVARIANT)], ctypes.c_long)
    _sig(ole32.CoCreateInstance, [ctypes.POINTER(GUID), c_void_p, c_uint32, ctypes.POINTER(GUID),
                                  ctypes.POINTER(c_void_p)], ctypes.c_long)

    def window_app_id(hwnd):
        """The app id a window carries (Chromium sets one per browser profile)."""
        store = c_void_p()
        if (shell32.SHGetPropertyStoreForWindow(hwnd, ctypes.byref(IID_IPropertyStore), ctypes.byref(store)) != 0
                or not store.value):
            return None
        try:
            pv = PROPVARIANT()
            if _vcall(store.value, 5, ctypes.c_long, [ctypes.POINTER(PROPERTYKEY), ctypes.POINTER(PROPVARIANT)],
                      ctypes.byref(PKEY_AppUserModel_ID), ctypes.byref(pv)) != 0:
                return None
            try:
                return ctypes.wstring_at(pv.ptr) if pv.vt == 31 and pv.ptr else None  # VT_LPWSTR
            finally:
                ole32.PropVariantClear(ctypes.byref(pv))
        finally:
            _vcall(store.value, 2, ctypes.c_ulong, [])

    def make_shortcut(exe, args):
        """Write a .lnk for exe + args (used to start programs as the normal user)."""
        os.makedirs(LINK_DIR, exist_ok=True)
        arg_text = args if isinstance(args, str) else subprocess.list2cmdline(list(args))
        path = os.path.join(LINK_DIR, hashlib.md5((exe + "|" + arg_text).encode("utf-8")).hexdigest()[:12] + ".lnk")
        ole32.CoInitializeEx(None, 0x2)
        link = c_void_p()
        hr = ole32.CoCreateInstance(ctypes.byref(CLSID_ShellLink), None, 1, ctypes.byref(IID_IShellLinkW),
                                    ctypes.byref(link))
        if hr != 0 or not link.value:
            raise OSError(f"could not create shortcut (0x{hr & 0xFFFFFFFF:08x})")
        try:
            _vcall(link.value, 20, ctypes.c_long, [ctypes.c_wchar_p], exe)                    # SetPath
            _vcall(link.value, 11, ctypes.c_long, [ctypes.c_wchar_p], arg_text)               # SetArguments
            _vcall(link.value, 9, ctypes.c_long, [ctypes.c_wchar_p], os.path.dirname(exe))    # SetWorkingDirectory
            pf = c_void_p()
            hr = _vcall(link.value, 0, ctypes.c_long, [ctypes.POINTER(GUID), ctypes.POINTER(c_void_p)],
                        ctypes.byref(IID_IPersistFile), ctypes.byref(pf))
            if hr != 0 or not pf.value:
                raise OSError(f"could not save shortcut (0x{hr & 0xFFFFFFFF:08x})")
            try:
                hr = _vcall(pf.value, 6, ctypes.c_long, [ctypes.c_wchar_p, ctypes.c_int], path, 1)  # Save
                if hr != 0:
                    raise OSError(f"could not save shortcut (0x{hr & 0xFFFFFFFF:08x})")
            finally:
                _vcall(pf.value, 2, ctypes.c_ulong, [])
        finally:
            _vcall(link.value, 2, ctypes.c_ulong, [])
        return path

    def window_identity(hwnd):
        """(lowercase exe path, AppUserModelID or None) of the program behind a window."""
        exe, aumid = process_info(_real_pid(hwnd))
        return (exe or "").lower(), aumid

    def resolve_app_id_path(app_id):
        """Start-menu AppID → exe path, for ids like '{KnownFolderGUID}\\Android\\...\\studio64.exe'."""
        if os.path.isabs(app_id) and os.path.isfile(app_id):
            return app_id
        if app_id.startswith("{") and "}\\" in app_id:
            guid, rest = app_id[1:].split("}\\", 1)
            try:
                folder = GUID(guid)
            except ValueError:
                return None
            ptr = c_void_p()
            if shell32.SHGetKnownFolderPath(ctypes.byref(folder), 0, None, ctypes.byref(ptr)) == 0:
                base = ctypes.wstring_at(ptr.value)
                ole32.CoTaskMemFree(ptr)
                path = os.path.join(base, rest)
                if os.path.isfile(path):
                    return path
        return None

    def launch_binding(b):
        if b["profile"]:
            PENDING_LAUNCHES[b["profile"]] = time.monotonic()
            start_program(b["exe"], b["args"] or [])
        elif b["app_id"]:
            subprocess.Popen(["explorer.exe", "shell:AppsFolder\\" + b["app_id"]])
        else:
            start_program(b["exe"])

    def activate_binding(b, wins_recent, wins_stable):
        """Switch to the app's window (cycling through several), minimize it, or start it.
        Returns (result, hwnd)."""
        wins_recent = [h for h in wins_recent if user32.IsWindow(h)]
        wins_stable = [h for h in wins_stable if user32.IsWindow(h)]
        if not wins_recent:
            launch_binding(b)
            return "launched", None
        fg = user32.GetForegroundWindow()
        if fg in wins_stable:
            if len(wins_stable) == 1:
                return toggle_window(fg), fg
            nxt = wins_stable[(wins_stable.index(fg) + 1) % len(wins_stable)]
            return focus_window(nxt), nxt
        return focus_window(wins_recent[0]), wins_recent[0]

    def process_elevated(pid):
        """True if the process runs as administrator, None if unknown."""
        h = kernel32.OpenProcess(0x1000, False, pid)
        if not h:
            return None
        try:
            tok = c_void_p()
            if not advapi32.OpenProcessToken(h, 0x8, ctypes.byref(tok)):  # TOKEN_QUERY
                return None
            try:
                val, n = c_uint32(), c_uint32()
                if advapi32.GetTokenInformation(tok, 20, ctypes.byref(val), 4, ctypes.byref(n)):  # TokenElevation
                    return bool(val.value)
                return None
            finally:
                kernel32.CloseHandle(tok)
        finally:
            kernel32.CloseHandle(h)

    def close_window(hwnd):
        user32.PostMessageW(hwnd, 0x10, None, None)  # WM_CLOSE

    # ---------------------------------------------------------- always on top, see-through, minimize, kill
    _sig(user32.GetLayeredWindowAttributes, [c_void_p, ctypes.POINTER(c_uint32), ctypes.POINTER(ctypes.c_ubyte),
                                             ctypes.POINTER(c_uint32)])
    _sig(user32.SetLayeredWindowAttributes, [c_void_p, c_uint32, ctypes.c_ubyte, c_uint32])
    _sig(kernel32.TerminateProcess, [c_void_p, ctypes.c_uint])
    _LAYERED_BY_US = set()  # windows we made layered (to undo it when they're solid again)

    def is_topmost(hwnd):
        return bool(user32.GetWindowLongW(hwnd, -20) & 0x8)  # WS_EX_TOPMOST

    def set_topmost(hwnd, on):
        """Keep a window above all others (or stop). Returns the new state."""
        user32.SetWindowPos(hwnd, c_void_p(-1 if on else -2), 0, 0, 0, 0,
                            0x0001 | 0x0002 | 0x0010)  # HWND_(NO)TOPMOST; NOSIZE | NOMOVE | NOACTIVATE
        return is_topmost(hwnd)

    def get_opacity(hwnd):
        """Opacity of a window in % (100 = solid)."""
        if not user32.GetWindowLongW(hwnd, -20) & 0x80000:  # WS_EX_LAYERED
            return 100
        key, alpha, flags = c_uint32(), ctypes.c_ubyte(), c_uint32()
        if user32.GetLayeredWindowAttributes(hwnd, ctypes.byref(key), ctypes.byref(alpha), ctypes.byref(flags)) \
                and flags.value & 0x2:  # LWA_ALPHA
            return round(alpha.value * 100 / 255)
        return 100

    def set_opacity(hwnd, pct):
        """Make a window see-through (pct < 100) or solid again. Returns the new opacity."""
        ex = user32.GetWindowLongW(hwnd, -20)
        key, alpha, flags = c_uint32(), ctypes.c_ubyte(), c_uint32()
        if ex & 0x80000:
            user32.GetLayeredWindowAttributes(hwnd, ctypes.byref(key), ctypes.byref(alpha), ctypes.byref(flags))
        keep = flags.value & 0x1  # LWA_COLORKEY the app set itself
        if pct >= 100:
            if ex & 0x80000:
                user32.SetLayeredWindowAttributes(hwnd, key.value, 255, keep | 0x2)
                if hwnd in _LAYERED_BY_US:
                    user32.SetWindowLongW(hwnd, -20, ex & ~0x80000)
                    _LAYERED_BY_US.discard(hwnd)
            return 100
        if not ex & 0x80000:
            user32.SetWindowLongW(hwnd, -20, ex | 0x80000)
            _LAYERED_BY_US.add(hwnd)
        user32.SetLayeredWindowAttributes(hwnd, key.value, max(1, min(255, round(pct * 255 / 100))), keep | 0x2)
        return get_opacity(hwnd)

    def minimize_window(hwnd, activate_next=True):
        user32.ShowWindow(hwnd, 6 if activate_next else 7)  # SW_MINIMIZE / SW_SHOWMINNOACTIVE

    def kill_window_process(hwnd):
        """End the program behind a window right away (unsaved work is lost). True if it worked."""
        pid = _real_pid(hwnd)
        if not pid or pid == os.getpid():
            return False
        h = kernel32.OpenProcess(0x0001, False, pid)  # PROCESS_TERMINATE
        if not h:
            return False
        try:
            return bool(kernel32.TerminateProcess(h, 1))
        finally:
            kernel32.CloseHandle(h)


    # ---------------------------------------------------------- per-app mute (Windows audio sessions)
    CLSID_MMDeviceEnumerator = GUID("BCDE0395-E52F-467C-8E3D-C4579291692E")
    IID_IMMDeviceEnumerator = GUID("A95664D2-9614-4F35-A746-DE8DB63617E6")
    IID_IAudioSessionManager2 = GUID("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F")
    IID_IAudioSessionControl2 = GUID("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D")
    IID_ISimpleAudioVolume = GUID("87CE5498-68D6-44E5-9215-6DA47EF883D8")
    _HR = ctypes.c_long
    _PP = ctypes.POINTER(c_void_p)

    def _com_release(p):
        if p:
            _vcall(p, 2, ctypes.c_ulong, [])

    def _audio_sessions():
        """[(pid, ISimpleAudioVolume*)] for every audio session on the active output devices.
        The caller releases the pointers (_com_release)."""
        out = []
        enum = c_void_p()
        if ole32.CoCreateInstance(ctypes.byref(CLSID_MMDeviceEnumerator), None, 0x17,
                                  ctypes.byref(IID_IMMDeviceEnumerator), ctypes.byref(enum)) != 0 or not enum.value:
            return out
        try:
            coll = c_void_p()
            # EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE)
            if _vcall(enum.value, 3, _HR, [ctypes.c_uint, c_uint32, _PP], 0, 1, ctypes.byref(coll)) != 0:
                return out
            try:
                n = c_uint32()
                _vcall(coll.value, 3, _HR, [ctypes.POINTER(c_uint32)], ctypes.byref(n))
                for i in range(n.value):
                    dev = c_void_p()
                    if _vcall(coll.value, 4, _HR, [ctypes.c_uint, _PP], i, ctypes.byref(dev)) != 0:
                        continue
                    try:
                        mgr = c_void_p()
                        if _vcall(dev.value, 3, _HR, [ctypes.POINTER(GUID), c_uint32, c_void_p, _PP],
                                  ctypes.byref(IID_IAudioSessionManager2), 0x17, None, ctypes.byref(mgr)) != 0:
                            continue
                        try:
                            se = c_void_p()
                            if _vcall(mgr.value, 5, _HR, [_PP], ctypes.byref(se)) != 0:  # GetSessionEnumerator
                                continue
                            try:
                                cnt = ctypes.c_int()
                                _vcall(se.value, 3, _HR, [ctypes.POINTER(ctypes.c_int)], ctypes.byref(cnt))
                                for j in range(cnt.value):
                                    ctl = c_void_p()
                                    if _vcall(se.value, 4, _HR, [ctypes.c_int, _PP], j, ctypes.byref(ctl)) != 0:
                                        continue
                                    try:
                                        c2, vol = c_void_p(), c_void_p()
                                        if _vcall(ctl.value, 0, _HR, [ctypes.POINTER(GUID), _PP],
                                                  ctypes.byref(IID_IAudioSessionControl2), ctypes.byref(c2)) != 0:
                                            continue
                                        try:
                                            pid = c_uint32()
                                            _vcall(c2.value, 14, _HR, [ctypes.POINTER(c_uint32)], ctypes.byref(pid))
                                        finally:
                                            _com_release(c2.value)
                                        if pid.value and _vcall(ctl.value, 0, _HR, [ctypes.POINTER(GUID), _PP],
                                                                ctypes.byref(IID_ISimpleAudioVolume),
                                                                ctypes.byref(vol)) == 0 and vol.value:
                                            out.append((pid.value, vol.value))
                                    finally:
                                        _com_release(ctl.value)
                            finally:
                                _com_release(se.value)
                        finally:
                            _com_release(mgr.value)
                    finally:
                        _com_release(dev.value)
            finally:
                _com_release(coll.value)
        finally:
            _com_release(enum.value)
        return out

    def app_mute(hwnd, mute=None):
        """Mute / unmute every sound stream of the program behind a window (mute=None: toggle; "get": only read).
        Returns (muted, number of streams); (None, 0) when the program has no sound streams."""
        target = _real_pid(hwnd)
        exe = (process_info(target)[0] or "").lower()
        sessions = _audio_sessions()
        try:
            exes, mine = {}, []
            for pid, vol in sessions:
                if pid not in exes:
                    exes[pid] = (process_info(pid)[0] or "").lower()
                if pid == target or (exe and exes[pid] == exe):  # browsers play sound from a helper process
                    mine.append(vol)
            if not mine:
                return None, 0
            cur = ctypes.c_int()
            _vcall(mine[0], 6, _HR, [ctypes.POINTER(ctypes.c_int)], ctypes.byref(cur))  # GetMute
            if mute == "get":
                return bool(cur.value), len(mine)
            if mute is None:
                mute = not cur.value
            for vol in mine:
                _vcall(vol, 5, _HR, [ctypes.c_int, c_void_p], int(bool(mute)), None)  # SetMute
            return bool(mute), len(mine)
        finally:
            for _pid, vol in sessions:
                _com_release(vol)

    def raise_with(target, others):
        """Bring windows up right behind the target (without activating them), restoring minimized ones."""
        for h in others:
            if not user32.IsWindow(h) or h == target:
                continue
            if user32.IsIconic(h):
                user32.ShowWindow(h, 4)  # SW_SHOWNOACTIVATE
            user32.SetWindowPos(h, c_void_p(target), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)

    def send_mask_key():
        _send_mask_key()

    # ---------------------------------------------------------- typed text (snippets)
    _sig(user32.ToUnicodeEx, [ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_wchar_p,
                              ctypes.c_int, ctypes.c_uint, c_void_p])
    _sig(user32.GetKeyboardLayout, [c_uint32], c_void_p)
    _sig(user32.GetKeyState, [ctypes.c_int], ctypes.c_short)

    def typed_text(vk, sc):
        """The character(s) a key press types in the foreground window, without disturbing dead keys."""
        state = (ctypes.c_ubyte * 256)()
        for v in (0x10, 0xA0, 0xA1, 0x11, 0xA2, 0xA3, 0x12, 0xA4, 0xA5):
            if user32.GetAsyncKeyState(v) & 0x8000:
                state[v] = 0x80
        if user32.GetKeyState(0x14) & 1:  # Caps Lock on
            state[0x14] = 1
        fg = user32.GetForegroundWindow()
        layout = user32.GetKeyboardLayout(user32.GetWindowThreadProcessId(fg, None) if fg else 0)
        buf = ctypes.create_unicode_buffer(8)
        n = user32.ToUnicodeEx(vk, sc, state, buf, 8, 0x4, layout)  # 0x4: keep the keyboard state as it is
        return buf.value[:n] if n > 0 else ""

    def send_text(backspaces, text, left=0):
        """Type text into the focused window: Backspace × n, the text (any language), then ← × left."""
        events = []

        def key(vk):
            events.extend(((vk, 0, 0), (vk, 0, 0x2)))
        for _ in range(backspaces):
            key(0x08)
        for ch in text:
            if ch == "\r":
                continue
            if ch == "\n":
                key(0x0D)
            elif ch == "\t":
                key(0x09)
            else:
                data = ch.encode("utf-16-le")
                for unit in struct.unpack(f"<{len(data) // 2}H", data):
                    events.extend(((0, unit, 0x4), (0, unit, 0x4 | 0x2)))  # KEYEVENTF_UNICODE
        for _ in range(left):
            key(0x25)
        for start in range(0, len(events), 300):
            chunk = events[start:start + 300]
            arr = (INPUT * len(chunk))()
            for i, (vk, scan, flags) in enumerate(chunk):
                arr[i].type = 1
                arr[i].u.ki = KEYBDINPUT(wVk=vk, wScan=scan, dwFlags=flags)
            user32.SendInput(len(chunk), arr, ctypes.sizeof(INPUT))
            if start + 300 < len(events):
                time.sleep(0.01)

    # ---------------------------------------------------------- hotkeys

    class KBDLLHOOKSTRUCT(ctypes.Structure):
        _fields_ = [("vkCode", ctypes.c_uint32), ("scanCode", ctypes.c_uint32),
                    ("flags", ctypes.c_uint32), ("time", ctypes.c_uint32),
                    ("dwExtraInfo", ctypes.c_size_t)]

    HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int, ctypes.c_size_t, c_void_p)

    def _modifiers_match(extra_shift=False):
        """Exactly the configured modifiers are held (so AltGr = Ctrl+Alt doesn't count as Alt)."""
        def down(*vks):
            return any(user32.GetAsyncKeyState(v) & 0x8000 for v in vks)
        wanted = MODIFIERS | {"shift"} if extra_shift else MODIFIERS
        state = {"alt": down(0x12), "ctrl": down(0x11), "shift": down(0x10), "win": down(0x5B, 0x5C)}
        return all(state[m] == (m in wanted) for m in state)

    def _send_mask_key():
        """Tap an unused key so releasing Alt doesn't open the target app's menu bar."""
        events = (INPUT * 2)()
        for i, flags in enumerate((0, 0x2)):  # down, KEYEVENTF_KEYUP
            events[i].type = 1  # INPUT_KEYBOARD
            events[i].u.ki = KEYBDINPUT(wVk=0xE8, dwFlags=flags)
        user32.SendInput(2, events, ctypes.sizeof(INPUT))

    class HotkeyThread(threading.Thread):
        """Watches the keyboard for Alt+1 … Alt+0 (top row or numpad) and Alt+Q … Alt+P."""

        def __init__(self, get_slots, report, on_command):
            super().__init__(daemon=True)
            self.on_command = on_command
            self.get_slots = get_slots
            self.report = report
            self.error = None
            self.failed = []
            self.thread_id = None
            self.ready = threading.Event()
            self.jobs = queue.Queue()
            self.held = set()
            self.capture = {}  # while cycling: plain Enter / Esc / Del / M → command (nothing else is touched)
            self.typed, self.typed_fg = "", None  # what was typed lately in the focused window (snippets)
            self.mods_down = set()   # modifier keys held right now (cheat sheet)
            self.mod_alone = False   # …and nothing else was pressed since the first one went down
            self.cheat_visible = False
            self.snip_swallowed = set()
            self._proc = HOOKPROC(self._hook)  # keep a reference so it isn't garbage-collected
            threading.Thread(target=self._worker, daemon=True).start()

        # Runs for every key press on the system: must stay tiny and fast.
        def _hook(self, n_code, w_param, l_param):
            try:
                if n_code == 0:
                    kb = ctypes.cast(l_param, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
                    vk, sc = kb.vkCode, kb.scanCode
                    action = None
                    if SETTINGS.get("cheat_ms") and not kb.flags & 0x10:
                        self._track_modifiers(w_param, vk)
                    capture = self.capture
                    if capture and vk in capture and not kb.flags & 0x10:
                        if w_param in (0x101, 0x105):  # key up
                            if vk in self.held:
                                self.held.discard(vk)
                                return 1
                        elif vk not in self.held:
                            self.held.add(vk)
                            self.jobs.put(("cmd", capture[vk]))
                            return 1
                        else:
                            return 1
                    shifted = False
                    if vk in CMD_BY_VK_SHIFT and user32.GetAsyncKeyState(0x10) & 0x8000:
                        action, shifted = ("cmd", CMD_BY_VK_SHIFT[vk]), True
                    elif vk in CMD_BY_VK:
                        action = ("cmd", CMD_BY_VK[vk])
                    elif vk in NUMPAD_SLOT:
                        action = ("slot", NUMPAD_SLOT[vk])
                    elif sc in SLOT_BY_SC and not kb.flags & 0x1:
                        slot = SLOT_BY_SC[sc]
                        if slot in ALWAYS_SLOTS or slot in self.get_slots() or vk in self.held:
                            action = ("slot", slot)               # else leave the letter to the app
                    if action is not None and not kb.flags & 0x10:  # ignore injected keys
                        if w_param in (0x101, 0x105):  # key up
                            if vk in self.held:
                                self.held.discard(vk)
                                return 1
                        elif _modifiers_match(shifted):
                            if vk not in self.held:  # ignore auto-repeat
                                self.held.add(vk)
                                _send_mask_key()
                                self.jobs.put(action)
                            self.typed = ""
                            return 1  # swallow: the focused app doesn't see Alt+digit
                    if SNIPPET_TRIGGERS and not kb.flags & 0x10 and self._snippet_key(w_param, vk, sc):
                        return 1
            except Exception:
                log("hook error:\n" + traceback.format_exc())
            return user32.CallNextHookEx(None, n_code, w_param, l_param)

        CHEAT_MOD_VKS = {0x12, 0xA4, 0xA5, 0x11, 0xA2, 0xA3, 0x10, 0xA0, 0xA1, 0x5B, 0x5C}

        def _track_modifiers(self, w_param, vk):
            """Hold the modifier alone → the app shows the cheat sheet; any other key or letting go hides it."""
            down = w_param in (0x100, 0x104)
            if vk in self.CHEAT_MOD_VKS:
                if down:
                    if vk not in self.mods_down:
                        if not self.mods_down:
                            self.mod_alone = True
                        self.mods_down.add(vk)
                        self.jobs.put(("cmd", "cheat_arm"))
                else:
                    self.mods_down.discard(vk)
                    self.jobs.put(("cmd", "cheat_hide"))
            elif down:
                self.mod_alone = False
                if self.cheat_visible:
                    self.jobs.put(("cmd", "cheat_hide"))

        # Snippets: keep the last characters typed; a trigger at the end is replaced by its text.
        RESET_VKS = {0x0D, 0x09, 0x1B, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E}
        MOD_VKS = {0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x14, 0x5B, 0x5C, 0xE8}

        def _snippet_key(self, w_param, vk, sc):
            """True = this key press completes a trigger: it's swallowed and the worker types the snippet."""
            if w_param in (0x101, 0x105):  # key up
                if vk in self.snip_swallowed:
                    self.snip_swallowed.discard(vk)
                    return True
                return False
            if vk in self.MOD_VKS:
                return False
            fg = user32.GetForegroundWindow()
            if fg != self.typed_fg:
                self.typed, self.typed_fg = "", fg
            if fg and fg == SNIPPET_PAUSE_HWND:  # typing in the snippets editor itself
                return False
            if w_param == 0x104 or vk in self.RESET_VKS:  # Alt+key, Enter, Tab, arrows…
                self.typed = ""
                return False
            if vk == 0x08:  # Backspace
                self.typed = self.typed[:-1]
                return False
            ctrl = user32.GetAsyncKeyState(0x11) & 0x8000
            alt = user32.GetAsyncKeyState(0x12) & 0x8000
            win = user32.GetAsyncKeyState(0x5B) & 0x8000 or user32.GetAsyncKeyState(0x5C) & 0x8000
            if (ctrl and not alt) or win:  # Ctrl+C, Win+… : shortcuts, not typing
                self.typed = ""
                return False
            ch = typed_text(vk, sc)
            if not ch:
                return False
            self.typed = (self.typed + ch)[-80:]
            trig = match_trigger(self.typed, SNIPPET_TRIGGERS)
            if not trig:
                return False
            self.typed = ""
            self.snip_swallowed.add(vk)
            self.jobs.put(("snippet", trig))
            return True

        def _worker(self):
            while True:
                kind, idx = self.jobs.get()
                if kind == "cmd":
                    self.on_command(idx)
                    continue
                if kind == "snippet":
                    try:
                        text = SNIPPET_TRIGGERS.get(idx)
                        if text is not None:
                            clip = read_clipboard_text(None) if "{clipboard" in text.lower() else None
                            out, left = expand_snippet(text, clipboard=clip)
                            send_text(len(idx) - 1, out, left)  # the last key of the trigger was never typed
                            log(f"snippet {idx!r} → {len(out)} characters")
                    except Exception:
                        log("snippet error:\n" + traceback.format_exc())
                    continue
                key = slot_label(idx)
                try:
                    entry = self.get_slots().get(idx)
                    log(f"{key} pressed; slot={entry[0] if entry else None}")
                    if entry is None:
                        self.report(key, None, "none")
                    elif entry[0] == "window":
                        self.report(key, entry[1], toggle_window(entry[1]))
                    elif entry[1].get("workspace"):
                        self.on_command(("workspace", entry[1]["workspace"]))
                    else:
                        b = entry[1]
                        result, hwnd = activate_binding(b, entry[2], entry[3])
                        self.report(key, hwnd, result, b["name"])
                except Exception:
                    log("switch error:\n" + traceback.format_exc())
                    self.report(key, None, "error")

        def run(self):
            try:
                self._run()
            except Exception:
                self.error = traceback.format_exc()
                log("keyboard watcher crashed:\n" + self.error)
            finally:
                self.ready.set()

        def _run(self):
            self.thread_id = kernel32.GetCurrentThreadId()
            hook = user32.SetWindowsHookExW(13, ctypes.cast(self._proc, c_void_p),  # WH_KEYBOARD_LL
                                            kernel32.GetModuleHandleW(None), 0)
            if not hook:
                raise OSError(f"SetWindowsHookExW failed, error {kernel32.GetLastError()}")
            log("keyboard watcher running")
            self.ready.set()
            msg = wintypes.MSG()
            try:
                while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
                    user32.TranslateMessage(ctypes.byref(msg))
                    user32.DispatchMessageW(ctypes.byref(msg))
            finally:
                user32.UnhookWindowsHookEx(hook)

        def stop(self):
            if self.thread_id:
                user32.PostThreadMessageW(self.thread_id, 0x12, None, None)  # WM_QUIT

    # ---------------------------------------------------------- single instance
    k32e = ctypes.WinDLL("kernel32", use_last_error=True)
    _sig(k32e.CreateMutexW, [c_void_p, ctypes.c_bool, ctypes.c_wchar_p], c_void_p)
    _sig(k32e.CreateEventW, [c_void_p, ctypes.c_bool, ctypes.c_bool, ctypes.c_wchar_p], c_void_p)
    _sig(k32e.OpenEventW, [c_uint32, ctypes.c_bool, ctypes.c_wchar_p], c_void_p)
    _sig(k32e.SetEvent, [c_void_p])
    _sig(k32e.WaitForSingleObject, [c_void_p, c_uint32], c_uint32)
    _sig(user32.MessageBoxW, [c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint])
    INSTANCE_MUTEX = "Local\\WindowManager.Instance"
    SHOW_EVENT = "Local\\WindowManager.Show"

    def claim_single_instance():
        """True if this is the only launcher running. Otherwise ask the running one to show itself."""
        global _INSTANCE_HANDLE
        _INSTANCE_HANDLE = k32e.CreateMutexW(None, False, INSTANCE_MUTEX)
        err = ctypes.get_last_error()
        if _INSTANCE_HANDLE and err != 183:  # ERROR_ALREADY_EXISTS
            return True
        ev = k32e.OpenEventW(0x2, False, SHOW_EVENT)  # EVENT_MODIFY_STATE
        if ev:
            k32e.SetEvent(ev)
            kernel32.CloseHandle(ev)
        else:  # e.g. the running one is elevated and this one isn't
            user32.MessageBoxW(None, "Window Manager is already running (look for its icon in the tray).",
                               "Window Manager", 0x40)
        return False

    def watch_show_requests(callback):
        """Call callback() whenever a second copy of the launcher is started."""
        ev = k32e.CreateEventW(None, False, False, SHOW_EVENT)
        if not ev:
            return

        def loop():
            while k32e.WaitForSingleObject(ev, 0xFFFFFFFF) == 0:
                callback()
        threading.Thread(target=loop, daemon=True).start()

    # ---------------------------------------------------------- start with Windows
    RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
    AUTOSTART_NAME = "Window Manager"
    LEGACY_AUTOSTART = "WinApp Launcher"  # name used by earlier versions

    def autostart_command():
        """(program, arguments) that start the launcher hidden in the tray."""
        if getattr(sys, "frozen", False):
            return sys.executable, "--hidden"
        pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
        return (pyw if os.path.isfile(pyw) else sys.executable), f'"{os.path.abspath(__file__)}" --hidden'

    def _schtasks(*args):
        return subprocess.run(["schtasks", *args], capture_output=True, text=True, errors="replace",
                              creationflags=CREATE_NO_WINDOW)

    def autostart_state(name=None):
        """'task' (starts as administrator), 'run' (starts normally) or None."""
        name = name or AUTOSTART_NAME
        try:
            if _schtasks("/Query", "/TN", name).returncode == 0:
                return "task"
        except OSError:
            pass
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
                winreg.QueryValueEx(k, name)
                return "run"
        except OSError:
            return None

    def _remove_legacy_autostart():
        """Earlier versions registered start-up as "WinApp Launcher" (pointing to winapp.py)."""
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
                winreg.DeleteValue(k, LEGACY_AUTOSTART)
        except OSError:
            pass
        try:
            if _schtasks("/Query", "/TN", LEGACY_AUTOSTART).returncode == 0:
                _schtasks("/Delete", "/TN", LEGACY_AUTOSTART, "/F")
        except OSError:
            pass

    def set_autostart(on):
        """Turn starting with Windows on/off. Returns a message for the user."""
        _remove_legacy_autostart()
        state = autostart_state()
        if state == "run" or not on:
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
                    winreg.DeleteValue(k, AUTOSTART_NAME)
            except OSError:
                pass
        if state == "task" and (not on or not LAUNCHER_ELEVATED):
            r = _schtasks("/Delete", "/TN", AUTOSTART_NAME, "/F")
            if r.returncode != 0:
                return "Couldn't remove the start-up task. Run the launcher as administrator and try again."
        if not on:
            return "Window Manager won't start with Windows any more."
        exe, args = autostart_command()
        if LAUNCHER_ELEVATED:
            # A scheduled task keeps administrator rights at sign-in (a Run entry would start it without them).
            user = f"{os.environ.get('USERDOMAIN', '')}\\{os.environ.get('USERNAME', '')}".strip("\\")
            xml = f"""<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo><Description>Starts Window Manager when you sign in.</Description></RegistrationInfo>
  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>{xml_escape(user)}</UserId><Delay>PT3S</Delay></LogonTrigger></Triggers>
  <Principals><Principal id="Author"><UserId>{xml_escape(user)}</UserId><LogonType>InteractiveToken</LogonType>
    <RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>false</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled><Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>4</Priority>
  </Settings>
  <Actions Context="Author"><Exec><Command>{xml_escape(exe)}</Command><Arguments>{xml_escape(args)}</Arguments>
    <WorkingDirectory>{xml_escape(APP_DIR)}</WorkingDirectory></Exec></Actions>
</Task>
"""
            path = os.path.join(DATA_DIR, "autostart-task.xml")
            os.makedirs(DATA_DIR, exist_ok=True)
            with open(path, "w", encoding="utf-16") as f:
                f.write(xml)
            try:
                r = _schtasks("/Create", "/TN", AUTOSTART_NAME, "/XML", path, "/F")
            finally:
                try:
                    os.remove(path)
                except OSError:
                    pass
            if r.returncode != 0:
                log("schtasks failed: " + (r.stderr or r.stdout))
                return "Couldn't create the start-up task: " + (r.stderr or r.stdout).strip()
            return "Window Manager will start with Windows (as administrator, hidden in the tray)."
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
            winreg.SetValueEx(k, AUTOSTART_NAME, 0, winreg.REG_SZ, f'"{exe}" {args}')
        return "Window Manager will start with Windows (hidden in the tray)."

    # ---------------------------------------------------------- clipboard
    _sig(user32.OpenClipboard, [c_void_p])
    _sig(user32.CloseClipboard, [])
    _sig(user32.EmptyClipboard, [])
    _sig(user32.GetClipboardData, [ctypes.c_uint], c_void_p)
    _sig(user32.SetClipboardData, [ctypes.c_uint, c_void_p], c_void_p)
    _sig(user32.IsClipboardFormatAvailable, [ctypes.c_uint])
    _sig(user32.RegisterClipboardFormatW, [ctypes.c_wchar_p], ctypes.c_uint)
    _sig(user32.AddClipboardFormatListener, [c_void_p])
    _sig(user32.RemoveClipboardFormatListener, [c_void_p])
    _sig(kernel32.GlobalAlloc, [ctypes.c_uint, ctypes.c_size_t], c_void_p)
    _sig(kernel32.GlobalLock, [c_void_p], c_void_p)
    _sig(kernel32.GlobalUnlock, [c_void_p])
    _sig(kernel32.GlobalFree, [c_void_p], c_void_p)
    _sig(kernel32.GlobalSize, [c_void_p], ctypes.c_size_t)
    CF_UNICODETEXT = 13
    # Formats password managers and other apps add to say "don't keep this in a clipboard history".
    CF_EXCLUDE = [user32.RegisterClipboardFormatW(n) for n in
                  ("ExcludeClipboardContentFromMonitorProcessing", "Clipboard Viewer Ignore")]
    CF_CAN_INCLUDE = user32.RegisterClipboardFormatW("CanIncludeInClipboardHistory")
    CF_CLOUD_EXCLUDE = user32.RegisterClipboardFormatW("CanUploadToCloudClipboard")

    def _open_clipboard(hwnd, tries=10):
        for _ in range(tries):
            if user32.OpenClipboard(hwnd):
                return True
            time.sleep(0.025)
        return False

    def read_clipboard_text(hwnd):
        """Text on the clipboard, or None (no text, busy, or marked private)."""
        if not _open_clipboard(hwnd):
            return None
        try:
            if any(f and user32.IsClipboardFormatAvailable(f) for f in CF_EXCLUDE):
                return None
            if CF_CAN_INCLUDE and user32.IsClipboardFormatAvailable(CF_CAN_INCLUDE):
                h = user32.GetClipboardData(CF_CAN_INCLUDE)
                p = kernel32.GlobalLock(h) if h else None
                if p:
                    allowed = ctypes.c_uint32.from_address(p).value
                    kernel32.GlobalUnlock(h)
                    if allowed == 0:
                        return None
            if not user32.IsClipboardFormatAvailable(CF_UNICODETEXT):
                return None
            h = user32.GetClipboardData(CF_UNICODETEXT)
            if not h:
                return None
            p = kernel32.GlobalLock(h)
            if not p:
                return None
            try:
                n = kernel32.GlobalSize(h) // 2
                return ctypes.wstring_at(p, n).split("\0", 1)[0]
            finally:
                kernel32.GlobalUnlock(h)
        finally:
            user32.CloseClipboard()

    def set_clipboard_text(text, hwnd):
        buf = ctypes.create_unicode_buffer(text)
        size = ctypes.sizeof(buf)
        if not _open_clipboard(hwnd):
            raise OSError("the clipboard is busy")
        try:
            user32.EmptyClipboard()
            h = kernel32.GlobalAlloc(0x0002, size)  # GMEM_MOVEABLE
            p = kernel32.GlobalLock(h)
            ctypes.memmove(p, buf, size)
            kernel32.GlobalUnlock(h)
            if not user32.SetClipboardData(CF_UNICODETEXT, h):
                kernel32.GlobalFree(h)
                raise OSError("couldn't set the clipboard")
        finally:
            user32.CloseClipboard()

    class DATA_BLOB(ctypes.Structure):
        _fields_ = [("cbData", c_uint32), ("pbData", c_void_p)]

    crypt32 = ctypes.windll.crypt32
    _sig(crypt32.CryptProtectData, [ctypes.POINTER(DATA_BLOB), ctypes.c_wchar_p, ctypes.POINTER(DATA_BLOB),
                                    c_void_p, c_void_p, c_uint32, ctypes.POINTER(DATA_BLOB)])
    _sig(crypt32.CryptUnprotectData, [ctypes.POINTER(DATA_BLOB), c_void_p, ctypes.POINTER(DATA_BLOB),
                                      c_void_p, c_void_p, c_uint32, ctypes.POINTER(DATA_BLOB)])
    _sig(kernel32.LocalFree, [c_void_p], c_void_p)
    DPAPI_ENTROPY = b"applauncher-clipboard-v1"

    def _blob(data):
        buf = ctypes.create_string_buffer(data, len(data))
        return DATA_BLOB(len(data), ctypes.addressof(buf)), buf

    def _dpapi(fn, data, *desc):
        src, _keep1 = _blob(data)
        ent, _keep2 = _blob(DPAPI_ENTROPY)
        out = DATA_BLOB()
        if not fn(ctypes.byref(src), *(desc or (None,)), ctypes.byref(ent), None, None, 0x1, ctypes.byref(out)):
            raise ValueError("Windows couldn't encrypt/decrypt this item")
        try:
            return ctypes.string_at(out.pbData, out.cbData)
        finally:
            kernel32.LocalFree(out.pbData)

    def dpapi_protect(data):
        """Encrypt bytes so only this Windows account can read them (CryptProtectData)."""
        return _dpapi(crypt32.CryptProtectData, data, "clipboard history")

    def dpapi_unprotect(data):
        return _dpapi(crypt32.CryptUnprotectData, data)

    def send_ctrl_v():
        events = (INPUT * 4)()
        for i, (vk, up) in enumerate(((0x11, 0), (0x56, 0), (0x56, 0x2), (0x11, 0x2))):
            events[i].type = 1
            events[i].u.ki = KEYBDINPUT(wVk=vk, dwFlags=up)
        user32.SendInput(4, events, ctypes.sizeof(INPUT))

    # ---------------------------------------------------------- monitors & window placement
    class RECT(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long), ("right", ctypes.c_long),
                    ("bottom", ctypes.c_long)]

    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", c_uint32), ("rcMonitor", RECT), ("rcWork", RECT), ("dwFlags", c_uint32)]

    MONITORENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, c_void_p, c_void_p, ctypes.POINTER(RECT), ctypes.c_ssize_t)
    _sig(user32.EnumDisplayMonitors, [c_void_p, c_void_p, MONITORENUMPROC, ctypes.c_ssize_t])
    _sig(user32.GetMonitorInfoW, [c_void_p, ctypes.POINTER(MONITORINFO)])
    _sig(user32.MonitorFromWindow, [c_void_p, c_uint32], c_void_p)
    _sig(user32.GetWindowRect, [c_void_p, ctypes.POINTER(RECT)])
    _sig(user32.SetWindowPos, [c_void_p, c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                               ctypes.c_uint])
    _sig(user32.IsZoomed, [c_void_p])

    def list_monitors():
        """[(work area (l, t, r, b), handle, is_main)], main monitor first, then left to right."""
        found = []

        def cb(hmon, _hdc, _rect, _data):
            mi = MONITORINFO(cbSize=ctypes.sizeof(MONITORINFO))
            if user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
                w = mi.rcWork
                found.append(((w.left, w.top, w.right, w.bottom), hmon, bool(mi.dwFlags & 1)))
            return True

        user32.EnumDisplayMonitors(None, None, MONITORENUMPROC(cb), 0)
        found.sort(key=lambda m: (not m[2], m[0][0], m[0][1]))
        return found

    def _frame_borders(hwnd):
        """Invisible resize borders around a window (Windows 10/11 add them outside the visible frame)."""
        wr, fr = RECT(), RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(wr))
        if dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(fr), ctypes.sizeof(fr)) != 0:
            return 0, 0, 0, 0
        return fr.left - wr.left, fr.top - wr.top, wr.right - fr.right, wr.bottom - fr.bottom

    def window_rect(hwnd):
        """The visible rectangle of a window (without the invisible resize borders): (l, t, r, b)."""
        r = RECT()
        if dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
            user32.GetWindowRect(hwnd, ctypes.byref(r))
        return r.left, r.top, r.right, r.bottom

    def window_placement_now(hwnd):
        """Where a window is: (pos, monitor index, visible rect [x, y, w, h])."""
        mons = list_monitors()
        hmon = user32.MonitorFromWindow(hwnd, 2)
        idx = next((i for i, m in enumerate(mons) if m[1] == hmon), 0)
        if user32.IsZoomed(hwnd):
            return "max", idx, None
        fr = RECT()
        if dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(fr), ctypes.sizeof(fr)) != 0:
            user32.GetWindowRect(hwnd, ctypes.byref(fr))
        return "saved", idx, [fr.left, fr.top, fr.right - fr.left, fr.bottom - fr.top]

    def place_window(hwnd, place):
        """Move a window to a position on a monitor (see WS_POSITIONS)."""
        pos = (place or {}).get("pos") or "none"
        if pos == "none" or not user32.IsWindow(hwnd):
            return
        mons = list_monitors()
        if not mons:
            return
        (l, t, r, b), _h, _main = mons[min(max(int(place.get("monitor") or 0), 0), len(mons) - 1)]
        W, H = r - l, b - t
        if user32.IsIconic(hwnd) or user32.IsZoomed(hwnd):
            user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        flags = 0x0004 | 0x0010  # SWP_NOZORDER | SWP_NOACTIVATE
        if pos == "max":
            user32.SetWindowPos(hwnd, None, l + W // 4, t + H // 4, W // 2, H // 2, flags)
            user32.ShowWindow(hwnd, 3)  # SW_MAXIMIZE
            return
        if pos == "saved" and place.get("rect"):
            x, y, w, h = place["rect"]
        else:
            fx, fy, fw, fh = WS_POSITION_RECTS.get(pos, WS_POSITION_RECTS["center"])
            x, y = l + round(W * fx), t + round(H * fy)
            w, h = round(W * fw), round(H * fh)
        bl, bt, br, bb = _frame_borders(hwnd)
        user32.SetWindowPos(hwnd, None, x - bl, y - bt, w + bl + br, h + bt + bb, flags)

    # ---------------------------------------------------------- system commands
    _sig(user32.LockWorkStation, [])
    _sig(shell32.SHEmptyRecycleBinW, [c_void_p, ctypes.c_wchar_p, c_uint32], ctypes.c_long)

    def system_sleep():
        ctypes.windll.powrprof.SetSuspendState(False, False, False)

    # ---------------------------------------------------------- tray icon + clipboard listener
    WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t)

    class WNDCLASSW(ctypes.Structure):
        _fields_ = [("style", ctypes.c_uint), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                    ("cbWndExtra", ctypes.c_int), ("hInstance", c_void_p), ("hIcon", c_void_p),
                    ("hCursor", c_void_p), ("hbrBackground", c_void_p), ("lpszMenuName", ctypes.c_wchar_p),
                    ("lpszClassName", ctypes.c_wchar_p)]

    class NOTIFYICONDATAW(ctypes.Structure):
        _fields_ = [("cbSize", c_uint32), ("hWnd", c_void_p), ("uID", ctypes.c_uint), ("uFlags", ctypes.c_uint),
                    ("uCallbackMessage", ctypes.c_uint), ("hIcon", c_void_p), ("szTip", ctypes.c_wchar * 128),
                    ("dwState", c_uint32), ("dwStateMask", c_uint32), ("szInfo", ctypes.c_wchar * 256),
                    ("uVersion", ctypes.c_uint), ("szInfoTitle", ctypes.c_wchar * 64), ("dwInfoFlags", c_uint32),
                    ("guidItem", GUID), ("hBalloonIcon", c_void_p)]

    class ICONINFO(ctypes.Structure):
        _fields_ = [("fIcon", ctypes.c_int), ("xHotspot", c_uint32), ("yHotspot", c_uint32),
                    ("hbmMask", c_void_p), ("hbmColor", c_void_p)]

    class POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

    _sig(user32.RegisterClassW, [ctypes.POINTER(WNDCLASSW)], ctypes.c_ushort)
    _sig(user32.CreateWindowExW, [c_uint32, ctypes.c_wchar_p, ctypes.c_wchar_p, c_uint32, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_int, c_void_p, c_void_p, c_void_p,
                                  c_void_p], c_void_p)
    _sig(user32.DefWindowProcW, [c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t], ctypes.c_ssize_t)
    _sig(user32.DestroyWindow, [c_void_p])
    _sig(user32.PostQuitMessage, [ctypes.c_int], None)
    _sig(user32.RegisterWindowMessageW, [ctypes.c_wchar_p], ctypes.c_uint)
    _sig(user32.CreatePopupMenu, [], c_void_p)
    _sig(user32.AppendMenuW, [c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_wchar_p])
    _sig(user32.TrackPopupMenu, [c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, c_void_p,
                                 c_void_p])
    _sig(user32.DestroyMenu, [c_void_p])
    _sig(user32.GetCursorPos, [ctypes.POINTER(POINT)])
    _sig(user32.GetSystemMetrics, [ctypes.c_int])
    _sig(user32.CreateIconIndirect, [ctypes.POINTER(ICONINFO)], c_void_p)
    _sig(user32.DestroyIcon, [c_void_p])
    _sig(gdi32.CreateBitmap, [ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, c_void_p], c_void_p)
    _sig(shell32.Shell_NotifyIconW, [c_uint32, ctypes.POINTER(NOTIFYICONDATAW)])

    def make_hicon(size):
        rgba = make_logo_rgba(size)
        bgra = bytearray(rgba)
        bgra[0::4], bgra[2::4] = rgba[2::4], rgba[0::4]
        bi = BITMAPINFOHEADER(biSize=ctypes.sizeof(BITMAPINFOHEADER), biWidth=size, biHeight=-size,
                              biPlanes=1, biBitCount=32)
        bits = c_void_p()
        color = gdi32.CreateDIBSection(None, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
        ctypes.memmove(bits.value, bytes(bgra), len(bgra))
        mask = gdi32.CreateBitmap(size, size, 1, 1, None)
        info = ICONINFO(fIcon=True, hbmMask=mask, hbmColor=color)
        hicon = user32.CreateIconIndirect(ctypes.byref(info))
        gdi32.DeleteObject(color)
        gdi32.DeleteObject(mask)
        return hicon

    class ShellThread(threading.Thread):
        """A hidden window that owns the tray icon and listens for clipboard changes."""
        WM_TRAY = 0x8001
        MENU = [(1, "Show launcher"), (2, "Notes"), (3, "Clipboard history"), (10, "Snippets…"),
                (4, "Workspaces…"), (11, "Restore last session…"), (0, None),
                (5, "Settings (settings.ini)"), (6, "Start with Windows"), (0, None), (9, "Quit")]
        MENU_CMDS = {1: "toggle", 2: "notes", 3: "clipboard", 4: "workspaces", 5: "settings",
                     6: "autostart", 9: "quit", 10: "snippets", 11: "restore"}

        def __init__(self, post, on_clip, listen_clipboard):
            super().__init__(daemon=True)
            self.post, self.on_clip, self.listen_clipboard = post, on_clip, listen_clipboard
            self.hwnd = None
            self.hicon = None
            self.error = None
            self.ready = threading.Event()
            self.icon_added = False
            self.autostart_on = False  # shown as a check mark in the menu; refreshed by the app
            self._proc = WNDPROC(self._wndproc)
            self.msg_taskbar = 0
            self._last_text = None

        def run(self):
            try:
                self._run()
            except Exception:
                self.error = traceback.format_exc()
                log("tray thread crashed:\n" + self.error)
            finally:
                self.ready.set()

        def _nid(self):
            nid = NOTIFYICONDATAW()
            nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
            nid.hWnd, nid.uID = self.hwnd, 1
            return nid

        def _add_icon(self):
            nid = self._nid()
            nid.uFlags = 0x1 | 0x2 | 0x4  # NIF_MESSAGE | NIF_ICON | NIF_TIP
            nid.uCallbackMessage, nid.hIcon = self.WM_TRAY, self.hicon
            nid.szTip = f"Window Manager — {MOD_LABEL}{CMD_KEYS.get('toggle', '.')} to show"
            self.icon_added = bool(shell32.Shell_NotifyIconW(0, ctypes.byref(nid)))  # NIM_ADD

        def balloon(self, title, text, sound=True):
            if not self.icon_added:
                return
            nid = self._nid()
            nid.uFlags = 0x10  # NIF_INFO
            nid.szInfoTitle, nid.szInfo = title[:63], text[:255]
            nid.dwInfoFlags = 0x4 | 0x20 | (0 if sound else 0x10)  # USER | LARGE_ICON (| NOSOUND)
            nid.hBalloonIcon = self.hicon
            shell32.Shell_NotifyIconW(1, ctypes.byref(nid))  # NIM_MODIFY

        def _run(self):
            hinst = kernel32.GetModuleHandleW(None)
            wc = WNDCLASSW(lpfnWndProc=self._proc, hInstance=hinst, lpszClassName="WindowManagerShell")
            user32.RegisterClassW(ctypes.byref(wc))
            self.hwnd = user32.CreateWindowExW(0x80, "WindowManagerShell", "Window Manager (tray)", 0,
                                               0, 0, 0, 0, None, None, hinst, None)
            if not self.hwnd:
                raise OSError("couldn't create the tray window")
            self.msg_taskbar = user32.RegisterWindowMessageW("TaskbarCreated")
            try:  # hear about shutdown early, before other apps close (keeps the session list complete)
                kernel32.SetProcessShutdownParameters(0x3FF, 0)
            except Exception:
                pass
            size = user32.GetSystemMetrics(49) or 16  # SM_CXSMICON
            self.hicon = make_hicon(max(16, size))
            self._add_icon()
            if self.listen_clipboard:
                user32.AddClipboardFormatListener(self.hwnd)
            log(f"tray ready (icon={self.icon_added}, clipboard={self.listen_clipboard})")
            self.ready.set()
            msg = wintypes.MSG()
            while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
                user32.TranslateMessage(ctypes.byref(msg))
                user32.DispatchMessageW(ctypes.byref(msg))

        def _wndproc(self, hwnd, msg, wparam, lparam):
            try:
                if msg == self.WM_TRAY:
                    ev = lparam & 0xFFFF
                    if ev == 0x202:  # WM_LBUTTONUP
                        self.post("toggle")
                    elif ev in (0x205, 0x7B):  # WM_RBUTTONUP, WM_CONTEXTMENU
                        self._menu()
                    return 0
                if msg == 0x031D:  # WM_CLIPBOARDUPDATE
                    text = read_clipboard_text(self.hwnd)
                    if text and text.strip():
                        self.on_clip(text)
                    return 0
                if msg == 0x0011:  # WM_QUERYENDSESSION: shutting down / restarting → freeze the session list
                    global SESSION_FROZEN
                    SESSION_FROZEN = True
                    log("Windows is shutting down: session list kept")
                    return 1
                if msg == 0x0016 and not wparam:  # WM_ENDSESSION(False): the shutdown was cancelled
                    SESSION_FROZEN = False
                    return 0
                if msg == self.msg_taskbar and self.msg_taskbar:  # Explorer restarted
                    self._add_icon()
                    return 0
                if msg == 0x0002:  # WM_DESTROY
                    if self.icon_added:
                        shell32.Shell_NotifyIconW(2, ctypes.byref(self._nid()))  # NIM_DELETE
                        self.icon_added = False
                    if self.listen_clipboard:
                        user32.RemoveClipboardFormatListener(hwnd)
                    user32.PostQuitMessage(0)
                    return 0
            except Exception:
                log("tray error:\n" + traceback.format_exc())
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

        def _menu(self):
            menu = user32.CreatePopupMenu()
            for mid, text in self.MENU:
                if text is None:
                    user32.AppendMenuW(menu, 0x800, 0, None)  # MF_SEPARATOR
                else:
                    checked = 0x8 if mid == 6 and self.autostart_on else 0  # MF_CHECKED
                    user32.AppendMenuW(menu, checked, mid, text)
            pt = POINT()
            user32.GetCursorPos(ctypes.byref(pt))
            user32.SetForegroundWindow(self.hwnd)  # so the menu closes when you click elsewhere
            cmd = user32.TrackPopupMenu(menu, 0x100 | 0x80 | 0x2, pt.x, pt.y, 0, self.hwnd, None)
            user32.PostMessageW(self.hwnd, 0, None, None)
            user32.DestroyMenu(menu)
            if cmd in self.MENU_CMDS:
                self.post(self.MENU_CMDS[cmd])

        def stop(self):
            if self.hwnd:
                user32.PostMessageW(self.hwnd, 0x10, None, None)  # WM_CLOSE → WM_DESTROY
                self.join(1.5)

else:
    def extract_icon_png(parsing_name, size):
        return None

    def window_icon_png(hwnd, size):
        return None

    def list_open_windows():
        return []

    def window_app_id(hwnd):
        return None

    def window_pid(hwnd):
        return 0

    def list_monitors():
        return [((0, 0, 1920, 1040), 0, True)]

    def place_window(hwnd, place):
        pass

    def window_placement_now(hwnd):
        return "none", 0, None

    def window_rect(hwnd):
        return 260, 160, 1160, 760

    def set_clipboard_text(text, hwnd):
        CLIPBOARD_FALLBACK[:] = [text]

    def read_clipboard_text(hwnd):
        return CLIPBOARD_FALLBACK[0] if CLIPBOARD_FALLBACK else None

    FAKE_WIN = {"top": set(), "alpha": {}, "closed": [], "min": [], "killed": []}  # for tests off Windows

    def close_window(hwnd):
        FAKE_WIN["closed"].append(hwnd)

    def is_topmost(hwnd):
        return hwnd in FAKE_WIN["top"]

    def set_topmost(hwnd, on):
        (FAKE_WIN["top"].add if on else FAKE_WIN["top"].discard)(hwnd)
        return on

    def get_opacity(hwnd):
        return FAKE_WIN["alpha"].get(hwnd, 100)

    def set_opacity(hwnd, pct):
        FAKE_WIN["alpha"][hwnd] = min(100, pct)
        return min(100, pct)

    def minimize_window(hwnd, activate_next=True):
        FAKE_WIN["min"].append(hwnd)

    def kill_window_process(hwnd):
        FAKE_WIN["killed"].append(hwnd)
        return True

    def send_text(backspaces, text, left=0):
        FAKE_WIN.setdefault("typed", []).append((backspaces, text, left))

    def app_mute(hwnd, mute=None):
        muted = FAKE_WIN.setdefault("muted", set())
        if hwnd in FAKE_WIN.get("silent", ()):
            return None, 0
        if mute == "get":
            return hwnd in muted, 1
        mute = (hwnd not in muted) if mute is None else bool(mute)
        (muted.add if mute else muted.discard)(hwnd)
        return mute, 1

    def raise_with(target, others):
        FAKE_WIN.setdefault("raised", []).append((target, list(others)))

    def send_mask_key():
        pass

    def autostart_state():
        return None

    def dpapi_protect(data):
        raise OSError("Windows only")

    def dpapi_unprotect(data):
        raise OSError("Windows only")

    def set_autostart(on):
        return "Starting with Windows is only available on Windows."

    def claim_single_instance():
        return True

    def watch_show_requests(callback):
        pass


CLIPBOARD_FALLBACK = []
_INSTANCE_HANDLE = None


def start_program(exe, args=()):
    """Start a program. If the launcher runs as administrator, start it as the normal user instead
    (through Explorer), so browsers and apps don't run elevated."""
    if LAUNCHER_ELEVATED:
        target = make_shortcut(exe, args) if args else exe
        log(f"start (as normal user): {exe} {args}")
        subprocess.Popen(["explorer.exe", target])
    elif isinstance(args, str):
        subprocess.Popen(f'"{exe}" {args}'.strip(), cwd=os.path.dirname(exe) or None)
    else:
        subprocess.Popen([exe, *args], cwd=os.path.dirname(exe) or None)


def launch(item):
    if item["kind"] == "window":
        focus_window(item["hwnd"])
    elif item["kind"] == "pin":
        if item["target"] == "app":
            subprocess.Popen(["explorer.exe", "shell:AppsFolder\\" + item["app_id"]])
        elif LAUNCHER_ELEVATED:
            start_program(item["path"])
        else:
            os.startfile(item["path"])
    elif item["kind"] == "app":
        subprocess.Popen(["explorer.exe", "shell:AppsFolder\\" + item["app_id"]])
    elif item["kind"] == "profile":
        PENDING_LAUNCHES[profile_id(item)] = time.monotonic()
        start_program(item["exe"], item["args"])


# ======================================================================
# Theme
# ======================================================================

def apply_theme(root):
    global UI_FONT
    families = set(tkfont.families(root))
    UI_FONT = "Segoe UI Variable Text" if "Segoe UI Variable Text" in families else "Segoe UI"
    f = UI_FONT

    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure(".", background=T["bg"], foreground=T["fg"], font=(f, 10),
                    bordercolor=T["border"], lightcolor=T["surface"], darkcolor=T["surface"],
                    troughcolor=T["bg"], focuscolor=T["accent"], insertcolor=T["fg"],
                    selectbackground=T["select"], selectforeground=T["fg"])
    style.configure("TFrame", background=T["bg"])
    style.configure("TLabel", background=T["bg"], foreground=T["fg"])
    style.configure("TLabelframe", background=T["bg"], bordercolor=T["border"])
    style.configure("TLabelframe.Label", background=T["bg"], foreground=T["muted"])
    style.configure("TCheckbutton", background=T["bg"], foreground=T["fg"], indicatorbackground=T["field"],
                    indicatorforeground=T["accent"], upperbordercolor=T["border"], lowerbordercolor=T["border"])
    style.map("TCheckbutton", background=[("active", T["bg"])],
              indicatorbackground=[("selected", T["field"]), ("active", T["field"])])
    style.configure("TButton", background=T["field"], foreground=T["fg"], padding=(12, 5),
                    bordercolor=T["border"], lightcolor=T["field"], darkcolor=T["field"], relief="flat")
    style.map("TButton", background=[("active", T["border"]), ("pressed", T["select"])])
    style.configure("TEntry", fieldbackground=T["field"], foreground=T["fg"], bordercolor=T["border"],
                    lightcolor=T["field"], darkcolor=T["field"], padding=5)
    style.map("TEntry", bordercolor=[("focus", T["accent"])])

    style.configure("Treeview", background=T["bg"], fieldbackground=T["bg"], foreground=T["fg"],
                    font=(f, 10), borderwidth=0, relief="flat")
    # An explicit map also works around Tk 8.6.9 ignoring tag colors.
    style.map("Treeview", background=[("selected", T["select"])], foreground=[("selected", T["fg"])])
    style.layout("Treeview", [("Treeview.treearea", {"sticky": "nswe"})])
    style.layout("Treeview.Item", [("Treeitem.padding", {"sticky": "nswe", "children": [
        ("Treeitem.image", {"side": "left", "sticky": ""}),
        ("Treeitem.text", {"side": "left", "sticky": ""})]})])

    style.configure("Accent.TButton", background=T["accent"], foreground="#0b0d12",
                    lightcolor=T["accent"], darkcolor=T["accent"], bordercolor=T["accent"],
                    font=(f, 10, "bold"), padding=(14, 6))
    style.map("Accent.TButton", background=[("active", "#a9bcff"), ("disabled", T["border"])],
              foreground=[("disabled", T["faint"])])
    style.configure("TCombobox", fieldbackground=T["field"], background=T["field"], foreground=T["fg"],
                    arrowcolor=T["muted"], bordercolor=T["border"], lightcolor=T["field"], darkcolor=T["field"],
                    selectbackground=T["field"], selectforeground=T["fg"], padding=4)
    style.map("TCombobox", fieldbackground=[("readonly", T["field"])], foreground=[("readonly", T["fg"])],
              bordercolor=[("focus", T["accent"])])
    root.option_add("*TCombobox*Listbox.background", T["surface"])
    root.option_add("*TCombobox*Listbox.foreground", T["fg"])
    root.option_add("*TCombobox*Listbox.selectBackground", T["select"])
    root.option_add("*TCombobox*Listbox.font", (f, 10))
    style.configure("Card.Treeview", background=T["surface"], fieldbackground=T["surface"], rowheight=30)
    style.layout("Card.Treeview", [("Treeview.treearea", {"sticky": "nswe"})])
    style.configure("Card.Treeview.Heading", background=T["bg"], foreground=T["faint"], relief="flat",
                    font=(f, 8, "bold"), borderwidth=0, padding=(6, 4))
    style.map("Card.Treeview.Heading", background=[("active", T["bg"])])
    style.configure("TNotebook", background=T["bg"], borderwidth=0, tabmargins=(0, 0, 0, 0))
    style.configure("TNotebook.Tab", background=T["surface"], foreground=T["muted"], padding=(12, 6),
                    borderwidth=0, lightcolor=T["surface"], bordercolor=T["bg"], font=(f, 9))
    style.map("TNotebook.Tab", background=[("selected", T["field"]), ("active", T["hover"])],
              foreground=[("selected", T["fg"])], lightcolor=[("selected", T["field"])])

    style.layout("Thin.Vertical.TScrollbar", [("Vertical.Scrollbar.trough", {"sticky": "ns", "children": [
        ("Vertical.Scrollbar.thumb", {"expand": "1", "sticky": "nswe"})]})])
    style.configure("Thin.Vertical.TScrollbar", troughcolor=T["bg"], background=T["border"],
                    bordercolor=T["bg"], lightcolor=T["border"], darkcolor=T["border"],
                    arrowsize=7, gripcount=0, relief="flat")
    style.map("Thin.Vertical.TScrollbar", background=[("active", T["faint"])])

    for pattern, value in (("*Background", T["bg"]), ("*Foreground", T["fg"]),
                           ("*Entry.Background", T["field"]), ("*insertBackground", T["fg"]),
                           ("*Button.Background", T["field"]), ("*selectBackground", T["select"]),
                           ("*selectForeground", T["fg"]), ("*Menu.Background", T["surface"]),
                           ("*Menu.Foreground", T["fg"]), ("*Menu.activeBackground", T["select"]),
                           ("*Menu.activeForeground", T["fg"]), ("*Menu.relief", "flat"),
                           ("*Font", (f, 10))):
        root.option_add(pattern, value)
    root.configure(bg=T["bg"])


def center_over(win, master):
    win.update_idletasks()
    w, h = win.winfo_reqwidth(), win.winfo_reqheight()
    if win.winfo_width() > 1:
        w, h = max(w, win.winfo_width()), max(h, win.winfo_height())
    x = master.winfo_rootx() + (master.winfo_width() - w) // 2
    y = master.winfo_rooty() + max(0, (master.winfo_height() - h) // 4)
    win.geometry(f"+{max(0, x)}+{max(0, y)}")


def dark_titlebar(win):
    """Dark title bar matching the window (Windows 10 1809+ / 11)."""
    if not IS_WIN:
        return
    try:
        win.update_idletasks()
        hwnd = int(win.wm_frame(), 16)
        on = ctypes.c_int(1)
        dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(on), 4)  # DWMWA_USE_IMMERSIVE_DARK_MODE
        r, g, b = (int(T["bg"][i:i + 2], 16) for i in (1, 3, 5))
        color = ctypes.c_uint32(r | (g << 8) | (b << 16))
        dwmapi.DwmSetWindowAttribute(hwnd, 35, ctypes.byref(color), 4)  # DWMWA_CAPTION_COLOR (Win 11)
    except (OSError, ValueError, tk.TclError):
        pass


# ======================================================================
# Add-profile dialog
# ======================================================================

class AddProfileDialog(tk.Toplevel):
    LIST_MAX_HEIGHT = 340

    def __init__(self, master, current):
        super().__init__(master)
        self.title("Browser profiles")
        self.transient(master)
        self.configure(bg=T["bg"])
        self.minsize(460, 300)
        self.current = current
        self.result = None
        f = UI_FONT

        # Buttons first, pinned to the bottom, so they're always visible.
        buttons = tk.Frame(self, bg=T["bg"], padx=16, pady=14)
        buttons.pack(side="bottom", fill="x")
        style = ttk.Style(self)
        style.configure("Accent.TButton", background=T["accent"], foreground="#0b0d12",
                        lightcolor=T["accent"], darkcolor=T["accent"], bordercolor=T["accent"],
                        font=(f, 10, "bold"), padding=(14, 6))
        style.map("Accent.TButton", background=[("active", "#a9bcff"), ("disabled", T["border"])],
                  foreground=[("disabled", T["faint"])])
        self.save_btn = ttk.Button(buttons, text="Save", style="Accent.TButton", command=self._save)
        self.save_btn.pack(side="right")
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(side="right", padx=(0, 8))
        self.custom_btn = ttk.Button(buttons, text="+ Custom profile…", command=self._toggle_custom)
        self.custom_btn.pack(side="left")
        tk.Frame(self, bg=T["border"], height=1).pack(side="bottom", fill="x")

        # Custom profile form (hidden until asked for)
        self.custom = tk.Frame(self, bg=T["bg"], padx=16, pady=10)
        self.c_name, self.c_exe, self.c_args = tk.StringVar(), tk.StringVar(), tk.StringVar()
        for row, (label, var) in enumerate((("Name", self.c_name), ("Browser", self.c_exe),
                                            ("Arguments", self.c_args))):
            tk.Label(self.custom, text=label, bg=T["bg"], fg=T["muted"], font=(f, 9)).grid(
                row=row, column=0, sticky="w", padx=(0, 10), pady=3)
            ttk.Entry(self.custom, textvariable=var).grid(row=row, column=1, sticky="we", pady=3)
        ttk.Button(self.custom, text="Browse…", command=self._browse).grid(row=1, column=2, padx=(6, 0))
        tk.Label(self.custom, text='e.g.  --profile-directory="Profile 2"    or    -P "Work" -no-remote',
                 bg=T["bg"], fg=T["faint"], font=(f, 8)).grid(row=3, column=1, columnspan=2, sticky="w")
        self.custom.columnconfigure(1, weight=1)
        self.custom_shown = False

        # Header
        head = tk.Frame(self, bg=T["bg"], padx=16, pady=14)
        head.pack(side="top", fill="x")
        tk.Label(head, text="Browser profiles", bg=T["bg"], fg=T["fg"], font=(f, 13, "bold")).pack(anchor="w")
        tk.Label(head, text="Tick the profiles to show in the launcher, then press Save.",
                 bg=T["bg"], fg=T["muted"], font=(f, 9)).pack(anchor="w", pady=(2, 0))

        # Scrollable list of detected profiles
        self.detected = detect_browser_profiles()
        current_ids = {profile_id(p) for p in current}
        self.vars = []
        wrap = tk.Frame(self, bg=T["bg"], padx=16)
        wrap.pack(side="top", fill="both", expand=True)
        canvas = tk.Canvas(wrap, bg=T["bg"], highlightthickness=0, borderwidth=0)
        inner = tk.Frame(canvas, bg=T["bg"])
        canvas.create_window((0, 0), window=inner, anchor="nw")
        sb = ttk.Scrollbar(wrap, orient="vertical", command=canvas.yview, style="Thin.Vertical.TScrollbar")
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True)

        if self.detected:
            for p in self.detected:
                v = tk.BooleanVar(value=profile_id(p) in current_ids)
                v.trace_add("write", lambda *_: self._update_save())
                ttk.Checkbutton(inner, text=p["name"], variable=v).pack(anchor="w", pady=3)
                self.vars.append(v)
        else:
            tk.Label(inner, text="No Chrome, Edge, Brave, Vivaldi or Firefox profiles were found.\n"
                                 "Use “+ Custom profile…” to add one by hand.",
                     bg=T["bg"], fg=T["muted"], font=(f, 10), justify="left").pack(anchor="w", pady=6)

        inner.update_idletasks()
        list_h = min(inner.winfo_reqheight(), self.LIST_MAX_HEIGHT)
        canvas.configure(height=list_h, width=max(420, inner.winfo_reqwidth()))
        canvas.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        if inner.winfo_reqheight() > self.LIST_MAX_HEIGHT:
            sb.pack(side="right", fill="y")
            wheel = lambda e: canvas.yview_scroll(-1 if e.delta > 0 else 1, "units")  # noqa: E731
            self.bind_all("<MouseWheel>", wheel)
            self.bind("<Destroy>", lambda e: self.unbind_all("<MouseWheel>") if e.widget is self else None)

        self._update_save()
        self.bind("<Escape>", lambda e: self.destroy())
        self.bind("<Return>", lambda e: self._save())
        center_over(self, master)
        dark_titlebar(self)
        self.lift()
        self.focus_force()
        self.grab_set()
        self.wait_window()

    def _toggle_custom(self):
        if self.custom_shown:
            self.custom.pack_forget()
            self.custom_btn.configure(text="+ Custom profile…")
        else:
            self.custom.pack(side="bottom", fill="x")
            self.custom_btn.configure(text="− Custom profile")
        self.custom_shown = not self.custom_shown
        self._update_save()

    def _update_save(self):
        n = sum(v.get() for v in self.vars)
        label = f"Save  ({n} selected)" if self.vars else "Save"
        self.save_btn.configure(text=label)

    def _browse(self):
        path = filedialog.askopenfilename(parent=self, title="Choose browser",
                                          filetypes=[("Programs", "*.exe")])
        if path:
            self.c_exe.set(os.path.normpath(path))

    def _save(self):
        detected_ids = {profile_id(p) for p in self.detected}
        checked_ids = {profile_id(p) for p, v in zip(self.detected, self.vars) if v.get()}

        # keep custom/earlier profiles and still-checked detected ones (keeps renames + order)
        result = [p for p in self.current
                  if profile_id(p) not in detected_ids or profile_id(p) in checked_ids]
        have = {profile_id(p) for p in result}
        result += [p for p, v in zip(self.detected, self.vars)
                   if v.get() and profile_id(p) not in have]

        name, exe, args = self.c_name.get().strip(), self.c_exe.get().strip(), self.c_args.get().strip()
        if name or exe or args:
            if not name or not exe:
                messagebox.showwarning("Custom profile", "Enter a name and choose a browser.", parent=self)
                return
            if not os.path.isfile(exe):
                messagebox.showwarning("Custom profile", "Browser file not found.", parent=self)
                return
            result.append(make_profile(name, exe, args))

        self.result = result
        self.destroy()


class AppPickerDialog(tk.Toplevel):
    """Pick one or more installed apps to add to My apps."""

    def __init__(self, master, apps, existing_ids):
        super().__init__(master)
        self.title("Add to My apps")
        self.transient(master)
        self.geometry("380x480")
        self.apps = [a for a in apps if "app|" + a["app_id"] not in existing_ids]
        self.shown = []
        self.result = None

        body = ttk.Frame(self, padding=10)
        body.pack(fill="both", expand=True)
        self.q = tk.StringVar()
        self.q.trace_add("write", lambda *_: self._filter())
        entry = ttk.Entry(body, textvariable=self.q)
        entry.pack(fill="x")
        entry.focus()

        lf = ttk.Frame(body)
        lf.pack(fill="both", expand=True, pady=8)
        self.lb = tk.Listbox(lf, selectmode="extended", activestyle="none",
                             font=(UI_FONT, 10), borderwidth=0, highlightthickness=0,
                             bg=T["surface"], fg=T["fg"], selectbackground=T["select"],
                             selectforeground=T["fg"])
        sb = ttk.Scrollbar(lf, orient="vertical", command=self.lb.yview, style="Thin.Vertical.TScrollbar")
        self.lb.configure(yscrollcommand=sb.set)
        self.lb.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.lb.bind("<Double-1>", lambda e: self._add())

        ttk.Label(body, text="Ctrl/Shift-click to pick several.", foreground="gray").pack(anchor="w")
        buttons = ttk.Frame(body)
        buttons.pack(fill="x", pady=(8, 0))
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(side="right")
        ttk.Button(buttons, text="Add", command=self._add).pack(side="right", padx=6)

        self.bind("<Escape>", lambda e: self.destroy())
        self.bind("<Return>", lambda e: self._add())
        self._filter()
        self.configure(bg=T["bg"])
        center_over(self, master)
        dark_titlebar(self)
        self.lift()
        self.grab_set()
        self.wait_window()

    def _filter(self):
        q = self.q.get().strip().lower()
        self.shown = [a for a in self.apps if q in a["name"].lower()]
        self.lb.delete(0, "end")
        for a in self.shown:
            self.lb.insert("end", a["name"])
        if len(self.shown) == 1:
            self.lb.selection_set(0)

    def _add(self):
        picked = [self.shown[i] for i in self.lb.curselection()]
        if picked:
            self.result = [make_pin_app(a["name"], a["app_id"]) for a in picked]
            self.destroy()


# ======================================================================
# Notes window
# ======================================================================

def flat_button(parent, text, command):
    bg = parent["bg"]
    b = tk.Label(parent, text=text, bg=bg, fg=T["muted"], font=(UI_FONT, 9), padx=8, pady=2, cursor="hand2")
    b.bind("<Enter>", lambda e: b.configure(fg=T["fg"], bg=T["border"]))
    b.bind("<Leave>", lambda e: b.configure(fg=T["muted"], bg=bg))
    b.bind("<Button-1>", lambda e: command())
    return b


class NotesWindow(tk.Toplevel):
    """Tabbed notes, encrypted on disk. Alt+N shows/hides; hidden notes lock after a while."""
    SAVE_DELAY_MS = 1200

    def __init__(self, master):
        super().__init__(master)
        self.withdraw()
        self.title("Notes")
        self.configure(bg=T["bg"])
        w, h = 780, 580
        self.geometry(f"{w}x{h}+{(self.winfo_screenwidth() - w) // 2}+{max(0, (self.winfo_screenheight() - h) // 3)}")
        self.minsize(440, 320)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.bind("<Escape>", lambda e: self.hide())
        self.key = self.salt = self.kdf = None
        self.pending_clip = []    # clipboard text waiting for the notes to be unlocked
        self.tabs = []
        self.closed = []          # closed (hidden) tabs: still saved, can be reopened
        self.deleted_ids = set()
        self.body = None
        self.pw = self.pw2 = None
        self._save_job = self._lock_job = None
        self._styled = False
        self._show_lock()

    @property
    def unlocked(self):
        return self.key is not None

    def _clear(self):
        if self.body is not None:
            self.body.destroy()
        self.body = tk.Frame(self, bg=T["bg"])
        self.body.pack(fill="both", expand=True)
        return self.body

    # ---- lock screen
    def _show_lock(self, error=""):
        body = self._clear()
        f = UI_FONT
        self.pw = self.pw2 = None
        card = tk.Frame(body, bg=T["bg"])
        card.place(relx=0.5, rely=0.42, anchor="center")
        tk.Label(card, text="🔒  Notes", bg=T["bg"], fg=T["fg"], font=(f, 17, "bold")).pack(anchor="w")

        if not notes_crypto_available():
            tk.Label(card, text="Encrypted notes need the “cryptography” package.\n"
                                "Run this once in a terminal, then restart the launcher:",
                     bg=T["bg"], fg=T["muted"], font=(f, 10), justify="left").pack(anchor="w", pady=(8, 6))
            cmd = tk.Entry(card, bg=T["field"], fg=T["fg"], relief="flat", font=("Consolas", 10), width=60,
                           readonlybackground=T["field"])
            cmd.insert(0, f'"{sys.executable}" -m pip install cryptography')
            cmd.configure(state="readonly")
            cmd.pack(anchor="w", ipady=6)
            return

        new = not notes_exist()
        tk.Label(card, text="Create a password for your notes." if new else "Enter your password to open your notes.",
                 bg=T["bg"], fg=T["muted"], font=(f, 10)).pack(anchor="w", pady=(6, 12))
        self.pw = ttk.Entry(card, show="•", width=36, font=(f, 11))
        self.pw.pack(anchor="w", ipady=3)
        if new:
            tk.Label(card, text="Confirm password", bg=T["bg"], fg=T["faint"], font=(f, 9)).pack(anchor="w", pady=(10, 2))
            self.pw2 = ttk.Entry(card, show="•", width=36, font=(f, 11))
            self.pw2.pack(anchor="w", ipady=3)
            tk.Label(card, text=f"At least {notes_min_password()} characters. Notes are encrypted with this "
                                "password.\nIf you forget it, they can't be recovered — there's no reset.",
                     bg=T["bg"], fg=T["faint"], font=(f, 9), justify="left").pack(anchor="w", pady=(10, 0))
        if self.pending_clip:
            n = sum(c for _, c in self.pending_clip)
            tk.Label(card, text=f"Unlock to save {n} clipboard item{'s' if n > 1 else ''} to your notes.",
                     bg=T["bg"], fg=T["accent"], font=(f, 9)).pack(anchor="w", pady=(10, 0))
        self.err = tk.Label(card, text=error, bg=T["bg"], fg="#ff8f8f", font=(f, 9))
        self.err.pack(anchor="w", pady=(10, 0))
        ttk.Button(card, text="Create" if new else "Unlock", style="Accent.TButton",
                   command=self._unlock).pack(anchor="e", pady=(10, 0))
        for e in (self.pw, self.pw2):
            if e is not None:
                e.bind("<Return>", lambda ev: self._unlock())
        pw = self.pw
        self.after(60, lambda: pw.winfo_exists() and pw.focus_force())

    def _unlock(self):
        pw = self.pw.get()
        failed, migrated = [], False
        try:
            if not notes_exist():
                if len(pw) < notes_min_password():
                    self.err.configure(text=f"Use at least {notes_min_password()} characters.")
                    return
                if pw != self.pw2.get():
                    self.err.configure(text="The passwords don't match.")
                    return
                self.err.configure(text="Creating…", fg=T["muted"])
                self.update_idletasks()
                self.salt, self.kdf = os.urandom(16), dict(NOTES_KDF)
                self.key = notes_key(pw, self.salt, self.kdf)
                tabs, drop = [{"id": None, "title": "Notes", "text": "", "order": None, "dirty": True}], []
            else:
                self.err.configure(text="Unlocking…", fg=T["muted"])
                self.update_idletasks()
                result = notes_load_all(pw)
                if result is None:  # only the older single notes.enc exists: split it into tab files
                    payload, self.key, self.salt, self.kdf = notes_decrypt_file(NOTES_FILE, pw)
                    tabs = [{"id": None, "title": t.get("title") or "Notes", "text": t.get("text", ""),
                             "order": None, "dirty": True} for t in payload_tabs(payload)]
                    drop, migrated = [], True
                else:
                    tabs, self.key, self.salt, self.kdf, failed, drop = result
        except ValueError:
            self._show_lock("Wrong password.")
            return
        except Exception as e:
            log("notes open error:\n" + traceback.format_exc())
            self.key = None
            self._show_lock(f"Couldn't open notes: {e}")
            return
        log("notes unlocked")
        try:
            self.master.on_notes_unlocked((self.key, self.salt, self.kdf))
        except Exception:
            log("notes unlock hook error:\n" + traceback.format_exc())
        self.deleted_ids = set(drop)
        self.closed = [{"id": t["id"] or new_tab_id(), "title": t["title"], "text": t["text"]}
                       for t in tabs if t.get("closed")]
        self._show_editor([t for t in tabs if not t.get("closed")], load_ui().get("notes_active", 0))
        self.save_now()
        if migrated and os.path.isfile(NOTES_FILE) and notes_tab_files():
            os.replace(NOTES_FILE, NOTES_FILE + ".old")
            log("notes: split notes.enc into one encrypted file per tab (old file kept as notes.enc.old)")
        if self.pending_clip:
            waiting, self.pending_clip = self.pending_clip, []
            text = "\n\n".join(t for t, _ in waiting)
            count = sum(c for _, c in waiting)
            self.after(120, lambda: self.master.notes_receive(text, count))
        if failed:
            messagebox.showwarning(
                "Notes", f"{len(failed)} note file(s) in data/notes couldn't be opened with this password, "
                         "so they were left as they are:\n\n" + "\n".join(os.path.basename(x) for x in failed),
                parent=self)

    # ---- editor
    def _show_editor(self, tabs, active=0):
        body = self._clear()
        f = UI_FONT
        bar = tk.Frame(body, bg=T["bg"], padx=12, pady=8)
        bar.pack(fill="x")
        for text, cmd in (("+ Tab", self.new_tab), ("Rename", self.rename_tab), ("Close tab", self.close_tab)):
            flat_button(bar, text, cmd).pack(side="left", padx=(0, 2))
        self.closed_btn = flat_button(bar, "Closed tabs ▾", lambda: self._closed_menu())
        self.closed_btn.pack(side="left", padx=(0, 2))
        for text, cmd in (("Delete tab", self.delete_tab), ("Import…", self.import_notes)):
            flat_button(bar, text, cmd).pack(side="left", padx=(0, 2))
        self.export_btn = flat_button(bar, "Export…", lambda: self._export_menu())
        self.export_btn.pack(side="left", padx=(0, 2))
        for text, cmd in (("🔒 Lock", self.lock), ("Change password", self.change_password)):
            flat_button(bar, text, cmd).pack(side="right", padx=(2, 0))
        self.saved_lbl = tk.Label(bar, text="Encrypted ✓", bg=T["bg"], fg=T["faint"], font=(f, 8))
        self.saved_lbl.pack(side="right", padx=10)

        self.nb = ttk.Notebook(body)
        self.nb.pack(fill="both", expand=True, padx=12, pady=(0, 12))
        self.tabs = []
        for t in tabs or [{"id": None, "title": "Notes", "text": "", "order": None, "dirty": True}]:
            self._add_tab(t["title"], t["text"], t["id"], t["dirty"], t["order"])
        try:
            self.nb.select(max(0, min(int(active), len(self.tabs) - 1)))
        except (TypeError, ValueError):
            pass
        self.nb.enable_traversal()  # Ctrl+Tab / Ctrl+Shift+Tab
        self.nb.bind("<Double-1>", lambda e: self._tab_at(e) is not None and self.rename_tab(self._tab_at(e)))
        self.nb.bind("<Button-2>", lambda e: self._tab_at(e) is not None and self.close_tab(self._tab_at(e)))
        self.nb.bind("<<NotebookTabChanged>>", lambda e: (self._focus_current(), self._schedule_save(quiet=True)))
        for seq, fn in (("<Control-t>", self.new_tab), ("<Control-w>", self.close_tab),
                        ("<Control-s>", self.save_now), ("<Control-l>", self.lock)):
            self.bind(seq, lambda e, fn=fn: fn())
        self.after(30, self._focus_current)

    def _add_tab(self, title, text="", tab_id=None, dirty=True, order=None):
        frame = tk.Frame(self.nb, bg=T["field"])
        txt = tk.Text(frame, wrap="word", undo=True, autoseparators=True, bg=T["field"], fg=T["fg"],
                      insertbackground=T["fg"], selectbackground=T["select"], selectforeground=T["fg"],
                      relief="flat", borderwidth=0, highlightthickness=0, font=(UI_FONT, 11),
                      padx=18, pady=14, spacing3=4)
        sb = ttk.Scrollbar(frame, orient="vertical", command=txt.yview, style="Thin.Vertical.TScrollbar")
        txt.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        txt.pack(side="left", fill="both", expand=True)
        txt.insert("1.0", text)
        txt.edit_reset()
        txt.edit_modified(False)
        txt.bind("<<Modified>>", lambda e, t=txt: self._on_modified(t))
        # Instance bindings run before the Text class ones (e.g. Ctrl+T = transpose), so "break" wins.
        for seq, fn in (("<Control-t>", self.new_tab), ("<Control-w>", self.close_tab),
                        ("<Control-s>", self.save_now), ("<Control-l>", self.lock)):
            txt.bind(seq, lambda e, fn=fn: (fn(), "break")[1])
        self.nb.add(frame, text=f"  {title}  ")
        self.tabs.append({"frame": frame, "text": txt, "title": title, "id": tab_id or new_tab_id(),
                          "dirty": dirty, "order": order})
        return len(self.tabs) - 1

    def _tab_at(self, e):
        try:
            return self.nb.index(f"@{e.x},{e.y}")
        except tk.TclError:
            return None

    def _current_index(self):
        try:
            return self.nb.index(self.nb.select())
        except (tk.TclError, AttributeError):
            return 0

    def _focus_current(self):
        if self.tabs:
            self.tabs[min(self._current_index(), len(self.tabs) - 1)]["text"].focus_set()

    def _on_modified(self, txt):
        if txt.edit_modified():
            txt.edit_modified(False)
            for t in self.tabs:
                if t["text"] is txt:
                    t["dirty"] = True
            self._schedule_save()

    def _schedule_save(self, quiet=False):
        if not self.unlocked:
            return
        if not quiet:
            self.saved_lbl.configure(text="Editing…")
        if self._save_job:
            self.after_cancel(self._save_job)
        self._save_job = self.after(self.SAVE_DELAY_MS, self.save_now)

    @staticmethod
    def _tab_path(tab_id):
        return os.path.join(NOTES_DIR, tab_id + ".enc")

    def save_now(self):
        """Write each changed tab to its own encrypted file; remove files of closed tabs."""
        if self._save_job:
            self.after_cancel(self._save_job)
            self._save_job = None
        if not self.unlocked or not self.tabs:
            return
        try:
            written = 0
            for i, t in enumerate(self.tabs):
                if t["dirty"] or t["order"] != i:
                    payload = {"title": t["title"], "text": t["text"].get("1.0", "end-1c"), "order": i, "closed": False}
                    write_json_atomic(self._tab_path(t["id"]), notes_encrypt(payload, self.key, self.salt, self.kdf))
                    t["dirty"], t["order"] = False, i
                    written += 1
            for tid in list(self.deleted_ids):
                try:
                    os.remove(self._tab_path(tid))
                except FileNotFoundError:
                    pass
                self.deleted_ids.discard(tid)
            ui = load_ui()
            if ui.get("notes_active") != self._current_index():
                save_ui({**ui, "notes_active": self._current_index()})
            self.saved_lbl.configure(text="Saved ✓  encrypted")
        except Exception as e:
            log("notes save error:\n" + traceback.format_exc())
            self.saved_lbl.configure(text=f"Save failed: {e}")

    def new_tab(self):
        i = self._add_tab(f"Note {len(self.tabs) + 1}")
        self.nb.select(i)
        self._focus_current()
        self._schedule_save()

    def rename_tab(self, index=None):
        i = self._current_index() if index is None else index
        if not 0 <= i < len(self.tabs):
            return
        new = simpledialog.askstring("Rename tab", "Name:", initialvalue=self.tabs[i]["title"], parent=self)
        if new and new.strip():
            self.tabs[i]["title"] = new.strip()
            self.tabs[i]["dirty"] = True
            self.nb.tab(self.tabs[i]["frame"], text=f"  {new.strip()}  ")
            self._schedule_save()

    def _detach_tab(self, i):
        """Take tab i out of the notebook; keep at least one open tab."""
        t = self.tabs.pop(i)
        self.nb.forget(t["frame"])
        text = t["text"].get("1.0", "end-1c")
        t["frame"].destroy()
        if not self.tabs:
            self._add_tab("Notes")
            self.nb.select(0)
        return t, text

    def close_tab(self, index=None):
        """Hide a tab. It stays saved (encrypted) and can be reopened from “Closed tabs”."""
        i = self._current_index() if index is None else index
        if not 0 <= i < len(self.tabs):
            return
        t, text = self._detach_tab(i)
        payload = {"title": t["title"], "text": text, "order": None, "closed": True}
        try:
            write_json_atomic(self._tab_path(t["id"]), notes_encrypt(payload, self.key, self.salt, self.kdf))
        except Exception as e:
            log("notes close error:\n" + traceback.format_exc())
            self.saved_lbl.configure(text=f"Save failed: {e}")
        self.closed.append({"id": t["id"], "title": t["title"], "text": text})
        self.saved_lbl.configure(text=f"Closed “{t['title']}” — reopen it from Closed tabs")
        self._schedule_save(quiet=True)
        self._focus_current()

    def delete_tab(self, index=None):
        """Remove a tab and its encrypted file for good."""
        i = self._current_index() if index is None else index
        if not 0 <= i < len(self.tabs):
            return
        title = self.tabs[i]["title"]
        if not messagebox.askyesno("Delete tab", f"Delete “{title}” permanently?\n\n"
                                                 "Its encrypted file is removed and it can't be reopened.", parent=self):
            return
        t, _ = self._detach_tab(i)
        self.deleted_ids.add(t["id"])
        self.save_now()
        self.saved_lbl.configure(text=f"Deleted “{title}”")
        self._focus_current()

    def _closed_menu(self):
        menu = tk.Menu(self, tearoff=0)
        if not self.closed:
            menu.add_command(label="No closed tabs", state="disabled")
        else:
            for c in self.closed:
                menu.add_command(label=f"Reopen  “{c['title']}”", command=lambda c=c: self.reopen_tab(c))
            menu.add_separator()
            delete = tk.Menu(menu, tearoff=0)
            for c in self.closed:
                delete.add_command(label=c["title"], command=lambda c=c: self.delete_closed(c))
            menu.add_cascade(label="Delete permanently", menu=delete)
        b = self.closed_btn
        menu.tk_popup(b.winfo_rootx(), b.winfo_rooty() + b.winfo_height())

    def reopen_tab(self, c):
        self.closed = [x for x in self.closed if x is not c]
        i = self._add_tab(c["title"], c["text"], c["id"], dirty=True)
        self.nb.select(i)
        self.save_now()
        self._focus_current()

    def delete_closed(self, c):
        if not messagebox.askyesno("Delete tab", f"Delete “{c['title']}” permanently?\n\n"
                                                 "Its encrypted file is removed and it can't be reopened.", parent=self):
            return
        self.closed = [x for x in self.closed if x is not c]
        self.deleted_ids.add(c["id"])
        self.save_now()
        self.saved_lbl.configure(text=f"Deleted “{c['title']}”")

    # ---- import / export
    def import_notes(self):
        paths = filedialog.askopenfilenames(
            parent=self, title="Import notes",
            filetypes=[("Notes and text files", "*.enc *.txt *.md"), ("Encrypted notes", "*.enc"),
                       ("Text files", "*.txt *.md"), ("All files", "*.*")])
        added, plain = 0, False
        for path in paths:
            name = os.path.basename(path)
            if path.lower().endswith(".enc"):
                try:
                    blob = notes_read_blob(path)
                except (OSError, ValueError) as e:
                    messagebox.showerror("Import", f"Couldn't read {name}:\n{e}", parent=self)
                    continue
                payload = None
                if blob.get("salt") == _b64(self.salt) and blob.get("kdf") == self.kdf:
                    try:  # encrypted with these notes' password: no need to ask
                        payload = notes_decrypt_blob(blob, self.key)
                    except ValueError:
                        payload = None
                while payload is None:
                    pw = simpledialog.askstring("Import encrypted notes", f"Password for {name}:", show="•",
                                                parent=self)
                    if pw is None:
                        break
                    try:
                        payload = notes_decrypt_blob(blob, notes_blob_key(blob, pw)[0])
                    except ValueError:
                        messagebox.showerror("Import", f"Wrong password for {name}.", parent=self)
                if payload is None:
                    continue
                for t in payload_tabs(payload):
                    self._add_tab(t.get("title") or "Imported", t.get("text", ""))
                    added += 1
            else:
                try:
                    with open(path, encoding="utf-8", errors="replace") as f:
                        text = f.read()
                except OSError as e:
                    messagebox.showerror("Import", f"Couldn't read {name}:\n{e}", parent=self)
                    continue
                self._add_tab(os.path.splitext(name)[0], text)
                added, plain = added + 1, True
        if added:
            self.nb.select(len(self.tabs) - 1)
            self.save_now()
            self.saved_lbl.configure(text=f"Imported {added} tab{'s' if added > 1 else ''} ✓  encrypted")
            if plain:
                messagebox.showinfo("Import", "Imported and encrypted. The original text files were not changed — "
                                              "delete them if they contain anything private.", parent=self)

    def _export_menu(self):
        menu = tk.Menu(self, tearoff=0)
        menu.add_command(label="Current tab — encrypted file…", command=self.export_tab_encrypted)
        menu.add_command(label="All tabs — encrypted backup folder…", command=self.export_backup)
        menu.add_separator()
        menu.add_command(label="Current tab — plain text file…", command=self.export_tab_text)
        b = self.export_btn
        menu.tk_popup(b.winfo_rootx(), b.winfo_rooty() + b.winfo_height())

    @staticmethod
    def _safe_name(title):
        return re.sub(r'[\\/:*?"<>|]+', "_", title).strip() or "note"

    def export_tab_encrypted(self):
        if not self.tabs:
            return
        self.save_now()
        t = self.tabs[self._current_index()]
        path = filedialog.asksaveasfilename(parent=self, title="Save encrypted tab", defaultextension=".enc",
                                            initialfile=f"{self._safe_name(t['title'])}.enc",
                                            filetypes=[("Encrypted note", "*.enc")])
        if path:
            shutil.copyfile(self._tab_path(t["id"]), path)
            self.saved_lbl.configure(text="Tab exported ✓  encrypted (same password)")

    def export_backup(self):
        self.save_now()
        folder = filedialog.askdirectory(parent=self, title="Choose where to put the backup folder")
        if folder:
            dest = os.path.join(folder, f"notes-backup-{time.strftime('%Y-%m-%d_%H%M')}")
            shutil.copytree(NOTES_DIR, dest, ignore=shutil.ignore_patterns("*.tmp"))
            self.saved_lbl.configure(text="Backup saved ✓  (same password)")
            log(f"notes backup written to {dest}")

    def export_tab_text(self):
        if not self.tabs:
            return
        if not messagebox.askyesno("Export as text", "The exported file will NOT be encrypted — anyone who can "
                                                     "open it can read it.\n\nContinue?", parent=self):
            return
        t = self.tabs[self._current_index()]
        path = filedialog.asksaveasfilename(parent=self, title="Export tab", defaultextension=".txt",
                                            initialfile=f"{self._safe_name(t['title'])}.txt",
                                            filetypes=[("Text", "*.txt"), ("Markdown", "*.md")])
        if path:
            with open(path, "w", encoding="utf-8") as f:
                f.write(t["text"].get("1.0", "end-1c"))
            self.saved_lbl.configure(text="Exported (not encrypted)")

    def lock(self):
        if self.unlocked:
            self.save_now()
        self.key = self.salt = self.kdf = None
        self.tabs = []
        self.closed = []
        self.deleted_ids = set()
        if self._lock_job:
            self.after_cancel(self._lock_job)
            self._lock_job = None
        log("notes locked")
        try:
            self.master.on_notes_locked()
        except Exception:
            log("notes lock hook error:\n" + traceback.format_exc())
        self._show_lock()

    def change_password(self):
        cur = simpledialog.askstring("Change password", "Current password:", show="•", parent=self)
        if cur is None:
            return
        if not hmac.compare_digest(notes_key(cur, self.salt, self.kdf), self.key):
            messagebox.showerror("Change password", "That's not the current password.", parent=self)
            return
        new = simpledialog.askstring("Change password", "New password:", show="•", parent=self)
        if new is None:
            return
        if len(new) < notes_min_password():
            messagebox.showerror("Change password", f"Use at least {notes_min_password()} characters.", parent=self)
            return
        if simpledialog.askstring("Change password", "Type the new password again:", show="•", parent=self) != new:
            messagebox.showerror("Change password", "The passwords don't match.", parent=self)
            return
        old_vault = (self.key, self.salt, self.kdf)
        self.salt, self.kdf = os.urandom(16), dict(NOTES_KDF)
        self.key = notes_key(new, self.salt, self.kdf)
        try:
            clip_rewrap(old_vault, (self.key, self.salt, self.kdf))
        except Exception:
            log("clipboard key rewrap error:\n" + traceback.format_exc())
        for t in self.tabs:
            t["dirty"] = True  # every tab file is re-encrypted with the new password
        for c in self.closed:
            write_json_atomic(self._tab_path(c["id"]), notes_encrypt(
                {"title": c["title"], "text": c["text"], "order": None, "closed": True}, self.key, self.salt, self.kdf))
        self.save_now()
        messagebox.showinfo("Change password", "Password changed — all tabs were re-encrypted.", parent=self)

    # ---- show / hide
    def show(self):
        if self._lock_job:
            self.after_cancel(self._lock_job)
            self._lock_job = None
        self.deiconify()
        if not self._styled:
            dark_titlebar(self)
            self._styled = True
        self.lift()
        if IS_WIN:
            try:
                focus_window(int(self.wm_frame(), 16))
            except Exception:
                log("notes focus error:\n" + traceback.format_exc())
        self.focus_force()
        if self.unlocked:
            self._focus_current()
        elif self.pw is not None:
            self.pw.focus_force()

    def hide(self):
        if self.unlocked:
            self.save_now()
            minutes = SETTINGS.get("autolock_min", 10)
            if minutes:
                self._lock_job = self.after(minutes * 60 * 1000, self.lock)
        self.withdraw()

    def close(self):
        self.lock()
        self.withdraw()

    def toggle(self):
        front = IS_WIN and user32.GetForegroundWindow() == int(self.wm_frame(), 16)
        if self.state() == "normal" and front:
            self.hide()
        else:
            self.show()


# ======================================================================
# Small list picker (used by workspaces)
# ======================================================================

class ListPicker(tk.Toplevel):
    """Pick one or several entries from a list. self.result = list of chosen indexes, or None."""

    def __init__(self, master, title, labels, multi=True, hint=None, ok_text="Add"):
        super().__init__(master)
        self.title(title)
        self.transient(master)
        self.configure(bg=T["bg"])
        self.geometry("440x480")
        self.labels, self.shown, self.result = labels, [], None
        body = tk.Frame(self, bg=T["bg"], padx=14, pady=12)
        body.pack(fill="both", expand=True)
        buttons = tk.Frame(body, bg=T["bg"])
        buttons.pack(side="bottom", fill="x", pady=(10, 0))
        ttk.Button(buttons, text=ok_text, style="Accent.TButton", command=self._ok).pack(side="right")
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(side="right", padx=(0, 8))
        if hint:
            tk.Label(body, text=hint, bg=T["bg"], fg=T["faint"], font=(UI_FONT, 9), justify="left",
                     wraplength=400).pack(side="bottom", anchor="w", pady=(8, 0))
        self.q = tk.StringVar()
        self.q.trace_add("write", lambda *_: self._filter())
        entry = ttk.Entry(body, textvariable=self.q)
        entry.pack(fill="x")
        frame = tk.Frame(body, bg=T["bg"])
        frame.pack(fill="both", expand=True, pady=(8, 0))
        self.lb = tk.Listbox(frame, selectmode="extended" if multi else "browse", activestyle="none",
                             font=(UI_FONT, 10), borderwidth=0, highlightthickness=0, bg=T["surface"],
                             fg=T["fg"], selectbackground=T["select"], selectforeground=T["fg"],
                             exportselection=False)
        sb = ttk.Scrollbar(frame, orient="vertical", command=self.lb.yview, style="Thin.Vertical.TScrollbar")
        self.lb.configure(yscrollcommand=sb.set)
        self.lb.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.lb.bind("<Double-1>", lambda e: self._ok())
        self.bind("<Return>", lambda e: self._ok())
        self.bind("<Escape>", lambda e: self.destroy())
        self._filter()
        if len(labels) == 1 or not multi:
            self.lb.selection_set(0)
        center_over(self, master)
        dark_titlebar(self)
        entry.focus_set()
        self.grab_set()
        self.wait_window()

    def _filter(self):
        q = self.q.get().strip().lower()
        self.shown = [i for i, label in enumerate(self.labels) if q in label.lower()]
        self.lb.delete(0, "end")
        for i in self.shown:
            self.lb.insert("end", "  " + self.labels[i])
        if len(self.shown) == 1:
            self.lb.selection_set(0)

    def _ok(self):
        picked = [self.shown[i] for i in self.lb.curselection()]
        if picked:
            self.result = picked
            self.destroy()


# ======================================================================
# Clipboard history window (Alt+V)
# ======================================================================

def _age(t):
    d = max(0, time.time() - t)
    if d < 60:
        return "now"
    if d < 3600:
        return f"{int(d // 60)} min"
    if d < 86400:
        return f"{int(d // 3600)} h"
    if d < 7 * 86400:
        return time.strftime("%a", time.localtime(t))
    return time.strftime("%d %b", time.localtime(t))


class ClipboardWindow(tk.Toplevel):
    """Alt+V. Tick the boxes to choose several items, then use the buttons in the bar above the list."""
    BOX = 18

    def __init__(self, app):
        super().__init__(app)
        self.withdraw()
        self.app, self.store = app, app.clip_store
        self.title("Clipboard history")
        self.configure(bg=T["bg"])
        self.minsize(440, 380)
        self.protocol("WM_DELETE_WINDOW", self.hide)
        self.prev_hwnd = None
        self.rows = {}
        self.checked = set()   # id() of ticked items
        self._styled = False
        f = UI_FONT
        box_px = max(14, round(self.BOX * self.winfo_fpixels("1i") / 96))
        self.img_off = tk.PhotoImage(data=base64.b64encode(
            encode_png(box_px, box_px, make_checkbox_rgba(box_px, False))).decode("ascii"))
        self.img_on = tk.PhotoImage(data=base64.b64encode(
            encode_png(box_px, box_px, make_checkbox_rgba(box_px, True))).decode("ascii"))
        self.box_px = box_px

        bar = tk.Frame(self, bg=T["bg"], padx=14, pady=12)
        bar.pack(fill="x")
        box = tk.Frame(bar, bg=T["field"], padx=10, pady=7, highlightthickness=1,
                       highlightbackground=T["accent"], highlightcolor=T["accent"])
        box.pack(fill="x")
        tk.Label(box, text="⌕", bg=T["field"], fg=T["muted"], font=(f, 13)).pack(side="left", padx=(0, 6))
        self.q = tk.StringVar()
        self.q.trace_add("write", lambda *_: self.refresh())
        self.entry = tk.Entry(box, textvariable=self.q, bg=T["field"], fg=T["fg"], insertbackground=T["fg"],
                              relief="flat", borderwidth=0, highlightthickness=0, font=(f, 12))
        self.entry.pack(side="left", fill="x", expand=True)
        self.lock_btn = flat_button(box, "🔒 Lock", self.lock)
        self.lock_btn.pack(side="right")
        flat_button(box, "Clear all", self.clear).pack(side="right")

        # selection bar: "Select all" — or, with ticked items, what you can do with them
        self.selbar = tk.Frame(self, bg=T["bg"], padx=14)
        self.selbar.pack(fill="x", pady=(0, 6))
        self.sel_label = tk.Label(self.selbar, bg=T["bg"], fg=T["muted"], font=(f, 9))
        self.sel_label.pack(side="left")
        self.sel_btns = tk.Frame(self.selbar, bg=T["bg"])
        self.sel_btns.pack(side="right")

        self.footer = tk.Label(self, bg=T["bg"], fg=T["faint"], font=(f, 8), anchor="w", padx=16, pady=7)
        self.footer.pack(side="bottom", fill="x")
        tk.Frame(self, bg=T["border"], height=1).pack(side="bottom", fill="x")
        self.preview = tk.Text(self, height=5, wrap="word", bg=T["surface"], fg=T["muted"], relief="flat",
                               borderwidth=0, highlightthickness=0, font=(f, 9), padx=14, pady=8)
        self.preview.pack(side="bottom", fill="x")
        self.preview.configure(state="disabled")

        frame = tk.Frame(self, bg=T["bg"], padx=8)
        frame.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(frame, columns=("age",), show="tree", selectmode="browse")
        self.tree.column("#0", stretch=True)
        self.tree.column("age", width=80, stretch=False, anchor="e")
        self.tree.tag_configure("dim", foreground=T["muted"])
        self.tree.tag_configure("pinned", foreground=T["accent"])
        sb = ttk.Scrollbar(frame, orient="vertical", command=self.tree.yview, style="Thin.Vertical.TScrollbar")
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        for w in (self.entry, self.tree):
            w.bind("<Return>", lambda e: self.pick())
            w.bind("<Shift-Return>", lambda e: self.pick(copy_only=True))
            w.bind("<Control-p>", lambda e: self.toggle_pin())
            w.bind("<Control-s>", lambda e: self.save_to_notes())
            w.bind("<Escape>", lambda e: self._escape())
            w.bind("<Down>", lambda e: self._move(1))
            w.bind("<Up>", lambda e: self._move(-1))
        self.tree.bind("<space>", lambda e: self.toggle_check())
        self.tree.bind("<Control-a>", lambda e: (self.check_all(), "break")[1])
        self.tree.bind("<Delete>", lambda e: self.remove())
        self.entry.bind("<Delete>", lambda e: self.remove() if not self.q.get() else None)
        self.tree.bind("<Button-1>", self._click)
        self.tree.bind("<Double-1>", self._double)
        self.tree.bind("<<TreeviewSelect>>", lambda e: self._show_preview())
        self.tree.bind("<Button-3>", self._menu)
        self.tree.bind("<Key>", self._type)

    # ---- show / hide
    def show(self, prev_hwnd=None):
        self.prev_hwnd = prev_hwnd
        self.q.set("")
        self.checked.clear()
        w, h = 580, 620
        x, y = self.app.winfo_pointerxy()
        mon = next((m for m in list_monitors() if m[0][0] <= x < m[0][2] and m[0][1] <= y < m[0][3]),
                   ((0, 0, self.winfo_screenwidth(), self.winfo_screenheight()), 0, True))
        l, t, r, b = mon[0]
        self.geometry(f"{w}x{h}+{l + (r - l - w) // 2}+{t + max(0, (b - t - h) // 3)}")
        self.refresh()
        self.deiconify()
        if not self._styled:
            dark_titlebar(self)
            self._styled = True
        self.lift()
        if IS_WIN:
            try:
                focus_window(int(self.wm_frame(), 16))
            except Exception:
                log("clipboard focus error:\n" + traceback.format_exc())
        self.entry.focus_force()
        first = next(iter(self.rows), None)
        if first:
            self.tree.selection_set(first)
            self.tree.see(first)

    def hide(self):
        self.withdraw()

    def is_open(self):
        return self.state() == "normal"

    def _escape(self):
        if self.checked:
            self.uncheck_all()
        else:
            self.hide()
        return "break"

    # ---- list
    def refresh(self):
        q = self.q.get().strip().lower()
        sel = self.tree.selection()
        self.tree.delete(*self.tree.get_children())
        self.rows = {}
        alive = {id(it) for it in self.store.items}
        self.checked &= alive
        for i, it in enumerate(self.store.visible()):
            if q and q not in it["text"].lower():
                continue
            one = " ".join(it["text"].split())
            if len(one) > 88:
                one = one[:87] + "…"
            if "\n" in it["text"].strip():
                one += "   ⏎"
            iid = f"c{i}"
            self.tree.insert("", "end", iid=iid, text="   " + one,
                             image=self.img_on if id(it) in self.checked else self.img_off,
                             values=(("★ pinned" if it["pinned"] else _age(it["t"])) + "  ",),
                             tags=("pinned",) if it["pinned"] else ())
            self.rows[iid] = it
        locked = self.store.locked_count()
        if locked:
            self.tree.insert("", "end", iid="locked", tags=("dim",),
                             text=f"  🔒  {locked} older item{'s' if locked > 1 else ''} — Enter to unlock with your notes password")
        if not self.rows and not locked:
            msg = "  Nothing copied yet." if not q else "  No match."
            if not SETTINGS.get("clip_enabled", True):
                msg = "  Clipboard history is turned off in settings.ini ([clipboard] enabled = yes)."
            self.tree.insert("", "end", iid="empty", text=msg, tags=("dim",))
        if not self.store.can_save and SETTINGS.get("clip_save") and SETTINGS.get("clip_enabled", True):
            self.footer.configure(text="Kept until the launcher quits — create a notes password (Alt+N) "
                                       "to keep it encrypted after restarts.")
        elif not SETTINGS.get("clip_save", True):
            self.footer.configure(text="Not saved after a restart (save_history = no in settings.ini).")
        else:
            self.footer.configure(text="Click a box (or Space) to tick items    Enter paste    Ctrl+S save to Notes    "
                                       "Del delete    Esc close")
        if self.store.mode == "password":
            self.lock_btn.pack(side="right")
        else:
            self.lock_btn.pack_forget()
        keep = sel[0] if sel and self.tree.exists(sel[0]) else next(iter(self.rows), None)
        if keep:
            self.tree.selection_set(keep)
            self.tree.see(keep)
        self._update_selbar()
        self._show_preview()

    def _update_selbar(self):
        for w in self.sel_btns.winfo_children():
            w.destroy()
        n = len(self.checked)
        if n:
            self.sel_label.configure(text=f"{n} selected", fg=T["accent"])
            for text, cmd in (("Save to Notes", self.save_to_notes), ("Paste", self.pick),
                              ("Copy", lambda: self.pick(copy_only=True)), ("Pin", self.toggle_pin),
                              ("Delete", self.remove), ("Unselect", self.uncheck_all)):
                flat_button(self.sel_btns, text, cmd).pack(side="left")
        else:
            self.sel_label.configure(text="Tick items to save several to Notes, paste or delete them together",
                                     fg=T["faint"])
            if self.rows:
                flat_button(self.sel_btns, "Select all", self.check_all).pack(side="left")

    def _move(self, step):
        rows = [i for i in self.tree.get_children() if i in self.rows or i == "locked"]
        if not rows:
            return "break"
        sel = self.tree.selection()
        i = rows.index(sel[0]) + step if sel and sel[0] in rows else 0
        i = max(0, min(i, len(rows) - 1))
        self.tree.selection_set(rows[i])
        self.tree.see(rows[i])
        return "break"

    def _type(self, e):
        if e.char and e.char.isprintable() and e.char != " " and not e.state & 0x4:
            self.entry.focus_set()
            self.entry.insert("end", e.char)
            return "break"
        return None

    def _on_box(self, e):
        """Was the click on the checkbox at the start of the row?"""
        bbox = self.tree.bbox(self.tree.identify_row(e.y), "#0")
        return bool(bbox) and e.x <= bbox[0] + self.box_px + 10

    def _click(self, e):
        iid = self.tree.identify_row(e.y)
        if iid in self.rows and self._on_box(e):
            self.tree.selection_set(iid)
            self.toggle_check(iid)
            return "break"
        return None

    def _double(self, e):
        iid = self.tree.identify_row(e.y)
        if iid in self.rows and self._on_box(e):
            self.toggle_check(iid)  # a quick second click on the box ticks it again, it doesn't paste
            return "break"
        return self.pick()

    def toggle_check(self, iid=None):
        iid = iid or (self.tree.selection() or (None,))[0]
        it = self.rows.get(iid)
        if not it:
            return "break"
        self.checked.symmetric_difference_update({id(it)})
        self.tree.item(iid, image=self.img_on if id(it) in self.checked else self.img_off)
        self._update_selbar()
        self._show_preview()
        return "break"

    def check_all(self):
        for iid, it in self.rows.items():
            self.checked.add(id(it))
            self.tree.item(iid, image=self.img_on)
        self._update_selbar()
        self._show_preview()

    def uncheck_all(self):
        self.checked.clear()
        for iid in self.rows:
            self.tree.item(iid, image=self.img_off)
        self._update_selbar()
        self._show_preview()

    def _targets(self):
        """Ticked items (in list order) — or the highlighted one if nothing is ticked."""
        if self.checked:
            return [it for it in self.store.visible() if id(it) in self.checked]
        sel = self.tree.selection()
        it = self.rows.get(sel[0]) if sel else None
        return [it] if it else []

    def _show_preview(self):
        items = self._targets()
        self.preview.configure(state="normal")
        self.preview.delete("1.0", "end")
        if len(items) == 1:
            self.preview.insert("1.0", items[0]["text"][:5000])
        elif items:
            self.preview.insert("1.0", f"{len(items)} items — Paste puts them one per line; Save to Notes adds "
                                       f"them to a note.\n\n" + "\n".join(it["text"] for it in items)[:5000])
        self.preview.configure(state="disabled")

    # ---- actions
    def pick(self, copy_only=False):
        sel = self.tree.selection()
        if sel and sel[0] == "locked" and not self.checked:
            self.unlock()
            return "break"
        items = self._targets()
        if not items:
            return "break"
        text = "\n".join(it["text"] for it in items)
        try:
            set_clipboard_text(text, int(self.app.wm_frame(), 16) if IS_WIN else None)
        except OSError as e:
            self.footer.configure(text=f"Couldn't copy: {e}")
            return "break"
        if len(items) == 1:
            self.store.add(text)       # move it to the top
        else:
            self.store.ignore = text   # don't add the combined text as a new item
        self.checked.clear()
        self.hide()
        target = self.prev_hwnd
        if IS_WIN and not copy_only and SETTINGS.get("clip_paste", True) and target and user32.IsWindow(target):
            try:
                focus_window(target)
                self.after(90, send_ctrl_v)
            except Exception:
                log("paste error:\n" + traceback.format_exc())
        else:
            self.app._set_status("Copied to the clipboard ✓")
        return "break"

    def remove(self):
        items = self._targets()
        if not items:
            return "break"
        if len(items) > 1 and not messagebox.askyesno("Delete", f"Delete {len(items)} items?", parent=self):
            return "break"
        rows = [i for i in self.tree.get_children() if i in self.rows]
        sel = self.tree.selection()
        pos = rows.index(sel[0]) if sel and sel[0] in rows else 0
        self.store.remove(items)
        self.checked.clear()
        self.refresh()
        rows = [i for i in self.tree.get_children() if i in self.rows]
        if rows:
            nxt = rows[min(pos, len(rows) - 1)]
            self.tree.selection_set(nxt)
            self.tree.see(nxt)
        return "break"

    def toggle_pin(self):
        items = self._targets()
        if items:
            self.store.toggle_pin(items)
            self.refresh()
        return "break"

    def save_to_notes(self):
        items = self._targets()
        if not items:
            return "break"
        self.app.clip_to_notes("\n\n".join(it["text"] for it in items), len(items))
        self.uncheck_all()
        return "break"

    def clear(self):
        n = len([it for it in self.store.items if not it["pinned"]])
        if n and messagebox.askyesno("Clear clipboard history",
                                     f"Delete all {n} item{'s' if n > 1 else ''}? Pinned items are kept.", parent=self):
            self.store.clear()
            self.checked.clear()
            self.refresh()

    def unlock(self):
        if not os.path.isfile(CLIP_KEY_FILE):
            messagebox.showinfo("Clipboard history", "Create a notes password first (Alt+N).", parent=self)
            return
        notes = self.app.notes
        vault = (notes.key, notes.salt, notes.kdf) if notes is not None and notes.unlocked else None
        try:
            if vault:
                priv = clip_private_key(vault=vault)
            else:
                pw = simpledialog.askstring("Clipboard history", "Notes password:", show="•", parent=self)
                if pw is None:
                    return
                priv = clip_private_key(password=pw)
        except ValueError:
            messagebox.showerror("Clipboard history", "Wrong password.", parent=self)
            return
        except Exception as e:
            log("clipboard unlock error:\n" + traceback.format_exc())
            messagebox.showerror("Clipboard history", f"Couldn't unlock: {e}", parent=self)
            return
        self.store.unlock(priv)
        self.refresh()
        self.entry.focus_set()

    def lock(self):
        self.store.lock()
        self.refresh()

    def _menu(self, e):
        iid = self.tree.identify_row(e.y)
        if iid not in self.rows:
            return
        it = self.rows[iid]
        if not self.checked or id(it) not in self.checked:
            self.tree.selection_set(iid)
            if self.checked and id(it) not in self.checked:
                self.uncheck_all()
        items = self._targets()
        n = len(items)
        many = f" {n} items" if n > 1 else ""
        m = tk.Menu(self, tearoff=0)
        m.add_command(label="Paste" + many, command=self.pick)
        m.add_command(label="Copy" + many, command=lambda: self.pick(copy_only=True))
        m.add_command(label="Save to Notes…" if n == 1 else f"Save{many} to Notes…", command=self.save_to_notes)
        if n == 1 and items[0].get("text"):
            m.add_command(label="Save as snippet…", command=lambda: self.app.open_snippets(items[0]["text"]))
        m.add_command(label="Unpin" if all(x["pinned"] for x in items) else "Pin", command=self.toggle_pin)
        if n == 1:
            m.add_command(label="Untick" if id(it) in self.checked else "Tick", command=lambda: self.toggle_check(iid))
        m.add_separator()
        m.add_command(label="Delete" + many, command=self.remove)
        m.tk_popup(e.x_root, e.y_root)


# ======================================================================
# Alt+Enter cycle: really switch to each open window in turn; Enter stays, Esc goes back
# ======================================================================

class CycleBanner(tk.Toplevel):
    """A small always-on-top banner that never takes the focus."""

    def __init__(self, app):
        super().__init__(app)
        self.withdraw()
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.configure(bg=T["accent"])
        f = UI_FONT
        card = tk.Frame(self, bg=T["bg"], padx=14, pady=8)
        card.pack(padx=1, pady=1)
        self.count = tk.Label(card, bg=T["accent"], fg="#0b0d12", font=(f, 10, "bold"), padx=8)
        self.count.pack(side="left", padx=(0, 10))
        self.icon = tk.Label(card, bg=T["bg"])
        self.icon.pack(side="left", padx=(0, 6))
        self.name = tk.Label(card, bg=T["bg"], fg=T["fg"], font=(f, 11), width=34, anchor="w")
        self.name.pack(side="left")
        tk.Label(card, text="Enter stay  ·  Esc back  ·  Del close  ·  M minimize", bg=T["bg"], fg=T["faint"],
                 font=(f, 8)).pack(side="left", padx=(10, 0))
        self.bar = tk.Frame(self, bg=T["accent"], height=2)
        self._styled = False

    def show(self, n, total, name, image):
        self.count.configure(text=f"{n} / {total}")
        self.name.configure(text=name if len(name) <= 40 else name[:39] + "…")
        self.icon.configure(image=image)
        if self.state() != "normal":
            self.update_idletasks()
            w, h = self.winfo_reqwidth(), self.winfo_reqheight()
            x, y = self.master.winfo_pointerxy()
            mon = next((m for m in list_monitors() if m[0][0] <= x < m[0][2] and m[0][1] <= y < m[0][3]),
                       ((0, 0, self.winfo_screenwidth(), self.winfo_screenheight()), 0, True))
            l, t, r, _b = mon[0]
            self.geometry(f"{w}x{h}+{l + (r - l - w) // 2}+{t + 24}")
            self.deiconify()
            if IS_WIN and not self._styled:
                try:  # don't take the focus, don't show in Alt+Tab
                    hwnd = int(self.wm_frame(), 16)
                    ex = user32.GetWindowLongW(hwnd, -20)
                    user32.SetWindowLongW(hwnd, -20, ex | 0x08000000 | 0x80)
                except Exception:
                    log("banner style error:\n" + traceback.format_exc())
                self._styled = True
        self.lift()

    def hide(self):
        self.withdraw()


def _ease(t):
    """Smooth start and end (ease-in-out cubic)."""
    return 4 * t * t * t if t < .5 else 1 - (-2 * t + 2) ** 3 / 2


def _ease_out(t):
    return 1 - (1 - t) ** 3


class TransitionFX(tk.Toplevel):
    """A click-through, never-focused overlay over one monitor that draws the cycle's transitions.
    It stays mapped (fully transparent) while a cycle runs, so showing it never steals the focus."""
    KEY = "#010203"  # this colour is see-through
    FRAME_MS = 15

    def __init__(self, app):
        super().__init__(app)
        self.withdraw()
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.configure(bg=self.KEY)
        try:
            self.attributes("-transparentcolor", self.KEY)
        except tk.TclError:
            pass  # not on Windows: fine for tests
        self.cv = tk.Canvas(self, bg=self.KEY, highlightthickness=0, borderwidth=0)
        self.cv.pack(fill="both", expand=True)
        self._job = None
        self._pending = None   # switch still to do (fade / wipe switch half-way)
        self._mapped = False
        self._styled = False
        self.mon = (0, 0, 1, 1)

    # ---- set-up
    def _alpha(self, a):
        try:
            self.attributes("-alpha", max(0.0, min(1.0, a)))
        except tk.TclError:
            pass

    def ensure_mapped(self):
        """Map the overlay once (transparent) at the start of a cycle."""
        if self._mapped:
            return
        self._alpha(0)
        self.geometry("1x1+0+0")
        self.deiconify()
        self.update_idletasks()
        if IS_WIN and not self._styled:
            try:  # click-through, never activated, not in Alt+Tab
                hwnd = int(self.wm_frame(), 16)
                ex = user32.GetWindowLongW(hwnd, -20)
                user32.SetWindowLongW(hwnd, -20, ex | 0x20 | 0x80 | 0x08000000)
            except Exception:
                log("transition style error:\n" + traceback.format_exc())
            self._styled = True
        self._mapped = True

    def _cover(self, rect):
        """Cover the monitor that holds this window rectangle."""
        l, t, r, b = rect
        cx, cy = (l + r) // 2, (t + b) // 2
        mons = list_monitors()
        mon = next((m[0] for m in mons if m[0][0] <= cx < m[0][2] and m[0][1] <= cy < m[0][3]),
                   mons[0][0] if mons else (0, 0, self.winfo_screenwidth(), self.winfo_screenheight()))
        self.mon = mon
        self.geometry(f"{mon[2] - mon[0]}x{mon[3] - mon[1]}+{mon[0]}+{mon[1]}")
        self.cv.delete("all")

    def _local(self, rect):
        l, t, r, b = rect
        return l - self.mon[0], t - self.mon[1], r - self.mon[0], b - self.mon[1]

    def _rounded(self, x0, y0, x1, y1, rad, **kw):
        rad = max(0, min(rad, (x1 - x0) / 2, (y1 - y0) / 2))
        pts = [x0 + rad, y0, x1 - rad, y0, x1, y0, x1, y0 + rad, x1, y1 - rad, x1, y1, x1 - rad, y1,
               x0 + rad, y1, x0, y1, x0, y1 - rad, x0, y0 + rad, x0, y0]
        return self.cv.create_polygon(pts, smooth=True, **kw)

    def _outline(self, rect):
        x0, y0, x1, y1 = rect
        self._rounded(x0 - 6, y0 - 6, x1 + 6, y1 + 6, 16, outline=T["select"], fill="", width=8)
        self._rounded(x0 - 3, y0 - 3, x1 + 3, y1 + 3, 13, outline=T["accent"], fill="", width=3)

    # ---- playing
    def _finish_now(self):
        if self._job:
            self.after_cancel(self._job)
            self._job = None
        if self._pending:
            fn, self._pending = self._pending, None
            fn()
        self.cv.delete("all")
        self._alpha(0)

    def _run(self, duration, frame, done=None):
        start = time.monotonic()

        def tick():
            t = min(1.0, (time.monotonic() - start) * 1000 / duration)
            try:
                frame(t)
            except tk.TclError:
                return
            if t < 1:
                self._job = self.after(self.FRAME_MS, tick)
            else:
                self._job = None
                self.cv.delete("all")
                self._alpha(0)
                if done:
                    done()
        tick()

    def play(self, style, hwnd, switch):
        """Animate the move to hwnd. switch() brings the window up — at the right moment for the style."""
        self._finish_now()
        if style == "none" or not self._mapped:
            switch()
            return
        duration = max(60, min(SETTINGS.get("transition_ms", 260), int(SETTINGS.get("cycle_step_ms", 500) * .8)))
        if style in ("highlight", "zoom"):
            switch()  # show the window first, then draw on top of it
            self.update_idletasks()
        try:
            target = window_rect(hwnd)
        except Exception:
            target = (0, 0, 800, 600)
        self._cover(target)
        local = self._local(target)
        w, h = self.mon[2] - self.mon[0], self.mon[3] - self.mon[1]

        if style == "fade":
            dim = self.cv.create_rectangle(0, 0, w, h, fill=T["bg"], outline="")
            self._pending = switch

            def frame(t):
                if t >= .5 and self._pending:
                    fn, self._pending = self._pending, None
                    fn()
                    self.lift()
                self._alpha(.9 * (_ease(t / .5) if t < .5 else 1 - _ease((t - .5) / .5)))
                self.cv.itemconfigure(dim, fill=T["bg"])
            self._run(duration, frame)
        elif style == "wipe":
            panel = self.cv.create_rectangle(0, 0, 0, h, fill=T["bg"], outline="")
            edge = self.cv.create_rectangle(0, 0, 0, h, fill=T["accent"], outline="")
            self._pending = switch
            self._alpha(.95)

            def frame(t):
                if t < .5:
                    x0, x1 = 0, w * _ease(t / .5)
                else:
                    if self._pending:
                        fn, self._pending = self._pending, None
                        fn()
                        self.lift()
                    x0, x1 = w * _ease((t - .5) / .5), w
                self.cv.coords(panel, x0, 0, x1, h)
                ex = x1 if t < .5 else x0
                self.cv.coords(edge, ex - 3, 0, ex + 3, h)
            self._run(duration, frame)
        elif style == "zoom":
            x0, y0, x1, y1 = local
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2

            def frame(t):
                self.cv.delete("all")
                grow = _ease_out(min(1.0, t / .6))
                s = .35 + .65 * grow
                hw, hh = (x1 - x0) / 2 * s, (y1 - y0) / 2 * s
                self._outline((cx - hw, cy - hh, cx + hw, cy + hh))
                self._alpha(1.0 if t < .6 else 1 - _ease((t - .6) / .4))
            self._run(duration, frame)
        else:  # highlight
            self._outline(local)

            def frame(t):
                self._alpha(1 - _ease(t))
            self._run(max(duration, 300), frame)

    def flash(self, hwnd, done=None):
        """A short outline on the window you chose, then hide."""
        self._finish_now()
        try:
            self._cover(window_rect(hwnd))
            self._outline(self._local(window_rect(hwnd)))
        except Exception:
            self.hide()
            return
        self._run(380, lambda t: self._alpha(1 - _ease(t)), done or self.hide)

    def hide(self):
        if self._job:
            self.after_cancel(self._job)
            self._job = None
        self._pending = None
        self.cv.delete("all")
        self._alpha(0)
        self.withdraw()
        self._mapped = False


class Cycler:
    MAX_ROUNDS = 3  # nobody pressed anything after this many rounds → go back and stop

    def __init__(self, app):
        self.app = app
        self.active = False
        self.order, self.idx, self.origin, self.current = [], 0, None, None
        self.was_min = set()
        self.steps = 0
        self._job = None
        self.banner = None
        self.fx = None

    def start(self):
        app = self.app
        self.order = [w["hwnd"] for w in app.windows]
        if len(self.order) < 2:
            app._set_status("Open at least two windows to cycle through them.")
            return
        self.origin = user32.GetForegroundWindow() if IS_WIN else None
        self.idx = self.order.index(self.origin) if self.origin in self.order else -1
        self.current, self.steps, self.active = None, 0, True
        self.was_min = {h for h in self.order if IS_WIN and user32.IsIconic(h)}
        if app.hotkeys:
            app.hotkeys.capture = {0x0D: "cycle_stop", 0x1B: "cycle_cancel", 0x2E: "cycle_close",
                                   0x4D: "cycle_min"}
        if self.banner is None or not self.banner.winfo_exists():
            self.banner = CycleBanner(app)
        if self.fx is None or not self.fx.winfo_exists():
            self.fx = TransitionFX(app)
        if SETTINGS.get("transition", "fade") != "none":
            self.fx.ensure_mapped()
        self._step()

    def _step(self):
        if not self.active:
            return
        alive = [h for h in self.order if not IS_WIN or user32.IsWindow(h)]
        if len(alive) < 2 or self.steps >= self.MAX_ROUNDS * len(self.order):
            self.stop(go_back=True)
            return
        prev = self.current
        self.idx = (self.idx + 1) % len(self.order)
        h = self.order[self.idx]
        if IS_WIN and not user32.IsWindow(h):
            self._job = self.app.after(1, self._step)
            return
        self.current = h
        self.steps += 1
        w = next((x for x in self.app.windows if x["hwnd"] == h), None)
        try:
            self.banner.show(self.idx + 1, len(self.order), w["name"] if w else "?",
                             self.app.icons.get(w["icon_key"], self.app.blank) if w else self.app.blank)
        except tk.TclError:
            pass
        def switch():
            if not self.active or not IS_WIN:
                return
            try:
                focus_window(h)
                if prev in self.was_min and prev != h:  # it was minimized before: put it back
                    user32.ShowWindow(prev, 7)  # SW_SHOWMINNOACTIVE
            except Exception:
                log("cycle error:\n" + traceback.format_exc())
            self.banner.lift()

        try:
            self.fx.play(SETTINGS.get("transition", "fade"), h, switch)
        except Exception:
            log("transition error:\n" + traceback.format_exc())
            switch()
        self._job = self.app.after(SETTINGS.get("cycle_step_ms", 500), self._step)

    def act(self, what):
        """Del / M while cycling: close / minimize the window you see, then go on to the next one."""
        if not self.active or not self.current:
            return
        h = self.current
        w = next((x for x in self.app.windows if x["hwnd"] == h), None)
        name = w["name"] if w else "window"
        try:
            if what == "close":
                close_window(h)
            else:
                minimize_window(h, activate_next=False)
        except Exception:
            log("cycle action error:\n" + traceback.format_exc())
        if h in self.order:
            i = self.order.index(h)
            self.order.pop(i)
            if i <= self.idx:
                self.idx -= 1
        self.was_min.discard(h)
        if self.origin == h:
            self.origin = None
        self.current = None
        self.app.toast(("Closed " if what == "close" else "Minimized ") + (name if len(name) <= 40 else name[:39] + "…"))
        if self._job:
            self.app.after_cancel(self._job)
            self._job = None
        if len(self.order) < 2:
            self.current = self.order[0] if self.order else None
            self.stop()
            return
        self._step()

    def stop(self, go_back=False):
        if not self.active:
            return
        self.active = False
        if self._job:
            self.app.after_cancel(self._job)
            self._job = None
        if self.app.hotkeys:
            self.app.hotkeys.capture = {}
        if self.banner is not None and self.banner.winfo_exists():
            self.banner.hide()
        target = self.origin if go_back else self.current
        if self.fx is not None and self.fx.winfo_exists():
            self.fx._pending = None  # a half-done fade/wipe must not switch any more
            if not go_back and target and SETTINGS.get("transition", "fade") != "none":
                self.fx.flash(target)
            else:
                self.fx.hide()
        if IS_WIN and go_back and self.current in self.was_min and self.current != self.origin:
            user32.ShowWindow(self.current, 7)
        if IS_WIN and target and user32.IsWindow(target):
            try:
                focus_window(target)
            except Exception:
                log("cycle stop error:\n" + traceback.format_exc())
        w = next((x for x in self.app.windows if x["hwnd"] == target), None)
        if w and not go_back:
            self.app.usage.bump(self.app._usage_key(w))
        name = w["name"] if w else "the window"
        self.app._set_status(("Back to " if go_back else "Stayed on ") + name + ".")


# ======================================================================
# Alt+Space switcher: the highlight walks through your open windows; Enter picks
# ======================================================================

class SwitcherOverlay(tk.Toplevel):
    def __init__(self, app):
        super().__init__(app)
        self.withdraw()
        self.app = app
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.configure(bg=T["accent"])
        self.items, self.rows, self.idx, self._job, self._open = [], [], 0, None, False
        self.minimized, self.paused = set(), False
        self.card = tk.Frame(self, bg=T["bg"], padx=16, pady=14)
        self.card.pack(padx=1, pady=1)
        self.bind("<Return>", lambda e: self.pick())
        self.bind("<KP_Enter>", lambda e: self.pick())
        self.bind("<Escape>", lambda e: self.hide())
        self.bind("<Down>", lambda e: self._step(1, manual=True))
        self.bind("<Up>", lambda e: self._step(-1, manual=True))
        self.bind("<space>", lambda e: self._pause())
        self.bind("<Delete>", lambda e: self.act("close"))
        self.bind("<Key>", self._key)
        self.bind("<FocusOut>", lambda e: self.after(200, self._check_focus) if e.widget is self else None)

    def is_open(self):
        return self._open

    def show(self):
        self.items = list(self.app.windows)
        if not self.items:
            self.app._set_status("No open windows.")
            return
        fg = user32.GetForegroundWindow() if IS_WIN else None
        hwnds = [w["hwnd"] for w in self.items]
        self.idx = (hwnds.index(fg) + 1) % len(hwnds) if fg in hwnds else 0
        self.paused = False
        self.minimized = set()
        self._render()
        self._open = True
        self.deiconify()
        self.lift()
        if IS_WIN:
            try:
                focus_window(int(self.wm_frame(), 16))
            except Exception:
                log("switcher focus error:\n" + traceback.format_exc())
        self.focus_force()
        self._paint()
        self._schedule()

    def _render(self):
        """Build the rows for self.items and size / place the overlay."""
        for w in self.card.winfo_children():
            w.destroy()
        f = UI_FONT
        head = tk.Frame(self.card, bg=T["bg"])
        head.grid(row=0, column=0, columnspan=8, sticky="we", pady=(0, 10))
        tk.Label(head, text="Switch to…", bg=T["bg"], fg=T["fg"], font=(f, 12, "bold")).pack(side="left")
        tk.Label(head, text="   Space / ↓ next  ·  ↑ previous  ·  Enter switch  ·  1-9 jump  ·  Del close  ·  "
                            "M minimize  ·  Esc cancel",
                 bg=T["bg"], fg=T["faint"], font=(f, 8)).pack(side="left")
        n = len(self.items)
        cols = 1 if n <= 10 else 2 if n <= 24 else 3
        per_col = -(-n // cols)
        self.rows = []
        for i, w in enumerate(self.items):
            row = tk.Frame(self.card, bg=T["bg"], padx=8, pady=6, cursor="hand2")
            row.grid(row=1 + i % per_col, column=i // per_col, sticky="we", padx=(0, 10 if cols > 1 else 0))
            num = tk.Label(row, text=str(i + 1), width=3, bg=T["field"], fg=T["fg"], font=(f, 11, "bold"))
            num.pack(side="left", padx=(0, 10))
            icon = tk.Label(row, image=self.app.icons.get(w["icon_key"], self.app.blank), bg=T["bg"])
            icon.pack(side="left", padx=(0, 8))
            name = w["name"] if len(w["name"]) <= 52 else w["name"][:51] + "…"
            lbl = tk.Label(row, text=name, bg=T["bg"], fg=T["fg"], font=(f, 11), anchor="w", width=40 if cols == 1 else 30)
            lbl.pack(side="left")
            key = slot_label(w["slot"]) if w.get("slot") is not None else ""
            if w["hwnd"] in self.minimized:
                key = "minimized"
            kl = tk.Label(row, text=key, bg=T["bg"], fg=T["faint"], font=(f, 9), width=9, anchor="e")
            kl.pack(side="left")
            for wid in (row, num, icon, lbl, kl):
                wid.bind("<Button-1>", lambda e, i=i: self.pick(i))
            self.rows.append((row, num, (icon, lbl, kl)))
        self.idx = min(self.idx, n - 1)
        self.update_idletasks()
        w, h = self.winfo_reqwidth(), self.winfo_reqheight()
        x, y = self.app.winfo_pointerxy()
        mon = next((m for m in list_monitors() if m[0][0] <= x < m[0][2] and m[0][1] <= y < m[0][3]),
                   ((0, 0, self.winfo_screenwidth(), self.winfo_screenheight()), 0, True))
        l, t, r, b = mon[0]
        self.geometry(f"{w}x{h}+{l + (r - l - w) // 2}+{t + max(0, (b - t - h) // 3)}")

    def act(self, what):
        """Del: close the highlighted window. M: minimize it. The switcher stays open."""
        if not self._open or not self.items:
            return "break"
        i = self.idx
        w = self.items[i]
        try:
            if what == "close":
                close_window(w["hwnd"])
            else:
                minimize_window(w["hwnd"], activate_next=False)
        except Exception:
            log("switcher action error:\n" + traceback.format_exc())
        if what == "close":
            self.items.pop(i)
            if not self.items:
                self.hide()
                return "break"
            self.idx = i % len(self.items)
        else:
            self.minimized.add(w["hwnd"])
            self.idx = (i + 1) % len(self.items)
        self._render()
        self._paint()
        self._schedule()
        self.after(10, self.focus_force)
        return "break"

    def _paint(self):
        for i, (row, num, rest) in enumerate(self.rows):
            on = i == self.idx
            bg = T["select"] if on else T["bg"]
            row.configure(bg=bg)
            num.configure(bg=T["accent"] if on else T["field"], fg="#0b0d12" if on else T["fg"])
            for wid in rest:
                wid.configure(bg=bg)

    def _schedule(self):
        """The highlight only moves when you press a key (no timer)."""
        if self._job:
            self.after_cancel(self._job)
            self._job = None

    def _step(self, d, manual=False):
        if not self._open or not self.rows:
            return "break"
        self.idx = (self.idx + d) % len(self.rows)
        self._paint()
        self._schedule()
        return "break"

    def _pause(self):
        return self._step(1, manual=True)  # Space: next window

    def _key(self, e):
        if e.char in ("m", "M"):
            return self.act("minimize")
        if e.char and e.char.isdigit():
            i = (int(e.char) - 1) % 10
            if i < len(self.items):
                self.pick(i)
            return "break"
        return None

    def _check_focus(self):
        if self._open and self.focus_get() is None:
            self.hide()

    def pick(self, i=None):
        if not self._open:
            return "break"
        i = self.idx if i is None else i
        w = self.items[i] if 0 <= i < len(self.items) else None
        self.hide()
        if w:
            self.app.usage.bump(self.app._usage_key(w))
            try:
                result = focus_window(w["hwnd"]) if IS_WIN else True
            except Exception:
                log("switch error:\n" + traceback.format_exc())
                result = "error"
            self.app._show_switch(f"{MOD_LABEL}{CMD_KEYS.get('switcher', '')}", w["hwnd"], result)
        return "break"

    def hide(self):
        self._open = False
        if self._job:
            self.after_cancel(self._job)
            self._job = None
        self.withdraw()


# ======================================================================
# Small overlays: a toast message and reminder pop-ups (both never take the focus)
# ======================================================================

def _no_activate(win):
    """Don't take the focus when shown or clicked, and don't show in Alt+Tab."""
    if not IS_WIN:
        return
    try:
        hwnd = int(win.wm_frame(), 16)
        user32.SetWindowLongW(hwnd, -20, user32.GetWindowLongW(hwnd, -20) | 0x08000000 | 0x80)
    except Exception:
        log("overlay style error:\n" + traceback.format_exc())


def _pointer_monitor(win):
    x, y = win.winfo_pointerxy()
    return next((m[0] for m in list_monitors() if m[0][0] <= x < m[0][2] and m[0][1] <= y < m[0][3]),
                (0, 0, win.winfo_screenwidth(), win.winfo_screenheight()))


def play_alert():
    if not SETTINGS.get("timer_sound", True):
        return
    try:
        if IS_WIN:
            import winsound
            winsound.PlaySound("SystemNotification", winsound.SND_ALIAS | winsound.SND_ASYNC)
    except Exception:
        log("sound error:\n" + traceback.format_exc())


class Toast(tk.Toplevel):
    """A short message near the top of the screen (e.g. "Always on top: on")."""

    def __init__(self, app):
        super().__init__(app)
        self.withdraw()
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.configure(bg=T["accent"])
        card = tk.Frame(self, bg=T["bg"], padx=16, pady=9)
        card.pack(padx=1, pady=1)
        self.label = tk.Label(card, bg=T["bg"], fg=T["fg"], font=(UI_FONT, 11))
        self.label.pack()
        self._job = None
        self._styled = False
        self.last = ""

    def show(self, text, ms=1600):
        self.last = text
        self.label.configure(text=text)
        self.update_idletasks()
        w, h = self.winfo_reqwidth(), self.winfo_reqheight()
        l, t, r, _b = _pointer_monitor(self)
        self.geometry(f"{w}x{h}+{l + (r - l - w) // 2}+{t + 70}")
        if self.state() != "normal":
            self.deiconify()
            if not self._styled:
                _no_activate(self)
                self._styled = True
        self.lift()
        if self._job:
            self.after_cancel(self._job)
        self._job = self.after(ms, self.withdraw)


class ReminderPopup(tk.Toplevel):
    """A timer / reminder that went off. Stays until you click a button."""
    WIDTH = 340

    def __init__(self, app, title, detail, buttons):
        super().__init__(app)
        self.withdraw()
        self.app = app
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.configure(bg=T["accent"])
        card = tk.Frame(self, bg=T["bg"], padx=16, pady=12)
        card.pack(padx=1, pady=1, fill="both", expand=True)
        top = tk.Frame(card, bg=T["bg"])
        top.pack(fill="x")
        tk.Label(top, text="⏰", bg=T["bg"], fg=T["accent"], font=(UI_FONT, 18)).pack(side="left", padx=(0, 10))
        texts = tk.Frame(top, bg=T["bg"])
        texts.pack(side="left", fill="x", expand=True)
        self.title_label = tk.Label(texts, text=title, bg=T["bg"], fg=T["fg"], font=(UI_FONT, 12, "bold"),
                                    anchor="w", justify="left", wraplength=self.WIDTH - 90)
        self.title_label.pack(fill="x")
        tk.Label(texts, text=detail, bg=T["bg"], fg=T["muted"], font=(UI_FONT, 9), anchor="w", justify="left",
                 wraplength=self.WIDTH - 90).pack(fill="x")
        row = tk.Frame(card, bg=T["bg"])
        row.pack(fill="x", pady=(10, 0))
        self.buttons = {}
        for i, (text, cb) in enumerate(buttons):
            b = tk.Label(row, text=text, bg=T["accent"] if i == 0 else T["field"],
                         fg="#0b0d12" if i == 0 else T["fg"], font=(UI_FONT, 9, "bold" if i == 0 else "normal"),
                         padx=10, pady=4, cursor="hand2")
            b.pack(side="right", padx=(6, 0))
            b.bind("<Button-1>", lambda e, cb=cb: self._click(cb))
            self.buttons[text] = cb

    def _click(self, cb):
        self.close()
        try:
            cb()
        except Exception:
            log("reminder button error:\n" + traceback.format_exc())

    def place_at(self, index):
        """Bottom-right of the main screen; later pop-ups stack above earlier ones."""
        self.update_idletasks()
        h = self.winfo_reqheight()
        mons = list_monitors()
        l, t, r, b = mons[0][0] if mons else (0, 0, self.winfo_screenwidth(), self.winfo_screenheight())
        below = 0
        for p in self.app.popups[:index]:
            try:
                below += p.winfo_reqheight() + 10
            except tk.TclError:
                pass
        y = b - 16 - below - h
        self.geometry(f"{self.WIDTH}x{h}+{r - self.WIDTH - 16}+{max(t, y)}")

    def show(self, index):
        self.place_at(index)
        self.deiconify()
        _no_activate(self)
        self.lift()

    def close(self):
        if self in self.app.popups:
            self.app.popups.remove(self)
        try:
            self.destroy()
        except tk.TclError:
            pass
        for i, p in enumerate(self.app.popups):
            p.place_at(i)


# ======================================================================
# Cheat sheet (hold the modifier) and quick note box
# ======================================================================

CMD_LABELS = (("toggle", "Show / hide launcher"), ("search", "Search"), ("notes", "Notes"),
              ("quicknote", "Quick note"), ("clipboard", "Clipboard history"), ("next", "Next window"),
              ("prev", "Previous window"), ("switcher", "Switcher"), ("cycle", "Cycle windows"),
              ("topmost", "Always on top"), ("opacity", "See-through"), ("mute", "Mute app"),
              ("explorer", "File explorer"))


class CheatSheet(tk.Toplevel):
    """Every shortcut at a glance, shown while you hold the modifier. Never takes the focus."""
    MAX_WINDOWS = 20

    def __init__(self, app):
        super().__init__(app)
        self.withdraw()
        self.app = app
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.configure(bg=T["accent"])
        self.card = tk.Frame(self, bg=T["bg"], padx=20, pady=16)
        self.card.pack(padx=1, pady=1)
        self._styled = False

    def _key(self, parent, text, row, col):
        tk.Label(parent, text=text, bg=T["field"], fg=T["fg"], font=(UI_FONT, 9, "bold"), padx=6,
                 anchor="center").grid(row=row, column=col, sticky="we", padx=(0, 10), pady=2)

    def _section(self, parent, title, row):
        tk.Label(parent, text=title, bg=T["bg"], fg=T["muted"], font=(UI_FONT, 8, "bold"), anchor="w"
                 ).grid(row=row, column=0, columnspan=3, sticky="w", pady=(10 if row else 0, 4))

    def show(self):
        app = self.app
        for w in self.card.winfo_children():
            w.destroy()
        f = UI_FONT
        mod = MOD_LABEL.rstrip("+")
        head = tk.Frame(self.card, bg=T["bg"])
        head.pack(fill="x", pady=(0, 8))
        tk.Label(head, text="Shortcuts", bg=T["bg"], fg=T["fg"], font=(f, 13, "bold")).pack(side="left")
        tk.Label(head, text=f"   keep holding {mod} and press a key  ·  let go to close", bg=T["bg"], fg=T["faint"],
                 font=(f, 8)).pack(side="left")
        cols = tk.Frame(self.card, bg=T["bg"])
        cols.pack(fill="both")
        left = tk.Frame(cols, bg=T["bg"])
        left.pack(side="left", anchor="n", padx=(0, 28))
        right = tk.Frame(cols, bg=T["bg"])
        right.pack(side="left", anchor="n")

        # windows on number / letter keys
        self._section(left, "OPEN WINDOWS", 0)
        r = 1
        wins = [w for w in app.windows if w.get("slot") is not None]
        for w in wins[:self.MAX_WINDOWS]:
            self._key(left, slot_label(w["slot"]), r, 0)
            tk.Label(left, image=app.icons.get(w["icon_key"], app.blank), bg=T["bg"]).grid(row=r, column=1, padx=(0, 6))
            name = w["name"] if len(w["name"]) <= 46 else w["name"][:45] + "…"
            extra = app.group_label(w["hwnd"]) if hasattr(app, "group_label") else ""
            tk.Label(left, text=name + (f"   · {extra}" if extra else ""), bg=T["bg"], fg=T["fg"], font=(f, 10),
                     anchor="w").grid(row=r, column=2, sticky="w")
            r += 1
        if not wins:
            tk.Label(left, text="No open windows", bg=T["bg"], fg=T["faint"], font=(f, 10)).grid(row=r, column=0,
                                                                                               columnspan=3, sticky="w")
        elif len(wins) > self.MAX_WINDOWS:
            tk.Label(left, text=f"+ {len(wins) - self.MAX_WINDOWS} more", bg=T["bg"], fg=T["faint"], font=(f, 9)
                     ).grid(row=r, column=2, sticky="w")

        # fixed shortcuts that aren't showing a window above
        r = 0
        fixed = [b for b in sorted(app.bindings, key=lambda b: b["slot"])
                 if not any(w.get("slot") == b["slot"] for w in wins)]
        if fixed:
            self._section(right, "FIXED SHORTCUTS", r)
            r += 1
            for b in fixed[:12]:
                self._key(right, slot_label(b["slot"]), r, 0)
                tk.Label(right, text=b["name"] + ("" if b.get("workspace") else "   · starts it"), bg=T["bg"],
                         fg=T["fg"], font=(f, 10), anchor="w").grid(row=r, column=1, columnspan=2, sticky="w")
                r += 1
        self._section(right, "ACTIONS", r)
        r += 1
        for cmd, label in CMD_LABELS:
            if cmd in CMD_KEYS:
                self._key(right, MOD_LABEL + CMD_KEYS[cmd], r, 0)
                tk.Label(right, text=label, bg=T["bg"], fg=T["fg"], font=(f, 10), anchor="w").grid(
                    row=r, column=1, columnspan=2, sticky="w")
                r += 1
        snips = len(SNIPPET_TRIGGERS)
        tips = [f"{snips} text snippet{'s' if snips != 1 else ''}: " + "  ".join(list(SNIPPET_TRIGGERS)[:5])
                if snips else "", "Search bar: timer 10m · remind 18:30 · pomodoro · = 2*21"]
        for tip in [t for t in tips if t]:
            tk.Label(right, text=tip, bg=T["bg"], fg=T["faint"], font=(f, 8), anchor="w").grid(
                row=r, column=0, columnspan=3, sticky="w", pady=(6 if tip is tips[0] else 0, 0))
            r += 1

        self.update_idletasks()
        w, h = self.winfo_reqwidth(), self.winfo_reqheight()
        l, t, rr, b = _pointer_monitor(self)
        self.geometry(f"{w}x{h}+{l + max(0, (rr - l - w) // 2)}+{t + max(0, (b - t - h) // 3)}")
        if self.state() != "normal":
            self.deiconify()
            if not self._styled:
                _no_activate(self)
                self._styled = True
        self.lift()

    def hide(self):
        self.withdraw()


class QuickNoteBox(tk.Toplevel):
    """One line for your notes: Enter saves, Esc closes (what you typed stays for next time)."""

    def __init__(self, app):
        super().__init__(app)
        self.withdraw()
        self.app = app
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.configure(bg=T["accent"])
        card = tk.Frame(self, bg=T["bg"], padx=14, pady=12)
        card.pack(padx=1, pady=1, fill="both", expand=True)
        top = tk.Frame(card, bg=T["bg"])
        top.pack(fill="x")
        tk.Label(top, text="Quick note", bg=T["bg"], fg=T["fg"], font=(UI_FONT, 11, "bold")).pack(side="left")
        self.where = tk.Label(top, text="", bg=T["bg"], fg=T["faint"], font=(UI_FONT, 8))
        self.where.pack(side="left", padx=(10, 0))
        self.var = tk.StringVar()
        box = tk.Frame(card, bg=T["field"], padx=10, pady=7, highlightthickness=1,
                       highlightbackground=T["border"], highlightcolor=T["accent"])
        box.pack(fill="x", pady=(8, 0))
        self.entry = tk.Entry(box, textvariable=self.var, bg=T["field"], fg=T["fg"], insertbackground=T["fg"],
                              relief="flat", font=(UI_FONT, 12), width=48, highlightthickness=0, bd=0)
        self.entry.pack(fill="x")
        self.entry.bind("<Return>", lambda e: self.save())
        self.entry.bind("<KP_Enter>", lambda e: self.save())
        self.bind("<Escape>", lambda e: self.hide())
        self.bind("<FocusOut>", lambda e: self.after(200, self._check_focus) if e.widget is self.entry else None)
        self.prev = None

    def show(self, prev_hwnd=None):
        self.prev = prev_hwnd
        self.where.configure(text=f"→ “{SETTINGS.get('quick_tab', 'Quick notes')}” in your encrypted notes  ·  "
                                  "Enter save  ·  Esc close")
        self.update_idletasks()
        w, h = max(560, self.winfo_reqwidth()), self.winfo_reqheight()
        l, t, r, _b = _pointer_monitor(self)
        self.geometry(f"{w}x{h}+{l + (r - l - w) // 2}+{t + 120}")
        self.deiconify()
        self.lift()
        if IS_WIN:
            try:
                focus_window(int(self.wm_frame(), 16))
            except Exception:
                log("quick note focus error:\n" + traceback.format_exc())
        self.focus_force()
        self.entry.focus_set()
        self.entry.icursor("end")

    def _check_focus(self):
        if self.state() == "normal" and self.focus_get() is None:
            self.withdraw()

    def is_open(self):
        return self.state() == "normal"

    def save(self):
        text = self.var.get().strip()
        if not text:
            self.hide()
            return "break"
        self.var.set("")
        self.hide()
        self.app.add_quick_note(text)
        return "break"

    def hide(self):
        self.withdraw()
        if IS_WIN and self.prev:
            try:
                focus_window(self.prev)
            except Exception:
                pass
        return "break"


# ======================================================================
# Text snippets editor
# ======================================================================

SNIPPET_PAUSE_HWND = 0  # the editor: triggers don't fire while you type in it


class SnippetsWindow(tk.Toplevel):
    HELP = ("Type the trigger anywhere and it's replaced by the text.  Start triggers with ; or / so they "
            "never fire by accident (e.g. ;mail, ;addr, /sig).\n"
            "In the text:  {date}  {time}  {datetime}  {date:%A %d %B}  {clipboard}  {cursor} "
            "(where the caret ends).  Enter in the text presses Enter — in chat apps that sends the message.")

    def __init__(self, app):
        super().__init__(app)
        self.withdraw()
        self.app = app
        self.store = app.snippets
        self.title("Text snippets")
        self.configure(bg=T["bg"])
        self.geometry("820x520")
        self.minsize(560, 360)
        self.cur = None
        self._loading = False
        self._save_job = None
        f = UI_FONT
        body = tk.Frame(self, bg=T["bg"], padx=14, pady=12)
        body.pack(fill="both", expand=True)
        left = tk.Frame(body, bg=T["bg"], width=250)
        left.pack(side="left", fill="y")
        left.pack_propagate(False)
        tk.Label(left, text="SNIPPETS", bg=T["bg"], fg=T["muted"], font=(f, 9, "bold")).pack(anchor="w")
        bar = tk.Frame(left, bg=T["bg"])
        bar.pack(side="bottom", fill="x", pady=(8, 0))
        flat_button(bar, "+ New", self.new).pack(side="left")
        flat_button(bar, "Delete", self.delete).pack(side="left")
        self.lb = tk.Listbox(left, activestyle="none", font=(f, 10), borderwidth=0, highlightthickness=0,
                             bg=T["surface"], fg=T["fg"], selectbackground=T["select"], selectforeground=T["fg"],
                             exportselection=False)
        self.lb.pack(fill="both", expand=True, pady=(6, 0))
        self.lb.bind("<<ListboxSelect>>", lambda e: self._on_pick())
        right = tk.Frame(body, bg=T["bg"], padx=14)
        right.pack(side="left", fill="both", expand=True)
        tk.Label(right, text="Trigger", bg=T["bg"], fg=T["muted"], font=(f, 9, "bold")).pack(anchor="w")
        self.trig_var = tk.StringVar()
        self.trig_var.trace_add("write", lambda *_: self._changed())
        self.trig = ttk.Entry(right, textvariable=self.trig_var, font=(f, 11))
        self.trig.pack(fill="x", pady=(2, 10))
        tk.Label(right, text="Text", bg=T["bg"], fg=T["muted"], font=(f, 9, "bold")).pack(anchor="w")
        self.help = tk.Label(right, text=self.HELP, bg=T["bg"], fg=T["faint"], font=(f, 8), justify="left",
                             anchor="w", wraplength=520)
        self.help.pack(side="bottom", fill="x", pady=(8, 0))
        self.status = tk.Label(right, text="", bg=T["bg"], fg="#f0b46a", font=(f, 9), anchor="w", justify="left",
                               wraplength=520)
        self.status.pack(side="bottom", fill="x", pady=(6, 0))
        self.text = tk.Text(right, wrap="word", undo=True, font=(f, 11), bg=T["field"], fg=T["fg"],
                            insertbackground=T["fg"], relief="flat", padx=10, pady=8, highlightthickness=1,
                            highlightbackground=T["border"], highlightcolor=T["accent"])
        self.text.pack(fill="both", expand=True, pady=(2, 0))
        self.text.bind("<<Modified>>", self._text_modified)
        self.right = right
        right.bind("<Configure>", lambda e: [w.configure(wraplength=max(200, e.width - 36))
                                              for w in (self.help, self.status)])
        self._placed = False
        self.protocol("WM_DELETE_WINDOW", self.hide)
        self.bind("<Escape>", lambda e: self.hide())
        self.bind("<Control-n>", lambda e: self.new())
        dark_titlebar(self)

    # ---- list
    def _label(self, it):
        text = " ".join(it["text"].split())
        text = text if len(text) <= 34 else text[:33] + "…"
        return f"  {it['trigger'] or '(no trigger)'}    {text}"

    def _fill(self):
        self.lb.delete(0, "end")
        for it in self.store.items:
            self.lb.insert("end", self._label(it))
        if self.cur in self.store.items:
            i = self.store.items.index(self.cur)
            self.lb.selection_clear(0, "end")
            self.lb.selection_set(i)
            self.lb.see(i)

    def _on_pick(self):
        sel = self.lb.curselection()
        if sel and sel[0] < len(self.store.items):
            self.edit(self.store.items[sel[0]])

    def edit(self, it):
        self.save_now()
        self.cur = it
        self._loading = True
        self.trig_var.set(it["trigger"] if it else "")
        self.text.delete("1.0", "end")
        if it:
            self.text.insert("1.0", it["text"])
        self.text.edit_modified(False)
        self._loading = False
        state = "normal" if it else "disabled"
        self.trig.configure(state=state)
        self.text.configure(state=state)
        self._fill()
        self._show_problem()

    # ---- editing
    def _text_modified(self, _e=None):
        if self.text.edit_modified():
            self.text.edit_modified(False)
            self._changed()

    def _changed(self):
        if self._loading or self.cur is None:
            return
        self.cur["trigger"] = self.trig_var.get().strip()
        self.cur["text"] = self.text.get("1.0", "end-1c")
        i = self.store.items.index(self.cur) if self.cur in self.store.items else -1
        if i >= 0:
            self.lb.delete(i)
            self.lb.insert(i, self._label(self.cur))
            self.lb.selection_set(i)
        self._show_problem()
        if self._save_job:
            self.after_cancel(self._save_job)
        self._save_job = self.after(500, self.save_now)

    def _show_problem(self):
        msg = self.store.problems(self.cur) if self.cur else ""
        n = len(self.store.items)
        self.status.configure(text=("⚠ " + msg) if msg else (f"{n} snippet{'s' if n != 1 else ''} · saved "
                                                            "encrypted" if n else "No snippets yet — + New"),
                              fg="#f0b46a" if msg else T["faint"])

    def save_now(self):
        if self._save_job:
            self.after_cancel(self._save_job)
            self._save_job = None
        try:
            self.store.save()
        except Exception as e:
            log("snippets save error:\n" + traceback.format_exc())
            self.status.configure(text=f"⚠ Couldn't save: {e}", fg="#f0b46a")

    def new(self, text=""):
        it = {"id": uuid.uuid4().hex[:10], "trigger": "", "text": text}
        self.store.items.append(it)
        self.edit(it)
        self.trig.focus_set()
        return "break"

    def delete(self):
        if self.cur is None:
            return
        if (self.cur["text"].strip() or self.cur["trigger"]) and not messagebox.askyesno(
                "Delete snippet", f"Delete the snippet “{self.cur['trigger'] or '(no trigger)'}”?", parent=self):
            return
        i = self.store.items.index(self.cur)
        self.store.items.remove(self.cur)
        self.cur = None
        self.save_now()
        nxt = self.store.items[min(i, len(self.store.items) - 1)] if self.store.items else None
        self.edit(nxt)

    # ---- show / hide
    def show(self, new_text=None):
        global SNIPPET_PAUSE_HWND
        if not self._placed:
            self._placed = True
            mons = list_monitors()
            l, t, r, b = mons[0][0] if mons else (0, 0, self.winfo_screenwidth(), self.winfo_screenheight())
            r, b = min(r, self.winfo_screenwidth()), min(b, self.winfo_screenheight())
            self.geometry(f"820x520+{l + max(0, (r - l - 820) // 2)}+{t + max(0, (b - t - 520) // 3)}")
        self.deiconify()
        self.lift()
        self.focus_force()
        if IS_WIN:
            try:
                SNIPPET_PAUSE_HWND = int(self.wm_frame(), 16)
            except (tk.TclError, ValueError):
                pass
        if new_text is not None:
            self.new(new_text)
        else:
            self.edit(self.cur if self.cur in self.store.items else
                      (self.store.items[0] if self.store.items else None))
            if not self.store.items:
                self.new()

    def hide(self):
        # drop empty new snippets
        empty = [x for x in self.store.items if not x["trigger"].strip() and not x["text"].strip()]
        for x in empty:
            self.store.items.remove(x)
        if self.cur in empty:
            self.cur = None
        self.save_now()
        self.withdraw()
        return "break"


# ======================================================================
# Restore the apps that were open before a restart
# ======================================================================

class RestoreDialog(tk.Toplevel):
    MAX_ROWS = 24

    def __init__(self, app, session, title="Reopen your apps?"):
        super().__init__(app)
        self.app = app
        self.items = list(session.get("items", []))[:60]
        self.title("Restore last session")
        self.configure(bg=T["bg"])
        self.attributes("-topmost", True)
        f = UI_FONT
        body = tk.Frame(self, bg=T["bg"], padx=18, pady=14)
        body.pack(fill="both", expand=True)
        tk.Label(body, text=title, bg=T["bg"], fg=T["fg"], font=(f, 13, "bold")).pack(anchor="w")
        saved = session.get("saved")
        when = time.strftime("%a %d %b, %H:%M", time.localtime(saved)) if saved else "before the restart"
        tk.Label(body, text=f"These were open on {when}. Untick what you don't need.", bg=T["bg"], fg=T["muted"],
                 font=(f, 9)).pack(anchor="w", pady=(2, 10))
        listf = tk.Frame(body, bg=T["bg"])
        listf.pack(fill="both", expand=True)
        self.vars = []
        cols = 1 if len(self.items) <= self.MAX_ROWS // 2 else 2
        per = -(-len(self.items) // cols) if self.items else 1
        for i, it in enumerate(self.items):
            v = tk.BooleanVar(value=True)
            self.vars.append(v)
            where = {"max": " · maximized"}.get((it.get("place") or {}).get("pos"), "")
            tk.Checkbutton(listf, text=f" {it.get('name', '?')}{where}", variable=v, bg=T["bg"], fg=T["fg"],
                           selectcolor=T["field"], activebackground=T["bg"], activeforeground=T["fg"],
                           font=(f, 10), anchor="w", highlightthickness=0, bd=0
                           ).grid(row=i % per, column=i // per, sticky="w", padx=(0, 18), pady=1)
        row = tk.Frame(body, bg=T["bg"])
        row.pack(fill="x", pady=(14, 0))
        ttk.Button(row, text="Reopen", style="Accent.TButton", command=self._ok).pack(side="right")
        ttk.Button(row, text="Not now", command=self.destroy).pack(side="right", padx=(0, 8))
        flat_button(row, "Untick all", lambda: [v.set(False) for v in self.vars]).pack(side="left")
        self.bind("<Return>", lambda e: self._ok())
        self.bind("<Escape>", lambda e: self.destroy())
        self.update_idletasks()
        w, h = max(420, self.winfo_reqwidth()), self.winfo_reqheight()
        mons = list_monitors()
        l, t, r, b = mons[0][0] if mons else (0, 0, self.winfo_screenwidth(), self.winfo_screenheight())
        self.geometry(f"{w}x{h}+{l + (r - l - w) // 2}+{t + max(0, (b - t - h) // 3)}")
        dark_titlebar(self)
        self.lift()
        self.focus_force()

    def chosen(self):
        return [it for it, v in zip(self.items, self.vars) if v.get()]

    def _ok(self):
        picked = self.chosen()
        self.destroy()
        if picked:
            self.app.restore_session(picked)


# ======================================================================
# Workspaces editor
# ======================================================================

class WorkspacesDialog(tk.Toplevel):
    def __init__(self, app, select_id=None, start_new=False):
        super().__init__(app)
        self.app = app
        self.title("Workspaces")
        self.transient(app)
        self.configure(bg=T["bg"])
        self.geometry("940x600")
        self.minsize(780, 480)
        f = UI_FONT
        self.mons = list_monitors()

        head = tk.Frame(self, bg=T["bg"], padx=18, pady=14)
        head.pack(fill="x")
        tk.Label(head, text="Workspaces", bg=T["bg"], fg=T["fg"], font=(f, 14, "bold")).pack(anchor="w")
        tk.Label(head, text="A workspace opens a group of apps, browser profiles, links and folders in one go — "
                            "and can put each window where you want it. Give it a key with right-click → "
                            "Fixed shortcut in the launcher.", bg=T["bg"], fg=T["muted"], font=(f, 9),
                 wraplength=880, justify="left").pack(anchor="w", pady=(2, 0))

        foot = tk.Frame(self, bg=T["bg"], padx=18, pady=12)
        foot.pack(side="bottom", fill="x")
        ttk.Button(foot, text="Done", command=self.destroy).pack(side="right")
        self.open_btn = ttk.Button(foot, text="Open now", style="Accent.TButton", command=self.open_now)
        self.open_btn.pack(side="left")
        ttk.Button(foot, text="Close its windows", command=self.close_windows).pack(side="left", padx=(8, 0))
        tk.Frame(self, bg=T["border"], height=1).pack(side="bottom", fill="x")

        body = tk.Frame(self, bg=T["bg"], padx=18)
        body.pack(fill="both", expand=True)

        left = tk.Frame(body, bg=T["bg"], width=220)
        left.pack(side="left", fill="y")
        left.pack_propagate(False)
        tk.Label(left, text="GROUPS", bg=T["bg"], fg=T["faint"], font=(f, 8, "bold")).pack(anchor="w", pady=(0, 4))
        lbtns = tk.Frame(left, bg=T["bg"])
        lbtns.pack(side="bottom", fill="x", pady=(6, 0))
        for text, cmd in (("+ New", self.new_ws), ("Rename", self.rename_ws), ("Delete", self.delete_ws),
                          ("▲", lambda: self.move_ws(-1)), ("▼", lambda: self.move_ws(1))):
            flat_button(lbtns, text, cmd).pack(side="left")
        self.lst = tk.Listbox(left, activestyle="none", font=(f, 11), borderwidth=0, highlightthickness=0,
                              bg=T["surface"], fg=T["fg"], selectbackground=T["select"], selectforeground=T["fg"],
                              exportselection=False)
        self.lst.pack(fill="both", expand=True)
        self.lst.bind("<<ListboxSelect>>", lambda e: self.show_items())
        self.lst.bind("<Double-1>", lambda e: self.rename_ws())

        right = tk.Frame(body, bg=T["bg"])
        right.pack(side="left", fill="both", expand=True, padx=(18, 0))
        self.items_title = tk.Label(right, text="ITEMS", bg=T["bg"], fg=T["faint"], font=(f, 8, "bold"))
        self.items_title.pack(anchor="w", pady=(0, 4))
        add = tk.Frame(right, bg=T["bg"])
        add.pack(fill="x", pady=(0, 6))
        for text, cmd in (("+ App…", self.add_apps), ("+ Open window…", self.add_windows),
                          ("+ Browser profile…", self.add_profiles), ("+ Link…", self.add_link),
                          ("+ File…", self.add_file), ("+ Folder…", self.add_folder)):
            flat_button(add, text, cmd).pack(side="left")

        place = tk.Frame(right, bg=T["bg"])
        place.pack(side="bottom", fill="x", pady=(8, 0))
        tk.Label(place, text="Position", bg=T["bg"], fg=T["muted"], font=(f, 9)).pack(side="left")
        self.pos_cb = ttk.Combobox(place, state="readonly", width=20, values=[lbl for _, lbl in WS_POSITIONS])
        self.pos_cb.pack(side="left", padx=(6, 14))
        tk.Label(place, text="Monitor", bg=T["bg"], fg=T["muted"], font=(f, 9)).pack(side="left")
        self.mon_cb = ttk.Combobox(place, state="readonly", width=16, values=self._monitor_labels())
        self.mon_cb.pack(side="left", padx=(6, 14))
        flat_button(place, "Use its current position", self.use_current).pack(side="left")
        self.pos_cb.bind("<<ComboboxSelected>>", lambda e: self.on_pos())
        self.mon_cb.bind("<<ComboboxSelected>>", lambda e: self.on_mon())

        ibtns = tk.Frame(right, bg=T["bg"])
        ibtns.pack(side="bottom", fill="x", pady=(6, 0))
        for text, cmd in (("Remove", self.remove_item), ("▲", lambda: self.move_item(-1)),
                          ("▼", lambda: self.move_item(1))):
            flat_button(ibtns, text, cmd).pack(side="left")
        tk.Label(ibtns, text="Tip: “+ Open window…” also remembers where that window is right now.",
                 bg=T["bg"], fg=T["faint"], font=(f, 8)).pack(side="right")

        frame = tk.Frame(right, bg=T["surface"])
        frame.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(frame, style="Card.Treeview", columns=("type", "pos", "mon"),
                                 show="tree headings", selectmode="browse")
        for col, text, w, stretch in (("#0", "NAME", 260, True), ("type", "TYPE", 120, False),
                                      ("pos", "POSITION", 150, False), ("mon", "MONITOR", 80, False)):
            self.tree.heading(col, text=text, anchor="w")
            self.tree.column(col, width=w, stretch=stretch, anchor="w")
        sb = ttk.Scrollbar(frame, orient="vertical", command=self.tree.yview, style="Thin.Vertical.TScrollbar")
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self._sync_place())
        self.tree.bind("<Delete>", lambda e: self.remove_item())

        self.bind("<Escape>", lambda e: self.destroy())
        self._fill_list(select_id)
        center_over(self, app)
        dark_titlebar(self)
        if start_new or not app.workspaces:
            self.after(80, self.new_ws)
        self.grab_set()
        self.wait_window()

    # ---- helpers
    def _monitor_labels(self):
        out = []
        for i, ((l, t, r, b), _h, main) in enumerate(self.mons):
            out.append(f"{i + 1}{' · main' if main else ''}  ({r - l}×{b - t})")
        return out or ["1 · main"]

    def ws(self):
        sel = self.lst.curselection()
        return self.app.workspaces[sel[0]] if sel and sel[0] < len(self.app.workspaces) else None

    def _item_index(self):
        sel = self.tree.selection()
        return int(sel[0]) if sel else None

    def _changed(self, keep_item=None):
        self.app._save()
        self.show_items(keep_item)

    def _fill_list(self, select_id=None):
        self.lst.delete(0, "end")
        for w in self.app.workspaces:
            self.lst.insert("end", "  " + w["name"])
        idx = next((i for i, w in enumerate(self.app.workspaces) if w["id"] == select_id), 0)
        if self.app.workspaces:
            self.lst.selection_set(idx)
            self.lst.see(idx)
        self.show_items()

    def show_items(self, keep_item=None):
        ws = self.ws()
        self.tree.delete(*self.tree.get_children())
        self.items_title.configure(text=f"ITEMS IN “{ws['name'].upper()}”" if ws else "ITEMS")
        state = "normal" if ws else "disabled"
        self.open_btn.configure(state=state)
        if not ws:
            return
        pos_labels = dict(WS_POSITIONS)
        for i, it in enumerate(ws["items"]):
            pl = it["place"]
            mon = "" if pl["pos"] == "none" else str(pl.get("monitor", 0) + 1)
            self.tree.insert("", "end", iid=str(i), text="  " + it["name"],
                             values=(ws_item_label(it), pos_labels.get(pl["pos"], ""), mon))
        if ws["items"]:
            k = keep_item if keep_item is not None and keep_item < len(ws["items"]) else 0
            self.tree.selection_set(str(k))
            self.tree.see(str(k))
        self._sync_place()

    def _sync_place(self):
        ws, i = self.ws(), self._item_index()
        if not ws or i is None or i >= len(ws["items"]):
            self.pos_cb.set("")
            self.mon_cb.set("")
            return
        pl = ws["items"][i]["place"]
        keys = [k for k, _ in WS_POSITIONS]
        self.pos_cb.current(keys.index(pl["pos"]) if pl["pos"] in keys else 0)
        mons = self.mon_cb["values"]
        self.mon_cb.current(min(pl.get("monitor", 0), len(mons) - 1) if mons else 0)

    # ---- groups
    def new_ws(self):
        name = simpledialog.askstring("New workspace", "Name (e.g. Work, Study, Gaming):", parent=self)
        if not name or not name.strip():
            return
        ws = make_workspace(name.strip())
        self.app.workspaces.append(ws)
        self.app._save()
        self._fill_list(ws["id"])

    def rename_ws(self):
        ws = self.ws()
        if ws:
            self.app.rename_workspace(ws, parent=self)
            self._fill_list(ws["id"])

    def delete_ws(self):
        ws = self.ws()
        if ws and self.app.delete_workspace(ws, parent=self):
            self._fill_list()

    def move_ws(self, step):
        ws = self.ws()
        if not ws:
            return
        lst = self.app.workspaces
        i = lst.index(ws)
        j = i + step
        if 0 <= j < len(lst):
            lst[i], lst[j] = lst[j], lst[i]
            self.app._save()
            self._fill_list(ws["id"])

    # ---- items
    def _add(self, items):
        ws = self.ws()
        if not ws:
            messagebox.showinfo("Workspaces", "Create a workspace first (+ New).", parent=self)
            return
        added = [x for x in (clean_ws_item(i) for i in items) if x]
        if added:
            ws["items"].extend(added)
            self._changed(len(ws["items"]) - 1)

    def add_apps(self):
        if not self.app.apps:
            messagebox.showinfo("Workspaces", "Apps are still loading, try again in a moment.", parent=self)
            return
        dlg = AppPickerDialog(self, self.app.apps, set())
        if dlg.result:
            self._add([{"type": "app", "name": p["name"], "app_id": p["app_id"]} for p in dlg.result])

    def add_windows(self):
        wins = self.app.windows
        if not wins:
            messagebox.showinfo("Workspaces", "No open windows.", parent=self)
            return
        dlg = ListPicker(self, "Add open windows", [w["name"] for w in wins],
                         hint="Each window's app is added, and the window's current position is remembered.")
        if dlg.result:
            items = [self.app.item_to_ws(wins[i]) for i in dlg.result]
            if any(x is None for x in items):
                messagebox.showinfo("Workspaces", "Some windows couldn't be matched to a program and were "
                                                  "skipped.", parent=self)
            self._add([x for x in items if x])

    def add_profiles(self):
        profs = self.app.profiles
        if not profs:
            messagebox.showinfo("Workspaces", "Add browser profiles first (+ Add → Browser profiles).", parent=self)
            return
        dlg = ListPicker(self, "Add browser profiles", [p["name"] for p in profs])
        if dlg.result:
            self._add([self.app.item_to_ws(profs[i]) for i in dlg.result])

    def add_link(self):
        text = simpledialog.askstring("Add link", "Web address (e.g. github.com or https://…):", parent=self)
        if not text or not text.strip():
            return
        text = text.strip()
        url = looks_like_url(text) or (text if "://" in text else "https://" + text)
        profile = None
        if self.app.profiles:
            labels = ["Default browser"] + [p["name"] for p in self.app.profiles]
            dlg = ListPicker(self, "Open the link in…", labels, multi=False, ok_text="Choose")
            if dlg.result is None:
                return
            if dlg.result[0] > 0:
                profile = profile_id(self.app.profiles[dlg.result[0] - 1])
        parts = urllib.parse.urlsplit(url)
        name = (parts.netloc + parts.path).rstrip("/") or url
        self._add([{"type": "url", "name": name, "url": url, "profile": profile}])

    def add_file(self):
        path = filedialog.askopenfilename(parent=self, title="Add a program or file")
        if path:
            path = os.path.normpath(path)
            self._add([{"type": "file", "name": os.path.splitext(os.path.basename(path))[0], "path": path}])

    def add_folder(self):
        path = filedialog.askdirectory(parent=self, title="Add a folder")
        if path:
            path = os.path.normpath(path)
            self._add([{"type": "file", "name": os.path.basename(path) or path, "path": path}])

    def remove_item(self):
        ws, i = self.ws(), self._item_index()
        if ws and i is not None:
            del ws["items"][i]
            self._changed(max(0, i - 1))

    def move_item(self, step):
        ws, i = self.ws(), self._item_index()
        if ws and i is not None and 0 <= i + step < len(ws["items"]):
            ws["items"][i], ws["items"][i + step] = ws["items"][i + step], ws["items"][i]
            self._changed(i + step)

    def on_pos(self):
        ws, i = self.ws(), self._item_index()
        if not ws or i is None:
            return
        pos = WS_POSITIONS[self.pos_cb.current()][0]
        pl = ws["items"][i]["place"]
        if pos == "saved" and not pl.get("rect"):
            messagebox.showinfo("Saved position", "Open the app, put its window where you want it, then press "
                                                  "“Use its current position”.", parent=self)
            self._sync_place()
            return
        pl["pos"] = pos
        self._changed(i)

    def on_mon(self):
        ws, i = self.ws(), self._item_index()
        if ws and i is not None:
            ws["items"][i]["place"]["monitor"] = max(0, self.mon_cb.current())
            self._changed(i)

    def use_current(self):
        ws, i = self.ws(), self._item_index()
        if not ws or i is None:
            return
        wins = self.app.ws_entry(ws["items"][i])["wins"]
        if not wins:
            messagebox.showinfo("Use its current position", f"“{ws['items'][i]['name']}” isn't open. Open it, "
                                "place its window, then try again.", parent=self)
            return
        pos, mon, rect = window_placement_now(wins[0])
        ws["items"][i]["place"] = {"pos": pos, "monitor": mon, "rect": rect}
        self._changed(i)

    def open_now(self):
        ws = self.ws()
        if ws:
            self.app.activate_workspace(ws)

    def close_windows(self):
        ws = self.ws()
        if ws:
            self.app.close_workspace(ws, parent=self)


# ======================================================================
# Main window
# ======================================================================


# ======================================================================
# Small encrypted files (snippets, timers): encrypted with your Windows account (DPAPI)
# ======================================================================

def protected_save(path, obj):
    raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    if IS_WIN:
        blob = {"v": 1, "mode": "windows", "data": _b64(dpapi_protect(raw))}
    else:  # tests on other systems: nothing to encrypt with
        blob = {"v": 1, "mode": "plain", "data": obj}
    write_json_atomic(path, blob)


def protected_load(path, default):
    blob = _read_json(path)
    if not isinstance(blob, dict):
        return default
    try:
        if blob.get("mode") == "windows":
            return json.loads(dpapi_unprotect(base64.b64decode(blob["data"])).decode("utf-8"))
        if blob.get("mode") == "plain":
            return blob["data"]
    except Exception:
        log(f"couldn't read {os.path.basename(path)}:\n" + traceback.format_exc())
    return default


# ======================================================================
# Text snippets: type a trigger (e.g. ;mail) anywhere and it's replaced by its text
# ======================================================================

SNIPPET_TRIGGERS = {}  # trigger -> text, read by the keyboard watcher (empty = snippets off)


def match_trigger(typed, triggers):
    """The longest trigger that the typed text ends with, or None."""
    best = None
    for t in triggers:
        if t and typed.endswith(t) and (best is None or len(t) > len(best)):
            best = t
    return best


SNIPPET_FIELDS = re.compile(r"\{(date|time|datetime|clipboard|cursor)(?::([^{}]*))?\}", re.I)


def expand_snippet(text, clipboard=None, now=None):
    """Fill in {date}, {time}, {datetime}, {date:%A %d %B}, {clipboard}; {cursor} = where the caret ends.
    Returns (text, how many characters the caret goes back from the end)."""
    now = now or datetime.datetime.now()
    defaults = {"date": "%d/%m/%Y", "time": "%H:%M", "datetime": "%d/%m/%Y %H:%M"}
    cursor_mark = "\x00"

    def fill(m):
        name, fmt = m.group(1).lower(), m.group(2)
        if name == "clipboard":
            return clipboard or ""
        if name == "cursor":
            return cursor_mark
        try:
            return now.strftime(fmt or defaults[name])
        except ValueError:
            return m.group(0)
    out = SNIPPET_FIELDS.sub(fill, text).replace("\r\n", "\n")
    if cursor_mark in out:
        before, after = out.split(cursor_mark, 1)
        after = after.replace(cursor_mark, "")
        return before + after, len(after)
    return out, 0


class SnippetStore:
    def __init__(self):
        self.items = []  # [{"id", "trigger", "text"}]

    def load(self):
        data = protected_load(SNIPPETS_FILE, {"items": []})
        self.items = [{"id": str(x.get("id") or uuid.uuid4().hex[:10]), "trigger": str(x.get("trigger", "")),
                       "text": str(x.get("text", ""))}
                      for x in data.get("items", []) if isinstance(x, dict)]
        self.apply()

    def save(self):
        protected_save(SNIPPETS_FILE, {"items": self.items})
        self.apply()

    def apply(self):
        """Give the keyboard watcher the current triggers (a new dict: it's swapped in one step)."""
        global SNIPPET_TRIGGERS
        on = SETTINGS.get("snippets_on", True)
        SNIPPET_TRIGGERS = {x["trigger"]: x["text"] for x in self.items if on and x["trigger"].strip()}

    def problems(self, item):
        """Why a trigger might not work as expected ('' when it's fine)."""
        t = item["trigger"]
        if not t.strip():
            return "Give it a trigger, e.g. ;mail"
        if any(c in t for c in "\n\t"):
            return "A trigger can't contain Enter or Tab."
        same = [x for x in self.items if x is not item and x["trigger"] == t]
        if same:
            return "Another snippet has the same trigger."
        longer = [x["trigger"] for x in self.items if x is not item and x["trigger"].startswith(t)
                  and x["trigger"] != t]
        if longer:
            return f"“{longer[0]}” starts with this trigger, so it could never be typed — pick different ones."
        shorter = [x["trigger"] for x in self.items if x is not item and x["trigger"] and t.startswith(x["trigger"])
                   and x["trigger"] != t]
        if shorter:
            return f"“{shorter[0]}” fires first while you type this one — pick different ones."
        if len(t) < 3 and not re.search(r"[^\w\s]", t):
            return "Short plain words fire while you type normal text — start triggers with ; or / e.g. ;mail"
        return ""


# ======================================================================
# Timers, reminders and pomodoro (search bar: "timer 10m tea", "remind 18:30 call prof", "pomodoro")
# ======================================================================

_DUR_PART = re.compile(r"(\d+(?:\.\d+)?)\s*(h(?:ours?|rs?)?|m(?:in(?:ute)?s?|n)?|s(?:ec(?:ond)?s?)?)\s*")


def parse_duration(text):
    """'25' (minutes), '10m', '1h30', '1h 30m', '90s', '1.5h' → seconds, or None."""
    s = text.strip().lower().replace(",", ".")
    if not s:
        return None
    if re.fullmatch(r"\d+(?:\.\d+)?", s):
        return float(s) * 60
    m = re.fullmatch(r"(\d+)\s*h\s*(\d{1,2})", s)  # 1h30
    if m:
        return int(m.group(1)) * 3600 + int(m.group(2)) * 60
    total, pos = 0.0, 0
    for m in _DUR_PART.finditer(s):
        if m.start() != pos:
            return None
        total += float(m.group(1)) * {"h": 3600, "m": 60, "s": 1}[m.group(2)[0]]
        pos = m.end()
    return total if pos == len(s) and total > 0 else None


def parse_clock(text, now=None):
    """'18:30', '18h30', '6:30pm', '6pm', '7.15' → the next time it's that o'clock (datetime), or None."""
    now = now or datetime.datetime.now()
    s = text.strip().lower().replace(" ", "")
    m = re.fullmatch(r"(\d{1,2})(?:[:h.](\d{2})|h)?(am|pm)?", s)
    if not m or (m.group(2) is None and m.group(3) is None and "h" not in s):
        return None
    hour, minute, ampm = int(m.group(1)), int(m.group(2) or 0), m.group(3)
    if ampm:
        if not 1 <= hour <= 12:
            return None
        hour = hour % 12 + (12 if ampm == "pm" else 0)
    if hour > 23 or minute > 59:
        return None
    when = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
    if when <= now:
        when += datetime.timedelta(days=1)
    return when


def _when_options(words, mode=None):
    """Readings of the start of words as a duration and/or a clock time: [(seconds | datetime, rest)].
    mode "in" = durations only, "at" = clock times only. '9h30' gives both (09:30, or in 9 h 30 min)."""
    for n in range(min(4, len(words)), 0, -1):
        head = " ".join(words[:n])
        d = parse_duration(head) if mode != "at" else None
        c = parse_clock(head) if mode != "in" else None
        if not d and not c:
            continue
        clock_like = bool(re.search(r":|am$|pm$|h\d\d$|^\d{1,2}\.\d\d$", head.lower().replace(" ", "")))
        opts = [(c, words[n:])] if c else []
        if d:
            opts.insert(len(opts) if clock_like else 0, (d, words[n:]))
        return opts
    return []


def fmt_left(seconds):
    seconds = max(0, int(round(seconds)))
    h, rem = divmod(seconds, 3600)
    m, sec = divmod(rem, 60)
    return f"{h}:{m:02d}:{sec:02d}" if h else f"{m}:{sec:02d}"


def fmt_duration(seconds):
    seconds = int(round(seconds))
    h, rem = divmod(seconds, 3600)
    m, sec = divmod(rem, 60)
    parts = ([f"{h} h"] if h else []) + ([f"{m} min"] if m else []) + ([f"{sec} s"] if sec else [])
    return " ".join(parts) or "0 s"


def parse_timer_query(raw, now=None):
    """Search-bar text → timers it could start: [{"kind", "due" (epoch), "label", "dur", "title"}] (may be empty)."""
    now = now or datetime.datetime.now()
    words = raw.strip().split()
    if not words:
        return []
    head = words[0].lower()
    out = []
    if head in ("timer", "countdown"):
        for d, rest in _when_options(words[1:], "in"):
            label = " ".join(rest).strip()
            out.append({"kind": "timer", "due": now.timestamp() + d, "label": label or "Timer", "dur": d,
                        "title": f"Start a {fmt_duration(d)} timer" + (f" — {label}" if label else "")})
    elif head in ("remind", "reminder", "alarm"):
        rest = words[1:]
        if rest and rest[0].lower() == "me":
            rest = rest[1:]
        mode = None
        if rest and rest[0].lower() in ("at", "in"):
            mode, rest = rest[0].lower(), rest[1:]
        for when, more in _when_options(rest, mode):
            if more and more[0].lower() in ("to", "that"):
                more = more[1:]
            label = " ".join(more).strip() or ("Alarm" if head == "alarm" else "Reminder")
            if isinstance(when, datetime.datetime):
                day = "today" if when.date() == now.date() else "tomorrow"
                out.append({"kind": "remind", "due": when.timestamp(), "label": label, "dur": None,
                            "title": f"Remind me at {when:%H:%M} ({day}) — {label}"})
            else:
                out.append({"kind": "remind", "due": now.timestamp() + when, "label": label, "dur": when,
                            "title": f"Remind me in {fmt_duration(when)} — {label}"})
    return out


class TimerStore:
    """Running timers and reminders, kept on disk so they survive a restart."""

    def __init__(self):
        self.items = []   # [{"id", "kind": timer|remind|pomo, "due", "label", "dur", "phase", "round"}]
        self.pomo = None  # {"phase": "work"|"break"|"long", "round": n} while a pomodoro runs

    def load(self):
        data = protected_load(TIMERS_FILE, {})
        self.items = [x for x in data.get("items", []) if isinstance(x, dict) and "due" in x]
        self.pomo = data.get("pomo") if isinstance(data.get("pomo"), dict) else None
        if self.pomo and not any(x["kind"] == "pomo" for x in self.items):
            self.pomo = None

    def save(self):
        try:
            protected_save(TIMERS_FILE, {"items": self.items, "pomo": self.pomo})
        except Exception:
            log("couldn't save timers:\n" + traceback.format_exc())

    def add(self, kind, due, label, dur=None, **extra):
        it = {"id": uuid.uuid4().hex[:10], "kind": kind, "due": float(due), "label": label, "dur": dur,
              "created": time.time(), **extra}
        self.items.append(it)
        self.items.sort(key=lambda x: x["due"])
        self.save()
        return it

    def remove(self, tid):
        self.items = [x for x in self.items if x["id"] != tid]
        if self.pomo and not any(x["kind"] == "pomo" for x in self.items):
            self.pomo = None
        self.save()

    def get(self, tid):
        return next((x for x in self.items if x["id"] == tid), None)

    def due(self, now=None):
        now = time.time() if now is None else now
        return [x for x in self.items if x["due"] <= now]

    # pomodoro
    def start_pomodoro(self, now=None):
        now = time.time() if now is None else now
        self.stop_pomodoro(save=False)
        self.pomo = {"phase": "work", "round": 1}
        return self.add("pomo", now + SETTINGS.get("pomo_work", 25) * 60, "Pomodoro — work 1", phase="work",
                        round=1)

    def stop_pomodoro(self, save=True):
        self.items = [x for x in self.items if x["kind"] != "pomo"]
        self.pomo = None
        if save:
            self.save()

    def next_pomodoro(self, done, now=None):
        """A pomodoro phase ended: start the next one. Returns (message, the new timer)."""
        now = time.time() if now is None else now
        rounds = SETTINGS.get("pomo_rounds", 4)
        n = int(done.get("round", 1))
        if done.get("phase") == "work":
            long_break = n % rounds == 0
            phase, minutes = ("long", SETTINGS.get("pomo_long", 15)) if long_break else \
                ("break", SETTINGS.get("pomo_break", 5))
            msg = (f"Work session {n} done — " + ("long break" if long_break else "short break") + f", {minutes} min.")
            label = f"Pomodoro — {'long break' if long_break else 'break'}"
            nxt_round = n
        else:
            phase, minutes, nxt_round = "work", SETTINGS.get("pomo_work", 25), n + 1
            msg = f"Break over — work session {nxt_round}, {minutes} min."
            label = f"Pomodoro — work {nxt_round}"
        self.pomo = {"phase": phase, "round": nxt_round}
        return msg, self.add("pomo", now + minutes * 60, label, phase=phase, round=nxt_round)


# ======================================================================
# Session: which apps were open, to open them again after a restart
# ======================================================================

SESSION_SKIP_EXES = {"explorer.exe", "taskmgr.exe", "applicationframehost.exe", "searchhost.exe",
                     "searchapp.exe", "shellexperiencehost.exe", "startmenuexperiencehost.exe",
                     "textinputhost.exe", "lockapp.exe", "systemsettingsbroker.exe", "mmc.exe", "consent.exe"}
SESSION_FROZEN = False  # Windows is shutting down: keep the last list as it is


def boot_time():
    """When this computer last started (seconds since 1970)."""
    if IS_WIN:
        k = ctypes.windll.kernel32
        k.GetTickCount64.restype = ctypes.c_uint64
        return time.time() - k.GetTickCount64() / 1000
    try:
        with open("/proc/uptime") as f:
            return time.time() - float(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return time.time() - time.monotonic()


def session_entry_key(it):
    return json.dumps([it.get("type"), it.get("app_id"), (it.get("path") or "").lower(),
                       (it.get("exe") or "").lower(), it.get("args")], sort_keys=True)


def rotate_session_file():
    """At start-up: the list saved before this restart becomes the "previous session". Returns it (or None)."""
    cur = _read_json(SESSION_FILE)
    if not isinstance(cur, dict) or not cur.get("items"):
        return None
    if abs(float(cur.get("boot", 0)) - boot_time()) < 120:
        return None  # same Windows session: the launcher was only restarted
    try:
        write_json_atomic(SESSION_PREV_FILE, cur)
        os.remove(SESSION_FILE)
    except OSError:
        log("session rotate error:\n" + traceback.format_exc())
    return cur


COMMANDS = [  # (id, name, other words that find it, needs confirmation)
    ("lock", "Lock computer", ["lock", "lock screen"], False),
    ("sleep", "Sleep", ["sleep", "suspend"], False),
    ("shutdown", "Shut down", ["shutdown", "power off", "turn off"], True),
    ("restart", "Restart", ["reboot"], True),
    ("signout", "Sign out", ["log off", "logout", "log out"], True),
    ("recycle", "Empty Recycle Bin", ["trash", "bin"], False),
    ("taskmgr", "Task Manager", ["taskmgr", "processes"], False),
    ("settings", "Launcher settings", ["settings", "config", "preferences", "options", "settings.ini"], False),
    ("notes", "Notes", ["notes"], False),
    ("clipboard", "Clipboard history", ["clipboard", "paste history"], False),
    ("workspaces", "Edit workspaces", ["workspaces", "groups"], False),
    ("newws", "New workspace", ["new workspace", "create workspace"], False),
    ("reload", "Reload app list", ["reload", "refresh"], False),
    ("autostart", "Start with Windows (on / off)", ["startup", "autostart", "start with windows"], False),
    ("snippets", "Text snippets", ["snippets", "text expansion", "abbreviations", "templates"], False),
    ("restore", "Restore last session", ["restore session", "reopen apps", "session"], False),
    ("explorer", "File explorer (tabs, favourite folders)",
     ["file explorer", "files", "folders", "explorer", "browse files"], False),
    ("sysmon", "System monitor (CPU, GPU, RAM, temperatures)",
     ["system monitor", "temperature", "cpu", "gpu", "ram", "hardware monitor"], False),
    ("quit", "Quit launcher", ["quit", "exit"], False),
]


class LauncherApp(tk.Tk):
    def __init__(self, start_hidden=False):
        super().__init__()
        if start_hidden:
            self.withdraw()
        self.title("Window Manager")
        apply_theme(self)
        log(f"start {VERSION}; python={sys.version.split()[0]} at {sys.executable}")
        self.minsize(380, 420)
        w, h = 600, 720
        self.geometry(f"{w}x{h}+{(self.winfo_screenwidth() - w) // 2}+{max(0, (self.winfo_screenheight() - h) // 3)}")
        self.summoned = False
        self._quitting = False
        self.notes = None
        self.clip_win = None
        self.switcher = None
        self.cycler = Cycler(self)
        self.shell = None
        self._told_tray = False
        self.usage = Usage()
        self.clip_store = ClipboardStore()
        self.clip_store.load()
        self.popups = []          # reminder pop-ups on screen
        self._toast = None
        self.snip_win = None
        self.snippets = SnippetStore()
        self.timers = TimerStore()
        for what, fn in (("snippets", self.snippets.load), ("timers", self.timers.load)):
            try:
                fn()
            except Exception:
                log(f"couldn't load {what}:\n" + traceback.format_exc())
        self.prev_session = None
        self._session_sig = None
        self.cheat = None
        self._cheat_job = None
        self.quick_box = None
        self.muted_exes = set()   # apps muted from here (shown as "muted" in the OPEN list)
        self.groups = [list(g) for g in load_ui().get("window_groups", [])
                       if isinstance(g, list) and len(g) >= 2]
        if SETTINGS.get("session_restore", "ask") != "no":
            try:
                self.prev_session = rotate_session_file()
            except Exception:
                log("session error:\n" + traceback.format_exc())
        self.collapsed = set(load_ui().get("collapsed", []))
        self.order_pref = [k for k in load_ui().get("window_order", []) if isinstance(k, str)]
        self._drag = None
        self._hover = None
        self._status_job = None

        self.icon_px = max(16, round(24 * self.winfo_fpixels("1i") / 96))
        self.blank = tk.PhotoImage(width=self.icon_px, height=self.icon_px)
        self.icons = {}          # icon_key -> PhotoImage
        self.requested = {"logo", "url", "none"}   # icon keys already queued (these are drawn here)
        self._logo_imgs = [tk.PhotoImage(data=base64.b64encode(encode_png(n, n, make_logo_rgba(n))).decode("ascii"))
                           for n in (32, 16)]
        self.iconphoto(True, *self._logo_imgs)
        self.icons["logo"] = tk.PhotoImage(data=base64.b64encode(
            encode_png(self.icon_px, self.icon_px, make_logo_rgba(self.icon_px))).decode("ascii"))
        self.iids_by_key = {}
        self.row_items = {}      # tree iid -> item dict
        self.q = queue.Queue()

        self.apps = []
        self.profiles, self.pins, all_bindings, self.workspaces = load_config()
        self.bindings = [b for b in all_bindings if b["slot"] is not None]
        self.inactive_bindings = [b for b in all_bindings if b["slot"] is None]  # their key row is turned off
        self.settings_mtime = self._settings_mtime()
        self.windows = []
        self.window_order = []
        self.window_sig = None
        self.titles = {}
        self.z_order = []         # open windows, most recently used first
        self.win_ident = {}       # hwnd -> (exe, aumid, profile key or None)
        self.pid_profile = {}     # pid -> profile key (browsers that run one process per profile)
        self.profile_windows = {}  # profile key -> (recent hwnds, stable hwnds)
        if IS_WIN:
            ole32.CoInitializeEx(None, 0x2)
        self.slot_map = {}        # slot -> ("window", hwnd) | ("binding", b, recent_hwnds, stable_hwnds)

        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        self.hotkeys = (HotkeyThread(lambda: self.slot_map,
                                     lambda key, hwnd, result, name=None:
                                     self.q.put(("switch", key, hwnd, result, name)),
                                     lambda cmd: self.q.put(("cmd", cmd)))
                        if IS_WIN else None)
        if self.hotkeys:
            self.hotkeys.start()
            self.after(500, self._check_hotkeys)

        if IS_WIN:
            try:
                self.shell = ShellThread(lambda c: self.q.put(("cmd", c)), lambda t: self.q.put(("clip", t)), True)
                self.shell.start()
                self.shell.ready.wait(3)
                threading.Thread(target=self._refresh_autostart, daemon=True).start()
            except Exception:
                log("tray error:\n" + traceback.format_exc())
            watch_show_requests(lambda: self.q.put(("cmd", "show")))
        if start_hidden and not self.tray_ok:
            self.deiconify()  # no tray icon: don't hide where nobody can find it

        self.after(50, self._poll)
        self._scan_windows()
        self.after(1000, self._timer_tick)
        self.after(SESSION_SAVE_MS, self._session_tick)
        if self.prev_session:
            self.after(3000, self._offer_restore)
        self.load_apps()
        self._request_icons(self.pins + self.profiles + self.bindings)

    # ---- layout
    def _build_ui(self):
        f = UI_FONT
        ttk.Style(self).configure("Treeview", rowheight=self.icon_px + 16)

        # Search bar
        bar = tk.Frame(self, bg=T["bg"], padx=14, pady=14)
        bar.pack(fill="x")
        box = tk.Frame(bar, bg=T["field"], padx=12, pady=9, highlightthickness=1,
                       highlightbackground=T["border"], highlightcolor=T["accent"])
        box.pack(fill="x")
        tk.Label(box, text="⌕", bg=T["field"], fg=T["muted"], font=(f, 15)).pack(side="left", padx=(0, 8))

        self.search_var = tk.StringVar()
        self.search_var.trace_add("write", lambda *_: self._on_search_change())
        self.search = tk.Entry(box, textvariable=self.search_var, bg=T["field"], fg=T["fg"],
                               insertbackground=T["fg"], relief="flat", borderwidth=0,
                               highlightthickness=0, font=(f, 13))
        self.search.pack(side="left", fill="x", expand=True)
        self.placeholder = tk.Label(box, text="Search  ·  = math  ·  g web", bg=T["field"],
                                    fg=T["faint"], font=(f, 13))
        self.placeholder.place(in_=self.search, x=1, rely=0.5, anchor="w")
        self.placeholder.bind("<Button-1>", lambda e: self.search.focus_set())
        self.search.bind("<FocusIn>", lambda e: box.configure(highlightbackground=T["accent"]))
        self.search.bind("<FocusOut>", lambda e: box.configure(highlightbackground=T["border"]))
        self.search.bind("<Return>", lambda e: self._launch_first())
        self.search.bind("<Down>", lambda e: self._move(1))
        self.search.bind("<Escape>", lambda e: self._on_escape())

        self.add_btn = self._flat_button(box, "+ Add", self._add_menu)
        self._flat_button(box, "Notes", self.toggle_notes)
        self._flat_button(box, "Clipboard", lambda: self.toggle_clipboard(None))
        self._flat_button(box, "⚙", self.open_settings)
        self._flat_button(box, "⟳", self.load_apps)
        for btn in reversed(box.winfo_children()[-5:]):
            btn.pack(side="right", padx=(2, 0))

        # Footer
        self.status = tk.Label(self, text="Starting…", bg=T["bg"], fg=T["faint"],
                               font=(f, 8), anchor="w", padx=16, pady=8)
        self.status.pack(side="bottom", fill="x")
        tk.Frame(self, bg=T["border"], height=1).pack(side="bottom", fill="x")

        # List
        frame = tk.Frame(self, bg=T["bg"], padx=8)
        frame.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(frame, columns=("key",), show="tree", selectmode="browse")
        self.tree.column("#0", stretch=True)
        self.tree.column("key", width=self._key_width(), minwidth=60, stretch=False, anchor="e")
        self.tree.tag_configure("header", font=(f, 8, "bold"), foreground=T["faint"])
        self.tree.tag_configure("dim", foreground=T["muted"])
        self.tree.tag_configure("hover", background=T["hover"])
        sb = ttk.Scrollbar(frame, orient="vertical", command=self.tree.yview, style="Thin.Vertical.TScrollbar")
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y", padx=(4, 0))

        self.tree.bind("<Double-1>", lambda e: self.launch_selected())
        self.tree.bind("<Return>", lambda e: self.launch_selected())
        self.tree.bind("<Delete>", lambda e: self.remove_selected())
        self.tree.bind("<Button-3>", self._context_menu)
        self.tree.bind("<Up>", lambda e: self._move(-1))
        self.tree.bind("<Down>", lambda e: self._move(1))
        self.tree.bind("<Escape>", lambda e: self._on_escape())
        self.tree.bind("<Key>", self._type_to_search)
        self.tree.bind("<<TreeviewSelect>>", self._skip_headers)
        self.tree.bind("<Motion>", self._on_motion)
        self.tree.bind("<Button-1>", self._on_click)
        self.tree.bind("<B1-Motion>", self._drag_motion)
        self.tree.bind("<ButtonRelease-1>", self._drag_end)
        self.tree.bind("<Control-Up>", lambda e: self._move_selected_window("up"))
        self.tree.bind("<Control-Down>", lambda e: self._move_selected_window("down"))
        self.tree.bind("<Leave>", lambda e: self._set_hover(None))

        dark_titlebar(self)
        self.search.focus()

    def _flat_button(self, parent, text, command):
        bg = parent["bg"]
        b = tk.Label(parent, text=text, bg=bg, fg=T["muted"], font=(UI_FONT, 9), padx=8, pady=2, cursor="hand2")
        b.bind("<Enter>", lambda e: b.configure(fg=T["fg"], bg=T["border"]))
        b.bind("<Leave>", lambda e: b.configure(fg=T["muted"], bg=bg))
        b.bind("<Button-1>", lambda e: command())
        return b

    def _set_status(self, text, seconds=5):
        """Show a message in the footer, then go back to the keyboard hints."""
        self.status.configure(text=text, fg=T["muted"])
        if self._status_job:
            self.after_cancel(self._status_job)
        self._status_job = self.after(seconds * 1000, lambda: self.status.configure(text=hints_text(), fg=T["faint"]))

    # ---- show / hide
    def _own_hwnd(self):
        return int(self.wm_frame(), 16)

    def show_launcher(self, search=False):
        self.summoned = True
        if self.state() in ("withdrawn", "iconic"):
            self.deiconify()
        self.lift()
        if IS_WIN:
            try:
                focus_window(self._own_hwnd())
            except Exception:
                log("show error:\n" + traceback.format_exc())
        self.search.focus_force()
        if search:
            self.search.select_range(0, "end")
            self.search.icursor("end")

    def hide_launcher(self):
        self.summoned = False
        self.withdraw()

    def toggle_launcher(self):
        visible = self.state() == "normal"
        if visible and IS_WIN and user32.GetForegroundWindow() == self._own_hwnd():
            self.hide_launcher()
        else:
            self.show_launcher()

    def _on_escape(self):
        if self.search_var.get():
            self.search_var.set("")
            self.search.focus_set()
        else:
            self.hide_launcher()

    # ---- list navigation
    def _on_search_change(self):
        if self.search_var.get():
            self.placeholder.place_forget()
        else:
            self.placeholder.place(in_=self.search, x=1, rely=0.5, anchor="w")
        self.refresh_view()

    def _select_row(self, iid):
        self.tree.focus_set()
        self.tree.selection_set(iid)
        self.tree.focus(iid)
        self.tree.see(iid)

    def _move(self, step):
        rows = list(self.row_items)
        if not rows:
            return "break"
        sel = self.tree.selection()
        cur = sel[0] if sel and sel[0] in self.row_items and self.focus_get() == self.tree else None
        if cur is None:
            self._select_row(rows[0] if step > 0 else rows[-1])
            return "break"
        i = rows.index(cur) + step
        if i < 0:
            self.tree.selection_remove(cur)
            self.search.focus_set()
            return "break"
        self._select_row(rows[min(i, len(rows) - 1)])
        return "break"

    def _type_to_search(self, e):
        if e.char and e.char.isprintable() and not e.state & 0x4:  # not with Ctrl
            self.search.focus_set()
            self.search.insert("end", e.char)
            return "break"
        if e.keysym == "BackSpace":
            self.search.focus_set()
            self.search.delete(len(self.search_var.get()) - 1, "end")
            return "break"
        return None

    def _skip_headers(self, _e=None):
        sel = self.tree.selection()
        if sel and sel[0] not in self.row_items:
            self.tree.selection_remove(sel[0])

    def _on_motion(self, e):
        iid = self.tree.identify_row(e.y)
        self._set_hover(iid if iid in self.row_items else None)
        self.tree.configure(cursor="hand2" if iid in SECTION_IDS else "")

    def _on_click(self, e):
        iid = self.tree.identify_row(e.y)
        if iid in SECTION_IDS:
            self.toggle_section(iid)
            return "break"
        self._drag_start(e)
        return None

    def toggle_section(self, hid):
        if self.search_var.get().strip():
            return  # while searching every section is shown
        self.collapsed.symmetric_difference_update({hid})
        save_ui({**load_ui(), "collapsed": sorted(self.collapsed)})
        self.refresh_view()

    def _set_hover(self, iid):
        if iid == self._hover:
            return
        for target, on in ((self._hover, False), (iid, True)):
            if target and self.tree.exists(target):
                tags = [t for t in self.tree.item(target, "tags") if t != "hover"]
                self.tree.item(target, tags=tags + ["hover"] if on else tags)
        self._hover = iid

    # ---- data
    def load_apps(self):
        threading.Thread(target=lambda: self.q.put(("apps", get_start_apps())), daemon=True).start()

    def _scan_windows(self):
        """Every second: refresh the open windows. Always reschedules, even after an error."""
        if self._quitting:
            return
        try:
            self._scan_windows_once()
        except Exception:
            log("window scan error:\n" + traceback.format_exc())
        finally:
            if not self._quitting:
                self.after(WINDOW_SCAN_MS, self._scan_windows)

    def _scan_windows_once(self):
        try:
            found = list_open_windows()
        except OSError:
            found = []
        titles = dict(found)
        for h, _ in found:
            if h not in self.win_ident:
                self.win_ident[h] = self._identify(h)
        for h in [h for h in self.win_ident if h not in titles]:
            del self.win_ident[h]

        # Stable order: windows keep their number. New windows go where your saved order puts their app,
        # otherwise at the end.
        rank = {k: i for i, k in enumerate(self.order_pref)}
        order = [h for h in self.window_order if h in titles]
        for h, _ in found:
            if h in order:
                continue
            r = rank.get(self._order_key(h))
            if r is None:
                order.append(h)
            else:
                pos = next((i for i, x in enumerate(order) if rank.get(self._order_key(x), 10 ** 9) > r), len(order))
                order.insert(pos, h)
        self.window_order = order
        self.titles = titles
        self.z_order = [h for h, _ in found]

        self._assign_slots()
        self._check_settings_file()

    # ---- ordering of the OPEN list (and so of the Alt+number keys)
    def _order_key(self, h):
        exe, aumid, prof = self.win_ident.get(h, ("", None, None))
        return prof or aumid or exe or f"title:{self.titles.get(h, '')}"

    def _order_changed(self, status=None):
        keys = list(dict.fromkeys(self._order_key(h) for h in self.window_order))
        self.order_pref = keys + [k for k in self.order_pref if k not in keys]
        save_ui({**load_ui(), "window_order": self.order_pref[:300]})
        self.window_sig = None
        self._assign_slots()
        if status:
            self._set_status(status)

    def move_window(self, hwnd, where):
        """where: "up", "down", "top", "bottom", or ("at", index) to drop it at a position."""
        order = self.window_order
        if hwnd not in order:
            return
        i = order.index(hwnd)
        order.pop(i)
        if where == "top":
            j = 0
        elif where == "bottom":
            j = len(order)
        elif where in ("up", "down"):
            j = max(0, min(len(order), i + (-1 if where == "up" else 1)))
        else:
            j = max(0, min(len(order), int(where[1])))
        order.insert(j, hwnd)
        self._order_changed()
        iid = f"w:{hwnd}"
        if self.tree.exists(iid):
            self.tree.selection_set(iid)
            self.tree.focus(iid)
            self.tree.see(iid)

    def sort_windows(self, how):
        if how == "name":
            self.window_order.sort(key=lambda h: self.titles.get(h, "").lower())
            msg = "Open windows sorted by name."
        elif how == "app":
            self.window_order.sort(key=lambda h: (os.path.basename(self.win_ident.get(h, ("",))[0] or "").lower(),
                                                  self.titles.get(h, "").lower()))
            msg = "Open windows grouped by app."
        else:  # forget: new windows go to the end again
            self.order_pref = []
            save_ui({**load_ui(), "window_order": []})
            self._set_status("Saved order forgotten — new windows go to the end.")
            return
        self._order_changed(msg + " The order is remembered.")

    def _sort_menu(self, parent):
        sub = tk.Menu(parent, tearoff=0)
        sub.add_command(label="By name", command=lambda: self.sort_windows("name"))
        sub.add_command(label="By app", command=lambda: self.sort_windows("app"))
        sub.add_separator()
        sub.add_command(label="Forget my order", command=lambda: self.sort_windows("forget"))
        return sub

    # drag & drop in the OPEN section
    def _drag_start(self, e):
        iid = self.tree.identify_row(e.y)
        it = self.row_items.get(iid)
        self._drag = (iid, e.y, False) if it and it["kind"] == "window" and not self.search_var.get().strip() else None

    def _drag_motion(self, e):
        if not self._drag:
            return
        iid, y0, moving = self._drag
        if not moving and abs(e.y - y0) < 6:
            return
        self._drag = (iid, y0, True)
        self.tree.configure(cursor="sb_v_double_arrow")
        target = self.tree.identify_row(e.y)
        it = self.row_items.get(target)
        self._set_hover(target if it and it["kind"] == "window" else None)

    def _drag_end(self, e):
        drag, self._drag = self._drag, None
        if not drag or not drag[2]:
            return
        self.tree.configure(cursor="")
        src = self.row_items.get(drag[0])
        dst = self.row_items.get(self.tree.identify_row(e.y))
        if src and dst and dst["kind"] == "window" and dst is not src:
            self.move_window(src["hwnd"], ("at", self.window_order.index(dst["hwnd"])))
            now = next((w for w in self.windows if w["hwnd"] == src["hwnd"]), None)
            key = slot_label(now["slot"]) if now and now.get("slot") is not None else ""
            self._set_status(f"Moved{' — now ' + key if key else ''}. The order is remembered.")
        return "break"

    def _move_selected_window(self, d):
        it = self._selected()
        if it and it["kind"] == "window" and not self.search_var.get().strip():
            self.move_window(it["hwnd"], d)
            return "break"
        return None

    # ---- settings.ini
    @staticmethod
    def _settings_mtime():
        try:
            return os.stat(SETTINGS_FILE).st_mtime
        except OSError:
            return None

    def _check_settings_file(self):
        m = self._settings_mtime()
        if m != self.settings_mtime:
            self.settings_mtime = m
            self.reload_settings()

    def reload_settings(self):
        warnings = load_settings()
        self.clip_store.reload_settings()
        pool = self.bindings + self.inactive_bindings
        for b in pool:
            b["slot"] = SLOT_INDEX.get(b["slot_id"])
        self.bindings = [b for b in pool if b["slot"] is not None]
        self.inactive_bindings = [b for b in pool if b["slot"] is None]
        self.tree.column("key", width=self._key_width())
        self.window_sig = None
        self._assign_slots()
        self.refresh_view()
        log("settings reloaded")
        if warnings:
            self._set_status("settings.ini: " + "; ".join(warnings), 12)
        else:
            self._set_status("Settings applied ✓")

    @staticmethod
    def _key_width():
        return 60 + 8 * len(MOD_LABEL)

    def open_settings(self):
        try:
            if IS_WIN:
                subprocess.Popen(["notepad.exe", SETTINGS_FILE])
            else:
                subprocess.Popen(["xdg-open", SETTINGS_FILE])
            self._set_status("Edit settings.ini and save — changes apply in a few seconds.", 8)
        except OSError as e:
            messagebox.showerror("Settings", str(e))

    def _identify(self, h):
        try:
            exe, aumid = window_identity(h)
        except OSError:
            exe, aumid = "", None
        try:
            prof = self._profile_for_window(h, exe)
        except Exception:
            log("profile match error:\n" + traceback.format_exc())
            prof = None
        return exe, aumid, prof

    def _profile_for_window(self, h, exe):
        """Which added browser profile a window belongs to, or None."""
        cands = [p for p in self.profiles if p["exe"].lower() == exe]
        if not cands:
            return None
        chromium = [(p, chromium_profile_dir(p)) for p in cands]
        if any(d for _, d in chromium):
            app_id = window_app_id(h) or ""
            for p, d in chromium:
                if not d:
                    continue
                if d == "Default":
                    if "._crx_" not in app_id and ".UserData." not in app_id:
                        return profile_id(p)
                elif app_id.endswith(chromium_app_id_suffix(d)):
                    return profile_id(p)
            return None
        # Other browsers (Firefox) run one process per profile: remember the process we started.
        pid = window_pid(h)
        if pid in self.pid_profile:
            return self.pid_profile[pid]
        now = time.monotonic()
        for p in cands:
            key = profile_id(p)
            started = PENDING_LAUNCHES.get(key)
            if started and now - started < 30:
                PENDING_LAUNCHES.pop(key, None)
                self.pid_profile[pid] = key
                return key
        return None

    def _assign_slots(self):
        """Fixed shortcuts first, then the remaining keys go to other open windows in order."""
        titles, order = self.titles, self.window_order
        bound = {b["slot"]: b for b in self.bindings}
        window_slot, slot_map = {}, {}

        ident = lambda h: self.win_ident.get(h, ("", None, None))  # noqa: E731
        profiles = {profile_id(p): p for p in self.profiles}

        self.profile_windows = {}
        for key in profiles:
            recent = [h for h in self.z_order if ident(h)[2] == key]
            self.profile_windows[key] = (recent, [h for h in order if h in recent])

        # Profile shortcuts claim their windows before general app shortcuts (e.g. plain "Chrome").
        for slot, b in sorted(bound.items(), key=lambda kv: not kv[1]["profile"]):
            recent = [h for h in self.z_order if h not in window_slot and binding_matches(b, ident(h))]
            stable = [h for h in order if h in recent]
            slot_map[slot] = ("binding", b, recent, stable)
            for h in stable:
                window_slot[h] = slot

        free = [s for s in range(HOTKEY_COUNT) if s not in bound]
        for h in order:
            if h not in window_slot and free:
                window_slot[h] = free.pop(0)
                slot_map[window_slot[h]] = ("window", h)
        self.slot_map = slot_map

        flags = {h: self._window_flags(h) for h in order}
        sig = ([(h, titles[h], window_slot.get(h), ident(h)[2], flags[h]) for h in order],
               [(b["slot"], b["name"], bool(slot_map[b["slot"]][2])) for b in self.bindings],
               sorted(k for k, (r, _) in self.profile_windows.items() if r))
        if sig == self.window_sig:
            return
        self.window_sig = sig
        def window_name(h):
            p = profiles.get(ident(h)[2])
            return f"{titles[h]}   ·   {short_profile_name(p)}" if p else titles[h]

        self.windows = [{
            "kind": "window",
            "name": window_name(h),
            "hwnd": h,
            "slot": window_slot.get(h),
            "icon_key": f"win:{h}",
            "icon_src": h,
            "flags": flags[h],
        } for h in order]

        alive = {f"win:{h}" for h in order}
        for k in [k for k in self.icons if k.startswith("win:") and k not in alive]:
            del self.icons[k]
            self.requested.discard(k)

        self.refresh_view()
        self._request_icons(self.windows)

    def _poll(self):
        """Every 50 ms: handle messages from the keyboard, tray and worker threads. Always reschedules."""
        if self._quitting:
            return
        try:
            for _ in range(200):
                try:
                    msg = self.q.get_nowait()
                except queue.Empty:
                    break
                try:
                    self._handle(msg)
                except Exception:
                    log(f"error handling {msg[0]!r}:\n" + traceback.format_exc())
        finally:
            if not self._quitting:
                self.after(50, self._poll)

    def _handle(self, msg):
        kind, *payload = msg
        if kind == "apps":
            self.apps = payload[0]
            self.refresh_view()
            self._request_icons(self.apps)
        elif kind == "icon":
            self._set_icon(*payload)
        elif kind == "switch":
            self._show_switch(*payload)
            self._scan_soon()
        elif kind == "cmd":
            self._command(payload[0])
        elif kind == "clip":
            self._on_clip(payload[0])
        elif kind == "status":
            self._set_status(payload[0], 6)
        elif kind == "open_url":
            self.open_url(*payload)

    def _command(self, cmd):
        try:
            if isinstance(cmd, tuple) and cmd[0] == "workspace":
                self.activate_workspace(cmd[1])
            elif cmd == "toggle":
                self.toggle_launcher()
            elif cmd == "next":
                self.switch_relative(1)
            elif cmd == "prev":
                self.switch_relative(-1)
            elif cmd == "switcher":
                self.toggle_switcher()
            elif cmd == "cycle":
                self.stop_cycle() if self.cycler.active else self.start_cycle()
            elif cmd == "cycle_stop":
                self.stop_cycle()
            elif cmd == "cycle_cancel":
                self.stop_cycle(go_back=True)
            elif cmd == "cycle_close":
                self.cycler.act("close")
            elif cmd == "cycle_min":
                self.cycler.act("minimize")
            elif cmd == "topmost":
                self.toggle_topmost()
            elif cmd == "opacity":
                self.cycle_opacity()
            elif cmd == "snippets":
                self.open_snippets()
            elif cmd == "cheat_arm":
                self.cheat_arm()
            elif cmd == "cheat_hide":
                self.hide_cheat_sheet()
            elif cmd == "mute":
                self.toggle_mute()
            elif cmd == "quicknote":
                self.open_quick_note()
            elif cmd == "explorer":
                self.open_file_explorer()
            elif cmd == "restore":
                self.restore_previous()
            elif cmd == "search":
                self.show_launcher(search=True)
            elif cmd == "show":
                self.show_launcher()
            elif cmd == "notes":
                self.toggle_notes()
            elif cmd == "clipboard":
                self.toggle_clipboard(user32.GetForegroundWindow() if IS_WIN else None)
            elif cmd == "workspaces":
                self.open_workspaces()
            elif cmd == "settings":
                self.open_settings()
            elif cmd == "autostart":
                self.toggle_autostart()
            elif cmd == "quit":
                self.quit_app()
        except Exception:
            log(f"command {cmd!r} error:\n" + traceback.format_exc())

    def switch_relative(self, step):
        """Go to the next / previous window in the OPEN order (wraps around)."""
        order = [w["hwnd"] for w in self.windows]
        if not order:
            self._set_status("No open windows.")
            return
        fg = user32.GetForegroundWindow() if IS_WIN else None
        i = order.index(fg) if fg in order else (-1 if step > 0 else 0)
        target = order[(i + step) % len(order)]
        key = f"{MOD_LABEL}{CMD_KEYS.get('next' if step > 0 else 'prev', '')}"
        try:
            result = toggle_window(target) if IS_WIN and len(order) == 1 else focus_window(target) if IS_WIN else True
        except Exception:
            log("switch error:\n" + traceback.format_exc())
            result = "error"
        self._show_switch(key, target, result)
        self._scan_soon()

    def start_cycle(self):
        self.cycler.start()

    def stop_cycle(self, go_back=False):
        self.cycler.stop(go_back)

    def toggle_switcher(self):
        if self.switcher is None or not self.switcher.winfo_exists():
            self.switcher = SwitcherOverlay(self)
        if self.switcher.is_open():
            self.switcher.focus_force()  # already open: it only opens the list
        else:
            self.switcher.show()

    def _scan_soon(self):
        """Refresh 'most recent' order right after a switch so cycling stays accurate."""
        try:
            self.z_order = [h for h, _ in list_open_windows()]
        except OSError:
            pass
        self._assign_slots()

    def _request_icons(self, items):
        jobs = []
        for it in items:
            k = it["icon_key"]
            if k not in self.requested:
                self.requested.add(k)
                jobs.append((k, it["icon_src"]))
        if jobs:
            threading.Thread(target=self._icon_worker, args=(jobs,), daemon=True).start()

    def _icon_worker(self, jobs):
        if IS_WIN:
            ole32.CoInitializeEx(None, 0x2)
        try:
            for key, src in jobs:
                try:
                    if isinstance(src, int):
                        png = window_icon_png(src, self.icon_px)
                    else:
                        png = extract_icon_png(src, self.icon_px)
                except Exception:
                    png = None
                self.q.put(("icon", key, png))
        finally:
            if IS_WIN:
                ole32.CoUninitialize()

    def _set_icon(self, key, png):
        if key not in self.requested:  # window closed meanwhile
            return
        img = self.blank
        if png:
            try:
                img = tk.PhotoImage(data=base64.b64encode(png).decode("ascii"))
            except tk.TclError:
                pass
        self.icons[key] = img
        for iid in self.iids_by_key.get(key, []):
            if self.tree.exists(iid):
                self.tree.item(iid, image=img)

    # ---- view
    @staticmethod
    def _row_id(it):
        if it["kind"] == "window":
            return f"w:{it['hwnd']}"
        if it["kind"] == "app":
            return "a:" + it["app_id"]
        if it["kind"] == "pin":
            return "m:" + pin_id(it)
        if it["kind"] == "binding":
            return f"b:{it['slot']}"
        if it["kind"] == "workspace":
            return "ws:" + it["id"]
        if it["kind"] == "command":
            return "c:" + it["cmd"]
        if it["kind"] in ("calc", "url", "web", "timer_new", "pomo_start", "pomo_stop", "hint"):
            return "s:" + it["kind"]
        if it["kind"] == "timer":
            return "t:" + it["id"]
        return "p:" + profile_id(it)

    def _usage_key(self, it):
        k = it["kind"]
        if k == "window":
            exe, _aumid, prof = self.win_ident.get(it["hwnd"], ("", None, None))
            return "x:" + (prof or exe) if (prof or exe) else None
        if k == "app":
            return "a:" + it["app_id"]
        if k == "pin":
            return "m:" + pin_id(it)
        if k == "profile":
            return "p:" + profile_id(it)
        if k == "binding":
            return "b:" + json.dumps(binding_key(it))
        if k == "workspace":
            return "ws:" + it["id"]
        if k == "command":
            return "c:" + it["cmd"]
        return None

    def _key_text(self, it):
        """The shortcut shown on the right of a row."""
        k = it["kind"]
        if k == "window":
            return slot_label(it["slot"]) if it["slot"] is not None else ""
        if k == "binding":
            return slot_label(it["slot"])
        if k == "profile":
            pk = profile_id(it)
            b = next((x for x in self.bindings if x["profile"] == pk), None)
            return slot_label(b["slot"]) if b else ""
        if k == "workspace":
            b = next((x for x in self.bindings if x.get("workspace") == it["id"]), None)
            return slot_label(b["slot"]) if b else ""
        if k == "timer":
            t = self.timers.get(it["id"])
            return time.strftime("%H:%M", time.localtime(t["due"])) if t else ""
        return {"calc": "copy", "url": "open", "web": "search", "timer_new": "start", "pomo_start": "start",
                "pomo_stop": "stop"}.get(k, "")

    def _search_results(self, raw):
        """Ranked results for the search bar: smart results first, then the best matches."""
        q = raw.lower()
        smart, ranked = [], []
        for opt in parse_timer_query(raw):
            smart.append({"kind": "timer_new", "spec": opt, "name": opt["title"], "icon_key": "none"})
        first = q.split()[0] if q.split() else ""
        if not smart and first in ("timer", "remind", "reminder", "alarm", "countdown"):
            smart.append({"kind": "hint", "icon_key": "none",
                          "name": "Add a time:  timer 10m tea  ·  remind 18:30 call prof  ·  remind in 2h stretch"})
        if first in ("timers", "timer", "reminders", "alarms", "remind"):
            smart += self.timer_rows()
        if first.startswith("pomo") or q in ("focus", "focus timer"):
            if self.timers.pomo:
                t = next((x for x in self.timers.items if x["kind"] == "pomo"), None)
                left = f" — {fmt_left(t['due'] - time.time())} left" if t else ""
                smart.append({"kind": "pomo_stop", "name": f"Stop pomodoro{left}", "icon_key": "none"})
            else:
                smart.append({"kind": "pomo_start", "icon_key": "none",
                              "name": f"Start pomodoro — {SETTINGS.get('pomo_work', 25)} min work, "
                                      f"{SETTINGS.get('pomo_break', 5)} min break"})
        calc = try_calc(raw)
        if calc:
            smart.append({"kind": "calc", "name": f"= {calc[1]}", "value": calc[1], "icon_key": "none"})
        engines = SETTINGS.get("engines", {})
        head, _, rest = raw.partition(" ")
        if head.lower() in engines and rest.strip():
            tpl = engines[head.lower()]
            smart.append({"kind": "web", "name": f"Search {engine_name(tpl)} for “{rest.strip()}”",
                          "url": engine_url(tpl, rest.strip()), "icon_key": "none"})
        url = looks_like_url(raw)
        if url:
            smart.append({"kind": "url", "name": f"Open {url}", "url": url, "icon_key": "none"})

        groups = ((8, self.windows), (5, self.workspaces), (4, self.bindings), (3, self.profiles),
                  (3, self.pins), (0, self.apps))
        for bonus, items in groups:
            for it in items:
                if it["kind"] == "binding" and it.get("workspace"):
                    continue  # the workspace row itself is shown
                m = match_score(q, it["name"])
                if m:
                    ranked.append((m + bonus + self.usage.bonus(self._usage_key(it)), it))
        for cid, name, words, _confirm in COMMANDS:
            if len(q) < 2 or (cid == "autostart" and not IS_WIN):
                continue
            m = match_score(q, name, words)
            if m >= 65:
                it = {"kind": "command", "cmd": cid, "name": name, "icon_key": "none"}
                ranked.append((m - 5 + self.usage.bonus(self._usage_key(it)), it))
        ranked.sort(key=lambda x: -x[0])
        out = smart + [it for _, it in ranked[:80]]
        default = SETTINGS.get("default_engine")
        if default in engines and not (head.lower() in engines and rest.strip()):
            tpl = engines[default]
            out.append({"kind": "web", "name": f"Search {engine_name(tpl)} for “{raw}”",
                        "url": engine_url(tpl, raw), "icon_key": "none"})
        return out

    def refresh_view(self):
        sel = self.tree.selection()
        sel = sel[0] if sel else None
        top = self.tree.yview()[0]

        raw = self.search_var.get().strip()
        q = raw.lower()
        self.tree.delete(*self.tree.get_children())
        self.iids_by_key, self.row_items = {}, {}
        self._hover = None

        if q:
            results = self._search_results(raw)
            self.tree.insert("", "end", iid="h:results", text="   RESULTS", tags=("header",))
            for it in results:
                self._insert_row(it)
            if sel and self.tree.exists(sel):
                self.tree.selection_set(sel)
            self.tree.yview_moveto(0)
            return

        fixed = sorted(self.bindings, key=lambda b: b["slot"])
        sections = (("h:timers", "TIMERS", self.timer_rows()),
                    ("h:open", "OPEN", self.windows),
                    ("h:fixed", "FIXED SHORTCUTS", fixed),
                    ("h:ws", "WORKSPACES", self.workspaces),
                    ("h:prof", "BROWSER PROFILES", self.profiles),
                    ("h:pins", "MY APPS", self.pins),
                    ("h:apps", "APPS", self.apps))
        collapsed = set() if q else self.collapsed  # searching always shows everything
        for hid, title, items in sections:
            matches = [it for it in items if q in it["name"].lower()]
            if not matches and not (hid in ("h:pins", "h:prof", "h:ws") and not q):
                continue
            folded = hid in collapsed
            label = f"   {'▸' if folded else '▾'}  {title}" + (f"   ·  {len(matches)}" if folded else "")
            self.tree.insert("", "end", iid=hid, text=label, tags=("header",))
            parent = ""
            if folded:
                continue
            if hid == "h:prof" and not self.profiles:
                self.tree.insert(parent, "end", iid="h:prof-hint", tags=("dim",),
                                 text="   Use “+ Add → Browser profiles” to add Chrome, Edge, Brave or Firefox profiles")
            if hid == "h:pins" and not self.pins:
                self.tree.insert(parent, "end", iid="h:pins-hint", tags=("dim",),
                                 text="   Use “+ Add”, or right-click any app → Add to My apps")
            if hid == "h:ws" and not self.workspaces:
                self.tree.insert(parent, "end", iid="h:ws-hint", tags=("dim",),
                                 text="   Use “+ Add → New workspace” to open a group of apps in one go")
            for it in matches:
                self._insert_row(it)

        if not self.apps:
            self.tree.insert("", "end", iid="h:loading", text="   Loading apps…", tags=("dim",))

        if sel and self.tree.exists(sel):
            self.tree.selection_set(sel)
            self.tree.focus(sel)
        self.tree.yview_moveto(top)

    def _insert_row(self, it):
        iid = self._row_id(it)
        n = 1
        while self.tree.exists(iid):
            n += 1
            iid = f"{self._row_id(it)}#{n}"
        tags, name = (), it["name"]
        if it["kind"] == "binding" and not it.get("workspace"):
            entry = self.slot_map.get(it["slot"])
            if not (entry and entry[2]):
                tags = ("dim",)  # not running: the shortcut will start it
        elif it["kind"] == "profile" and not self.profile_windows.get(profile_id(it), ([], []))[0]:
            tags = ("dim",)  # not open right now
        elif it["kind"] == "workspace":
            n_items = len(it["items"])
            name = f"{it['name']}   ·   {n_items} item{'s' if n_items != 1 else ''}"
        elif it["kind"] in ("command", "web", "hint"):
            tags = ("dim",)
        elif it["kind"] == "window" and it.get("flags"):
            name = f"{name}   ·   {it['flags']}"
        self.tree.insert("", "end", iid=iid, text="   " + name, values=(self._key_text(it) + "   ",),
                         image=self.icons.get(it["icon_key"], self.blank), tags=tags)
        self.row_items[iid] = it
        self.iids_by_key.setdefault(it["icon_key"], []).append(iid)

    # ---- actions
    def _selected(self):
        sel = self.tree.selection()
        return self.row_items.get(sel[0]) if sel else None

    def _launch_first(self):
        if self.row_items:
            self._launch(next(iter(self.row_items.values())))

    def launch_selected(self):
        item = self._selected()
        if item:
            self._launch(item)

    def _after_launch(self):
        if self.summoned and SETTINGS.get("hide_after_launch", True):
            self.hide_launcher()
            self.search_var.set("")

    def _launch(self, item):
        kind = item["kind"]
        self.usage.bump(self._usage_key(item))
        if kind == "calc":
            try:
                set_clipboard_text(item["value"], self._own_hwnd() if IS_WIN else None)
                self._set_status(f"Copied {item['value']} ✓")
            except OSError as e:
                self._set_status(f"Couldn't copy: {e}")
            self._after_launch()
            return
        if kind in ("url", "web"):
            self.open_url(item["url"])
            self._after_launch()
            return
        if kind == "command":
            self.run_command(item["cmd"])
            return
        if kind == "hint":
            return
        if kind == "timer_new":
            self.start_timer(item["spec"])
            self.search_var.set("")
            self._after_launch()
            return
        if kind == "pomo_start":
            self.start_pomodoro()
            self.search_var.set("")
            self._after_launch()
            return
        if kind == "pomo_stop":
            self.stop_pomodoro()
            self.search_var.set("")
            return
        if kind == "timer":
            self.cancel_timer(item["id"], ask=True)
            return
        if kind == "workspace" or (kind == "binding" and item.get("workspace")):
            self.activate_workspace(item["id"] if kind == "workspace" else item["workspace"])
            return
        if kind in ("app", "pin") and IS_WIN:
            # Already running? Switch to it (like clicking it in the taskbar). Right-click → Open new window.
            b = self._binding_for_item(item, -1)
            if b:
                recent = [h for h in self.z_order if binding_matches(b, self.win_ident.get(h, ("", None, None)))]
                if recent:
                    stable = [h for h in self.window_order if h in recent]
                    try:
                        result, hwnd = activate_binding(b, recent, stable)
                    except Exception:
                        log("switch error:\n" + traceback.format_exc())
                        result, hwnd = "error", None
                    self._show_switch("Click", hwnd, result, item["name"])
                    self._after_launch()
                    return
        if item["kind"] == "profile" and IS_WIN:
            recent, stable = self.profile_windows.get(profile_id(item), ([], []))
            try:
                result, hwnd = activate_binding(self._binding_for_item(item, -1), recent, stable)
            except Exception:
                log("profile open error:\n" + traceback.format_exc())
                result, hwnd = "error", None
            self._show_switch("Click", hwnd, result, item["name"])
            self._after_launch()
            return
        if item["kind"] == "binding":
            entry = self.slot_map.get(item["slot"]) or ("binding", item, [], [])
            try:
                result, hwnd = activate_binding(item, entry[2], entry[3])
            except Exception:
                log("switch error:\n" + traceback.format_exc())
                result, hwnd = "error", None
            self._show_switch("Click", hwnd, result, item["name"])
            self._after_launch()
            return
        if item["kind"] == "window":
            try:
                result = focus_window(item["hwnd"])
            except Exception:
                log("switch error:\n" + traceback.format_exc())
                result = "error"
            self._show_switch("Click", item["hwnd"], result)
            self._after_launch()
            return
        try:
            launch(item)
            self._after_launch()
        except OSError as e:
            messagebox.showerror("Could not open", f"{item['name']}\n\n{e}")

    def _launch_new(self, item):
        """Start another copy even if the app is already open."""
        try:
            launch(item)
            self.usage.bump(self._usage_key(item))
            self._set_status(f"{item['name']}: starting…")
            self._after_launch()
        except OSError as e:
            messagebox.showerror("Could not open", f"{item['name']}\n\n{e}")

    def open_url(self, url, profile_key=None):
        """Open a link in the chosen browser profile, the profile from settings.ini, or the default browser."""
        p = None
        if profile_key:
            p = next((x for x in self.profiles if profile_id(x) == profile_key), None)
        elif SETTINGS.get("browser_profile"):
            want = SETTINGS["browser_profile"].lower()
            p = next((x for x in self.profiles if want in x["name"].lower()), None)
        try:
            if p:
                args = p["args"]
                start_program(p["exe"], f'{args} "{url}"' if isinstance(args, str) else [*args, url])
            elif not IS_WIN:
                import webbrowser
                webbrowser.open(url)
            elif LAUNCHER_ELEVATED:
                subprocess.Popen(["explorer.exe", url])  # opens in the default browser as the normal user
            else:
                os.startfile(url)
        except OSError as e:
            messagebox.showerror("Could not open", f"{url}\n\n{e}")

    def run_command(self, cmd):
        confirm = {c[0]: c[3] for c in COMMANDS}.get(cmd)
        name = {c[0]: c[1] for c in COMMANDS}.get(cmd, cmd)
        if confirm and not messagebox.askyesno(name, f"{name} now? Unsaved work in other apps may be lost.",
                                               parent=self):
            return
        if cmd in ("lock", "sleep", "shutdown", "restart", "signout", "recycle", "taskmgr"):
            self._after_launch()
            if not IS_WIN:
                return
            if cmd == "lock":
                user32.LockWorkStation()
            elif cmd == "sleep":
                self.after(400, system_sleep)
            elif cmd in ("shutdown", "restart", "signout"):
                flag = {"shutdown": "/s", "restart": "/r", "signout": "/l"}[cmd]
                subprocess.Popen(["shutdown", flag] + ([] if cmd == "signout" else ["/t", "0"]),
                                 creationflags=CREATE_NO_WINDOW)
            elif cmd == "recycle":
                hr = shell32.SHEmptyRecycleBinW(self._own_hwnd(), None, 0)
                self._set_status("Recycle Bin emptied ✓" if hr == 0 else "The Recycle Bin is already empty.")
            elif cmd == "taskmgr":
                subprocess.Popen(["taskmgr.exe"])
            return
        self.search_var.set("")
        if cmd == "newws":
            self.open_workspaces(new=True)
        elif cmd == "reload":
            self.load_apps()
            self._set_status("Reloading the app list…")
        elif cmd == "clipboard":
            self.toggle_clipboard(None)
        elif cmd == "sysmon":
            self._after_launch()
            self.open_system_monitor()
        elif cmd == "explorer":
            self._after_launch()
            self.open_file_explorer()
        elif cmd in ("snippets", "restore"):
            self._after_launch()
            self._command(cmd)
        else:  # settings, notes, workspaces, autostart, quit
            self._command(cmd)

    # ---- clipboard
    def _on_clip(self, text):
        if not SETTINGS.get("clip_enabled", True):
            return
        if self.clip_store.add(text) and self.clip_win is not None and self.clip_win.is_open():
            self.clip_win.refresh()

    def toggle_clipboard(self, prev_hwnd=None):
        if self.clip_win is None or not self.clip_win.winfo_exists():
            self.clip_win = ClipboardWindow(self)
        if self.clip_win.is_open() and IS_WIN and user32.GetForegroundWindow() == int(self.clip_win.wm_frame(), 16):
            self.clip_win.hide()
        else:
            self.clip_win.show(prev_hwnd)

    def clip_to_notes(self, text, count=1):
        """Save clipboard items into the notes (asks which tab). Waits for the notes to be unlocked."""
        if not notes_crypto_available():
            messagebox.showinfo("Save to Notes", "Notes need the “cryptography” package:\n\n"
                                                 "python -m pip install cryptography")
            return
        if self.notes is None or not self.notes.winfo_exists():
            self.notes = NotesWindow(self)
        if self.notes.unlocked:
            self.notes_receive(text, count)
            return
        self.notes.pending_clip.append((text, count))
        self.notes._show_lock()
        self.notes.show()

    def notes_receive(self, text, count=1):
        n = self.notes
        if n is None or not n.unlocked:
            return
        n.show()
        labels = ["＋  New tab"] + [f"Add to the end of “{t['title']}”" for t in n.tabs]
        dlg = ListPicker(n, f"Save {count} clipboard item{'s' if count > 1 else ''} to Notes", labels,
                         multi=False, ok_text="Save")
        if dlg.result is None:
            return
        if dlg.result[0] == 0:
            i = n._add_tab(f"Clipboard {time.strftime('%d %b %H:%M')}", text)
        else:
            i = dlg.result[0] - 1
            box = n.tabs[i]["text"]
            sep = "\n\n" if box.get("1.0", "end-1c").strip() else ""
            box.insert("end", sep + text)
            n.tabs[i]["dirty"] = True
        n.nb.select(i)
        n.save_now()
        n.tabs[i]["text"].see("end")
        n._focus_current()
        n.saved_lbl.configure(text=f"Saved {count} clipboard item{'s' if count > 1 else ''} ✓  encrypted")

    def on_notes_unlocked(self, vault):
        self.after(300, self._flush_quick_notes)
        if (SETTINGS.get("clip_save") and SETTINGS.get("clip_encryption") == "password"
                and not os.path.isfile(CLIP_KEY_FILE)):
            clip_create_keys(*vault)
            self.clip_store.keys_created()
            log("clipboard history: created its encryption key")
        if self.clip_store.locked_count() and os.path.isfile(CLIP_KEY_FILE):
            try:  # unlocking the notes also unlocks older clipboard items
                self.clip_store.unlock(clip_private_key(vault=vault))
            except Exception:
                log("clipboard unlock error:\n" + traceback.format_exc())
            if self.clip_win is not None and self.clip_win.winfo_exists() and self.clip_win.is_open():
                self.clip_win.refresh()

    def on_notes_locked(self):
        self.clip_store.lock()
        if self.clip_win is not None and self.clip_win.winfo_exists() and self.clip_win.is_open():
            self.clip_win.refresh()

    # ---- workspaces
    def open_workspaces(self, new=False, select_id=None):
        WorkspacesDialog(self, select_id=select_id, start_new=new)

    def _ws_by_id(self, ws_id):
        return next((w for w in self.workspaces if w["id"] == ws_id), None)

    def ws_entry(self, it):
        """A workspace entry with how to recognise its windows and which are open now."""
        item = ws_item_to_item(it)
        b = None if item["kind"] == "url" else self._binding_for_item(item, -1)
        if b:
            wins = [h for h in self.z_order if binding_matches(b, self.win_ident.get(h, ("", None, None)))]
        elif item["kind"] == "app" and IS_WIN:
            wins = [h for h in self.z_order if window_app_id(h) == item["app_id"]]
        else:
            wins = []
        return {"it": it, "item": item, "b": b, "wins": wins}

    def item_to_ws(self, item):
        """A launcher row as a workspace entry (open windows keep their current position)."""
        kind = item["kind"]
        place = {"pos": "none", "monitor": 0, "rect": None}
        if kind == "app":
            return clean_ws_item({"type": "app", "name": item["name"], "app_id": item["app_id"], "place": place})
        if kind == "pin":
            if item["target"] == "app":
                return clean_ws_item({"type": "app", "name": item["name"], "app_id": item["app_id"], "place": place})
            return clean_ws_item({"type": "file", "name": item["name"], "path": item["path"], "place": place})
        if kind == "profile":
            return clean_ws_item({"type": "profile", "name": item["name"], "exe": item["exe"],
                                  "args": item["args"], "place": place})
        if kind == "binding" and not item.get("workspace"):
            if item["profile"]:
                p = next((x for x in self.profiles if profile_id(x) == item["profile"]), None)
                return self.item_to_ws(p) if p else None
            if item["app_id"]:
                return clean_ws_item({"type": "app", "name": item["name"], "app_id": item["app_id"], "place": place})
            if item["exe"]:
                return clean_ws_item({"type": "file", "name": item["name"], "path": item["exe"], "place": place})
            return None
        if kind == "window":
            exe, aumid, prof = self.win_ident.get(item["hwnd"]) or self._identify(item["hwnd"])
            pos, mon, rect = window_placement_now(item["hwnd"])
            place = {"pos": pos, "monitor": mon, "rect": rect}
            if prof:
                p = next((x for x in self.profiles if profile_id(x) == prof), None)
                if p:
                    out = self.item_to_ws(p)
                    out["place"] = place
                    return clean_ws_item(out)
            match = self._start_app_for(exe, aumid)
            if match:
                return clean_ws_item({"type": "app", "name": match["name"], "app_id": match["app_id"],
                                      "place": place})
            if aumid:
                return clean_ws_item({"type": "app", "name": item["name"], "app_id": aumid, "place": place})
            if exe:
                return clean_ws_item({"type": "file", "name": self._exe_name(exe), "path": exe, "place": place})
        return None

    def add_to_workspace(self, ws, item):
        entry = self.item_to_ws(item)
        if not entry:
            messagebox.showinfo("Add to workspace", f"Couldn't tell which program “{item['name']}” is.")
            return
        if ws is None:
            name = simpledialog.askstring("New workspace", "Name (e.g. Work, Study, Gaming):", parent=self)
            if not name or not name.strip():
                return
            ws = make_workspace(name.strip())
            self.workspaces.append(ws)
        ws["items"].append(entry)
        self._save()
        self._set_status(f"Added {entry['name']} to “{ws['name']}”.")

    def rename_workspace(self, ws, parent=None):
        new = simpledialog.askstring("Rename workspace", "Name:", initialvalue=ws["name"], parent=parent or self)
        if new and new.strip():
            ws["name"] = new.strip()
            for b in self.bindings + self.inactive_bindings:
                if b.get("workspace") == ws["id"]:
                    b["name"] = ws["name"]
            self._save()

    def delete_workspace(self, ws, parent=None):
        if not messagebox.askyesno("Delete workspace", f"Delete the workspace “{ws['name']}”?\n\n"
                                   "Your apps aren't touched — only this group is removed.", parent=parent or self):
            return False
        self.workspaces = [w for w in self.workspaces if w is not ws]
        self.bindings = [b for b in self.bindings if b.get("workspace") != ws["id"]]
        self.inactive_bindings = [b for b in self.inactive_bindings if b.get("workspace") != ws["id"]]
        self._save()
        return True

    def activate_workspace(self, ws):
        ws = self._ws_by_id(ws) if isinstance(ws, str) else ws
        if not ws:
            return
        if not ws["items"]:
            self._set_status(f"“{ws['name']}” is empty — right-click it → Edit to add apps.", 8)
            self.open_workspaces(select_id=ws["id"])
            return
        plan = [self.ws_entry(it) for it in ws["items"]]
        known = [p for p in plan if p["b"]]
        # Already open? Then just bring it all to the front. Otherwise start what's missing (and open links).
        start_mode = not known or any(not p["wins"] for p in known)
        self.usage.bump("ws:" + ws["id"])
        self._set_status(f"Opening “{ws['name']}”…")
        threading.Thread(target=self._run_workspace, args=(ws, plan, start_mode, set(self.titles)),
                         daemon=True).start()
        self._after_launch()

    def _run_workspace(self, ws, plan, start_mode, before):
        if IS_WIN:
            ole32.CoInitializeEx(None, 0x2)
        started, raised = 0, 0
        try:
            launched = []
            for p in plan:
                if p["wins"]:
                    continue
                if p["item"]["kind"] == "url":
                    if start_mode:  # Tk isn't thread-safe: let the main thread open it
                        self.q.put(("open_url", p["item"]["url"], p["it"].get("profile")))
                        started += 1
                    continue
                if not start_mode and not p["b"]:
                    continue
                try:
                    launch(p["item"])
                    launched.append(p)
                    started += 1
                except OSError:
                    log(f"workspace: couldn't start {p['it']['name']}:\n" + traceback.format_exc())
                time.sleep(0.2)
            # windows that are already open: put them in place and bring them up (first item ends on top)
            for p in reversed(plan):
                if p["wins"] and IS_WIN:
                    h = p["wins"][0]
                    place_window(h, p["it"]["place"])
                    focus_window(h)
                    raised += 1
            # wait for the new windows, then put them in place (twice: some apps move themselves at start)
            pending = [p for p in launched if p["b"] or p["item"]["kind"] == "app"]
            seen, todo = set(before), []
            deadline = time.monotonic() + 25
            while IS_WIN and (pending or todo) and time.monotonic() < deadline:
                time.sleep(0.35)
                for h, _title in list_open_windows():
                    if h in seen:
                        continue
                    seen.add(h)
                    ident = self._identify(h)
                    for p in pending:
                        ok = (binding_matches(p["b"], ident) if p["b"]
                              else window_app_id(h) == p["item"].get("app_id"))
                        if ok:
                            pending.remove(p)
                            if p["it"]["place"]["pos"] != "none":
                                now = time.monotonic()
                                todo += [(now + 0.4, h, p["it"]["place"]), (now + 1.8, h, p["it"]["place"])]
                            break
                now = time.monotonic()
                for job in [j for j in todo if j[0] <= now]:
                    todo.remove(job)
                    place_window(job[1], job[2])
        except Exception:
            log("workspace error:\n" + traceback.format_exc())
        finally:
            if IS_WIN:
                ole32.CoUninitialize()
        parts = ([f"started {started}"] if started else []) + ([f"brought {raised} to the front"] if raised else [])
        self.q.put(("status", f"Workspace “{ws['name']}”: " + (", ".join(parts) or "nothing to do") + " ✓"))

    def close_workspace(self, ws, parent=None):
        plan = [self.ws_entry(it) for it in ws["items"]]
        wins = list(dict.fromkeys(h for p in plan for h in p["wins"]))
        if not wins:
            self._set_status(f"Nothing from “{ws['name']}” is open.")
            return
        if not messagebox.askyesno("Close workspace", f"Close {len(wins)} window{'s' if len(wins) > 1 else ''} "
                                   f"of “{ws['name']}”? Apps will ask to save unsaved work.", parent=parent or self):
            return
        for h in wins:
            close_window(h)

    def _ws_menu(self, parent, item):
        sub = tk.Menu(parent, tearoff=0)
        for ws in self.workspaces:
            sub.add_command(label=ws["name"], command=lambda ws=ws: self.add_to_workspace(ws, item))
        if self.workspaces:
            sub.add_separator()
        sub.add_command(label="New workspace…", command=lambda: self.add_to_workspace(None, item))
        return sub


    # ---- always on top, see-through, force quit
    def toast(self, text, ms=1600):
        try:
            if self._toast is None or not self._toast.winfo_exists():
                self._toast = Toast(self)
            self._toast.show(text, ms)
        except tk.TclError:
            pass

    def _window_flags(self, h):
        try:
            parts = ["on top"] if is_topmost(h) else []
            op = get_opacity(h)
            if op < 99:
                parts.append(f"{op}%")
            exe = (self.win_ident.get(h) or ("", None, None))[0]
            if exe and exe in self.muted_exes:
                parts.append("muted")
            g = self.group_label(h)
            if g:
                parts.append(g)
            return " · ".join(parts)
        except Exception:
            return ""

    def _target_window(self):
        """The window the shortcut acts on: the active one (not the desktop or the taskbar)."""
        h = user32.GetForegroundWindow() if IS_WIN else FAKE_WIN.get("fg")
        return h if h in self.titles else None

    def _short_title(self, h):
        t = self.titles.get(h, "window")
        return t if len(t) <= 44 else t[:43] + "…"

    def toggle_topmost(self, h=None):
        h = h or self._target_window()
        if not h:
            self.toast("Click a window first, then press the shortcut again")
            return
        try:
            on = set_topmost(h, not is_topmost(h))
        except Exception:
            log("always on top error:\n" + traceback.format_exc())
            self.toast("Couldn't change that window")
            return
        self.toast(("Always on top: ON — " if on else "Always on top: off — ") + self._short_title(h))
        self.window_sig = None
        self._assign_slots()

    def cycle_opacity(self, h=None):
        h = h or self._target_window()
        if not h:
            self.toast("Click a window first, then press the shortcut again")
            return
        cur = get_opacity(h)
        nxt = next((lv for lv in SETTINGS.get("opacity_levels", [85, 70, 55]) if lv < cur - 1), 100)
        self.set_window_opacity(h, nxt)

    def set_window_opacity(self, h, pct):
        try:
            now = set_opacity(h, pct)
        except Exception:
            log("see-through error:\n" + traceback.format_exc())
            self.toast("Couldn't change that window")
            return
        self.toast(("Solid again — " if now >= 99 else f"See-through {now}% — ") + self._short_title(h))
        self.window_sig = None
        self._assign_slots()

    def kill_window(self, h):
        name = self._short_title(h)
        if not messagebox.askyesno("Force quit", f"End “{name}” right away?\n\nThe program closes without asking — "
                                   "unsaved work in it is lost. Use Close window when it still responds.",
                                   icon="warning", parent=self):
            return
        ok = kill_window_process(h)
        self._set_status(f"Ended “{name}”." if ok else
                         f"Couldn't end “{name}” (it may run as administrator, or be a system program).")


    # ---- cheat sheet (hold the modifier)
    def cheat_arm(self):
        if self._cheat_job:
            self.after_cancel(self._cheat_job)
            self._cheat_job = None
        ms = SETTINGS.get("cheat_ms", 800)
        if ms:
            self._cheat_job = self.after(ms, self._cheat_check)

    def _cheat_check(self):
        self._cheat_job = None
        hk = self.hotkeys
        if not hk or not hk.mod_alone or not hk.mods_down:
            return
        if IS_WIN and not _modifiers_match():
            return
        if self.cycler.active or (self.switcher is not None and self.switcher.winfo_exists() and self.switcher.is_open()):
            return
        self.show_cheat_sheet()
        hk.cheat_visible = True
        send_mask_key()  # so letting go of Alt doesn't open the app's menu bar

    def show_cheat_sheet(self):
        if self.cheat is None or not self.cheat.winfo_exists():
            self.cheat = CheatSheet(self)
        self.cheat.show()

    def hide_cheat_sheet(self):
        if self._cheat_job:
            self.after_cancel(self._cheat_job)
            self._cheat_job = None
        if self.hotkeys:
            self.hotkeys.cheat_visible = False
        if self.cheat is not None and self.cheat.winfo_exists():
            self.cheat.hide()

    # ---- mute an app
    def toggle_mute(self, h=None):
        h = h or self._target_window()
        if not h:
            self.toast("Click a window first, then press the shortcut again")
            return
        try:
            muted, n = app_mute(h)
        except Exception:
            log("mute error:\n" + traceback.format_exc())
            self.toast("Couldn't change its sound")
            return
        if not n:
            self.toast(f"No sound from {self._short_title(h)} — play something first, then try again", 2600)
            return
        exe = (self.win_ident.get(h) or ("", None, None))[0]
        if exe:
            (self.muted_exes.add if muted else self.muted_exes.discard)(exe)
        self.toast(("Muted — " if muted else "Sound on — ") + self._short_title(h))
        self.window_sig = None
        self._assign_slots()

    # ---- quick note
    def open_quick_note(self):
        prev = user32.GetForegroundWindow() if IS_WIN else None
        if self.quick_box is None or not self.quick_box.winfo_exists():
            self.quick_box = QuickNoteBox(self)
        if self.quick_box.is_open():
            self.quick_box.hide()
        else:
            self.quick_box.show(prev)

    def add_quick_note(self, text):
        line = (time.strftime("%d %b %H:%M") + "  " if SETTINGS.get("quick_time", True) else "") + text
        n = self.notes
        if n is not None and n.winfo_exists() and n.unlocked:
            try:
                self._append_quick(n, [line])
                self.toast(f"Saved to “{SETTINGS.get('quick_tab', 'Quick notes')}” ✓")
                return
            except Exception:
                log("quick note error:\n" + traceback.format_exc())
        try:
            data = protected_load(QUICK_PENDING_FILE, {"items": []})
            data.setdefault("items", []).append(line)
            protected_save(QUICK_PENDING_FILE, data)
            self.toast("Saved ✓ — it goes into your notes the next time you unlock them", 2600)
        except Exception as e:
            log("quick note save error:\n" + traceback.format_exc())
            self.toast(f"Couldn't save the note: {e}", 3000)

    def _append_quick(self, n, lines):
        title = SETTINGS.get("quick_tab", "Quick notes")
        i = next((k for k, t in enumerate(n.tabs) if t["title"] == title), None)
        if i is None:
            c = next((c for c in n.closed if c["title"] == title), None)
            if c:
                n.closed = [x for x in n.closed if x is not c]
                i = n._add_tab(c["title"], c["text"], c["id"], dirty=True)
            else:
                i = n._add_tab(title, "")
        box = n.tabs[i]["text"]
        cur = box.get("1.0", "end-1c")
        box.insert("end", ("" if not cur.strip() or cur.endswith("\n") else "\n") + "\n".join(lines) + "\n")
        n.tabs[i]["dirty"] = True
        n.save_now()

    def _flush_quick_notes(self):
        data = protected_load(QUICK_PENDING_FILE, None)
        n = self.notes
        if not data or not data.get("items") or n is None or not n.winfo_exists() or not n.unlocked:
            return
        try:
            self._append_quick(n, data["items"])
            os.remove(QUICK_PENDING_FILE)
            k = len(data["items"])
            self.toast(f"Added {k} quick note{'s' if k != 1 else ''} to “{SETTINGS.get('quick_tab', 'Quick notes')}”")
        except Exception:
            log("quick notes flush error:\n" + traceback.format_exc())

    # ---- window groups (per app): switching to one brings the others up behind it
    def _save_groups(self):
        ui = load_ui()
        ui["window_groups"] = self.groups
        save_ui(ui)
        self.window_sig = None
        self._assign_slots()

    def group_index(self, h):
        key = self._order_key(h)
        return next((i for i, g in enumerate(self.groups) if key in g), None)

    def group_label(self, h):
        i = self.group_index(h)
        return f"group {chr(65 + i % 26)}" if i is not None else ""

    def group_windows(self, h):
        """The other apps' windows in h's group (the most recently used window of each), open right now."""
        i = self.group_index(h)
        if i is None:
            return []
        keys = set(self.groups[i]) - {self._order_key(h)}
        out, seen = [], set()
        for w in self.z_order:
            k = self._order_key(w)
            if w != h and k in keys and k not in seen:
                seen.add(k)
                out.append(w)
        return out

    def group_with(self, h):
        others = [w for w in self.windows if w["hwnd"] != h and self._order_key(w["hwnd"]) != self._order_key(h)]
        if not others:
            self._set_status("Open another app first: groups put windows of different apps together.")
            return
        dlg = ListPicker(self, f"Group “{self._short_title(h)}” with…", [w["name"] for w in others], multi=True,
                         ok_text="Group",
                         hint="Pick one or more (Ctrl+click). Switching to any of them brings the others up behind it. "
                              "Groups are per app, so they come back after a restart.")
        if not dlg.result:
            return
        keys = [self._order_key(h)] + [self._order_key(others[i]["hwnd"]) for i in dlg.result]
        gi = self.group_index(h)
        if gi is not None:
            keys = self.groups[gi] + keys
        keys = list(dict.fromkeys(keys))
        self.groups = [g for g in ([k for k in g if k not in keys] for g in self.groups) if len(g) >= 2]
        self.groups.append(keys)
        self._save_groups()
        self._set_status(f"Grouped {len(keys)} apps ({self.group_label(h)}).")

    def leave_group(self, h):
        key = self._order_key(h)
        self.groups = [g for g in ([k for k in g if k != key] for g in self.groups) if len(g) >= 2]
        self._save_groups()

    def _group_follow(self, hwnd, result):
        if not hwnd or result in (None, False, "none", "gone", "error", "launched"):
            return
        others = self.group_windows(hwnd)
        if not others:
            return
        if result == "minimized":
            if SETTINGS.get("group_min", True):
                for o in others:
                    minimize_window(o, activate_next=False)
        else:
            raise_with(hwnd, others)

    # ---- companion apps: in their own folder (system_monitor\, file_explorer\) or next to this one
    def _find_companion(self, script_name, exe_name, folder=None):
        """(path, is_exe) of a companion app, or None. Looks next to Window Manager, then in its own folder."""
        dirs = [APP_DIR]
        if folder:
            dirs += [os.path.join(APP_DIR, folder), os.path.join(APP_DIR, folder, "dist")]
        for d in dirs:
            if os.path.isfile(os.path.join(d, exe_name)):
                return os.path.join(d, exe_name), True
        if not getattr(sys, "frozen", False):
            for d in dirs:
                if os.path.isfile(os.path.join(d, script_name)):
                    return os.path.join(d, script_name), False
        return None

    def _open_companion(self, script_name, exe_name, label, args=(), folder=None):
        found = self._find_companion(script_name, exe_name, folder)
        where = f"{folder}\\ or next to Window Manager" if folder else "next to Window Manager"
        try:
            if found is None:
                self._set_status(f"Put {script_name} (or {exe_name}) {where} to use it.", 10)
                self.toast(f"{label} not found ({where})")
                return False
            path, is_exe = found
            cwd = os.path.dirname(path)
            if is_exe:
                subprocess.Popen([path, *args], cwd=cwd)
            else:
                py = sys.executable
                pyw = os.path.join(os.path.dirname(py), "pythonw.exe")
                subprocess.Popen([pyw if IS_WIN and os.path.isfile(pyw) else py, path, *args], cwd=cwd,
                                 creationflags=CREATE_NO_WINDOW if IS_WIN else 0)
            self._set_status(f"Opening {label}…")
            return True
        except OSError as e:
            self._set_status(f"Couldn't start {label}: {e}", 10)
            return False

    def open_system_monitor(self):
        return self._open_companion("system_monitor.py", "SystemMonitor.exe", "System Monitor", folder="system_monitor")

    def open_file_explorer(self):
        return self._open_companion("file_explorer.py", "FileExplorer.exe", "File Explorer", folder="file_explorer")

    # ---- snippets
    def open_snippets(self, new_text=None):
        if self.snip_win is None or not self.snip_win.winfo_exists():
            self.snip_win = SnippetsWindow(self)
        self.snip_win.show(new_text)

    # ---- timers, reminders, pomodoro
    def timer_rows(self):
        return [{"kind": "timer", "id": t["id"], "name": self._timer_text(t), "icon_key": "none"}
                for t in self.timers.items]

    def _timer_text(self, t, now=None):
        now = time.time() if now is None else now
        left = fmt_left(t["due"] - now)
        if t["kind"] == "remind":
            return f"⏰  {t['label']}   ·   in {left}"
        if t["kind"] == "pomo":
            return f"◷  {t['label']}   ·   {left} left"
        return f"⏱  {t['label']}   ·   {left} left"

    def start_timer(self, spec):
        due = time.time() + spec["dur"] if spec.get("dur") else spec["due"]
        t = self.timers.add(spec["kind"], due, spec["label"], spec.get("dur"))
        when = time.strftime("%H:%M", time.localtime(t["due"]))
        self.toast(f"⏱  {t['label']} — at {when}" if t["kind"] == "remind" else
                   f"⏱  {t['label']} — {fmt_duration(t['dur'] or 0)} (rings at {when})")
        self.refresh_view()

    def start_pomodoro(self):
        t = self.timers.start_pomodoro()
        self.toast(f"◷  Pomodoro started — work until {time.strftime('%H:%M', time.localtime(t['due']))}")
        self.refresh_view()

    def stop_pomodoro(self):
        self.timers.stop_pomodoro()
        self.toast("Pomodoro stopped")
        self.refresh_view()

    def cancel_timer(self, tid, ask=False):
        t = self.timers.get(tid)
        if not t:
            return
        if t["kind"] == "pomo":
            if not ask or messagebox.askyesno("Pomodoro", "Stop the pomodoro?", parent=self):
                self.stop_pomodoro()
            return
        if ask and not messagebox.askyesno("Cancel", f"Cancel “{t['label']}” "
                                           f"({time.strftime('%H:%M', time.localtime(t['due']))})?", parent=self):
            return
        self.timers.remove(tid)
        self.refresh_view()

    def add_time(self, tid, seconds):
        t = self.timers.get(tid)
        if t:
            t["due"] += seconds
            self.timers.items.sort(key=lambda x: x["due"])
            self.timers.save()
            self.refresh_view()

    def snooze(self, t):
        minutes = SETTINGS.get("snooze_min", 5)
        self.timers.add("remind" if t["kind"] == "pomo" else t["kind"], time.time() + minutes * 60, t["label"],
                        minutes * 60)
        self.toast(f"Snoozed {minutes} min — {t['label']}")
        self.refresh_view()

    def _fire_timer(self, t, now):
        when = time.strftime("%H:%M", time.localtime(t["due"]))
        late = now - t["due"] > 90
        if t["kind"] == "pomo":
            msg, nxt = self.timers.next_pomodoro(t, now)
            self.timers.remove(t["id"])
            title = "Back to work" if nxt.get("phase") == "work" else "Break time"
            detail = msg + (f" (was due at {when})" if late else "")
            buttons = [("OK", lambda: None), ("Stop pomodoro", self.stop_pomodoro)]
        else:
            self.timers.remove(t["id"])
            title = t["label"]
            if t["kind"] == "timer":
                detail = f"Timer · {fmt_duration(t.get('dur') or 0)} · {when}"
            else:
                detail = f"Reminder · {when}"
            if late:
                detail += " · missed while Window Manager wasn't running"
            buttons = [("Done", lambda: None),
                       (f"Snooze {SETTINGS.get('snooze_min', 5)} min", lambda t=t: self.snooze(t))]
        log(f"timer went off: {t['kind']} {t['label']!r}")
        play_alert()
        pop = ReminderPopup(self, title, detail, buttons)
        self.popups.append(pop)
        pop.show(len(self.popups) - 1)
        if self.tray_ok:
            self.shell.balloon(title, detail, sound=False)
        self.refresh_view()

    def _timer_tick(self):
        """Every second: ring what's due and update the TIMERS rows. Always reschedules."""
        if self._quitting:
            return
        try:
            now = time.time()
            for t in self.timers.due(now):
                self._fire_timer(t, now)
            if self.timers.items and self.state() == "normal":
                for iid, it in list(self.row_items.items()):
                    if it["kind"] == "timer" and self.tree.exists(iid):
                        t = self.timers.get(it["id"])
                        if t:
                            self.tree.item(iid, text="   " + self._timer_text(t, now))
        except Exception:
            log("timer error:\n" + traceback.format_exc())
        finally:
            if not self._quitting:
                self.after(1000, self._timer_tick)

    # ---- session: reopen your apps after a restart
    def session_items(self):
        out, seen = [], set()
        for w in self.windows:
            exe = (self.win_ident.get(w["hwnd"]) or ("", None, None))[0] or ""
            if os.path.basename(exe).lower() in SESSION_SKIP_EXES:
                continue
            try:
                it = self.item_to_ws(w)
            except Exception:
                log("session item error:\n" + traceback.format_exc())
                it = None
            if not it:
                continue
            k = session_entry_key(it)
            if k not in seen:
                seen.add(k)
                out.append(it)
        return out

    def save_session(self):
        if SETTINGS.get("session_restore", "ask") == "no" or SESSION_FROZEN:
            return False
        items = self.session_items()
        sig = json.dumps(items, sort_keys=True)
        if sig == self._session_sig:
            return False
        self._session_sig = sig
        write_json_atomic(SESSION_FILE, {"v": 1, "boot": boot_time(), "saved": time.time(), "items": items})
        return True

    def _session_tick(self):
        if self._quitting:
            return
        try:
            self.save_session()
        except Exception:
            log("session save error:\n" + traceback.format_exc())
        finally:
            if not self._quitting:
                self.after(SESSION_SAVE_MS, self._session_tick)

    def _offer_restore(self):
        prev = self.prev_session
        if not prev or not prev.get("items"):
            return
        if SETTINGS.get("session_restore") == "yes":
            self.restore_session(prev["items"])
        elif SETTINGS.get("session_restore") == "ask":
            RestoreDialog(self, prev)

    def restore_previous(self):
        data = _read_json(SESSION_PREV_FILE)
        if not isinstance(data, dict) or not data.get("items"):
            msg = ("Nothing to restore yet: the apps you have open are remembered while Window Manager runs, "
                   "and offered again after a restart.")
            if SETTINGS.get("session_restore") == "no":
                msg = "Session restore is off ([session] restore = no in settings.ini)."
            self._set_status(msg, 10)
            self.toast("No previous session saved yet" if SETTINGS.get("session_restore") != "no"
                       else "Session restore is off in settings.ini")
            return
        RestoreDialog(self, data, title="Restore last session")

    def restore_session(self, items):
        ws = {"id": "session", "name": "Last session", "items": list(items)}
        plan = [self.ws_entry(it) for it in ws["items"]]
        self._set_status(f"Reopening {len(plan)} app{'s' if len(plan) != 1 else ''}…")
        threading.Thread(target=self._run_workspace, args=(ws, plan, True, set(self.titles)), daemon=True).start()

    # ---- tray, start with Windows, quit
    @property
    def tray_ok(self):
        return bool(self.shell is not None and self.shell.icon_added)

    def _refresh_autostart(self):
        try:
            on = bool(autostart_state())
        except Exception:
            on = False
        if self.shell is not None:
            self.shell.autostart_on = on

    def toggle_autostart(self):
        on = not bool(autostart_state())
        try:
            msg = set_autostart(on)
        except Exception as e:
            log("autostart error:\n" + traceback.format_exc())
            msg = f"Couldn't change start-up: {e}"
        self._refresh_autostart()
        self._set_status(msg, 10)
        if self.tray_ok and self.state() != "normal":
            self.shell.balloon("Window Manager", msg)

    def quit_app(self):
        """Save, remove the tray icon and keyboard hook, close every window. Safe to call twice."""
        if self._quitting:
            return
        try:
            self.cycler.stop()
        except Exception:
            pass
        self._quitting = True
        log("quitting")
        steps = (
            lambda: self.notes.save_now() if self.notes is not None and self.notes.winfo_exists()
            and self.notes.unlocked else None,
            self.clip_store.save,
            lambda: self.hotkeys.stop() if self.hotkeys else None,
            lambda: self.shell.stop() if self.shell is not None else None,
            self.destroy,
        )
        for step in steps:
            try:
                step()
            except Exception:
                log("quit step error:\n" + traceback.format_exc())

    def add_profiles(self):
        dlg = AddProfileDialog(self, self.profiles)
        if dlg.result is not None:
            self.profiles = dlg.result
            self._save()

    # ---- My apps
    def _add_menu(self):
        menu = tk.Menu(self, tearoff=0)
        menu.add_command(label="App to My apps…", command=self.add_installed)
        menu.add_command(label="Program or file to My apps…", command=self.add_file)
        menu.add_separator()
        menu.add_command(label="Browser profiles…", command=self.add_profiles)
        menu.add_separator()
        menu.add_command(label="New workspace…", command=lambda: self.open_workspaces(new=True))
        menu.add_command(label="Edit workspaces…", command=self.open_workspaces)
        x, y = self.add_btn.winfo_rootx(), self.add_btn.winfo_rooty() + self.add_btn.winfo_height()
        menu.tk_popup(x, y)

    def _add_pins(self, new):
        have = {pin_id(p) for p in self.pins}
        added = [p for p in new if pin_id(p) not in have]
        if added:
            self.pins.extend(added)
            self._save()

    def add_installed(self):
        if not self.apps:
            messagebox.showinfo("Add app", "Apps are still loading, try again in a moment.")
            return
        dlg = AppPickerDialog(self, self.apps, {pin_id(p) for p in self.pins})
        if dlg.result:
            self._add_pins(dlg.result)

    def add_file(self):
        path = filedialog.askopenfilename(
            parent=self, title="Choose a program or file",
            filetypes=[("Programs and shortcuts", "*.exe *.lnk *.bat *.cmd *.url"), ("All files", "*.*")])
        if path:
            path = os.path.normpath(path)
            name = os.path.splitext(os.path.basename(path))[0]
            self._add_pins([make_pin_file(name, path)])

    def _start_app_for(self, exe, aumid):
        """The Start-menu entry for a program, if there is one."""
        if aumid:
            return next((a for a in self.apps if a["app_id"] == aumid), None)
        if not exe:
            return None
        tail = "\\" + os.path.basename(exe).lower()
        return next((a for a in self.apps
                     if a["app_id"].lower() == exe.lower() or a["app_id"].lower().endswith(tail)), None)

    @staticmethod
    def _exe_name(exe):
        name = os.path.splitext(os.path.basename(exe))[0]
        return name[:1].upper() + name[1:]

    def pin_window(self, item):
        """Add the program behind an open window to My apps."""
        exe, aumid, _ = self.win_ident.get(item["hwnd"]) or self._identify(item["hwnd"])
        match = self._start_app_for(exe, aumid)
        if aumid:
            self._add_pins([make_pin_app(match["name"] if match else item["name"], aumid)])
        elif match:
            self._add_pins([make_pin_app(match["name"], match["app_id"])])
        elif exe:
            self._add_pins([make_pin_file(self._exe_name(exe), exe)])
        else:
            messagebox.showinfo("Add app", "Couldn't find the program for this window.")

    # ---- fixed shortcuts
    def _binding_for_item(self, item, slot):
        """Build a fixed-shortcut entry for a list row, or None if its program can't be identified."""
        kind = item["kind"]
        if kind == "workspace":
            return make_binding(slot, item["name"], workspace=item["id"])
        if kind == "binding":
            return make_binding(slot, item["name"], item["exe"], item["aumid"], item["app_id"],
                                item["profile"], item["args"], item.get("workspace"))
        if kind == "profile":
            return make_binding(slot, item["name"], item["exe"], None, None, profile_id(item), item["args"])
        if kind == "window":
            exe, aumid, prof = self.win_ident.get(item["hwnd"]) or self._identify(item["hwnd"])
            p = next((x for x in self.profiles if profile_id(x) == prof), None) if prof else None
            if p:  # a window of an added browser profile → shortcut for that profile
                return self._binding_for_item(p, slot)
            if not exe and not aumid:
                return None
            match = self._start_app_for(exe, aumid)
            name = match["name"] if match else (self._exe_name(exe) if exe else item["name"])
            return make_binding(slot, name, exe or None, aumid, match["app_id"] if match else None)
        app_id = item.get("app_id") if kind in ("app", "pin") else None
        if app_id:
            if "!" in app_id:  # Store app: its AppID is what its windows report
                return make_binding(slot, item["name"], None, app_id, app_id)
            exe = resolve_app_id_path(app_id)
            return make_binding(slot, item["name"], exe, None, app_id) if exe else None
        if kind == "pin" and item["path"].lower().endswith(".exe"):
            return make_binding(slot, item["name"], item["path"], None, None)
        return None

    def _binding_of(self, item):
        """The fixed shortcut that currently applies to this row, if any."""
        if item["kind"] == "binding":
            return item
        probe = self._binding_for_item(item, 0)
        if not probe:
            return None
        k = binding_key(probe)
        return next((b for b in self.bindings
                     if binding_key(b) == k or (probe["app_id"] and b["app_id"] == probe["app_id"])), None)

    def assign_shortcut(self, item, slot):
        b = self._binding_for_item(item, slot)
        if not b:
            messagebox.showinfo(
                "Fixed shortcut",
                f"Couldn't tell which program “{item['name']}” is.\n\n"
                "Open it, then right-click its window in OPEN → Fixed shortcut.")
            return
        old = self._binding_of(item)
        self.bindings = [x for x in self.bindings
                         if x["slot"] != slot and x is not old and binding_key(x) != binding_key(b)]
        self.inactive_bindings = [x for x in self.inactive_bindings if binding_key(x) != binding_key(b)]
        self.bindings.append(b)
        self._save()
        self._set_status(f"{slot_label(slot)} is now fixed to {b['name']}.")

    def _new_profile_window(self, item):
        try:
            launch(item)
            self._set_status(f"{item['name']}: starting…")
        except OSError as e:
            messagebox.showerror("Could not open", f"{item['name']}\n\n{e}")

    def remove_shortcut(self, b):
        self.bindings = [x for x in self.bindings if x is not b]
        self._save()
        self._set_status(f"{slot_label(b['slot'])} is free again.")

    def _shortcut_menu(self, parent, item):
        current = self._binding_of(item)
        bound = {b["slot"]: b for b in self.bindings}
        sub = tk.Menu(parent, tearoff=0)
        for slot in range(HOTKEY_COUNT):
            label = slot_label(slot)
            if current and current["slot"] == slot:
                label = "✓ " + label
            elif slot in bound:
                label += f"   ({bound[slot]['name']})"
            sub.add_command(label=label, command=lambda s=slot: self.assign_shortcut(item, s))
            if slot + 1 < HOTKEY_COUNT and SLOTS[slot + 1]["row"] != SLOTS[slot]["row"]:
                sub.add_separator()
        if current:
            sub.add_separator()
            sub.add_command(label="Remove fixed shortcut", command=lambda: self.remove_shortcut(current))
        return sub

    def move_selected(self, step):
        item = self._selected()
        if not item or item["kind"] not in ("pin", "profile"):
            return
        lst = self.pins if item["kind"] == "pin" else self.profiles
        i = next(i for i, p in enumerate(lst) if p is item)
        j = i + step
        if 0 <= j < len(lst):
            lst[i], lst[j] = lst[j], lst[i]
            self._save()

    # ---- shared
    def remove_selected(self):
        item = self._selected()
        if not item:
            return
        if item["kind"] == "profile":
            self.profiles = [p for p in self.profiles if p is not item]
        elif item["kind"] == "pin":
            self.pins = [p for p in self.pins if p is not item]
        elif item["kind"] == "binding":
            self.remove_shortcut(item)
            return
        elif item["kind"] == "workspace":
            self.delete_workspace(item)
            return
        else:
            return
        self._save()

    def rename_selected(self):
        item = self._selected()
        if item and item["kind"] == "workspace":
            self.rename_workspace(item)
            return
        if item and item["kind"] in ("profile", "pin", "binding"):
            new = simpledialog.askstring("Rename", "Name:", initialvalue=item["name"], parent=self)
            if new and new.strip():
                item["name"] = new.strip()
                self._save()

    def close_selected(self):
        item = self._selected()
        if item and item["kind"] == "window":
            close_window(item["hwnd"])

    def _save(self):
        try:
            save_config(self.profiles, self.pins, self.bindings + self.inactive_bindings, self.workspaces)
        except OSError as e:
            messagebox.showerror("Save failed", str(e))
        self.window_sig = None
        self.win_ident = {h: self._identify(h) for h in self.window_order}
        self._assign_slots()
        self.refresh_view()
        self._request_icons(self.pins + self.profiles + self.bindings)

    def _context_menu(self, event):
        iid = self.tree.identify_row(event.y)
        if iid == "h:open":
            menu = tk.Menu(self, tearoff=0)
            menu.add_cascade(label="Sort open windows", menu=self._sort_menu(menu))
            menu.tk_popup(event.x_root, event.y_root)
            return
        item = self.row_items.get(iid)
        if not item:
            return
        self.tree.selection_set(iid)
        kind = item["kind"]
        menu = tk.Menu(self, tearoff=0)

        if kind in ("calc", "url", "web", "command", "timer_new", "pomo_start", "pomo_stop", "hint"):
            return
        if kind == "timer":
            t = self.timers.get(item["id"])
            if not t:
                return
            if t["kind"] != "pomo":
                menu.add_command(label="+1 min", command=lambda: self.add_time(t["id"], 60))
                menu.add_command(label="+5 min", command=lambda: self.add_time(t["id"], 300))
                menu.add_separator()
                menu.add_command(label="Cancel", command=lambda: self.cancel_timer(t["id"]))
            else:
                menu.add_command(label="Stop pomodoro", command=self.stop_pomodoro)
            menu.tk_popup(event.x_root, event.y_root)
            return
        if kind == "workspace":
            menu.add_command(label="Open", command=lambda: self.activate_workspace(item))
            menu.add_command(label="Close its windows", command=lambda: self.close_workspace(item))
            menu.add_command(label="Edit…", command=lambda: self.open_workspaces(select_id=item["id"]))
            menu.add_cascade(label="Fixed shortcut", menu=self._shortcut_menu(menu, item))
            menu.add_command(label="Rename…", command=lambda: self.rename_workspace(item))
            menu.add_separator()
            menu.add_command(label="Delete", command=lambda: self.delete_workspace(item))
            menu.tk_popup(event.x_root, event.y_root)
            return

        if kind == "window":
            menu.add_command(label="Switch to", command=self.launch_selected)
            if not self.search_var.get().strip():
                order = self.window_order
                pos = order.index(item["hwnd"]) if item["hwnd"] in order else 0
                move = tk.Menu(menu, tearoff=0)
                move.add_command(label="Move up      Ctrl+↑", state="normal" if pos > 0 else "disabled",
                                 command=lambda: self.move_window(item["hwnd"], "up"))
                move.add_command(label="Move down    Ctrl+↓",
                                 state="normal" if pos < len(order) - 1 else "disabled",
                                 command=lambda: self.move_window(item["hwnd"], "down"))
                move.add_command(label="Move to top", command=lambda: self.move_window(item["hwnd"], "top"))
                move.add_command(label="Move to bottom", command=lambda: self.move_window(item["hwnd"], "bottom"))
                move.add_separator()
                move.add_cascade(label="Sort all", menu=self._sort_menu(move))
                menu.add_cascade(label="Order", menu=move)
            menu.add_command(label="Add to My apps", command=lambda: self.pin_window(item))
            menu.add_cascade(label="Add to workspace", menu=self._ws_menu(menu, item))
            h = item["hwnd"]
            menu.add_command(label="Group with…", command=lambda: self.group_with(h))
            if self.group_index(h) is not None:
                menu.add_command(label=f"Leave {self.group_label(h)}", command=lambda: self.leave_group(h))
            menu.add_separator()
            top = is_topmost(h)
            tkey = f"   {MOD_LABEL}{CMD_KEYS['topmost']}" if "topmost" in CMD_KEYS else ""
            menu.add_command(label=("✓ " if top else "    ") + "Always on top" + tkey,
                             command=lambda: self.toggle_topmost(h))
            see = tk.Menu(menu, tearoff=0)
            cur = get_opacity(h)
            for lv in [100] + SETTINGS.get("opacity_levels", [85, 70, 55]):
                see.add_command(label=("✓ " if abs(cur - lv) <= 1 else "    ") + ("Solid" if lv == 100 else f"{lv}%"),
                                command=lambda lv=lv: self.set_window_opacity(h, lv))
            menu.add_cascade(label=("✓ " if cur < 99 else "    ") + "See-through", menu=see)
            try:
                muted, _n = app_mute(h, "get")
            except Exception:
                muted = None
            mkey = f"   {MOD_LABEL}{CMD_KEYS['mute']}" if "mute" in CMD_KEYS else ""
            menu.add_command(label=("✓ Muted" if muted else "    Mute sound") + mkey, command=lambda: self.toggle_mute(h))
            menu.add_command(label="    Minimize", command=lambda: minimize_window(h))
            menu.add_separator()
            menu.add_command(label="Close window", command=self.close_selected)
            menu.add_command(label="Force quit (end process)…", command=lambda: self.kill_window(h))
        else:
            menu.add_command(label="Open", command=self.launch_selected)
        if kind in ("app", "pin"):
            menu.add_command(label="Open new window", command=lambda: self._launch_new(item))
        if kind in ("app", "pin", "profile") or (kind == "binding" and not item.get("workspace")):
            menu.add_cascade(label="Add to workspace", menu=self._ws_menu(menu, item))

        if kind == "app":
            pinned = "app|" + item["app_id"] in {pin_id(p) for p in self.pins}
            menu.add_command(label="Add to My apps",
                             state="disabled" if pinned else "normal",
                             command=lambda: self._add_pins([make_pin_app(item["name"], item["app_id"])]))

        if kind == "profile":
            menu.add_command(label="New window", command=lambda: self._new_profile_window(item))
        if kind in ("window", "app", "pin", "binding", "profile"):
            menu.add_cascade(label="Fixed shortcut", menu=self._shortcut_menu(menu, item))

        if kind == "binding":
            menu.add_command(label="Rename…", command=self.rename_selected)
            menu.add_separator()
            menu.add_command(label="Remove fixed shortcut", command=lambda: self.remove_shortcut(item))

        if kind in ("pin", "profile"):
            menu.add_command(label="Move up", command=lambda: self.move_selected(-1))
            menu.add_command(label="Move down", command=lambda: self.move_selected(1))
            menu.add_command(label="Rename…", command=self.rename_selected)
            menu.add_separator()
            menu.add_command(label="Remove", command=self.remove_selected)

        menu.tk_popup(event.x_root, event.y_root)

    def _show_switch(self, key, hwnd, result, name=None):
        try:
            self._group_follow(hwnd, result)
        except Exception:
            log("group error:\n" + traceback.format_exc())
        title = next((w["name"] for w in self.windows if w["hwnd"] == hwnd), name or "?")
        if len(title) > 40:
            title = title[:39] + "…"
        if result == "none":
            text = f"{key}: nothing is on this shortcut."
        elif result == "launched":
            text = f"{key} → {name}: starting…"
        elif result == "gone":
            text = f"{key}: that window was closed."
        elif result == "error":
            text = f"{key}: error — details in {LOG_FILE}"
        elif result == "minimized":
            text = f"{key} → {title}: minimized."
        elif result:
            text = f"{key} → {title}: switched ✓"
        else:
            target_admin = process_elevated(window_pid(hwnd)) if hwnd and IS_WIN else None
            if IS_WIN and target_admin is not False and not process_elevated(os.getpid()):
                text = (f"{key} → {title}: blocked. That app probably runs as administrator — "
                        f"run the launcher as administrator too.")
            else:
                text = f"{key} → {title}: Windows blocked the switch."
        log(f"{key} hwnd={hwnd} title={title!r} result={result} "
            f"foreground={user32.GetForegroundWindow() if IS_WIN else None}")
        self._set_status(text)

    def _check_hotkeys(self):
        if not self.hotkeys.ready.is_set():
            self.after(300, self._check_hotkeys)
            return
        if self.hotkeys.error:
            self._set_status(f"Keyboard watcher failed — see {os.path.basename(LOG_FILE)} next to the app.")
            return
        log(f"admin={process_elevated(os.getpid())}; open windows={len(self.windows)}")
        if IMPORT_MESSAGE:
            self._set_status(IMPORT_MESSAGE, 15)
        elif SETTINGS_WARNINGS:
            self._set_status("settings.ini: " + "; ".join(SETTINGS_WARNINGS), 12)
        elif not self.hotkeys.failed:
            self.status.configure(text=hints_text(), fg=T["faint"])
        if self.hotkeys.failed:
            taken = ", ".join(f"Alt+{d}" for d in self.hotkeys.failed)
            self._set_status(f"{taken} already used by another app — not available.")

    def report_callback_exception(self, exc, val, tb):
        if issubclass(exc, (KeyboardInterrupt, SystemExit)):
            self.quit_app()
            return
        log("error:\n" + "".join(traceback.format_exception(exc, val, tb)))
        try:
            self._set_status(f"Error: {val} — details in {os.path.basename(LOG_FILE)}", 10)
        except tk.TclError:
            pass

    def toggle_notes(self):
        if self.notes is None or not self.notes.winfo_exists():
            self.notes = NotesWindow(self)
        self.notes.toggle()

    def _on_close(self):
        if self.tray_ok and SETTINGS.get("close_to_tray", True):
            self.hide_launcher()
            if not self._told_tray:
                self._told_tray = True
                self.shell.balloon("Still running", "The launcher keeps working from the tray. "
                                                    "Right-click its icon to quit.")
            return
        self.quit_app()


def cli_main(argv):
    """Command line tools that work without the GUI (and on any OS)."""
    import argparse
    import getpass
    ap = argparse.ArgumentParser(prog=os.path.basename(sys.argv[0]),
                                 description="Window Manager. Run with no arguments to start the app.")
    sub = ap.add_subparsers(dest="command")
    d = sub.add_parser("decrypt", help="decrypt notes into .txt files (the .txt files are NOT encrypted)")
    d.add_argument("paths", nargs="*", help=".enc files and/or folders (default: your notes in data/notes)")
    d.add_argument("-o", "--out", help="folder for the .txt files (default: next to each .enc file)")
    d.add_argument("--print", action="store_true", help="show the notes on screen instead of writing files")
    ic = sub.add_parser("icon", help="write the app icon as a .ico file (used by build_window_manager.bat)")
    ic.add_argument("path", nargs="?", default="window_manager.ico")
    args = ap.parse_args(argv)
    if args.command == "icon":
        with open(args.path, "wb") as fh:
            fh.write(make_ico())
        print(f"Wrote {args.path}")
        return 0
    if args.command != "decrypt":
        ap.print_help()
        return 1
    try:
        sys.stdout.reconfigure(errors="replace")
    except AttributeError:
        pass
    if not notes_crypto_available():
        print("This needs the 'cryptography' package:  python -m pip install cryptography", file=sys.stderr)
        return 1

    paths = args.paths or [NOTES_DIR if notes_tab_files() else NOTES_FILE]
    files = []
    for p in paths:
        files += [p] if os.path.isfile(p) else notes_tab_files(p)
    if not files:
        print(f"No .enc files found at: {', '.join(paths)}", file=sys.stderr)
        return 1
    password = getpass.getpass("Notes password: ")
    if args.out:
        os.makedirs(args.out, exist_ok=True)

    cache, done, bad = {}, 0, 0
    for f in files:
        name = os.path.basename(f)
        try:
            payload = notes_decrypt_file(f, password, cache)[0]
        except (ValueError, KeyError, OSError) as e:
            print(f"[skipped] {name}: {e}", file=sys.stderr)
            bad += 1
            continue
        tabs = payload_tabs(payload)
        if len(tabs) == 1:
            text = tabs[0].get("text", "")
        else:  # an older single file holding several tabs
            text = "\n\n".join(f"===== {t.get('title') or 'Note'} =====\n{t.get('text', '')}" for t in tabs)
        title = tabs[0].get("title") or "Note" if len(tabs) == 1 else f"{len(tabs)} tabs"
        if args.print:
            print(f"===== {title} =====   ({name})\n{text}\n")
        else:
            target = os.path.join(args.out or os.path.dirname(os.path.abspath(f)),
                                  os.path.splitext(name)[0] + ".txt")
            with open(target, "w", encoding="utf-8") as fh:
                fh.write(text)
            print(f"{name}  ->  {os.path.basename(target)}   ({title})")
        done += 1
    if not done:
        print("Nothing could be decrypted with that password.", file=sys.stderr)
        return 2
    if not args.print:
        print(f"\nDecrypted {done} file(s). The .txt files are NOT encrypted - delete them when you're done.")
    if bad:
        print(f"{bad} file(s) were skipped (different password or damaged).", file=sys.stderr)
    return 0


def cleanup_links():
    """Remove old temporary shortcuts (used to start programs as the normal user)."""
    try:
        for n in os.listdir(LINK_DIR):
            path = os.path.join(LINK_DIR, n)
            if n.endswith(".lnk") and time.time() - os.path.getmtime(path) > 2 * 86400:
                os.remove(path)
    except OSError:
        pass


def run_gui(hidden=False):
    """Run the app until Quit, Ctrl+C or Ctrl+Break, then end the process."""
    app = LauncherApp(start_hidden=hidden)

    def stop(*_):
        log("Ctrl+C / console closed: quitting")
        try:
            app.after(0, app.quit_app)  # runs on the Tk thread within 50 ms (the poll timer wakes Python up)
        except Exception:
            os._exit(0)

    signal.signal(signal.SIGINT, stop)
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, stop)  # Ctrl+Break, and closing the console window
    try:
        app.mainloop()
    except KeyboardInterrupt:
        app.quit_app()
    finally:
        log("stopped")
        os._exit(0)  # don't wait for background threads (keyboard hook, tray, icon loaders)


def main():
    args = sys.argv[1:]
    hidden = "--hidden" in args
    args = [a for a in args if a != "--hidden"]
    if args:
        sys.exit(cli_main(args))
    if not IS_WIN:
        print("This app only runs on Windows.")
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # crisp text/icons on high-DPI screens
    except (AttributeError, OSError):
        pass
    if not claim_single_instance():
        return  # the running launcher shows itself instead
    global LAUNCHER_ELEVATED
    LAUNCHER_ELEVATED = bool(process_elevated(os.getpid()))
    try:
        migrate_old_data()
    except OSError:
        log("data migration error:\n" + traceback.format_exc())
    cleanup_links()
    run_gui(hidden)


load_settings()

if __name__ == "__main__":
    main()
