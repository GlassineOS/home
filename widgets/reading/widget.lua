-- SPDX-License-Identifier: GPL-2.0-or-later
-- Continue reading: KOReader's last book, from its history and the book's .sdr metadata.

local HISTORY = "~/.config/koreader/history.lua"

local function nonempty(s)
  if type(s) == "string" and s:find("%S") then return s end
end

-- "/x/Some Book.epub" -> "Some Book"
local function stem(path)
  local name = path:match("([^/]+)$") or path
  return (name:gsub("%.[^.]+$", ""))
end

return {
  render = function(ctx, props)
    local history = ctx.load_lua(HISTORY)
    local last = type(history) == "table" and history[1]
    if type(last) ~= "table" or type(last.file) ~= "string" then
      return { ui.title("Continue reading"), ui.text("Nothing read yet", { soft = true }) }
    end

    -- the metadata may be missing, or the book outside the readable library
    local ok, doc = pcall(ctx.load_lua, ctx.sidecar(last.file))
    if not ok or type(doc) ~= "table" then doc = {} end
    local p = type(doc.doc_props) == "table" and doc.doc_props or {}

    local title = nonempty(p.title) or stem(last.file)
    local out = {
      ui.image { src = "cover:" .. last.file, w = 64, h = 96, side = "left", alt = "Cover of " .. title },
      ui.title(title),
    }
    local authors = nonempty(p.authors)
    if authors then
      -- KOReader keeps several authors on separate lines
      out[#out + 1] = ui.text((authors:gsub("%s*\n%s*", ", ")))
    end
    local done = doc.percent_finished
    if type(done) == "number" and done == done then
      out[#out + 1] = ui.progress(done, math.floor(done * 100 + 0.5) .. " %")
    end
    out[#out + 1] = ui.button("Continue", "play_arrow", ui.open("koreader", last.file))
    if type(last.time) == "number" then
      out[#out + 1] = ui.meta("Read", { time = last.time })
    end
    return out
  end,
}
