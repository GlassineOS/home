# Writing a widget (widget format 1)

<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

This is the widget format that `glassine-home` owns (spec 001, Contracts). Apps ship widgets against
it; a breaking change raises the format number. Status: implemented with host tests; not yet run on a
device, and Home does not draw widgets yet (the drawing comes with `GlassineOS/ui`).

A widget only describes what to show. Home draws it in the current voice and decides when it runs,
so a widget can't animate, flash the panel or keep the CPU awake.

## Folder

```
reading/
  widget.toml       the manifest: data, never code
  widget.lua        returns { render = function(ctx, props) ... end }
  widget.edit.lua   optional: the settings sheet, same shape
```

Widgets come from three places: built into Home, brought by apps (`/usr/share/glassine/widgets`),
and your own (`~/.local/share/glassine/widgets`). The folder name is the widget's id:
`[a-z0-9][a-z0-9._-]*`, at most 64 bytes. Home reads only `widget.toml` to list, describe or add a
widget; it runs no code for that.

## widget.toml

```toml
name = "Continue reading"                  # one line, at most 64 bytes
icon = "menu_book"                         # an icon name: [a-z0-9_-]
description = "The book you read last."    # optional, at most 200 bytes
sizes = ["wide", "half"]                   # "half" and/or "wide"; the first is the default
reads = ["~/.config/koreader/history.lua", "~/Books/"]
refresh = { on_change = true }             # and/or every = "1h"
format = 1                                 # optional; a newer number needs a newer Home

[props]                                    # settings and their defaults
category = "Games"
```

- `reads`: the only files a run can read. A path is absolute or starts with `~/`; a trailing `/`
  makes it a directory prefix (everything beneath it). No `.` or `..` parts, nothing under `/proc`,
  `/sys`, `/dev` or `/run`, and `/` alone is refused. Symlinks beneath a listed directory are never
  followed.
- `refresh.on_change`: run when a file the last run read changes (needs `reads`).
- `refresh.every`: `"15m"`, `"1h"`, `"2h30m"`, `"1d"`: at least 15 minutes and a multiple of 15
  minutes. Runs sit on a shared grid aligned to local time, so widgets with the same interval run
  together.
- `props`: strings, integers, floats or booleans. Names are `[a-z_][a-z0-9_]*`, at most 32 of them.

There is no network key: widgets have no network access of any kind.

The file is a strict subset of TOML 1.0: comments, `key = value` with bare or quoted keys, `[table]`
headers, basic and literal strings (one line), decimal integers and floats, booleans, arrays (may span
lines) and one-line inline tables. Dotted keys, dates, multi-line strings, hex/octal/binary numbers and
`inf`/`nan` are errors, as are duplicate and unknown keys. Errors name the line or the key.

## The script

```lua
return {
  render = function(ctx, props)
    return { ui.title("Hello"), ui.text(props.who) }
  end,
}
```

Scripts are Lua 5.4, loaded as text only (precompiled chunks are refused). The environment holds
`string` (without `dump`), `table`, `math` (without `random` and `randomseed`, so the same inputs
give the same output), `utf8`, `ipairs`, `pairs`, `next`, `type`, `tostring`, `tonumber`, `select`,
`pcall`, `error`, `assert`, `print` (a log line) and `ui`. Nothing else: no `os`, `io`, `require`,
`load`, `debug`, `coroutine`, `collectgarbage`, metatables.

### ctx

| | |
|---|---|
| `ctx.read(path)` | The file's contents as a string; `nil, reason` if it doesn't exist or can't be read. Raises an error for a path outside `reads`. At most 2 MB. |
| `ctx.load_lua(path)` | Evaluates a Lua data file (like KOReader's `history.lua`) in an empty environment and returns its value: tables, strings, numbers, booleans. Functions are dropped. A data file can't reach anything: it has no globals except `inf` and `nan` (KOReader writes those as bare words), no libraries, no string methods. `nil, reason` on a missing or broken file. |
| `ctx.sidecar(book)` | The path of a KOReader book's metadata: `/x/Book.epub` → `/x/Book.sdr/metadata.epub.lua`. Reading it needs the book's directory in `reads`. |
| `ctx.now` | The time of the run, in seconds since 1970. |
| `ctx.apps` | App data for Home's own app widgets' settings (for example `categories = { {name=, count=} }`); absent for other widgets. |

`props` is the widget instance's settings: the manifest's defaults, overlaid with what the settings
script set.

## The seven primitives

`render` returns a list, in order, of at most 12 of these:

| | |
|---|---|
| `ui.title(text)` | One line, bold. At most one. 120 bytes. |
| `ui.text(text, {soft = true})` | One line; soft draws it in the secondary grey. 200 bytes. |
| `ui.progress(value, label)` | `value` 0…1 (clamped; Home keeps it in thousandths), an optional label. |
| `ui.list({ {label, meta}, … })` | Up to 4 rows; more are cut. Label 120, meta 60 bytes. |
| `ui.button(label, icon, action)` | At most 2. Label 40 bytes. |
| `ui.meta(text, {time = t})` | The small line at the bottom. With `time`, Home appends how long ago that was ("Read 2 h ago") when it draws, so the output doesn't change every minute. |
| `ui.image{src, w, h, side, fit, alt}` | A slot of a fixed size that Home fills later. At most 2. |

Text must be valid UTF-8; control characters become spaces; longer text is cut at a character
boundary.

Actions: `ui.open(app, file)` (an app id, an optional absolute file path) and `ui.settings(page)`.
Nothing else can run.

Image slots: `src` is `file:PATH` (inside `reads`), `cover:PATH` (an ebook's cover) or `icon:APP`;
no network sources. `h` (and `w` when `side` is set) are whole pixels from 1 to 2000; Home clamps
them to the card. `side` is `"left"`, `"right"` or absent/`"none"` (full width); `fit` is `"cover"`
(default) or `"contain"`; `alt` is required (shown in the dashed frame until the image is ready).

## Settings: widget.edit.lua

The same shape, primitives, sandbox and limits. In a settings script, list rows and buttons may
carry `ui.set{key = value}` as their action (keys must be declared in `[props]`, with the same type)
and `selected = true` to mark the current choice:

```lua
rows[#rows + 1] = { c.name, c.count .. " apps", selected = (c.name == props.category),
                    action = ui.set { category = c.name } }
-- a button: ui.button(label, icon, ui.set { ... }, { selected = true })
```

After every change both scripts run again.

## Limits and failures

A run gets 200 ms of CPU and 4 MB of memory (a data file read with `ctx.load_lua` gets its own
4 MB); Home also stops a run after 2 s of wall time. Over a limit, on an error, or with an invalid
description, Home keeps the last good output and the meta line says so. Three failures in a row
pause the widget until the gallery is opened.

Home runs a widget when a listed file it read changes, on its interval, once when Home appears, and
after a settings change. It compares the output (canonical bytes) and redraws only cards whose
output changed.

## How a run is isolated

Home forks a helper at startup; each run is a fresh child of it with one socket. Before any widget
code runs, the child caps its address space, processes, files and CPU time, and enters a seccomp
allowlist (reading from fds it was given, writing and messaging on its socket, memory management,
clocks, exit); any other system call ends it. It never opens a path: `ctx.read` asks Home over the
socket, Home checks `reads`, opens the file beneath the read's directory without following symlinks
(`openat2` with `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`, or an `O_NOFOLLOW` walk where `openat2`
is missing), refuses non-regular files, and passes the open file back. Home checks the run's
output again; the run is untrusted.

## Trying a widget

```
glassine-home-run [--edit] [--props JSON] [--now SECONDS] [-v] WIDGETDIR
```

runs one widget through the real sandbox and prints its canonical description; `-v` lists the files
it asked for and its `print` lines.
