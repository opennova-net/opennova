#include <editor/session/terrain_stroke.h>

#include <cmath>

namespace opennova::editor {

using io::JsonValue;

JsonValue terrain_stroke_to_json(const TerrainStrokeAsk &ask) {
	JsonValue out = JsonValue::make_object();
	out.set("plane", io::json_string(terrain_plane_token(ask.plane)));
	if (ask.brush_named) out.set("brush", terrain_brush_to_json(ask.brush, ask.brush_named));
	JsonValue points = JsonValue::make_array();
	for (const auto &[x, y] : ask.points) {
		JsonValue point = JsonValue::make_array();
		point.push(io::json_number(x));
		point.push(io::json_number(y));
		points.push(std::move(point));
	}
	out.set("points", std::move(points));
	out.set("gesture", io::json_number(double(ask.gesture)));
	out.set("end", JsonValue::make_bool(ask.end));
	out.set("cancel", JsonValue::make_bool(ask.cancel));
	return out;
}

bool terrain_stroke_from_json(const JsonValue &json, TerrainStrokeAsk &ask, std::string &error) {
	if (!json.is_object()) {
		error = "\"stroke\" is an object {plane, brush?, points, gesture?, end?, cancel?}.";
		return false;
	}
	if (!io::json_members_known(json, {"plane", "brush", "points", "gesture", "end", "cancel"}, "stroke", error))
		return false;
	TerrainStrokeAsk read;
	const JsonValue *plane = json.get("plane");
	if (!plane || !plane->is_string() || !terrain_plane_from_token(plane->string, read.plane)) {
		error = "stroke.plane is heights, surface, foliage, colour or blend.";
		return false;
	}
	if (const JsonValue *brush = json.get("brush"))
		if (!read_terrain_brush(*brush, read.brush, read.brush_named, true, error)) {
			error = "stroke." + error;
			return false;
		}
	if (const JsonValue *gesture = json.get("gesture"))
		if (!io::json_exact_whole(*gesture, read.gesture)) {
			error = "stroke.gesture is a gesture's token (0 begins one).";
			return false;
		}
	for (const char *flag : {"end", "cancel"})
		if (const JsonValue *value = json.get(flag)) {
			if (!value->is_bool()) {
				error = std::string("stroke.") + flag + " is true or false.";
				return false;
			}
			(std::string(flag) == "end" ? read.end : read.cancel) = value->boolean;
		}
	const JsonValue *points = json.get("points");
	if (points && !points->is_array()) {
		error = "stroke.points is [[x, y], ...] in mission metres.";
		return false;
	}
	if (points)
		for (const JsonValue &point : points->array) {
			if (!point.is_array() || point.array.size() != 2 || !point.array[0].is_number() || !point.array[1].is_number() ||
			    !std::isfinite(point.array[0].number) || !std::isfinite(point.array[1].number)) {
				error = "stroke.points is [[x, y], ...] in mission metres.";
				return false;
			}
			read.points.emplace_back(point.array[0].number, point.array[1].number);
		}
	if (read.points.size() > kTerrainStrokePoints) {
		error = "stroke.points holds at most " + std::to_string(kTerrainStrokePoints) + " points (send the rest as the gesture's next sample).";
		return false;
	}
	if (read.points.empty() && !read.cancel) {
		error = "stroke.points names at least one point (a cancel alone names none).";
		return false;
	}
	ask = std::move(read);
	return true;
}

JsonValue terrain_stroke_answer_to_json(const TerrainStrokeAnswer &answer) {
	JsonValue out = JsonValue::make_object();
	JsonValue changed = JsonValue::make_array();
	for (const TerrainStrokeRegion &region : answer.changed) {
		JsonValue item = JsonValue::make_object();
		item.set("path", io::json_string(region.path));
		item.set("plane", io::json_string(terrain_plane_token(region.plane)));
		JsonValue rect = JsonValue::make_array();
		for (const int v : {region.x0, region.y0, region.x1, region.y1}) rect.push(io::json_number(v));
		item.set("region", std::move(rect));
		changed.push(std::move(item));
	}
	out.set("changed", std::move(changed));
	JsonValue followed = JsonValue::make_array();
	for (const TerrainStrokeFollowed &each : answer.followed) {
		JsonValue item = JsonValue::make_object();
		item.set("path", io::json_string(each.path));
		item.set("entities", io::json_number(double(each.entities)));
		followed.push(std::move(item));
	}
	out.set("followed", std::move(followed));
	out.set("opened", io::json_string_array(answer.opened));
	out.set("bake", io::json_string(answer.bake));
	return out;
}

} // namespace opennova::editor
