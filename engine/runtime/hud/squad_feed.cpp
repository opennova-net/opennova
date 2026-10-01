#include <runtime/hud/squad_feed.h>

#include <runtime/hud/hud_game_text.h>

#include <string>
#include <vector>

namespace opennova::hud {

namespace {

constexpr uint32_t kSquadLineColor = 0xFFF0F000u;    // g_HUDColors.palette[4]
constexpr uint32_t kReceivedLineColor = 0xFFFFFFFFu; // the -1 colour

std::string with_name(const std::string &format, const std::string &name) {
	HudTextArg arg;
	arg.text = name;
	return hud_sprintf(format, std::vector<HudTextArg>{arg});
}

} // namespace

const char *go_code_name(uint8_t code) {
	switch (code) {
	case 0: return "UNIFORM";
	case 1: return "VICTOR";
	case 2: return "WHISKEY";
	case 3: return "XRAY";
	case 4: return "YANKEE";
	case 5: return "ZULU";
	default: return "";
	}
}

SquadFeedPost squad_feed_post(const SquadFeedLine &line, const GameTextLookup &gametext) {
	SquadFeedPost out;
	out.argb = kSquadLineColor;
	out.post = true;
	switch (line.kind) {
	case SquadFeedLine::Kind::Join:
		out.text = with_name(game_text(gametext, "HUD", "HUD_CMAP_JOIN", ""), line.text);
		break;
	case SquadFeedLine::Kind::Recruit:
		out.text = with_name(game_text(gametext, "HUD", "HUD_CMAP_RECRUIT", ""), line.text);
		break;
	case SquadFeedLine::Kind::Fireteam: {
		const char *key = line.value == 1 ? "STR_CMAP_FIRETEAMA"
				: line.value == 2 ? "STR_CMAP_FIRETEAMB"
				: line.value == 3 ? "STR_CMAP_FIRETEAMC"
								  : "STR_CMAP_NOFIRETEAM";
		out.text = with_name(game_text(gametext, "HUD", "HUD_CMAP_SETFIRETEAM", ""),
				game_text(gametext, "MENU", key, ""));
		break;
	}
	case SquadFeedLine::Kind::GoCode: {
		// A code past 5 composes no key: no line [orig: the switch default
		// @0x5524d8].
		if (line.value > 5) {
			out.post = false;
			break;
		}
		const std::string key = std::string("HUD_CMAP_GOCODE") + go_code_name(line.value);
		out.text = game_text(gametext, "HUD", key.c_str(), "");
		break;
	}
	case SquadFeedLine::Kind::WaypointReceived:
		out.system_ring = true;
		out.argb = kReceivedLineColor;
		out.text = line.text;
		break;
	}
	return out;
}

} // namespace opennova::hud
