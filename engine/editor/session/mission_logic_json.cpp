#include <editor/session/mission_logic_json.h>

namespace opennova::editor {

using io::json_number;
using io::json_string;
using io::JsonValue;
using mission::ParamKind;

namespace {

JsonValue boolean(bool value) { return JsonValue::make_bool(value); }

JsonValue param_to_json(const LogicParam &param) {
	JsonValue out = JsonValue::make_object();
	out.set("slot", json_number(double(param.slot)));
	out.set("field", json_string(param.field));
	out.set("kind", json_string(param_kind_token(param.kind)));
	out.set("label", json_string(param.label));
	out.set("value", json_number(double(param.value)));
	out.set("words", json_string(param.words));
	if (*param.unit) out.set("unit", json_string(param.unit));
	if (param.picks) out.set("picks", boolean(true));
	return out;
}

} // namespace

const char *param_kind_token(ParamKind kind) {
	switch (kind) {
	case ParamKind::Unused: return "unused";
	case ParamKind::Group: return "group";
	case ParamKind::Entity: return "entity";
	case ParamKind::Zone: return "zone";
	case ParamKind::Event: return "event";
	case ParamKind::Path: return "path";
	case ParamKind::PathNode: return "path_node";
	case ParamKind::MissionVar: return "variable";
	case ParamKind::Dialog: return "dialog";
	case ParamKind::Count: return "count";
	case ParamKind::Hp: return "hit_points";
	case ParamKind::DistanceM: return "distance_m";
	case ParamKind::Seconds: return "seconds";
	case ParamKind::SpeedKph: return "speed_kph";
	case ParamKind::Bool: return "bool";
	case ParamKind::Team: return "team";
	case ParamKind::SubGoal: return "sub_goal";
	case ParamKind::Bit: return "bit";
	case ParamKind::HudTimer: return "hud_item";
	case ParamKind::LightChannel: return "light_channel";
	case ParamKind::TeleportTarget: return "teleport_target";
	case ParamKind::WpNumber: return "marker_number";
	case ParamKind::Raw: return "number";
	}
	return "number";
}

const char *logic_join_token(LogicJoin join) {
	switch (join) {
	case LogicJoin::Or: return "or";
	case LogicJoin::Xor: return "or_else";
	case LogicJoin::And: break;
	}
	return "and";
}

bool logic_join_from_token(const std::string &token, LogicJoin &out) {
	for (const LogicJoin join : {LogicJoin::And, LogicJoin::Or, LogicJoin::Xor})
		if (token == logic_join_token(join)) {
			out = join;
			return true;
		}
	return false;
}

JsonValue logic_type_to_json(const LogicType &type) {
	JsonValue out = JsonValue::make_object();
	out.set("type", json_number(double(type.type)));
	out.set("sub", json_number(double(type.sub)));
	out.set("title", json_string(type.title));
	out.set("group", json_string(type.group));
	out.set("tip", json_string(type.tip));
	return out;
}

JsonValue logic_types_to_json(bool actions, const JsonPage &page) {
	const std::vector<LogicType> &types = logic_types(actions);
	JsonValue out = JsonValue::make_object();
	out.set("list", json_string(actions ? "action" : "trigger"));
	set_page(out, page, types.size());
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(types.size()); i < page.last(types.size()); ++i) list.push(logic_type_to_json(types[i]));
	out.set("types", std::move(list));
	return out;
}

JsonValue logic_form_to_json(const LogicForm &form) {
	JsonValue out = JsonValue::make_object();
	out.set("record", address_to_json(form.record));
	out.set("event", address_to_json(form.event));
	out.set("action", boolean(form.action));
	out.set("index", json_number(double(form.index)));
	out.set("count", json_number(double(form.count)));
	out.set("type", form.type ? logic_type_to_json(*form.type) : JsonValue::make_null());
	out.set("type_words", json_string(form.type_words));
	if (!form.action) {
		out.set("negated", boolean(form.negated));
		out.set("join", json_string(logic_join_token(form.join)));
		out.set("last", boolean(form.last));
	}
	JsonValue params = JsonValue::make_array();
	for (const LogicParam &param : form.params) params.push(param_to_json(param));
	out.set("params", std::move(params));
	JsonValue unread = JsonValue::make_array();
	for (const LogicParam &param : form.unread) unread.push(param_to_json(param));
	out.set("unread", std::move(unread));
	out.set("words", json_string(form.words));
	out.set("sentence", json_string(form.sentence));
	return out;
}

JsonValue logic_event_form_to_json(const LogicEventForm &form) {
	JsonValue out = JsonValue::make_object();
	out.set("event", address_to_json(form.event));
	out.set("index", json_number(double(form.index)));
	out.set("when", json_string(form.words.when));
	out.set("then", json_string(form.words.then));
	out.set("delay", json_string(form.words.delay));
	out.set("repeat", json_string(form.words.repeat));
	out.set("sentence", json_string(form.words.sentence));
	out.set("repeats", boolean(form.repeats));
	out.set("at_start", boolean(form.at_start));
	out.set("at_end", boolean(form.at_end));
	out.set("delay_steps", json_number(double(form.delay)));
	out.set("repeat_steps", json_number(double(form.repeat)));
	out.set("delay_seconds", json_number(logic_units_seconds(form.delay)));
	out.set("repeat_seconds", json_number(logic_units_seconds(form.repeat)));
	out.set("most_steps", json_number(double(form.most_steps)));
	out.set("triggers", json_number(double(form.triggers)));
	out.set("actions", json_number(double(form.actions)));
	out.set("most", json_number(double(form.most)));
	if (!form.trigger_refusal.empty()) out.set("trigger_refusal", json_string(form.trigger_refusal));
	if (!form.action_refusal.empty()) out.set("action_refusal", json_string(form.action_refusal));
	return out;
}

JsonValue mission_uses_to_json(const MissionUses &uses, const JsonPage &page) {
	JsonValue out = JsonValue::make_object();
	out.set("what", json_string(uses.what));
	if (!uses.inert.empty()) out.set("inert", json_string(uses.inert));
	set_page(out, page, uses.uses.size());
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(uses.uses.size()); i < page.last(uses.uses.size()); ++i) {
		const MissionUse &use = uses.uses[i];
		JsonValue entry = JsonValue::make_object();
		entry.set("event", address_to_json(use.event));
		entry.set("event_index", json_number(double(use.event_index)));
		entry.set("record", address_to_json(use.record));
		entry.set("action", boolean(use.action));
		entry.set("slot", json_number(double(use.slot)));
		entry.set("field", json_string(use.field));
		entry.set("words", json_string(use.words));
		entry.set("sentence", json_string(use.sentence));
		list.push(std::move(entry));
	}
	out.set("uses", std::move(list));
	return out;
}

} // namespace opennova::editor
