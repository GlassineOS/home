-- SPDX-License-Identifier: GPL-2.0-or-later
-- App category's settings: the categories that have apps, the current one marked.
-- A list holds 4 rows, so the rows go in lists of 4; the sheet shows them as one.
return {
  render = function(ctx, props)
    local out, rows = { ui.title("Show apps from") }, {}
    for _, c in ipairs(ctx.apps and ctx.apps.categories or {}) do
      local n = c.count == 1 and "1 app" or (c.count .. " apps")
      rows[#rows + 1] = { c.name, n, selected = (c.name == props.category),
                          action = ui.set { category = c.name } }
      if #rows == 4 then
        out[#out + 1] = ui.list(rows)
        rows = {}
      end
    end
    if #rows > 0 then out[#out + 1] = ui.list(rows) end
    if #out == 1 then out[2] = ui.text("No apps installed", { soft = true }) end
    return out
  end,
}
