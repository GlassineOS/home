-- SPDX-License-Identifier: GPL-2.0-or-later
return { render = function()
  local s = string.rep("a", 40)
  return { ui.text(tostring(string.find(s, string.rep("a?", 40) .. string.rep("a", 40) .. "b"))) }
end }
