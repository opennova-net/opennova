#include "mnu/end_round_screen.h"

#include "mnu/mnu_document.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/end_round_state.h"
#include "simulation/hud_view_records.h"
#include "simulation/simulation.h"

#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

// The screen's host over the Simulation's end-of-round feed (simulation_end_round.cpp) and its
// two requests as the binding's signals.
class EndRoundScreen::Host final : public opennova::inmatch::EndRoundScreenHost {
	EndRoundScreen *owner_;

	Simulation *sim() const { return owner_->sim_(); }

public:
	explicit Host(EndRoundScreen *p_owner) : owner_(p_owner) {}

	bool team_mode() override {
		Simulation *s = sim();
		if (s == nullptr) return false;
		const Ref<EndRoundState> state = s->get_end_round_state();
		return state.is_valid() && state->is_team_mode();
	}
	std::vector<opennova::inmatch::StatScreenColumn> columns(int p_table_width) override {
		std::vector<opennova::inmatch::StatScreenColumn> out;
		Simulation *s = sim();
		if (s == nullptr) return out;
		const TypedArray<EndRoundColumn> columns = s->get_end_round_columns(p_table_width, owner_->gametext_);
		out.reserve(static_cast<size_t>(columns.size()));
		for (int64_t i = 0; i < columns.size(); ++i) {
			const Ref<EndRoundColumn> column = columns[i];
			if (column.is_valid()) out.push_back(column->value());
		}
		return out;
	}
	std::vector<opennova::inmatch::StatScreenRow> rows() override {
		std::vector<opennova::inmatch::StatScreenRow> out;
		Simulation *s = sim();
		if (s == nullptr) return out;
		const TypedArray<EndRoundRow> rows = s->get_end_round_rows();
		out.reserve(static_cast<size_t>(rows.size()));
		for (int64_t i = 0; i < rows.size(); ++i) {
			const Ref<EndRoundRow> row = rows[i];
			if (row.is_valid()) out.push_back(row->value());
		}
		return out;
	}
	int local_slot() override {
		Simulation *s = sim();
		if (s == nullptr) return -1;
		const Ref<EndRoundState> state = s->get_end_round_state();
		return state.is_valid() ? state->get_local_slot() : -1;
	}
	void close_screen() override { owner_->emit_signal("close_requested"); }
	void exit_to_menu() override { owner_->emit_signal("exit_to_menu_requested"); }
};

EndRoundScreen::EndRoundScreen() : host_(std::make_unique<Host>(this)) {
	screen_.set_host(host_.get());
}

EndRoundScreen::~EndRoundScreen() {
	screen_.set_host(nullptr);
}

Simulation *EndRoundScreen::sim_() const {
	return sim_id_.is_null() ? nullptr : Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
}

void EndRoundScreen::set_simulation(const Ref<Simulation> &p_sim) {
	sim_id_ = p_sim.is_valid() ? ObjectID(p_sim->get_instance_id()) : ObjectID();
}

void EndRoundScreen::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_simulation", "sim"), &EndRoundScreen::set_simulation);
	ClassDB::bind_method(D_METHOD("set_gametext", "gametext"), &EndRoundScreen::set_gametext);
	ClassDB::bind_method(D_METHOD("get_tab"), &EndRoundScreen::get_tab);
	ADD_SIGNAL(MethodInfo("close_requested"));
	ADD_SIGNAL(MethodInfo("exit_to_menu_requested"));
}

} // namespace godot
