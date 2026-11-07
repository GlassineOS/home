# Feature Specification: Home screen and widgets

**Feature Branch**: `spec-home-screen`
**Created**: 2026-11-06
**Status**: Draft
**Input**: The "Glassine OS Shell" design canvas, Home screen page (boards `home.*`, `shell.home`,
`shell.home.empty`), and the shell spec's "Home screen" section. User decisions 2026-11-06: widget scripts
are Lua 5.4; a widget's manifest is a data file beside its script, never code.

## Context

The old launcher was a panel of the shell. Home replaces it as an app of its own: pages of widgets on a fixed
grid, where apps are one kind of widget and anyone can write more. A widget only describes what to show; Home
draws it in the current voice and decides when it runs, so a widget can't animate, flash the panel or keep the
CPU awake. Which app is the home app, and how it is shown and hidden, belongs to the session (`GlassineOS/shell` spec 001,
FR-030, FR-031, FR-040). Home is one app that fills that role; any other app may.

## User scenarios

### Scenario 1 – Open an app (Priority: P1)

**Given** Home with Favourite apps on page 1,
**When** I tap KOReader,
**Then** it opens on a new screen (or fills the empty screen Home sits on) and Home hides.
**When** I hold it instead,
**Then** the app menu offers Open on a new screen, Open on this screen (not with four windows there), Add to
or remove from favourites, and Configure (Settings › Apps › the app).

### Scenario 2 – Turn pages (Priority: P1)

**Given** two pages,
**When** I tap › in the header,
**Then** page 2 shows; the header reads "‹ 2/2 ›". An app widget with more apps than it holds pages them in
its own header.

### Scenario 3 – Arrange (Priority: P1)

**When** I tap Arrange,
**Then** the page zooms out into a frame 46 px narrower on each side, neighbouring pages peek in at the edges as
hatched outlines; Swap buttons sit on edges two widgets share (only where both still fit after the swap); a
round button in each gap to a neighbouring page swaps the whole page with it; each card has settings (only if
it has any), size and remove; an Add widget card sits where the next widget would go.

### Scenario 4 – Add a widget (Priority: P2)

**When** I tap Add widget,
**Then** a sheet lists built-in widgets, widgets apps bring and my own, each with where it comes from, its
sizes and when it refreshes; Add puts it after the last widget and turns to its page; a widget with settings
opens them right away.

### Scenario 5 – A widget with settings (Priority: P2)

**Given** an App category widget showing Games,
**When** I open its settings and tap Office,
**Then** the row is marked as the current choice and the widget behind the sheet shows Office apps at once;
a second App category widget keeps its own choice.

### Scenario 6 – Continue reading (Priority: P2)

**Given** I read a book in KOReader,
**When** Home appears,
**Then** the Continue reading widget shows the book's cover, title, author, progress and a Continue button that
opens the book in KOReader.

### Scenario 7 – Images arrive late (Priority: P3)

**Given** a widget with a cover image not yet prepared,
**When** Home appears,
**Then** the card shows a dashed frame with the image's alt text in the image's place, and the image fills in
later with one partial refresh of that rectangle only; nothing else moves.

## Requirements

**Pages and grid**
- **FR-001**: Home is pages of widgets on a fixed grid: 4 columns × 5 rows in portrait; in landscape two lanes of
  4 × 3 side by side.
- **FR-002**: A half widget is 2 columns, a wide one 4. Script widgets are one row tall; app widgets are 1, 2, 3 or
  5 rows (landscape caps at 3).
- **FR-003**: A page packs its widgets in order (a half fills the first free half on the page, then the next
  lane); a widget that no longer fits after a size change or a move spills to the front of the next page; empty
  pages close up. A new widget goes on the last page, or a new page when that is full; adding never shrinks
  others.
- **FR-004**: The header carries the date and status, the Home pager (only with more than one page) and Arrange.

**App widgets (built into Home, drawn natively)**
- **FR-010**: Favourite apps (in the user's order), All apps (A–Z), App category (one category chosen in its
  settings; may be added any number of times). Every widget can be removed, All apps included. Each shows as many rows of four apps as its
  height holds and pages the rest with its own pager.
- **FR-011**: Tap an app: open it on a new screen, or fill the empty screen Home sits on
  (`GlassineOS/shell` spec 001 FR-040).
  Hold: the app menu of Scenario 1.
- **FR-012**: When an app exists twice (a Flatpak and a package), both appear with their origin; Home opens
  the one opened last.

**Widgets**
- **FR-020**: A widget is a folder holding a manifest (data) and a render script; optionally a settings script.
  Home never runs code to list, describe or add a widget.
- **FR-021**: The manifest declares name, icon, sizes (the first is the default), the files it reads (fixed
  paths, or paths inside a named place, FR-028), when it
  refreshes (when a listed file changes and/or an interval of at
  least 15 minutes) and its settings with their defaults.
- **FR-022**: The render script returns a description built from seven primitives only: title (one line, at most
  one), text (one line, optionally soft), progress (0–1 with its number), list (up to 4 rows, more are cut),
  button (at most 2; it can only open an app, a file in an app, or a Settings page), meta (the bottom line), image
  slot (at most 2). Never pixels, colours, fonts or layout.
- **FR-023**: Home runs a widget when a listed file changes, on its interval, and once when Home appears. It
  compares the output and redraws only cards whose output changed, together, as one partial refresh.
- **FR-024**: A run gets 200 ms and 4 MB. It can read only the files its manifest lists (and a KOReader book's
  metadata beside a listed book). Widgets have no network access at all. Data files are read as data: a data
  file can't run code or reach anything. Over the limits or on an error, Home keeps the last good output and the
  meta line says so; three failures in a row pause the widget until the gallery is opened.
- **FR-025**: Widgets come from three places: built into Home, brought by apps (system-wide), and the user's own
  (per user, one folder each). Home picks up a new folder without a restart.
- **FR-026**: Settings: a widget with settings has a settings script with the same primitives, limits and
  lifecycle. Both scripts get the widget's settings; settings start from the manifest's defaults and are kept
  per widget instance. In the settings script, list rows and buttons may set settings and mark the current
  choice; after every change both scripts run again, so the widget behind the sheet updates at once. Built-in
  widgets use the same mechanism.
- **FR-027**: Image slots: a widget declares source, width, height (fixed, clamped to the card), side (left, right
  or none = full width), fit (cover or contain) and alt text. Home lays the card out at once, then outside the
  run's budget loads the source, scales it to exactly the slot, turns it grey and dithers it with the E-ink
  dithering setting, in the background, starting right after the run even while Home is hidden. A slot not yet
  ready shows a dashed frame with its alt text. Results are cached by source, its date or ETag, slot size and
  dithering. Sources are local only: a listed file, an ebook's cover, an app's icon. Over 20 MB or
  broken: the frame and alt text stay.
- **FR-028**: Places: a read may start with a named place instead of a fixed folder. The one place so far is
  the user's books folder (`{books}`), a user setting that defaults to `~/Books`; a widget may read all of it
  (KOReader's book metadata lives beside the books). Home resolves the place from the setting before it checks
  the reads, and the same rules apply inside it (no links followed, nothing outside it). An unknown place name
  makes the manifest invalid. When the place is unset, missing, a link, or points into the system's own areas,
  reads inside it fail; Home never falls back to a wider folder. The built-in Continue reading widget reads
  `{books}/`.

**Arranging**
- **FR-030**: Arrange as in Scenario 3; tapping a peek turns to that page; the page being arranged moves with a
  page swap and stays in view.
- **FR-031**: Each card: settings (only if the widget has any), size (its allowed sizes), remove.
- **FR-032**: Add widget as in Scenario 4; the Add button is icon-only and becomes a check once the widget is on
  Home (except App category, which can be added again).

**General**
- **FR-040**: The layout, each widget's settings and favourites persist across restarts.
- **FR-041**: Touch targets ≥ 48 px; Tab/Enter/Space/Escape as in `GlassineOS/shell` spec 001 FR-050; no animation; nothing redraws
  more than once a second; three voices; portrait and landscape.
- **FR-042**: A built-in Continue reading widget for KOReader ships with Home (Scenario 6).

## Contracts

| Contract | Owned / consumed | Version | Spec location |
|---|---|---|---|
| Widget format (folder layout, manifest keys, the seven primitives, actions, the context a script gets, limits) | owned | 1 | this repository, `docs/widgets.md` (written with the implementation) |
| The home-app rule (show, hide, fill the empty screen) and launching apps | consumed | 1 | `GlassineOS/shell` spec 001 |
| The voices' drawing library | consumed | 1 | `GlassineOS/ui` |
| KOReader history and book metadata (read only) | consumed | KOReader's current format | KOReader |
| The E-ink dithering setting | consumed | as Settings writes it | spec of Settings › E-ink |
| Places (the user's books folder; a user-level setting Settings can later own) | owned | 1 | this repository, `docs/widgets.md` |

## Device scope and risk

Device-neutral (grid and sizes follow the screen's logical size). No device risk.

## Acceptance

- Host: packing tests (adding, resizing, moving, spilling, closing pages) in both orientations.
- Host: sandbox tests: a widget can't read an unlisted file, follow a link out of its reads, run code from a
  data file, reach the network, exceed 200 ms or 4 MB; three failures pause it.
- Host: places: the default, a set folder, an unset or missing one, an unknown name, a place that is a link or
  points into system areas (refused); Continue reading with a library outside `~/Books`.
- Host: renders of every `home.*` board state in three voices and both orientations.
- Hardware (supervised): Home from the bar, open an app, Arrange, add a widget, Continue reading with a real book.

## Out of scope

- Network access of any kind for widgets: no HTTP fetch, no remote images (decided 2026-11-06).
- Android apps' own widgets.

## Success criteria

- **SC-001**: Home appears complete (no unfilled slot) in the common case, because image work starts while hidden.
- **SC-002**: A widget whose output didn't change causes no panel update.
- **SC-003**: Opening an app from Home is one tap; any app is at most two page turns away from page 1 in All apps.
