-- SPDX-License-Identifier: GPL-2.0-or-later
-- Home draws App category itself; this script is never run for the card.
return { render = function(ctx, props) return { ui.title(props.category) } end }
