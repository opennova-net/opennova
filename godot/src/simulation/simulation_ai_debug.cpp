// Simulation — the AI debug binding legs (ADR 0042 d5/d6): the native join
// push for the F3 AI window (DevTools) and the bound per-frame overlay payload
// for the Godot AI debug view (godot/game/debug/ai_debug_view.gd). Both read
// the ONE engine join, world::inspect::ai_debug_report.
#include "simulation/simulation_internal.h"

#include <runtime/world/angle.h>

#include <cmath>

using namespace sim_internal;

bool Simulation::native_ai_debug(
		opennova::world::inspect::AiDebugReport &r_out) const {
	// The joiner's tooling AI pool is non-authoritative and never joins the
	// decoded view (the directory's rule), so a joiner reports nothing.
	if (!kernel_ || joiner_) return false;
	r_out = opennova::world::inspect::ai_debug_report(kernel_->world, kernel_->ai);
	return true;
}

Dictionary Simulation::get_ai_debug() const {
	Dictionary out;
	out["valid"] = false;
	opennova::world::inspect::AiDebugReport report;
	if (!native_ai_debug(report)) return out;
	out["valid"] = true;
	out["logic_tick"] = get_logic_tick();

	// A brain's aim solution as a Godot-space unit direction, so the view does
	// no BAM math. The mission-frame vector is (cos yaw, sin yaw, sin pitch) —
	// the same bearing math the round spawner's witnessed direction leg uses
	// (engine/runtime/world/round_sim.cpp, the wire-fire-direction block);
	// mission -> Godot is (x, z, -y).
	constexpr double rad_per_bam =
			2.0 * 3.14159265358979323846 / opennova::world::kBamFullTurn;
	Array rows;
	for (const opennova::world::inspect::AiOverlayRow &r : report.rows) {
		Dictionary d;
		d["ai_index"] = r.ai_index;
		d["handle"] = static_cast<int>(r.handle);
		d["name"] = String::utf8(r.name.c_str());
		d["group"] = r.group_id;
		d["alive"] = r.alive;
		d["infantry"] = r.infantry;
		d["pos"] = godot_from_fixed3(r.pos);
		d["state"] = r.state;
		d["state_name"] = String::utf8(r.state_name.c_str());
		d["alert"] = r.alert;
		d["move_mode"] = r.move_mode;
		d["out_speed"] = r.out_speed;
		d["wp_channel"] = r.wp_channel;
		d["wp_node"] = r.wp_node;
		d["target_valid"] = r.target_valid;
		d["target_handle"] = static_cast<int>(r.target_handle);
		d["target_pos"] = godot_from_fixed3(r.target_pos);
		d["target_name"] = String::utf8(r.target_name.c_str());
		d["aim_valid"] = r.aim_valid;
		if (r.aim_valid) {
			const double bearing = static_cast<double>(r.aim_heading) * rad_per_bam;
			const double pitch = static_cast<double>(r.aim_pitch) * rad_per_bam;
			const double cp = std::cos(pitch);
			const Vector3 mission(static_cast<float>(std::cos(bearing) * cp),
					static_cast<float>(std::sin(bearing) * cp),
					static_cast<float>(std::sin(pitch)));
			d["aim_dir"] = Vector3(mission.x, mission.z, -mission.y);
		} else {
			d["aim_dir"] = Vector3();
		}
		d["muzzle_valid"] = r.muzzle_valid;
		d["muzzle"] = godot_from_fixed3(r.muzzle);
		d["sight_range"] = static_cast<float>(r.sight_range_q16 / kFixed16);
		d["attack_range"] = static_cast<float>(r.attack_range_q16 / kFixed16);
		d["combat_timer"] = r.combat_timer;
		d["fire_delay"] = r.fire_delay;
		d["damage_timer"] = r.damage_timer;
		d["combat_move_timer"] = r.combat_move_timer;
		rows.push_back(d);
	}
	out["rows"] = rows;

	Array channels;
	for (const opennova::world::inspect::AiNavChannelRow &ch : report.channels) {
		Dictionary d;
		d["index"] = ch.index;
		// loopflag bit0 set = one-shot (terminate at path end).
		d["once"] = (ch.loopflag & 1) != 0;
		d["followers"] = ch.followers;
		PackedVector3Array nodes;
		PackedFloat32Array radii;
		nodes.resize(static_cast<int64_t>(ch.nodes.size()));
		radii.resize(static_cast<int64_t>(ch.nodes.size()));
		Vector3 *nw = nodes.ptrw();
		float *rw = radii.ptrw();
		for (size_t k = 0; k < ch.nodes.size(); ++k) {
			nw[k] = godot_from_fixed3(ch.nodes[k].pos);
			rw[k] = static_cast<float>(ch.nodes[k].radius_q16 / kFixed16);
		}
		d["nodes"] = nodes;
		d["radii"] = radii;
		channels.push_back(d);
	}
	out["channels"] = channels;

	Array groups;
	for (const opennova::world::inspect::AiGroupRow &g : report.groups) {
		Dictionary d;
		d["id"] = g.id;
		d["alert"] = g.alert;
		d["initial_count"] = g.initial_count;
		d["live_count"] = g.live_count;
		groups.push_back(d);
	}
	out["groups"] = groups;

	Dictionary counters;
	counters["brain_count"] = report.counters.brain_count;
	counters["scheduler_budget"] = report.counters.scheduler_budget;
	counters["event_count"] = report.counters.event_count;
	counters["unported_calls"] = report.counters.unported_calls;
	counters["rel_ops"] = report.counters.rel_ops;
	counters["find_target_calls"] = report.counters.find_target_calls;
	out["counters"] = counters;
	return out;
}
