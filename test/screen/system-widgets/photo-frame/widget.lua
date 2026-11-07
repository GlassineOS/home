-- SPDX-License-Identifier: GPL-2.0-or-later
return {
  render = function(ctx, props)
    local d = ctx.load_lua("~/data/photo.lua")
    if not d then return { ui.text("No pictures in Pictures/Frame", { soft = true }) } end
    return { ui.image { src = "file:" .. d.file, w = 132, h = 78, side = "left",
                        alt = "Today's photo from Pictures/Frame" },
             ui.title(d.title), ui.text(d.where, { soft = true }), ui.meta("Changes at 06:00") }
  end,
}
