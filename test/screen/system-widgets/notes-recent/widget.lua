-- SPDX-License-Identifier: GPL-2.0-or-later
return {
  render = function(ctx, props)
    local d = ctx.load_lua("~/data/notes.lua")
    if not d then return { ui.text("No notes yet", { soft = true }) } end
    local rows = {}
    for i, n in ipairs(d.notes) do
      if i > 3 then break end
      rows[#rows + 1] = { n[1], n[2] }
    end
    return { ui.list(rows), ui.meta(#rows .. " of " .. d.total .. " notes") }
  end,
}
