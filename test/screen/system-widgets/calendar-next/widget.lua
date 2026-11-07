-- SPDX-License-Identifier: GPL-2.0-or-later
return {
  render = function(ctx, props)
    local d = ctx.load_lua("~/data/calendar.lua")
    if not d then return { ui.text("Nothing planned", { soft = true }) } end
    return { ui.title(d.title), ui.text(d.when), ui.text("Then: " .. d.next, { soft = true }),
             ui.meta("Updated " .. d.updated) }
  end,
}
