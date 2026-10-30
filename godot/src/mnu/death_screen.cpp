#include "mnu/death_screen.h"

#include "mnu/mnu_document.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/deploy_rows.h"
#include "simulation/hud_view_records.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <base/io/cp1252.h>

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

// The screen's host over the Simulation's deploy feed (simulation_deploy_screen.cpp): the reads
// and sends the engine controller asks for, and its two requests as the binding's signals.
class DeathScreen::Host final : public opennova::menu::DeathScreenHost {
	DeathScreen *owner_;

	Simulation *sim() const { return owner_->sim_(); }

public:
	explicit Host(DeathScreen *p_owner) : owner_(p_owner) {}

	bool held() override {
		Simulation *s = sim();
		return s != nullptr && s->is_death_menu_held();
	}
	bool pick_pending() override {
		Simulation *s = sim();
		return s != nullptr && s->is_join_deploy_pick_pending();
	}
	bool local_player_dead() override {
		Simulation *s = sim();
		return s != nullptr && s->local_player_dead();
	}
	bool session_lost() override {
		Simulation *s = sim();
		return s != nullptr && s->is_session_lost();
	}
	bool shroud_revealed() override {
		Simulation *s = sim();
		return s != nullptr && s->is_death_shroud_revealed();
	}
	int64_t logic_tick() override {
		Simulation *s = sim();
		return s != nullptr ? s->get_logic_tick() : 0;
	}
	opennova::world::DeployScreenStatus status() override {
		Simulation *s = sim();
		if (s == nullptr) return opennova::world::DeployScreenStatus{};
		const Ref<DeployStatus> status = s->get_deploy_status(owner_->gametext_, owner_->medic_key_label_);
		return status.is_valid() ? status->value() : opennova::world::DeployScreenStatus{};
	}
	std::vector<opennova::world::DeployListRow> list_rows(const std::string &p_default_key,
			const std::string &p_default_home,
			const std::function<std::string(const std::string &)> &p_zone_name) override {
		std::vector<opennova::world::DeployListRow> out;
		Simulation *s = sim();
		if (s == nullptr) return out;
		// The texts are the tables' bytes; the feed's builder takes UTF-8 and encodes it back
		// (world::build_deploy_rows, D-MNU-46).
		Dictionary names;
		const TypedArray<DeployZoneRow> zones = s->get_deploy_spawn_zones();
		for (int64_t i = 0; i < zones.size(); ++i) {
			const Ref<DeployZoneRow> zone = zones[i];
			if (zone.is_null()) continue;
			const std::string &key = zone->value().name_key;
			names[opennova::to_gd(key)] =
					opennova::to_gd(opennova::retail_text_to_utf8(p_zone_name(key)));
		}
		const TypedArray<DeployListRow> rows = s->get_deploy_list_rows(
				opennova::to_gd(opennova::retail_text_to_utf8(p_default_key)),
				opennova::to_gd(opennova::retail_text_to_utf8(p_default_home)), names);
		out.reserve(static_cast<size_t>(rows.size()));
		for (int64_t i = 0; i < rows.size(); ++i) {
			const Ref<DeployListRow> row = rows[i];
			if (row.is_valid()) out.push_back(row->value());
		}
		return out;
	}
	bool send_pick(int p_param) override {
		Simulation *s = sim();
		return s != nullptr && s->send_deployment_pick(p_param);
	}
	void send_team_change() override {
		if (Simulation *s = sim()) s->send_team_change_request();
	}
	void close_screen() override { owner_->emit_signal("close_requested"); }
	void exit_confirmed() override { owner_->emit_signal("exit_confirmed"); }
};

DeathScreen::DeathScreen() : host_(std::make_unique<Host>(this)) {
	screen_.set_host(host_.get());
}

DeathScreen::~DeathScreen() {
	screen_.set_host(nullptr);
}

Simulation *DeathScreen::sim_() const {
	return sim_id_.is_null() ? nullptr : Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
}

void DeathScreen::set_simulation(const Ref<Simulation> &p_sim) {
	sim_id_ = p_sim.is_valid() ? ObjectID(p_sim->get_instance_id()) : ObjectID();
}

void DeathScreen::set_text_tables(const Ref<RtxtStringFile> &p_gametext,
		const Ref<RtxtStringFile> &p_menutxt, const Ref<RtxtStringFile> &p_gameui) {
	gametext_ = p_gametext;
	screen_.set_text(game_text_lookup(p_gametext), game_text_lookup(p_menutxt),
			game_text_lookup(p_gameui));
}

void DeathScreen::set_medic_key_label(const String &p_label) {
	medic_key_label_ = p_label;
}

bool DeathScreen::can_open() const {
	return screen_.can_open();
}

int DeathScreen::frame(const Ref<MenuDriver> &p_driver, bool p_scene_drawn) {
	if (p_driver.is_null()) return FRAME_CLOSE;
	return static_cast<int>(screen_.frame(p_driver->runtime(), p_scene_drawn));
}

bool DeathScreen::send_pick(int p_param) {
	return screen_.pick(p_param);
}

Array DeathScreen::get_spawn_rows() const {
	Array out;
	for (const opennova::menu::DeathScreen::SpawnRow &row : screen_.rows()) {
		Dictionary entry;
		entry["label"] = opennova::cp1252_to_gd(row.label);
		entry["param"] = row.param;
		out.push_back(entry);
	}
	return out;
}

void DeathScreen::_bind_methods() {
	BIND_ENUM_CONSTANT(FRAME_KEEP);
	BIND_ENUM_CONSTANT(FRAME_CLOSE);
	BIND_ENUM_CONSTANT(FRAME_TEARDOWN);
	ClassDB::bind_method(D_METHOD("set_simulation", "sim"), &DeathScreen::set_simulation);
	ClassDB::bind_method(D_METHOD("set_text_tables", "gametext", "menutxt", "gameui"),
			&DeathScreen::set_text_tables);
	ClassDB::bind_method(D_METHOD("set_medic_key_label", "label"), &DeathScreen::set_medic_key_label);
	ClassDB::bind_method(D_METHOD("can_open"), &DeathScreen::can_open);
	ClassDB::bind_method(D_METHOD("frame", "driver", "scene_drawn"), &DeathScreen::frame);
	ClassDB::bind_method(D_METHOD("send_pick", "param"), &DeathScreen::send_pick);
	ClassDB::bind_method(D_METHOD("is_input_pass_through"), &DeathScreen::is_input_pass_through);
	ClassDB::bind_method(D_METHOD("get_spawn_rows"), &DeathScreen::get_spawn_rows);
	ClassDB::bind_method(D_METHOD("reset"), &DeathScreen::reset);
	ADD_SIGNAL(MethodInfo("close_requested"));
	ADD_SIGNAL(MethodInfo("exit_confirmed"));
}

} // namespace godot
