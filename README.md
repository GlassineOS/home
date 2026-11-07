# GlassineOS/home

`glassine-home`, the Home screen: pages of widgets on a fixed grid. Apps are one kind of widget; anyone
can write more. A widget only describes what to show, from seven primitives; Home draws it in the
current voice and decides when it runs, so a widget can't animate, flash the panel or keep the CPU
awake. Widget scripts are Lua 5.4, sandboxed; a widget's manifest is a data file beside its script.

Home is an ordinary app. The session keeps the home app in sway's scratchpad and shows it over the
current screen (`GlassineOS/shell`), so another app can take its place.

**Status:** the widget runtime and page packing are implemented with host tests (`make check`):
the sandbox, the manifest, the description format, refresh scheduling helpers, the layout file and
the built-in Continue reading widget. Nothing has run on a device yet, and Home does not draw anything
yet: the screen comes later, on the drawing code in `GlassineOS/ui`. Writing a widget:
`docs/widgets.md`.

GlassineOS is a hobby project, written largely with AI coding agents and not reviewed by subsystem
experts.

## Build and test

Needs a C11 compiler, Lua 5.4 (`lua5.4-dev`), `pkgconf` and the Linux UAPI headers
(`linux-headers`); CI builds in `alpine:3.24`.

```
make check                 # host tests
out/glassine-home-run widgets/reading     # run one widget in the sandbox
```

The seccomp filter supports x86_64 and aarch64.

## Licence

GPL-2.0-or-later (`LICENSE`).
