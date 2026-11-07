-- SPDX-License-Identifier: GPL-2.0-or-later
return { render = function(ctx, props)
  if props.case == "function" then return { ui.title("x"), print } end
  if props.case == "mixed" then return { ui.title("x"), { t = "text", text = "y", 1 } } end
  if props.case == "titles" then return { ui.title("a"), ui.title("b") } end
  if props.case == "number" then return 42 end
  if props.case == "set" then return { ui.button("x", nil, ui.set { a = 1 }) } end
  if props.case == "error" then error("boom") end
  if props.case == "errtable" then error({}) end
  if props.case == "bigstrings" then
    local s, t = string.rep("x", 1000000), {}
    for i = 1, 3000 do t[i] = s end
    return t
  end
  if props.case == "longtext" then return { ui.text(string.rep("\u{e9}", 100000)) } end
  if props.case == "badtail" then return { ui.text(string.rep("a", 10000) .. "\255") } end
  if props.case == "longerror" then error("\u{e9}e" .. string.rep("\u{65e5}\u{672c}", 400)) end
  if props.case == "longprint" then print("x" .. string.rep("\u{65e5}\u{672c}", 100)) return {} end
  if props.case == "biglist" then local t = {} for i = 1, 5000 do t[i] = 1 end return t end
  if props.case == "imgfile" then return { ui.image { src = "file:/etc/hostname", h = 10, alt = "x" } } end
  return {}
end }
