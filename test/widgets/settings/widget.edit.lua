-- SPDX-License-Identifier: GPL-2.0-or-later
return {
  render = function(ctx, props)
    local rows = {}
    for _, c in ipairs(ctx.apps and ctx.apps.categories or {}) do
      rows[#rows + 1] = { c.name, c.count .. " apps", selected = (c.name == props.category),
                          action = ui.set { category = c.name } }
    end
    return { ui.title("Show apps from"), ui.list(rows) }
  end,
}
