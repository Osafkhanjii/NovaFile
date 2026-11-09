# Nova File

A file manager written from scratch in **C** for Windows.

## Tech stack

| Component      | Technology                                         |
|----------------|----------------------------------------------------|
| Language       | C (gnu11)                                          |
| Platform layer | Custom Win32 layer (`src/platform/window_win.c`)   |
| Renderer       | Custom double-buffered GDI renderer                |
| UI framework   | Custom widgets + one shared layout/hit-test pass   |
| Thumbnails     | OS picture decoder (`IPicture` / `OleLoadPicturePath`) |
| Search         | Fuzzy matcher (`src/core/indexer.c`)               |
| External libs  | Only `lib/stb_sprintf.h`                           |

No GUI toolkit, no OpenGL/DirectX, no third-party frameworks - everything
is in this repository. Output: `build\NovaFile.exe` (~955 KB).

## Features

- **File Pilot style dark theme** - blue-grey panels, blue selection,
  rounded buttons and sidebar, plus a **dark title bar**
- **Explorer style top bar** (two rows, exactly like the reference):
  - row 1: **hamburger menu** (New tab, Bookmark, Refresh, Dark/Light,
    Home, Exit), **folder tabs** with a close **✕** and **+** button,
    theme toggle
  - row 2: icon navigation **back / forward / up / refresh** and a
    **breadcrumb** - `This PC > C: > Users > Desktop-Msi > Downloads`
    (every real folder hangs under This PC, exactly like Explorer, so a
    drive reads `This PC > D:`; the **Desktop folder is its own
    namespace** - arriving there from This PC reads just `Desktop`, and
    the physical `C: > Users > ...` form only appears when you really
    walk down C:\Users yourself); click a segment to jump there, click the
    empty part or **Tab** to type a path
- **Sidebar** with four sections, exactly like the reference:
  **Recents** (last visited, **fixed 4 slots** - the rows below never
  shift when recents change), **Bookmarks** (Ctrl+B to add),
  **Storage** (every drive with its volume label), **Places**
  (**This PC** - a virtual view of all drives - plus Home, Desktop,
  Downloads, Documents, Music, Pictures, Videos, Favorites,
  $Recycle.Bin). The list **scrolls with the wheel** when the rows no
  longer fit (blue pill scrollbar on the right edge); the search box in
  the top bar filters it as you type
- **Shell thumbnails for everything** - the same source Explorer uses
  (`IShellItemImageFactory` / `IThumbnailProvider`): crisp at any size,
  EXIF orientation handled correctly (never upside down), folder and
  drive images, video frames. Big cells ask the factory at **256px**
  (`SIIGBF_BIGGERSIZEOK`), and while a thumbnail is still queued every
  cell already draws the file's (or folder's) crisp **256px** icon from
  the system image list (the "jumbo" list) - so the scaler only ever
  **shrinks**, never upscales a 32px icon into a soft blur. Falls back
  to the classic shell icon (`SHGetFileInfo`), then to the OLE picture
  decoder, then to a drawn icon
- **OpenEXR thumbnails** - Windows has no `.exr` codec, so Explorer (and
  the shell) only shows a blank icon for every frame of a render / comp
  image sequence. NovaFile decodes EXR itself (`src/core/exr.c`, pure C,
  no libraries): scanline and tiled images, multi-part files (first
  part), HALF / FLOAT / UINT channels, and every standard compression -
  NONE, RLE, ZIPS, ZIP, PIZ, PXR24, B44, B44A, DWAA, DWAB. It shows RGB(A),
  else the first layer's RGB (`beauty.R` ...), else Y as grey, framed by
  the display window, shown in sRGB over black like Nuke / DJV. Fast: a
  thumbnail of a 4K DWA frame is built from its 8x8 block averages alone
  (~13 ms), bigger previews decode on several cores, and EXR thumbnails
  use three fixed sizes (256 / 512 / 1024) so zooming never decodes
  again - while a new size loads, the one already decoded stays on
  screen. Opening a folder starts decoding all its frames (up to 1000)
  in the background, below the UI's priority, into the disk cache.
  Deep or damaged files keep the normal icon
- **ProRes .mov thumbnails** - Windows has no ProRes codec either, so a
  ProRes .mov showed a blank icon on most PCs. NovaFile reads the
  QuickTime container and decodes one frame (10% in) of the ProRes track
  itself (`src/core/mov.c`, pure C): every profile (Proxy, LT, 422, HQ,
  4444, 4444 XQ), progressive or interlaced, from the 8x8 block averages
  alone - ~1.4 ms for HD, ~5 ms for 4K. Same fixed sizes, instant zoom and
  folder prefetch as EXR. Other videos (.mov / .mp4 / .m4v in H.264 etc.)
  use the shell's thumbnail, and when a PC has none, one frame through
  Windows Media Foundation (`src/platform/video_thumb.c`, loaded at run
  time so the app still starts on Windows "N" editions)
- **Five view modes** - from the **one corner control** in the bottom bar
  (right of the Hidden toggle): a plain **click steps automatically**
  through **Details** (Name / Size / Modified / Type columns) -> **List**
  (name only) -> **Tree** (folders expand in place - click a closed folder
  row, or the branch chevron, to open / close) -> **Icons** (the normal
  thumbnail grid, 20% preset) -> **XL icons** (the same grid at **100%** -
  the large-icon maximum) -> back to Details; the small **>** chevron on
  the control still opens the full menu to jump straight to a mode.
  **F3** jumps between Details and Icons and never touches your size
- **Size zoom in the bottom bar** - the **zoom %** in the right half of
  the same corner control shows the current size; hovering it (or
  zooming with Ctrl+wheel)
  opens a **vertical slider popup** with **+ / -** buttons and a drag track
  (top = **100%**, bottom = 15%). Range **15%-100%** - 100% is exactly as
  big as 300% used to be (the "large icon" size) - and the change is
  **animated**: the size eases to its target over a few frames instead
  of jumping - thumbnails, grid cells and details rows scale together
- **Bottom bar counts** - the right side shows
  `N folders - N files - N hidden`, then the **Hidden** toggle (same as
  Ctrl+H), then the **one view+zoom corner control** at the very edge;
  the middle of the bar stays empty and a transient message (errors,
  sort, view name) borrows the count spot for ~4 seconds
- **Hidden / system files stay hidden** - `$Recycle.Bin`, `pagefile.sys`,
  `System Volume Information` and friends are filtered out exactly like
  Explorer shows a folder; **Ctrl+H**, the **Hidden** button in the status
  bar (or the menu) brings them back
- **Loads fast, never freezes** - the UI thread never runs a shell
  lookup, an image decode or a directory scan: all of it happens on
  **background worker threads**, and `paint()` only enqueues jobs and
  drains what already finished (the selftest asserts that five paints
  of a cold `C:\Windows` at 300% zoom issue **0** shell calls on the
  UI thread). Each frame enqueues at most **512 icon** and **32
  thumbnail** jobs, and thumbnail enqueueing gets **10 ms** of frame
  time - raised to **35 ms for a short while** after a navigation (the
  burst, skipped while the zoom animation is easing) so roughly 3x the
  lookups land per frame. On the very first navigation the visible
  rows' icons are queued **before the first paint**, so the folder
  shows real shell icons within the first frames - the flat drawn
  placeholder is what used to look blurry, and it only shows while an
  icon is still pending; thumbnails follow in the same frame's burst.
  A full-size image is **never** decoded while painting (that
  synchronous decode was the "opening Downloads takes forever"
  freeze). The cheap type icons have their own queue and run before
  thumbnails, so a folder's icons land within a couple of frames and
  slow thumbnails can no longer starve them into white placeholders;
  the icon / thumbnail caches hold **1024 entries**,
  bigger than any zoomed-out screen, so 15% zoom never evicts and
  re-fetches (the old cache thrash made everything "run"), and failed
  shell lookups are **never dropped
  for good** (five fast attempts first, then retried after **8 frames**
  while loading and after a gentler back-off later - a shell that
  failed while the folder was loading still turns HD later). Thumbnails
  are requested in 32px size buckets so zooming reuses the cache
  instead of re-fetching everything (big cells ask **256px** inside
  the bucket), and every image - even an exact 1:1 - goes through a
  cached scaler: **area (box) averaging** for shrinks, bilinear for
  upscales. AlphaBlend's own nearest-neighbour stretch was the blocky
  "blurry" look. That scaler also **premultiplies straight alpha**
  first: shell bitmaps arrive unpremultiplied, `src + dst*(1-a)` then
  overflowed the byte on bright anti-aliased edges and msimg32 stored
  the wrap - the green fringes that used to sit on upscaled folder
  icons. Every cell size draws the icon's own 32-bit bitmap through it
  (from the **256px** system-image-list twin when the entry has one) -
  even the tiny details-row and sidebar icons at 10-16px, which are
  area-averaged down from the HD source (a plain nearest-neighbour
  shrink of a 32px icon made them mushy)
- **Never just closes** - an unhandled fault pops a dialog with the
  exception code, the offset into `build\NovaFile.map` (so the exact
  function is known) and the folder that was open
- **4K / high-DPI** - the process is DPI aware and every pixel in the
  UI goes through one scale factor, so it is sharp at 150-200%
- **Interface Scaling** - Settings -> Appearance: one slider scales the
  whole interface (text, icons, buttons, spacing) from 75% to 150%,
  previews live and is remembered across restarts - or hold **V** and
  press **+** / **-** (numpad too) to step the same setting from
  anywhere; a status line reports the exact percentage each step
- **Address bar** - type any path, press Enter to jump there (**Tab**)
- **Search / filter box** - top bar row 2, right of the breadcrumb:
  typing filters the current folder instantly (ranking: exact, then
  prefix, then word start, then substring - each step switchable in
  Settings -> Search); **Enter** also scans subfolders (progress and
  cancel in the status bar), **Esc** clears the query and leaves the
  box, the **✕** clears it - **Ctrl+F** or **Ctrl+L** focuses it, and
  the status bar shows "N of M files" while a query is active
- **Sorting** - click Name / Size / Modified / Type headers to sort
  (click again to reverse)
- **Selection box** - press on empty space (or, in Details / List, on a
  row to the right of its name) and drag: every item the box touches is
  selected live, and the list scrolls when you drag past its edge.
  **Ctrl**+drag adds to the selection you already have
- **Rename tabs** - double-click a tab, or right-click it -> **Rename
  Tab**; Enter saves, Esc cancels, an empty name goes back to the folder
  name. Tab names are kept with the session
- **Themes** - dark and light (**Ctrl+T**)
- **Fast scrolling** - the wheel moves **2 rows** (thumbnails) or **6
  rows** (details) per notch, with a blue pill scrollbar; the sidebar
  scrolls on its own when you turn the wheel over it
- **Navigation history** - back, forward, up and refresh icon buttons,
  plus double click / Enter to open items. The **up** button follows the
  Windows shell chain: folder -> drive -> **This PC** -> **Desktop**,
  where it stops (button greys out); **back** and **up** re-select the
  folder you came out of instead of highlighting the first row
- **Explorer file operations** - Delete sends to the **Recycle Bin**
  (Shift+Delete = permanent, both confirmed; Enter confirms); paste never
  overwrites: a copy in the same folder becomes `name - Copy`, a taken
  name becomes `name (2)`, and a folder can never be pasted into itself.
  Copies run on a **background thread** (progress in the status bar, Esc
  cancels), and **Ctrl+Z** undoes the last rename, new item or paste
- **Drag and drop with Windows** - drag files **out** to Explorer, the
  desktop or any app, and drop files **in** from them (same drive moves,
  another drive copies; Ctrl = copy, Shift = move); a drop lands in the
  pane under the pointer
- **System context menu (File Pilot style)** - right-click a file, a
  selection, a drive or empty space: one dark, searchable menu with
  EVERY command Windows offers for it (WinRAR, 7-Zip, Open with, Send to,
  Give access to, Scan with Defender, New > every file type, ...) with
  their icons and submenus, plus NovaFile's own rows (Open in New Tab /
  Pane, View / Sort / Group, custom thumbnail, bookmark, Open in
  Terminal).  It opens instantly - the system rows are built on a
  background thread - and typing filters every command
  ("Select a system command...").  Cut / Copy / Paste / Delete / Rename /
  New Folder from that menu run through NovaFile (Recycle Bin, Undo, safe
  paste).  The Menu key or Shift+F10 opens it from the keyboard
- **Easy to learn** - the first start shows a short 5-step welcome tour;
  **F1** opens every shortcut and mouse gesture on one searchable page
  (always the user's current keys); hovering a top-bar button shows its
  name and shortcut; a command picked from the right-click menu tells
  you its key once
- **Hide the sidebar** - the panel button next to the gear (or Ctrl+S)
  slides the sidebar shut so the files get the whole width; remembered
- **Inline rename** - clicking anywhere outside the field saves the new
  name (Esc cancels), a click inside places the caret
- **Auto-refresh** - the open folder is watched; files another program
  creates, renames or deletes appear on their own (selection and scroll
  are kept)
- **Every language** - file names in Urdu, Arabic, Chinese, emoji ... list,
  open and can be typed (UTF-8 manifest + a Unicode window)
- **Light on the machine** - the window paints at full rate only while
  something happens and otherwise sleeps on the message queue: idle CPU
  dropped from ~30% of a core to ~1-2%; the back buffer is reused instead
  of re-allocated every frame

## Controls

| Key                        | Action                          |
|----------------------------|---------------------------------|
| Up / Down                  | Move selection                  |
| PageUp / PageDown          | Jump a page                     |
| Home / End                 | First / last item               |
| Enter or Right             | Open folder / run file          |
| Backspace or Left          | Go to the parent folder         |
| F3                         | Details <-> Icons               |
| Ctrl+wheel                 | Size in / out (10% steps, animated) |
| Zoom popup slider (drag)   | Set the size directly (15%-100%) |
| Zoom popup + / -           | Size larger / smaller           |
| Bottom view control (click)| Step view: Details -> List -> Tree -> Icons -> XL |
| Bottom view chevron (>)    | Open the view menu directly      |
| Bottom zoom % (hover/click)| Show the vertical zoom slider    |
| Bottom Hidden button       | Show / hide hidden + system files |
| Ctrl+L (also Ctrl+F)        | Focus the search / filter box   |
| Ctrl+T                     | Dark <-> Light theme            |
| Ctrl+B                     | Bookmark the current folder     |
| Ctrl+H                     | Show / hide hidden + system files |
| Ctrl+N                     | New tab                         |
| Ctrl+Shift+N               | New folder here (type its name) |
| Ctrl+Tab / Ctrl+Shift+Tab  | Next / previous tab             |
| Ctrl+Shift+PgUp / PgDn     | Shift the active tab left/right |
| Tab                         | Address bar (breadcrumb)        |
| F2                          | Rename the selected item inline |
| Delete                      | Delete the selection (confirmation always runs) |
| Ctrl+C / Ctrl+X / Ctrl+V    | Copy / cut / paste files and folders |
| Ctrl+A                      | Select every row                |
| Ctrl+F (or Ctrl+L)          | Focus the search / filter box   |
| F5                          | Refresh the current folder      |
| Ctrl+E                      | Split the pane to the right     |
| Ctrl+W                      | Close the focused pane          |
| Move the mouse into a pane  | Make it the active pane (Settings -> Panes -> Pane follows the mouse) |
| C                           | Quick Access wheel (radial menu of starred folders) |
| Ctrl+,                      | Open Settings (gear panel)      |
| Ctrl+Q or Esc              | Exit (Esc first leaves a field or closes a menu/popup) |
| Alt+Left / Alt+Right       | Back / Forward (also the mouse side buttons) |
| Alt+Up                     | Parent folder                   |
| Hold Alt + move the mouse  | Left = Back, right = Forward, up = one folder up |
| Alt+Enter                  | Properties                      |
| Alt+D                      | Address bar                     |
| Shift+Delete               | Delete permanently (skips the Recycle Bin) |
| Ctrl+Z                     | Undo the last rename / new item / paste |
| F11                        | Maximize / restore              |
| Menu key / Shift+F10       | Context menu of the selection   |
| F1                         | Keyboard shortcuts guide (searchable) |
| Ctrl+S                     | Hide / show the sidebar (also the panel button top left) |
| Type in an open menu       | Search every command            |
| Drag a row out of the window | Copy / move it into Explorer or another app |
| Mouse wheel                | Scroll the list (2 / 6 rows per notch) |
| Wheel over the sidebar     | Scroll the sidebar              |
| Click a breadcrumb segment | Jump to that folder             |
| Hamburger button (top left)| Menu: tab / bookmark / refresh / theme / home / exit |
| Tab, its ✕, or the +       | Switch / close / open a tab     |
| Right-click a tab          | Tab menu: shift / duplicate / close (others, left, right, all) |
| Right-click a sidebar entry| Open / new tab / new pane / Quick Access star |
| Double click               | Open folder / run file          |
| Click a column header      | Sort by that column             |
| Up nav button              | One level up: folder > drive > This PC > Desktop |
| Back nav button            | Previous folder (re-selects the folder you left) |

## Settings

Everything configurable lives in one **command registry** (`g_ents[]`
in `src/app/main.c`): every entry carries an id, a description, a
default key binding and - for actions and switches - an on/off state.
The **gear button** in the top bar (or **Ctrl+,**) opens the Settings
panel, which is *generated* from that registry: a filter box at the
top finds any feature, clicking a key row enters capture mode (the
new combination is checked against every other command - a **conflict
is refused** with a message, it can never silently shadow another
shortcut), and each switch turns a feature on or off. The hamburger
menu's labels come from the same table, so the menu can never
advertise a stale shortcut.

**Where it is stored.** `%LOCALAPPDATA%\NovaFile\settings.txt` - the
live path is shown in the panel under *Advanced -> Settings file*.
Every change is saved **immediately**, atomically (a `.tmp` file plus
`MoveFileEx`), one line per command:

```
# NovaFile settings v1
focus_search	70	1	1
thumbnails	0	0	1
```

The fields are TAB-separated: `id`, `key` (virtual-key code - `70`
is `F`, `0` = unbound), `mods` (bitmask: `1` = Ctrl, `2` = Shift,
`4` = Alt) and `value` (`1`/`0` = the shortcut's on/off state, or
the switch's state for options).

**Loading is defensive.** Malformed, out-of-range or unknown lines
are skipped and that command keeps its default; if a hand-edited file
puts two commands on the same binding, the first one wins and the
other falls back to its default. The file is user data: neither the
build nor the selftest ever overwrites it (the selftest redirects the
store through the `NOVA_SETTINGS_FILE` environment variable).

**What persists:** every rebindable shortcut together with its
enable/disable switch (rename, delete, copy/cut/paste, split, close
pane, Quick Access wheel, exit, ...) and the feature options -
thumbnails, Alt + mouse gesture, the wheel, fast icon preloading and
the four search options (case-sensitive, broad/substring, word-start,
highlight).

## Project structure

```
Nova File/
├── include/
│   ├── core.h            # memory, strings, containers, file types
│   ├── exr.h             # OpenEXR preview decoder API
│   ├── platform.h        # platform + window + input API
│   ├── renderer.h        # renderer API, colours, themes
│   └── ui.h              # widget types
├── src/
│   ├── core/
│   │   ├── memory.c      # allocator, UTF-8, dynamic array, hash map
│   │   ├── indexer.c     # fuzzy search, size formatting, extensions
│   │   └── exr.c         # OpenEXR decoder for thumbnails (all codecs)
│   ├── platform/
│   │   ├── window_win.c  # Win32 window, events, file system
│   │   └── shell_ole.c   # native shell menu + OLE drag-out
│   ├── renderer/
│   │   └── renderer_gl.c # GDI renderer (double buffered)
│   ├── ui/
│   │   └── ui_framework.c# widget framework
│   └── app/
│       └── main.c        # Nova File: layout, paint, input, views
├── lib/
│   └── stb_sprintf.h     # the only external library
├── Makefile              # make / make run / make clean
├── compile.bat           # double-click Windows build
├── BUILDING.txt
└── README.md
```

## Building

```
compile.bat        (or:  make)
```

GCC ships with Dev-Cpp on this machine; the path is set at the top of
`compile.bat` (and in the `CC` line of the Makefile).

Test without clicking around:

```
build\NovaFile.exe --selftest
```

It exercises every feature and prints `SELFTEST OK` — including two colour-accuracy checks:

```
[selftest] premult straight alpha...   straight->premult r100=100 viol=0 -> ok
[selftest] box downscale average...    lines16->4 grey=64 viol=0 -> ok
```

(the first proves straight alpha is premultiplied before blending, the
second that a shrink is a true area average — a checkerboard
downscaled 2:1 must come out mid-grey in every pixel, not a
four-tap bilinear guess).

> **Note:** Smart App Control on this machine sometimes blocks a fresh
> build (it judges each binary hash separately). If you see
> "blocked by ... Device Guard policy", just build again - see
> BUILDING.txt for details.

## Safety notes

- **No silent damage**: every delete is confirmed and goes to the
  Recycle Bin unless you hold Shift; paste never overwrites an existing
  file; Ctrl+Z takes the last rename / new item / paste back.
- Files are handed to the OS default handler (`ShellExecuteA`); Nova File
  never parses or executes them itself.
- All strings bounded (`snprintf` / `strncpy`), allocations checked,
  pointers validated, UTF-8 sequences never cut in half.
- `read_file` refuses anything over 64 MB.
- Thumbnail `IPicture` objects are released on every directory change and
  on shutdown; failed loads are cached so a broken file is tried once.
- GDI brushes/pens/bitmaps are cached or deleted each frame - no handle
  leak while painting.
- One layout function feeds both drawing and hit testing, so a click can
  never select the wrong row.

## Installing & updates

For end users there is a per-user installer (no admin rights needed):
`dist\NovaFile-Setup-<version>.exe` installs to
`%LOCALAPPDATA%\Programs\NovaFile`, adds a Start Menu entry (optional
desktop icon) and an Add/Remove Programs entry. User data stays in
`%LOCALAPPDATA%\NovaFile`, so installing, upgrading or uninstalling never
touches settings, favorites or thumbnails.

Installed copies can update themselves: **Settings → Updates** checks a
release manifest over HTTPS, downloads the new installer in the
background, verifies its SHA-256 (and Authenticode signature when
configured), and installs it silently with rollback protection. See
`docs\RELEASE.md` for hosting the manifest and cutting releases.

## License / usage notice

Copyright (c) 2026 NovaFile. **All rights reserved.**

This repository is published for viewing only. No license is granted
to use, copy, modify, merge, publish, distribute, sublicense, or sell
the code, assets, or installer.
