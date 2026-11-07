-- SPDX-License-Identifier: GPL-2.0-or-later
return { render = function()
  while true do pcall(string.rep, "x", 5000000) end
end }
