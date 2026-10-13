#include <editor/session/view/terrain_tools_view.h>

#include <cmath>
#include <cstdio>
#include <iterator>

#include <base/io/json.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

struct ToolRow {
	TerrainTool tool;
	const char *token;
	const char *label;
	TerrainToolFamily family;
	TerrainPlane plane;
	bool plane_known;
};
constexpr ToolRow kTools[] = {
	{TerrainTool::None, "none", "None", TerrainToolFamily::None, TerrainPlane::Heights, false},
	{TerrainTool::Raise, "raise", "Raise", TerrainToolFamily::Sculpt, TerrainPlane::Heights, true},
	{TerrainTool::Lower, "lower", "Lower", TerrainToolFamily::Sculpt, TerrainPlane::Heights, true},
	{TerrainTool::Smooth, "smooth", "Smooth", TerrainToolFamily::Sculpt, TerrainPlane::Heights, true},
	{TerrainTool::Flatten, "flatten", "Flatten", TerrainToolFamily::Sculpt, TerrainPlane::Heights, true},
	{TerrainTool::Set, "set", "Set", TerrainToolFamily::Sculpt, TerrainPlane::Heights, true},
	{TerrainTool::Surface, "surface", "Surface", TerrainToolFamily::Paint, TerrainPlane::Surface, true},
	{TerrainTool::Foliage, "foliage", "Foliage", TerrainToolFamily::Paint, TerrainPlane::Foliage, true},
	{TerrainTool::Colour, "colour", "Colour", TerrainToolFamily::Paint, TerrainPlane::Colour, true},
	{TerrainTool::Detail, "detail", "Detail", TerrainToolFamily::Paint, TerrainPlane::Blend, true},
	{TerrainTool::Stamp, "stamp", "Stamp", TerrainToolFamily::Tiles, TerrainPlane::Heights, false},
	{TerrainTool::Tiles, "tiles", "Select", TerrainToolFamily::Tiles, TerrainPlane::Heights, false},
};
static_assert(std::size(kTools) == kTerrainToolCount, "a row per terrain tool");

const ToolRow &row_of(TerrainTool tool) {
	const size_t at = static_cast<size_t>(tool);
	return kTools[at < std::size(kTools) ? at : 0];
}

// A brush member: its token, its bit, how it reads and its bounds.
enum class MemberKind : uint8_t { Tool, Number, Whole, Colour, Flag, Cursor };
struct MemberRow {
	const char *token;
	uint32_t bit;
	MemberKind kind;
	double least, most;
	bool stroke; // a stroke's brush takes it
};
constexpr MemberRow kMembers[] = {
	{"tool", kBrushTool, MemberKind::Tool, 0, 0, true},
	{"radius", kBrushRadius, MemberKind::Number, 0.0, kTerrainRadiusMost, true},
	{"strength", kBrushStrength, MemberKind::Number, 0.0, 1.0, true},
	{"falloff", kBrushFalloff, MemberKind::Number, 0.0, 1.0, true},
	{"spacing", kBrushSpacing, MemberKind::Number, kTerrainSpacingLeast, kTerrainSpacingMost, true},
	{"height", kBrushHeight, MemberKind::Number, 0.0, kTerrainHeightMost, true},
	{"surface", kBrushSurface, MemberKind::Whole, 0, 255, true},
	{"foliage", kBrushFoliage, MemberKind::Whole, 0, 255, true},
	{"colour", kBrushColour, MemberKind::Colour, 0, 0, true},
	{"layer", kBrushLayer, MemberKind::Whole, 0, 2, true},
	{"cell", kBrushCell, MemberKind::Whole, 0, 255, false},
	{"rotation", kBrushRotation, MemberKind::Whole, 0, 3, false},
	{"flip_h", kBrushFlipH, MemberKind::Flag, 0, 0, false},
	{"flip_v", kBrushFlipV, MemberKind::Flag, 0, 0, false},
	{"cursor", kBrushCursor, MemberKind::Cursor, 0, 0, false},
	{"fill", kBrushFill, MemberKind::Flag, 0, 0, true},
	{"erase", kBrushErase, MemberKind::Flag, 0, 0, true},
};

std::string bounds_words(double least, double most) {
	char buffer[64];
	std::snprintf(buffer, sizeof buffer, "%g to %g", least, most);
	return buffer;
}

std::string colour_text(uint32_t colour) {
	char buffer[16];
	std::snprintf(buffer, sizeof buffer, "#%06X", unsigned(colour & 0xFFFFFFu));
	return buffer;
}

bool read_colour(const JsonValue &json, uint32_t &out) {
	if (!json.is_string() || json.string.size() != 7 || json.string[0] != '#') return false;
	uint32_t value = 0;
	for (size_t i = 1; i < 7; ++i) {
		const char c = json.string[i];
		int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (digit < 0) return false;
		value = value * 16 + uint32_t(digit);
	}
	out = value;
	return true;
}

std::string tool_tokens() {
	std::string out;
	for (const ToolRow &row : kTools) out += (out.empty() ? "" : ", ") + std::string(row.token);
	return out;
}

} // namespace

const char *terrain_tool_token(TerrainTool tool) { return row_of(tool).token; }

bool terrain_tool_from_token(const std::string &token, TerrainTool &out) {
	for (const ToolRow &row : kTools)
		if (token == row.token) {
			out = row.tool;
			return true;
		}
	return false;
}

const char *terrain_tool_label(TerrainTool tool) { return row_of(tool).label; }
TerrainToolFamily terrain_tool_family(TerrainTool tool) { return row_of(tool).family; }

const char *terrain_tool_family_label(TerrainToolFamily family) {
	switch (family) {
	case TerrainToolFamily::Sculpt: return "Sculpt";
	case TerrainToolFamily::Paint: return "Paint";
	case TerrainToolFamily::Tiles: return "Tiles";
	case TerrainToolFamily::None: break;
	}
	return "";
}

bool terrain_tool_plane(TerrainTool tool, TerrainPlane &out) {
	const ToolRow &row = row_of(tool);
	if (!row.plane_known) return false;
	out = row.plane;
	return true;
}

JsonValue terrain_brush_to_json(const TerrainBrush &brush, uint32_t only) {
	JsonValue out = JsonValue::make_object();
	const auto put = [&](uint32_t bit, const char *token, JsonValue value) {
		if (only & bit) out.set(token, std::move(value));
	};
	put(kBrushTool, "tool", io::json_string(terrain_tool_token(brush.tool)));
	put(kBrushRadius, "radius", io::json_number(brush.radius));
	put(kBrushStrength, "strength", io::json_number(brush.strength));
	put(kBrushFalloff, "falloff", io::json_number(brush.falloff));
	put(kBrushSpacing, "spacing", io::json_number(brush.spacing));
	put(kBrushHeight, "height", io::json_number(brush.height));
	put(kBrushSurface, "surface", io::json_number(brush.surface));
	put(kBrushFoliage, "foliage", io::json_number(brush.foliage));
	put(kBrushColour, "colour", io::json_string(colour_text(brush.colour)));
	put(kBrushLayer, "layer", io::json_number(brush.layer));
	put(kBrushCell, "cell", io::json_number(brush.cell));
	put(kBrushRotation, "rotation", io::json_number(brush.rotation));
	put(kBrushFlipH, "flip_h", JsonValue::make_bool(brush.flip_h));
	put(kBrushFlipV, "flip_v", JsonValue::make_bool(brush.flip_v));
	put(kBrushFill, "fill", JsonValue::make_bool(brush.fill));
	put(kBrushErase, "erase", JsonValue::make_bool(brush.erase));
	if (only & kBrushCursor) {
		if (brush.has_cursor) {
			JsonValue at = JsonValue::make_array();
			at.push(io::json_number(brush.cursor[0]));
			at.push(io::json_number(brush.cursor[1]));
			out.set("cursor", std::move(at));
		} else {
			out.set("cursor", JsonValue::make_null());
		}
	}
	return out;
}

bool read_terrain_brush(const JsonValue &json, TerrainBrush &brush, uint32_t &named, bool strokes, std::string &error) {
	if (!json.is_object()) {
		error = "The brush is an object of its members.";
		return false;
	}
	TerrainBrush read = brush;
	uint32_t bits = 0;
	for (const io::JsonMember &member : json.object) {
		const MemberRow *row = nullptr;
		for (const MemberRow &each : kMembers)
			if (member.key == each.token && (each.stroke || !strokes)) row = &each;
		if (!row) {
			std::string taken;
			for (const MemberRow &each : kMembers)
				if (each.stroke || !strokes) taken += (taken.empty() ? "" : ", ") + std::string(each.token);
			error = "The brush has no member \"" + member.key + "\" (it takes " + taken + ").";
			return false;
		}
		const JsonValue &value = member.value;
		const std::string at = std::string("brush.") + row->token;
		switch (row->kind) {
		case MemberKind::Tool: {
			if (!value.is_string() || !terrain_tool_from_token(value.string, read.tool)) {
				error = at + " is one of " + tool_tokens() + ".";
				return false;
			}
			break;
		}
		case MemberKind::Number: {
			float f = 0.0f;
			if (!io::json_float(value, f) || !std::isfinite(f) || f < row->least || f > row->most) {
				error = at + " is a number, " + bounds_words(row->least, row->most) + ".";
				return false;
			}
			float *target = row->bit == kBrushRadius     ? &read.radius
			                : row->bit == kBrushStrength ? &read.strength
			                : row->bit == kBrushFalloff  ? &read.falloff
			                : row->bit == kBrushSpacing  ? &read.spacing
			                                             : &read.height;
			*target = f;
			break;
		}
		case MemberKind::Whole: {
			int64_t whole = 0;
			if (!io::json_exact_whole_in(value, row->least, row->most, whole)) {
				error = at + " is a whole number, " + bounds_words(row->least, row->most) + ".";
				return false;
			}
			int *target = row->bit == kBrushSurface   ? &read.surface
			              : row->bit == kBrushFoliage ? &read.foliage
			              : row->bit == kBrushLayer   ? &read.layer
			              : row->bit == kBrushCell    ? &read.cell
			                                          : &read.rotation;
			*target = int(whole);
			break;
		}
		case MemberKind::Colour:
			if (!read_colour(value, read.colour)) {
				error = at + " is a colour \"#RRGGBB\".";
				return false;
			}
			break;
		case MemberKind::Flag:
			if (!value.is_bool()) {
				error = at + " is true or false.";
				return false;
			}
			(row->bit == kBrushFlipH   ? read.flip_h
			 : row->bit == kBrushFill  ? read.fill
			 : row->bit == kBrushErase ? read.erase
			                           : read.flip_v) = value.boolean;
			break;
		case MemberKind::Cursor: {
			if (value.is_null()) {
				read.has_cursor = false;
				break;
			}
			double xy[2] = {0.0, 0.0};
			bool numbers = value.is_array() && value.array.size() == 2;
			for (size_t i = 0; numbers && i < 2; ++i) {
				numbers = value.array[i].is_number() && std::isfinite(value.array[i].number);
				xy[i] = value.array[i].number;
			}
			if (!numbers) {
				error = at + " is [x, y] in mission metres, or null for none.";
				return false;
			}
			read.has_cursor = true;
			read.cursor[0] = xy[0];
			read.cursor[1] = xy[1];
			break;
		}
		}
		bits |= row->bit;
	}
	brush = read;
	named |= bits;
	return true;
}

TerrainBrush terrain_stroke_brush(const TerrainBrush &workspace, const TerrainStrokeAsk &ask) {
	TerrainBrush out = workspace;
	const uint32_t n = ask.brush_named;
	const TerrainBrush &b = ask.brush;
	if (n & kBrushTool) out.tool = b.tool;
	if (n & kBrushRadius) out.radius = b.radius;
	if (n & kBrushStrength) out.strength = b.strength;
	if (n & kBrushFalloff) out.falloff = b.falloff;
	if (n & kBrushSpacing) out.spacing = b.spacing;
	if (n & kBrushHeight) out.height = b.height;
	if (n & kBrushSurface) out.surface = b.surface;
	if (n & kBrushFoliage) out.foliage = b.foliage;
	if (n & kBrushColour) out.colour = b.colour;
	if (n & kBrushLayer) out.layer = b.layer;
	if (n & kBrushFill) out.fill = b.fill;
	if (n & kBrushErase) out.erase = b.erase;
	return out;
}

} // namespace opennova::editor
