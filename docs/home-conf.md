# The layout file, home.conf (version 1)

<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

`glassine-home` keeps its layout in one file: the widgets in order, each widget's size and
settings, and the favourite apps. Home owns the format (spec 001, Contracts, FR-043). Settings
(`GlassineOS/shell`) edits one key of it, `favourites`; nothing else may write it. This page is
enough to read and write it without Home's code.

## Where

`$XDG_CONFIG_HOME/glassine/home.conf`, or `~/.config/glassine/home.conf` when `XDG_CONFIG_HOME`
is unset or not an absolute path. Home creates the folders with mode 0700 and the file with mode
0600. Beside it: `home.conf.lock` (the lock, below) and `home.conf.tmp` (Home's temporary file
while it writes).

## An example

This is how Home writes it:

```toml
# glassine-home: the widgets in order. Pages follow from packing.
version = 1
favourites = ["koreader", "firefox-esr", "org.gnome.Calendar"]

[[widget]]
uuid = "0f6d3c1e-2b8a-4c5d-9e7f-1a2b3c4d5e6f"
widget = "builtin/reading"
size = "wide"

[[widget]]
uuid = "8c1d2e3f-4a5b-4c6d-8e9f-0a1b2c3d4e5f"
widget = "home/app-category"
size = 2
props = { category = "Games" }
```

## Keys

At the top, before the first `[[widget]]`:

| Key | Type | |
|---|---|---|
| `version` | integer ≥ 1 | Required. This is version 1. |
| `favourites` | array of strings | Desktop ids (`org.gnome.Calendar`, `koreader`), in the user's order. Optional; Home leaves it out when the list is empty, and `[]` means the same. A duplicate is kept as written. |

Then one `[[widget]]` table per widget, in order (at most 256). Pages are never stored: they
follow from the order and the sizes.

| Key | Type | |
|---|---|---|
| `uuid` | string | The instance's id: a version 4 UUID, 36 characters, lowercase hex. Unique in the file; it stays the same when the widget moves, changes size or settings. |
| `widget` | string | `SOURCE/ID`. SOURCE is `builtin` (ships with Home), `system` (brought by an app), `user` (the user's own) or `home` (Home's app widgets: `home/favourites`, `home/all-apps`, `home/app-category`). ID is a widget id, `[a-z0-9][a-z0-9._-]*`, at most 64 bytes (`docs/widgets.md`). |
| `size` | string or integer | `"half"` or `"wide"` for a widget with a script; `1`, `2`, `3` or `5` (rows) for an app widget. |
| `props` | inline table | Optional. The settings the user changed: strings, integers, floats, booleans; nothing nested. A key the widget doesn't declare, or of another type than its default, is kept in the file but not used. |

A widget whose folder is gone keeps its place and its table; Home shows it as "Missing widget".

## Syntax

The TOML subset of `docs/widgets.md` (comments, `key = value` with bare or quoted keys, basic and
literal one-line strings, decimal integers and floats, booleans, arrays that may span lines, one-line
inline tables) plus the `[[widget]]` array-of-tables header. Dotted keys, dates, multi-line strings,
hex/octal/binary numbers, `inf`/`nan` and duplicate keys are errors. At most 1 MiB, UTF-8.

Keys Home doesn't know are ignored, and gone after Home's next write. **A key belongs to the table
above it**: `favourites` written after the first `[[widget]]` is a key of the last widget, so Home
sees no favourites at all. Write it at the top.

## Reading it

A missing file is an empty layout. A file Home can't parse when it starts is renamed to
`home.conf.broken` (kept, never deleted) and Home starts empty. A `version` higher than the reader
knows: the reader uses nothing from it and never writes it.

## Writing it: the rules every writer follows

1. Take an exclusive `flock(2)` on `home.conf.lock` in the same folder (open it with `O_CREAT`,
   mode 0600). Wait for it; Home holds it only while it writes. Never delete the lock file: a
   writer holding the old one would not exclude a writer that made a new one.
2. Read the file again under the lock and make your change to what is there now.
3. If `version` is higher than you know, don't write: say so and stop.
4. Write the whole file to a temporary file in the same folder, `fsync` it, `rename(2)` it over
   `home.conf`, and `fsync` the folder. Then release the lock (close the descriptor).

Readers don't take the lock: a writer only ever renames a whole file into place.

**Settings** edits only `favourites`. It keeps every other line and table as it is, byte for
byte where it can (Home writes the whole file in its own layout anyway). With no file yet, it
creates one with only `version = 1` and `favourites`.

## What Home does with an outside change

Home watches the folder (inotify) and reads the file again when `home.conf` is renamed into
place or closed after writing; a file that reads the same as Home's last read or write is
nothing new.

- It takes the change at once, without the lock, keeping the page and Arrange: a removed widget
  disappears (its settings sheet closes), a widget with changed settings runs again, and a
  changed favourites list redraws only the Favourite apps card.
- Before Home writes (under the lock), it reads the file again. Whatever changed outside since
  Home's last read or write is kept unless Home changed the same part: the favourites and the
  widget list are the two parts, so Settings' favourites and Home's Arrange merge. When both
  changed the same part, Home's write wins.
- A file from outside that Home can't parse (or a newer `version`) is left alone: Home keeps
  what it shows, writes nothing over it, and logs it once. Its changes in the meantime are written
  when a readable file appears again.
- A file removed from outside: Home keeps its layout and writes the file again at its next change.
