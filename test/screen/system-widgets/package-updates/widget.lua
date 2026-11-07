-- SPDX-License-Identifier: GPL-2.0-or-later
return {
  render = function(ctx, props)
    local d = ctx.load_lua("~/data/updates.lua")
    if not d then return { ui.text("Up to date", { soft = true }) } end
    return { ui.title(#d.names .. " updates"), ui.text(table.concat(d.names, ", "), { soft = true }),
             ui.button("Review", "arrow_forward", ui.settings("updates")),
             ui.meta("Checked today " .. d.checked) }
  end,
}
