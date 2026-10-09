#pragma once

// A MARQUEE_WND's credits: what one DATASOURCE loads [orig:
// CMarqueeWnd_ParseXMLDefinition @ 0x65ceb0 -> CMarqueeWnd_LoadCreditsFromIni
// @ 0x65c5a0, reading the file through the ConfigFile text reader
// (ConfigFile_LoadFromFile @ 0x760a10 -> ConfigFile_ParseText @ 0x7608a0;
// ConfigFile_ReadKeyValue @ 0x75fc90 -> effect_get_param_value_0 @ 0x75fa00)] and
// the per-frame scroll the frame compiler draws them with [orig:
// CMarqueeWnd_RenderScrollingCredits @ 0x65ca00].
// Witness record: docs/mnu/menu-re.md ("Marquee credits").

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::menu {

// One credit node (the 224-byte node [orig: CMarqueeWnd_AppendCreditNode
// @ 0x65c4d0]).
struct MarqueeCreditNode {
	bool text = false;       // a [TEXT] line (else a ~I or ~F image node)
	std::string line;        // node+0: the line's first value
	std::string font;        // node+128: the font name the line's second value set
	uint32_t color = 0xFFFFFFFFu; // node+176: the ~C colour word (strtol base 16)
	int justify = 1;         // node+216: 0 left, 2 right, anything else centred
	std::string image;       // ~I / ~F: the texture
	bool fades = false;      // node+212: ~F (drawn fading at the edges)
	int fixed_x = 0;         // node+164 / +168: the ~F coordinates
	int fixed_y = 0;
	// The running offset (+752) at the append: the node starts at the widget's
	// bottom edge plus this.
	int offset = 0;
};

// What a marquee's DATASOURCEs loaded. The window starts zeroed [orig:
// CMarqueeWnd_Construct @ 0x65c430]; each file that loads resets the [ENV]
// values and the running offset first, so a later file's nodes start over at
// the bottom edge.
struct MarqueeCredits {
	float scroll_rate = 0.0f;  // +732: pixels per rendered frame
	int center_x = 0;          // +736: the images' centre line
	int vertical_space = 0;    // +748: the pitch a text line, ~I node or <CR> advances
	char space_mark = 0;       // +740: drawn as a space ('_' once a file loads)
	char comma_mark = 0;       // +744: drawn as a comma ('@' once a file loads)
	std::vector<MarqueeCreditNode> nodes;
};

// The [ENV] values and the two marks every credits file that loads starts over
// from, before its own [ENV] keys are read (a key the [ENV] lacks then reads 0):
// a scroll rate of 1, the images' centre at 400, no space between lines, a space
// marked '_' and a comma '@' [orig: CMarqueeWnd_LoadCreditsFromIni @ 0x65c605..0x65c63d].
inline constexpr float kMarqueeScrollRate = 1.0f;
inline constexpr int kMarqueeCenterX = 400;
inline constexpr int kMarqueeVerticalSpace = 0;
inline constexpr char kMarqueeSpaceMark = '_';
inline constexpr char kMarqueeCommaMark = '@';

// Load one DATASOURCE and append its credits. `data` is the file's bytes; a CBIN
// file (the "CBIN" magic) is the binary config form this port does not read here
// (false, untouched: the embedder's credits scroller serves it). A text file reads
// as the ConfigFile text reader reads it. `texture_loads` answers whether an image
// node's texture loads (a node whose texture does not is not appended).
bool marquee_load_credits(const uint8_t *data, size_t size, MarqueeCredits &io,
		const std::function<bool(const std::string &)> &texture_loads);

// The text a node draws: the node's line formatted as retail's sprintf formats it
// with no arguments ("%%" is '%'; any other conversion is left as written), then the
// two marks remapped [orig: CMarqueeWnd_RenderScrollingCredits @ 0x65ca00 — sprintf, then
// this[185] -> ' ', this[186] -> ','].
std::string marquee_node_text(const MarqueeCredits &credits, const MarqueeCreditNode &node);
// The [TEXT] line a credits file writes to draw `shown` once it loads: each '%'
// doubled, each space the space mark and each comma the comma mark the load sets,
// marquee_node_text's formatting and remap inverted (a shown '_' or '@' has no
// spelling: the remap draws it as a space or a comma).
std::string marquee_marked_line(const std::string &shown);

} // namespace opennova::menu
