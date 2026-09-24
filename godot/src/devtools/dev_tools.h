#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "devtools/debug_control_table.h"
#include "devtools/frame_stats.h"

#if OPENNOVA_DEVTOOLS
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/game_window.h>
#include <runtime/devtools/weapon_action_snapshot.h>

#include <memory>
#include <string>
#include <vector>
#endif

namespace godot {

class Camera3D;
class GameWorld;

class Simulation;
class SubViewport;

// The game's dev tools (ADR 0039): the ImGui workspace behind F3 with the
// embedded Game surface and Stats window. The engine owns the windows
// (engine/runtime/devtools/game_dev_tools.h); this node is their Godot seam —
// context/frame hand-off, SubViewport rendering, and typed input-mode requests.
//
// Open and Play/Interact state work whether or not an ImGui context was
// attached, so headless lifecycle tests can pin the policy; attachment only
// governs drawing.
//
// Release flavour (OPENNOVA_DEVTOOLS=0): the class still registers so scripts
// keep parsing, but window controls are inert and is_available() is false.
// The shared debug-control table and engine log remain available; the game's
// release export strips the addon.
class DevTools : public Node
#if OPENNOVA_DEVTOOLS
		, private opennova::devtools::GameViewport
#endif
{
	GDCLASS(DevTools, Node)

public:
	DevTools();
	~DevTools() override;

	void _ready() override;
	void _exit_tree() override;
	void _process(double p_delta) override;

	bool is_available() const;
	// Undocked tool windows are disabled before entering fullscreen, where
	// imgui-godot cannot present them alongside the main viewport.
	void set_platform_windows_allowed(bool p_allowed);
	bool are_platform_windows_allowed() const { return platform_windows_allowed_; }

	bool is_open() const;
	void set_open(bool p_open);
	void toggle() { set_open(!is_open()); }

	void set_frame_stats(const Ref<FrameStats> &p_stats);
	Ref<FrameStats> get_frame_stats() const { return frame_stats_; }

	// The Simulation the engine-fact windows read through (ADR 0042 d6): a
	// raw pointer the shell sets on world load and nulls on world unload (and
	// _exit_tree nulls) — the stats-board pattern. The per-frame leg pushes
	// the entity-directory record (built by the ENGINE join,
	// world::inspect::entity_directory, at the window's 0.5 s cadence and
	// only while the window shows).
	void set_simulation(const Ref<Simulation> &p_simulation);

	// The debug-control table the windows' control requests drain into (ADR
	// 0043 d12): the SAME instance MCP's game_debug drives, built by the
	// shell's debug adapter and lent here for the shell's lifetime. Every
	// window mutation — the spectator toggle, the entity edits, the weather
	// strip — lands in DebugControlTable::invoke with F3's local authority,
	// so the table's argument schema and its session-role gate decide once
	// for both surfaces; requests queued with no table drain and drop.
	void set_debug_control_table(const Ref<DebugControlTable> &p_table);
	Ref<DebugControlTable> get_debug_control_table() const { return control_table_; }

	// The shell's world pick lands here as a typed request into the Entities
	// window carrying only the engine handle: the window opens, focuses, and
	// selects that row (pending until the next directory push carries it).
	// A negative or out-of-range handle clears the selection; the handle also
	// clears when the Simulation changes (stale handles never cross missions).
	// selected_entity_handle reads it back for probes/tests (-1 = none).
	void select_entity(int p_handle);
	int selected_entity_handle() const;

	// Every tool window back inside the main viewport on the next layout pass
	// (ImGui's ini remembers a window dragged out to another monitor); the
	// "Reset layout" menu item's seam, and what a probe asks for before it
	// reads the Stats rows.
	void reset_layout();

	// The runtime workspace installs its single game viewport here. The engine
	// window owns sizing policy; this adapter owns the Godot resize/draw call.
	void set_game_viewport(SubViewport *p_viewport);
	void set_game_play_available(bool p_available);
	bool is_game_play_available() const;
	void set_game_playing(bool p_playing);
	bool is_game_playing() const;
	bool handle_tools_toggle();
	bool handle_game_escape();
	Vector2i get_rendered_game_viewport_size() const;

	// A probe that drains the board itself hands the Stats window its
	// re-accumulated reading (sums/peaks/sample_frames sized FrameStats.SLOT_COUNT).
	void feed_stats_window(int64_t p_frames, const PackedInt64Array &p_sums,
			const PackedInt64Array &p_peaks, const PackedInt32Array &p_sample_frames);

	// The Stats window's last reading, row by row (probes and tests read the
	// same text the window draws).
	int64_t stats_reading_frames() const;
	PackedStringArray stats_row_ids() const;
	String stats_row_average(const String &p_row_id) const;
	String stats_row_peak(const String &p_row_id) const;
	String stats_row_info(const String &p_row_id) const;

	// The engine io::log ring drain (base/io/log_ring.h; ADR 0042 d5): one
	// locked snapshot of every recorded entry with sequence > cursor, as the
	// parallel columns MCP's game_logs "engine" source pages —
	// { "sequences": PackedInt64Array, "levels": PackedStringArray
	// ("debug"/"info"/"warn"/"error"), "texts": PackedStringArray }. Static
	// because the ring is process-wide, installed at extension init
	// (register_types.cpp) in every flavour; the forward converts and never
	// composes.
	static Dictionary engine_log_after(int64_t p_cursor);

	// The overlays' projection as a test seam (both flavours; the math is the
	// engine's header-only overlay_camera.h): a mission-frame point projected
	// through `camera` onto its viewport, exactly as the Game-view overlays
	// place it (NaN behind the camera). Pinned against
	// Camera3D::unproject_position.
	static Vector2 project_mission_point(Camera3D *p_camera, const Vector3 &p_mission_point);

protected:
	static void _bind_methods();

private:
	Ref<FrameStats> frame_stats_;
	Ref<DebugControlTable> control_table_;
	bool platform_windows_allowed_ = true;
#if OPENNOVA_DEVTOOLS
	bool attach_imgui();
	bool window_allows_platform_windows() const;
	void set_layer_visible(bool p_visible);
	void sync_layer_visible();
	bool layer_visible_ = false;

	bool draw(int p_requested_width, int p_requested_height) override;
	void apply_game_requests();
	void sync_game_spectator_state();
	void apply_control_requests();
	bool push_entity_directory();
	void push_entity_detail(bool p_directory_pushed);
	void push_weapon_records();
	void apply_weapon_requests();
	void push_environment_snapshot();
	void push_ai_debug();
	void apply_rays_requests();
	void push_rays_snapshot();
	void apply_physics_requests();
	void push_physics_snapshot();
	void push_control_catalog();
	void push_control_states();
	void push_game_status();
	// The overlay feed (dev_tools_overlay.cpp): the camera and the per-tick
	// layer records, ahead of the layout pass.
	void push_overlay_frame();
	bool overlay_live_ = false;
	uint64_t overlay_tick_ = static_cast<uint64_t>(-1);
	uint16_t overlay_selection_ = 0xFFFF;
	// A Rays/Physics filter changed: the next frame re-reads the rows.
	bool overlay_filters_dirty_ = false;
	int64_t last_hitbox_push_ms_ = -1;
	// The per-domain windows' records (dev_tools_windows.cpp).
	GameWorld *loaded_world() const;
	void push_domain_records();
	void clear_domain_records();
	void push_script_snapshot();
	void push_player_snapshot();
	void push_render_snapshot();
	void push_particle_snapshot();
	void push_audio_snapshot();
	void push_net_snapshot();
	int64_t last_script_push_ms_ = -1;
	int64_t last_player_push_ms_ = -1;
	int64_t last_render_push_ms_ = -1;
	int64_t last_particle_push_ms_ = -1;
	int64_t last_audio_push_ms_ = -1;
	int64_t last_net_push_ms_ = -1;
	void set_game_playing_internal(bool p_playing);
	// The one cadence gate every record push shares: true (and the stamp
	// moved) when p_seconds have passed since the last push, or none was made
	// (a stamp of -1 means push on the next needy frame).
	static bool push_due(int64_t &r_last_ms, double p_seconds);

	std::unique_ptr<opennova::devtools::GameDevTools> tools_;
	bool open_ = false; // the last state the shell was told about
	// world-load..world-unload, never owned: an ObjectID so a runtime freed
	// off the shell's teardown legs resolves to null instead of dangling.
	ObjectID simulation_id_;
	Simulation *simulation() const;
	int64_t last_entity_push_ms_ = -1; // -1 = push on the next needy frame
	int last_detail_handle_ = -1;      // the handle the last detail push carried; -1 = none
	// The Weapon window's trace is drained incrementally: only samples newer
	// than this reach the window, so a per-frame push stays small; a newest
	// tick below it is a restarted logic clock and re-primes the cursor.
	uint32_t last_weapon_trace_tick_ = 0;
	bool weapon_trace_primed_ = false;
	// The definition is rebuilt only when something moved it: an applied
	// request, a different weapon, or the clip rings resolving.
	bool weapon_def_dirty_ = true;
	uint64_t weapon_def_serial_ = 0;
	std::string weapon_def_name_;
	size_t weapon_def_rings_ = 0;
	bool weapon_records_live_ = false;
	int64_t last_environment_push_ms_ = -1;
	int64_t last_ai_push_ms_ = -1;
	int64_t last_rays_push_ms_ = -1;
	bool rays_recording_ = false;
	int64_t last_physics_push_ms_ = -1;
	bool contacts_recording_ = false;
	int64_t last_control_state_push_ms_ = -1;
	int64_t last_status_push_ms_ = -1;
	// The display frame the status readout averages between pushes.
	double frame_ms_sum_ = 0.0;
	double frame_ms_peak_ = 0.0;
	int64_t frame_ms_count_ = 0;
	SubViewport *game_viewport_ = nullptr;
	Vector2i rendered_game_viewport_size_;
	bool game_play_available_ = false;
	bool game_playing_ = false;
	uint64_t last_tools_toggle_frame_ = static_cast<uint64_t>(-1);
	uint64_t last_game_escape_frame_ = static_cast<uint64_t>(-1);
#endif
};

} // namespace godot
