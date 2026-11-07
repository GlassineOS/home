-- SPDX-License-Identifier: GPL-2.0-or-later
return {
  render = function(ctx, props)
    local d = ctx.load_lua("~/.cache/sky/sky.lua")
    if not d then return { ui.text("No sky data", { soft = true }) } end
    return { ui.image { src = "file:" .. d.moon, w = 56, h = 56, side = "left", alt = d.alt },
             ui.title(d.title), ui.text(d.text, { soft = true }), ui.meta("Your widget · sky.lua") }
  end,
}
