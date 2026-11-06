-- SPDX-License-Identifier: GPL-2.0-or-later
return { render = function()
  local t = {}
  for i = 1, 1000000 do t[i] = "row " .. i end
  return { ui.text(#t) }
end }
