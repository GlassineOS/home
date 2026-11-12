# GlassineOS/home

`glassine-home`, the Home screen: pages of widgets on a fixed grid. Apps are one kind of widget; anyone
can write more. A widget only describes what to show, from seven primitives; Home draws it in the
current voice and decides when it runs, so a widget can't animate, flash the panel or keep the CPU
awake. Widget scripts are Lua 5.4, sandboxed; a widget's manifest is a data file beside its script.

Home is an ordinary app. The session keeps the home app in sway's scratchpad and shows it over the
current screen (`GlassineOS/shell`), so another app can take its place.

**Status:** implemented with host tests (`make check`), not yet run on a device: the widget runtime
(the sandbox, the manifest, the description format, refresh scheduling, the layout file), page
packing, the built-in Continue reading widget, and the screen itself, `glassine-home`: a Wayland app
(app_id `glassine-home`) drawn with `GlassineOS/ui`, with the app widgets, the widgets' cards and
image slots, Arrange, the Add widget gallery and widget settings. Writing a widget: `docs/widgets.md`.

GlassineOS is a hobby project, written largely with AI coding agents and not reviewed by subsystem
experts.

## Build and test

Needs a C11 compiler, Lua 5.4 (`lua5.4-dev`), `pkgconf` and the Linux UAPI headers
(`linux-headers`) for the runtime; cairo, pango, librsvg, glib and gdk-pixbuf plus `GlassineOS/ui`
for the drawing; `wayland-dev`, `wayland-protocols` and `libxkbcommon-dev` for the program. CI builds
in `alpine:3.24` (the apk list is in `.github/workflows/build.yml`).

`GlassineOS/ui` is found as `test/ui.sh` says: `$GLASSINE_UI`, else `../ui`, else a clone at
`$GLASSINE_UI_REF` (default `main`); it is built into `out/ui-stage`. `UI_SYSTEM=1` uses an installed
one instead.

```
make check                 # host tests; the screen's boards land in out/renders/*.png
out/glassine-home-run widgets/reading     # run one widget in the sandbox
out/glassine-home          # the screen, in a Wayland session
```

`out/renders/home.<board>-<voice>-<orientation>.png` are the design's `home.*` boards (screen,
page-2, page-3, images, edit, edit-focus, app-menu, widget-settings, add-widget) in the three
voices, portrait and landscape, drawn offscreen from the fixtures in `test/screen/`.

### How the screen behaves

- Opens apps only through `glassine-shell-wl launch DESKTOP-ID new|here` (a tap: new; the app menu's
  "Open on this screen": here). A widget's file or a Settings page goes as a third word; the shell
  doesn't take it yet. Hides itself only with `glassine-swayd home hide` (Escape). Both commands
  can be replaced through the environment (`src/cli/home.c` lists it).
- One surface, one commit per paint; nothing animates. Input paints at once; widget output,
  images and the clock wait until a second has passed since the last such paint. A paint that
  changes nothing commits nothing; a changed card damages only its rectangle, an image filling in
  only its slot.
- Widget runs go one at a time through the sandbox on the event loop (`run_start`/`run_step`),
  re-run on file changes (inotify, debounced) and on the shared 15-minute grid.
- Image slots are loaded, scaled and greyed on a worker thread (gdk-pixbuf), dithered only when the
  panel's dithering is on (`/etc/glassine/eink.conf`), and cached by source, its date and size, the
  slot size and the dithering, in memory and in `~/.cache/glassine-home/images`. Until then the
  slot is a dashed frame with its alt text. Sources are still PNG, JPEG, WebP or GIF, measured from
  their header before decoding; another format, an animation, or over 20 MB or 40 million pixels
  keeps the frame.
- Built-in widgets are installed in `/usr/share/glassine-home/widgets`; App category's settings
  script in `/usr/share/glassine-home/native`. Favourites live in the layout file; the shell's old
  `~/.config/glassine-shell/favourites` is imported once, when Home starts without a layout file.
- The layout file, `~/.config/glassine/home.conf`, is shared with Settings, which edits its
  favourites: its format and the rules for writing it (a lock, temp + fsync + rename) are in
  `docs/home-conf.md`. Home writes under the same lock and reloads the file when it changes
  outside; a file it can't read is left alone.

The seccomp filter supports x86_64 and aarch64.

## Licence

GPL-2.0-or-later (`LICENSE`).
