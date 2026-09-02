// Simulation — the AI debug binding legs (ADR 0042 d5/d6): the native join
// push for the F3 AI window (DevTools) and the bound per-frame overlay payload
// for the Godot AI debug view (godot/game/debug/ai_debug_view.gd). Both read
// the ONE engine join, world::inspect::ai_debug_report.
#include "simulation/simulation_internal.h"
#include "simulation/ai_debug_report.h"
#include "env/env_axes.h"

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

Ref<AiDebugReport> Simulation::get_ai_debug() const {
	Ref<AiDebugReport> out;
	out.instantiate();
	opennova::world::inspect::AiDebugReport report;
	if (!native_ai_debug(report)) return out;
	out->set_valid(true);
	out->set_logic_tick(get_logic_tick());

	// A brain's aim solution as a Godot-space unit direction, so the view does
	// no BAM math. The mission-frame vector is (cos yaw, sin yaw, sin pitch) —
	// the same bearing math the round spawner's witnessed direction leg uses
	// (engine/runtime/world/round_sim.cpp, the wire-fire-direction block);
	// mission -> Godot is (x, z, -y).
	constexpr double rad_per_bam =
			2.0 * 3.14159265358979323846 / opennova::world::kBamFullTurn;
	for (const opennova::world::inspect::AiOverlayRow &r : report.rows) {
		Ref<AiDebugRow> d;
		d.instantiate();
		d->set_ai_index(r.ai_index);
		d->set_handle(static_cast<int>(r.handle));
		d->set_name(String::utf8(r.name.c_str()));
		d->set_group(r.group_id);
		d->set_alive(r.alive);
		d->set_infantry(r.infantry);
		d->set_pos(godot_from_fixed3(r.pos));
		d->set_state(r.state);
		d->set_state_name(String::utf8(r.state_name.c_str()));
		d->set_alert(r.alert);
		d->set_move_mode(r.move_mode);
		d->set_out_speed(r.out_speed);
		d->set_wp_channel(r.wp_channel);
		d->set_wp_node(r.wp_node);
		d->set_target_valid(r.target_valid);
		d->set_target_handle(static_cast<int>(r.target_handle));
		d->set_target_pos(godot_from_fixed3(r.target_pos));
		d->set_target_name(String::utf8(r.target_name.c_str()));
		d->set_aim_valid(r.aim_valid);
		if (r.aim_valid) {
			const double bearing = static_cast<double>(r.aim_heading) * rad_per_bam;
			const double pitch = static_cast<double>(r.aim_pitch) * rad_per_bam;
			const double cp = std::cos(pitch);
			const Vector3 mission(static_cast<float>(std::cos(bearing) * cp),
					static_cast<float>(std::sin(bearing) * cp),
					static_cast<float>(std::sin(pitch)));
			d->set_aim_dir(mission_to_godot(mission));
		}
		d->set_muzzle_valid(r.muzzle_valid);
		d->set_muzzle(godot_from_fixed3(r.muzzle));
		d->set_sight_range(static_cast<float>(r.sight_range_q16 / kFixed16));
		d->set_attack_range(static_cast<float>(r.attack_range_q16 / kFixed16));
		d->set_combat_timer(r.combat_timer);
		d->set_fire_delay(r.fire_delay);
		d->set_damage_timer(r.damage_timer);
		d->set_combat_move_timer(r.combat_move_timer);
		out->add_row(d);
	}

	for (const opennova::world::inspect::AiNavChannelRow &ch : report.channels) {
		Ref<AiDebugChannel> d;
		d.instantiate();
		d->set_index(ch.index);
		// loopflag bit0 set = one-shot (terminate at path end).
		d->set_once((ch.loopflag & 1) != 0);
		d->set_followers(ch.followers);
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
		d->set_nodes(nodes);
		d->set_radii(radii);
		out->add_channel(d);
	}

	for (const opennova::world::inspect::AiGroupRow &g : report.groups) {
		out->add_group(AiDebugGroup::make(g.id, g.alert, g.initial_count, g.live_count));
	}

	out->set_brain_count(report.counters.brain_count);
	out->set_scheduler_budget(report.counters.scheduler_budget);
	out->set_event_count(report.counters.event_count);
	out->set_unported_calls(report.counters.unported_calls);
	out->set_rel_ops(report.counters.rel_ops);
	out->set_find_target_calls(report.counters.find_target_calls);
	return out;
}
