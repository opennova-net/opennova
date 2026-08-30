#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "devtools/imgui_pass_node.h"
#include "devtools/frame_stats.h"

#if OPENNOVA_DEVTOOLS
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/game_window.h>

#include <memory>
#endif

namespace godot {

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
// keep parsing, but is_available() is false and every call is a no-op; the
// game's release export also strips the addon.
class DevTools : public ImGuiPassNode
#if OPENNOVA_DEVTOOLS
		, private opennova::devtools::GameViewport
#endif
{
	GDCLASS(DevTools, ImGuiPassNode)

public:
	DevTools();
	~DevTools() override;

	void _exit_tree() override;

	bool is_open() const;
	void set_open(bool p_open);
	void toggle() { set_open(!is_open()); }

	void set_frame_stats(const Ref<FrameStats> &p_stats);
	Ref<FrameStats> get_frame_stats() const { return frame_stats_; }

	// The Simulation the engine-fact windows read and mutate through
	// (ADR 0042 d6): a raw pointer the shell sets on world load and nulls on
	// world unload (and _exit_tree nulls) — the stats-board pattern. The
	// per-frame leg pushes the entity-directory record (built by the ENGINE
	// join, world::inspect::entity_directory, at the window's 0.5 s cadence
	// and only while the window shows) and drains the windows' typed
	// DebugRequests into the same debug delegates the MCP control plane uses.
	void set_simulation(Simulation *p_simulation);

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

protected:
	static void _bind_methods();
	opennova::devtools::ImGuiPass *engine_pass() override;
	void after_layout(uint64_t p_frame_index, bool p_drew, int64_t p_layout_us) override;

private:
	Ref<FrameStats> frame_stats_;
#if OPENNOVA_DEVTOOLS
	void draw(int p_requested_width, int p_requested_height) override;
	void apply_game_requests();
	void apply_debug_requests();
	bool push_entity_directory();
	void push_entity_detail(bool p_directory_pushed);
	void apply_environment_requests();
	void push_environment_snapshot();
	void set_game_playing_internal(bool p_playing);

	std::unique_ptr<opennova::devtools::GameDevTools> tools_;
	bool open_ = false; // the last state the shell was told about
	// world-load..world-unload, never owned: an ObjectID so a runtime freed
	// off the shell's teardown legs resolves to null instead of dangling.
	ObjectID simulation_id_;
	Simulation *simulation() const;
	int64_t last_entity_push_ms_ = -1; // -1 = push on the next needy frame
	int last_detail_handle_ = -1;      // the handle the last detail push carried; -1 = none
	int64_t last_environment_push_ms_ = -1;
	SubViewport *game_viewport_ = nullptr;
	Vector2i rendered_game_viewport_size_;
	bool game_play_available_ = false;
	bool game_playing_ = false;
	uint64_t last_tools_toggle_frame_ = static_cast<uint64_t>(-1);
	uint64_t last_game_escape_frame_ = static_cast<uint64_t>(-1);
#endif
};

} // namespace godot
