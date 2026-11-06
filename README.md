# GlassineOS/home

`glassine-home`, the Home screen: pages of widgets on a fixed grid. Apps are one kind of widget; anyone
can write more. A widget only describes what to show, from seven primitives; Home draws it in the
current voice and decides when it runs, so a widget can't animate, flash the panel or keep the CPU
awake. Widget scripts are Lua 5.4, sandboxed; a widget's manifest is a data file beside its script.

Home is an ordinary app. The session keeps the home app in sway's scratchpad and shows it over the
current screen (`GlassineOS/shell`), so another app can take its place.

**Status:** being specified (`specs/`). It builds on the drawing code in `GlassineOS/ui`.

## Build and test

```
make check
```

## Licence

GPL-2.0-or-later (`LICENSE`).
