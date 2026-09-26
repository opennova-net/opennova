// DevTools — the per-domain windows' records (Script, Player, Render,
// Particles, Audio, Net): each pushed by value on its window's cadence while
// the window shows, from the ONE engine function per fact (the Simulation's
// native reads) or, for the facts that exist only because Godot does (the
// camera, the RenderingServer counters, the audio buses), sampled here and
// converted into the window's plain record.
#include "devtools/dev_tools.h"

#if OPENNOVA_DEVTOOLS

#include "audio/mission_audio.h"
#include "audio/mission_audio_records.h"
#include "lights/effect_light_report.h"
#include "particle/effect_world.h"
#include "simulation/simulation.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/terrain.h"
#include "util/axes.h"
#include "util/string_convert.h"
#include "world/game_world.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/time.hpp>

#include <runtime/devtools/audio_window.h>
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/net_window.h>
#include <runtime/devtools/particles_window.h>
#include <runtime/devtools/player_window.h>
#include <runtime/devtools/render_window.h>
#include <runtime/devtools/script_window.h>
#include <runtime/inmatch/net_debug_report.h>
#include <runtime/mission/script_debug_report.h>
#include <runtime/particle/effect_scene.h>
#include <runtime/world/inspect_local_player.h>

#include <cmath>

namespace godot {

// The loaded world behind the debug-control table's shell seam (the table
// resolves it live per call; so does this).
GameWorld *DevTools::loaded_world() const {
	if (control_table_.is_null()) return nullptr;
	const Ref<DebugShellHost> host = control_table_->get_host();
	if (host.is_null()) return nullptr;
	GameWorld *world = host->world();
	return world != nullptr && world->is_loaded() ? world : nullptr;
}

void DevTools::push_domain_records() {
	push_script_snapshot();
	push_player_snapshot();
	push_render_snapshot();
	push_particle_snapshot();
	push_audio_snapshot();
	push_net_snapshot();
}

void DevTools::clear_domain_records() {
	tools_->set_script_snapshot(opennova::devtools::ScriptSnapshot{});
	tools_->set_player_snapshot(opennova::devtools::PlayerSnapshot{});
	tools_->set_render_snapshot(opennova::devtools::RenderSnapshot{});
	tools_->set_particle_snapshot(opennova::devtools::ParticleSnapshot{});
	tools_->set_net_snapshot(opennova::devtools::NetSnapshot{});
	last_script_push_ms_ = -1;
	last_player_push_ms_ = -1;
	last_render_push_ms_ = -1;
	last_particle_push_ms_ = -1;
	last_net_push_ms_ = -1;
}

void DevTools::push_script_snapshot() {
	Simulation *sim = simulation();
	if (sim == nullptr || !tools_->needs_script_snapshot()) {
		last_script_push_ms_ = -1;
		return;
	}
	if (!push_due(last_script_push_ms_, opennova::devtools::ScriptWindow::kRefreshSeconds)) return;
	opennova::devtools::ScriptSnapshot snapshot;
	snapshot.valid = sim->native_script_report(snapshot.report);
	snapshot.logic_tick = static_cast<uint64_t>(sim->get_logic_tick());
	snapshot.authority = !sim->is_joiner();
	tools_->set_script_snapshot(std::move(snapshot));
}

void DevTools::push_player_snapshot() {
	Simulation *sim = simulation();
	if (sim == nullptr || !tools_->needs_player_snapshot()) {
		last_player_push_ms_ = -1;
		return;
	}
	if (!push_due(last_player_push_ms_, opennova::devtools::PlayerWindow::kRefreshSeconds)) return;
	opennova::devtools::PlayerSnapshot snapshot;
	snapshot.valid = true;
	(void)sim->native_local_player_report(snapshot.report);
	snapshot.logic_tick = static_cast<uint64_t>(sim->get_logic_tick());
	snapshot.authority = !sim->is_joiner();
	tools_->set_player_snapshot(std::move(snapshot));
}

void DevTools::push_render_snapshot() {
	if (!tools_->needs_render_snapshot()) {
		last_render_push_ms_ = -1;
		return;
	}
	if (!push_due(last_render_push_ms_, opennova::devtools::RenderWindow::kRefreshSeconds)) return;
	opennova::devtools::RenderSnapshot snapshot;
	GameWorld *world = loaded_world();
	if (world == nullptr || game_viewport_ == nullptr) {
		tools_->set_render_snapshot(snapshot);
		return;
	}
	snapshot.valid = true;
	if (Simulation *sim = simulation()) snapshot.logic_tick = static_cast<uint64_t>(sim->get_logic_tick());

	// The device side: the camera the Game image comes from and the render
	// counters of the viewport that camera draws (the stretched frame's
	// target while it is live: the surface then draws no 3D at all).
	opennova::devtools::RenderDeviceStats &d = snapshot.device;
	const Vector2i size = game_viewport_->get_size();
	d.viewport_width = size.x;
	d.viewport_height = size.y;
	Camera3D *image = image_camera();
	Viewport *frame = image != nullptr && image->get_viewport() != nullptr ? image->get_viewport() : game_viewport_;
	if (frame != game_viewport_) {
		const Vector2 frame_size = frame->get_visible_rect().size;
		d.frame_width = static_cast<int32_t>(frame_size.x);
		d.frame_height = static_cast<int32_t>(frame_size.y);
	}
	if (Camera3D *camera = image) {
		const Transform3D xform = camera->get_camera_transform();
		const opennova::env::Vec3 eye = godot_to_mission(xform.origin);
		const opennova::env::Vec3 forward = godot_to_mission(-xform.basis.get_column(2).normalized());
		d.camera_valid = true;
		d.camera_position[0] = eye.x;
		d.camera_position[1] = eye.y;
		d.camera_position[2] = eye.z;
		// Mission heading: 0 faces north (+y), 90 east (+x).
		d.camera_yaw_deg = static_cast<float>(std::atan2(forward.x, forward.y) * 57.29577951308232);
		d.camera_pitch_deg = static_cast<float>(std::asin(std::fmax(-1.0f, std::fmin(1.0f, forward.z))) *
				57.29577951308232);
		d.fov_deg = static_cast<float>(camera->get_fov());
		d.fov_horizontal = camera->get_keep_aspect_mode() == Camera3D::KEEP_WIDTH;
		d.near_m = static_cast<float>(camera->get_near());
		d.far_m = static_cast<float>(camera->get_far());
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID viewport = frame->get_viewport_rid();
	const auto info = [&](RenderingServer::ViewportRenderInfoType p_type, RenderingServer::ViewportRenderInfo p_info) {
		return rs->viewport_get_render_info(viewport, p_type, p_info);
	};
	d.visible_objects = info(RenderingServer::VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
			RenderingServer::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME);
	d.visible_primitives = info(RenderingServer::VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
			RenderingServer::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME);
	d.visible_draw_calls = info(RenderingServer::VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
			RenderingServer::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME);
	d.shadow_objects = info(RenderingServer::VIEWPORT_RENDER_INFO_TYPE_SHADOW,
			RenderingServer::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME);
	d.shadow_primitives = info(RenderingServer::VIEWPORT_RENDER_INFO_TYPE_SHADOW,
			RenderingServer::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME);
	d.shadow_draw_calls = info(RenderingServer::VIEWPORT_RENDER_INFO_TYPE_SHADOW,
			RenderingServer::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME);
	d.video_memory = static_cast<int64_t>(rs->get_rendering_info(RenderingServer::RENDERING_INFO_VIDEO_MEM_USED));
	d.texture_memory = static_cast<int64_t>(rs->get_rendering_info(RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED));
	d.buffer_memory = static_cast<int64_t>(rs->get_rendering_info(RenderingServer::RENDERING_INFO_BUFFER_MEM_USED));
	d.adapter = opennova::to_std(rs->get_video_adapter_name());
	Performance *performance = Performance::get_singleton();
	d.node_count = static_cast<int64_t>(performance->get_monitor(Performance::OBJECT_NODE_COUNT));
	d.object_count = static_cast<int64_t>(performance->get_monitor(Performance::OBJECT_COUNT));

	// The engine frames the world's nodes compiled this frame.
	if (Terrain *terrain = world->get_terrain_node(); terrain != nullptr && terrain->has_frame_draw_list()) {
		snapshot.terrain_valid = true;
		snapshot.terrain = terrain->get_frame_debug_counters_native();
	}
	if (FoliageDispatcher *foliage = world->get_foliage_dispatcher()) {
		snapshot.foliage_valid = true;
		snapshot.foliage = foliage->get_frame_debug_counters_native();
	}
	const Ref<EffectLightReport> lights = world->get_effect_light_report();
	if (lights.is_valid()) {
		snapshot.lights_valid = true;
		snapshot.lights_live = lights->get_live();
		snapshot.lights_high_water = lights->get_high_water();
		snapshot.lights_last_query = lights->get_last_query();
	}
	tools_->set_render_snapshot(snapshot);
}

void DevTools::push_particle_snapshot() {
	if (!tools_->needs_particle_snapshot()) {
		last_particle_push_ms_ = -1;
		return;
	}
	if (!push_due(last_particle_push_ms_, opennova::devtools::ParticlesWindow::kRefreshSeconds)) return;
	opennova::devtools::ParticleSnapshot snapshot;
	GameWorld *world = loaded_world();
	EffectWorld *effects = world != nullptr ? world->get_effect_world() : nullptr;
	const std::shared_ptr<opennova::particle::EffectScene> scene =
			effects != nullptr ? effects->shared_native_scene() : nullptr;
	if (scene != nullptr) {
		snapshot.valid = true;
		// No per-particle bounds: the UI hot path (effect_scene.h).
		snapshot.scene = scene->inspect(false);
		snapshot.active_entries = effects->active_entry_count();
		if (Simulation *sim = simulation()) snapshot.logic_tick = static_cast<uint64_t>(sim->get_logic_tick());
	}
	tools_->set_particle_snapshot(std::move(snapshot));
}

void DevTools::push_audio_snapshot() {
	if (!tools_->needs_audio_snapshot()) {
		last_audio_push_ms_ = -1;
		return;
	}
	if (!push_due(last_audio_push_ms_, opennova::devtools::AudioWindow::kRefreshSeconds)) return;
	opennova::devtools::AudioSnapshot snapshot;
	AudioServer *audio = AudioServer::get_singleton();
	snapshot.valid = audio != nullptr;
	if (audio != nullptr) {
		for (int bus = 0; bus < audio->get_bus_count(); ++bus) {
			opennova::devtools::AudioBusRow row;
			row.name = opennova::to_std(audio->get_bus_name(bus));
			row.volume_db = audio->get_bus_volume_db(bus);
			row.mute = audio->is_bus_mute(bus);
			row.solo = audio->is_bus_solo(bus);
			row.bypass = audio->is_bus_bypassing_effects(bus);
			if (audio->get_bus_channels(bus) > 0) {
				row.peak_left_db = audio->get_bus_peak_volume_left_db(bus, 0);
				row.peak_right_db = audio->get_bus_peak_volume_right_db(bus, 0);
			}
			snapshot.buses.push_back(std::move(row));
		}
	}
	GameWorld *world = loaded_world();
	MissionAudio *mission = world != nullptr ? world->get_mission_audio() : nullptr;
	if (mission != nullptr) {
		const Ref<MissionAudioStats> stats = mission->get_stats();
		const Ref<MissionAudioPerf> perf = mission->get_perf_counters();
		if (stats.is_valid() && perf.is_valid()) {
			snapshot.mission_valid = true;
			snapshot.markers_total = stats->get_markers_total();
			snapshot.markers_resolved = stats->get_markers_resolved();
			snapshot.banks_loaded = stats->get_banks_loaded();
			snapshot.ambient_candidates = stats->get_ambient_candidates();
			snapshot.ambient_candidates_validated = stats->get_ambient_candidates_validated();
			snapshot.ambient_decode_failures = stats->get_ambient_decode_failures();
			snapshot.channel_budget = stats->get_channel_budget();
			snapshot.dialogs = stats->get_dialogs();
			snapshot.tick_us = perf->get_tick_us();
			snapshot.markers = perf->get_markers();
			snapshot.voice_writes = perf->get_voice_writes();
			snapshot.physical_channels = perf->get_physical_channels();
			snapshot.active_channels = perf->get_active_channels();
		}
	}
	tools_->set_audio_snapshot(std::move(snapshot));
}

void DevTools::push_net_snapshot() {
	Simulation *sim = simulation();
	if (sim == nullptr || !tools_->needs_net_snapshot()) {
		last_net_push_ms_ = -1;
		return;
	}
	if (!push_due(last_net_push_ms_, opennova::devtools::NetWindow::kRefreshSeconds)) return;
	opennova::inmatch::NetDebugReport report;
	sim->native_net_report(report);
	opennova::devtools::NetSnapshot snapshot;
	snapshot.valid = true;
	snapshot.logic_tick = static_cast<uint64_t>(sim->get_logic_tick());
	snapshot.wall_seconds = static_cast<double>(Time::get_singleton()->get_ticks_usec()) / 1.0e6;
	// The roles and states mirror the session's by value (dev_tools.cpp
	// static_asserts the pairing).
	snapshot.role = static_cast<opennova::devtools::StatusRole>(report.role);
	snapshot.state = static_cast<opennova::devtools::StatusState>(report.state);
	snapshot.frame_tick_us = report.last_perf.tick_us;
	snapshot.frame_ticks = report.last_perf.ticks;
	snapshot.fps = Engine::get_singleton()->get_frames_per_second();
	const auto traffic = [](const opennova::DatagramTraffic &t) {
		opennova::devtools::NetTraffic out;
		out.tx_packets = t.tx_packets;
		out.tx_bytes = t.tx_bytes;
		out.rx_packets = t.rx_packets;
		out.rx_bytes = t.rx_bytes;
		return out;
	};
	snapshot.traffic_valid = report.traffic_valid;
	snapshot.traffic = traffic(report.traffic);
	for (const opennova::inmatch::NetPeerRow &p : report.peers) {
		opennova::devtools::NetPeerSnapshotRow row;
		row.slot = p.slot;
		row.name = p.name;
		row.address = p.address;
		row.phase = opennova::inmatch::connection_phase_name(p.phase);
		row.rtt_ms = p.rtt_ms;
		row.rtt_average_ms = p.rtt_average_ms;
		row.session_ping_ms = p.session_ping_ms;
		row.quality = p.quality;
		row.min_ping_strikes = p.min_ping_strikes;
		row.max_ping_strikes = p.max_ping_strikes;
		row.send_holdoff_ticks = p.send_holdoff_ticks;
		row.send_holdoff_countdown = p.send_holdoff_countdown;
		row.receive_inactive_ms = p.receive_inactive_ms;
		row.traffic_valid = p.traffic_valid;
		row.traffic = traffic(p.traffic);
		snapshot.peers.push_back(std::move(row));
	}
	const opennova::inmatch::JoinerNetworkDiagnostics &j = report.joiner;
	opennova::devtools::NetJoinerSnapshot &out = snapshot.joiner;
	out.present = j.present;
	out.stage = j.stage;
	out.in_match = j.in_match;
	out.deployed = j.deployed;
	out.frontier_seq = j.frontier_seq;
	out.outbound_seq = j.outbound_seq;
	out.records_applied = j.records_applied;
	out.gap_depth = j.gap_depth;
	out.retained_outbound = j.retained_outbound;
	out.flat_seconds = j.flat_seconds;
	out.freeze_suspected = j.freeze_suspected;
	out.ping_ms = j.ping_ms;
	out.average_ping_ms = j.average_ping_ms;
	out.session_ping_ms = j.session_ping_ms;
	out.quality = j.quality;
	out.send_holdoff_ticks = j.send_holdoff_ticks;
	out.send_holdoff_countdown = j.send_holdoff_countdown;
	out.entity_checksum_seen = j.entity_checksum_seen;
	out.entity_checksum_answered = j.entity_checksum_answered;
	out.loadout_crc_seen = j.loadout_crc_seen;
	out.loadout_crc_answered = j.loadout_crc_answered;
	out.charattr_seen = j.charattr_seen;
	out.charattr_row_missing = j.charattr_row_missing;
	out.property_clears = j.property_clears;
	if (j.reject_set) {
		out.last_reject = "jfc " + std::to_string(j.reject_jfc) + " jfp " + std::to_string(j.reject_jfp) + ": " +
				j.reject_jfs;
	}
	if (j.disconnect_set) {
		out.last_disconnect = "dc " + std::to_string(j.disconnect_dc) + " dpc " +
				std::to_string(j.disconnect_dpc) + ": " + j.disconnect_ddstr + " " + j.disconnect_dstr;
	}
	tools_->set_net_snapshot(std::move(snapshot));
}

} // namespace godot

#endif
