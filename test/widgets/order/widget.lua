-- SPDX-License-Identifier: GPL-2.0-or-later
-- the order pairs and next visit keys in, and what removing keys during a loop does
local function name(k) return type(k) == "table" and "table" or tostring(k) end
return { render = function(ctx, props)
  if props.case == "nextloop" then
    -- a walk with next over many keys costs about what pairs does
    local m, n = {}, 0
    for i = 1, 20000 do m["k" .. i] = i end
    for k, v in next, m do n = n + 1 end
    return { ui.text(tostring(n)) }
  end
  local t = { charlie = 1, alpha = 2, Bravo = 3, delta = 4, echo = 5, foxtrot = 6, golf = 7, hotel = 8,
    [3] = 9, [1.5] = 10, [-2] = 11, [true] = 12, [false] = 13, [{}] = 14 }
  local a, b, c = {}, {}, {}
  for k in pairs(t) do a[#a + 1] = name(k) end
  local k = next(t)
  while k ~= nil do b[#b + 1] = name(k) k = next(t, k) end
  for k in pairs(t) do
    c[#c + 1] = name(k)
    if k == "alpha" then t.delta = nil end
  end
  return { ui.text(table.concat(a, " ")), ui.text(table.concat(b, " ")), ui.text(table.concat(c, " ")) }
end }
