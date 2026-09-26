// The game's dev tools (ADR 0039): the ImGui pass behind F3 with its mandatory
// Game surface (the game image, the transport toolbar, the Game-view overlay
// layers), the Stats window (both open by default), the Entities and Entity
// Properties windows (opened by a world pick; the pushed-record /
// control-request channel, ADR 0042 d6 + ADR 0043 d12), the Weapon window
// (the DCC-style ACTION editor over the equipped weapon's FSM), the
// Environment, AI, Rays, Physics, Script, Player, Render, Particles, Audio,
// Net and Log windows (closed by default, opened from the "Windows" menu),
// the control board every window reads the debug-control rows through, and
// ImGui's demo window (the docking/multi-viewport smoke test, under Help).
// Debug builds only (OPENNOVA_DEVTOOLS); the release GDExtension flavour
// compiles this out and its DevTools node is inert.
#pragma once

#include <runtime/devtools/control_board.h>
#include <runtime/devtools/frame_stats_board.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/world/inspect_markers.h>

#include <cstdint>
#include <vector>

namespace opennova::io {
class LogRing;
}

namespace opennova::devtools {

class StatsWindow;
class GameWindow;
class GameViewport;
class EntitiesWindow;
class EntityPropertiesWindow;
class WeaponWindow;
class EnvironmentWindow;
class AiWindow;
class RaysWindow;
class PhysicsWindow;
class LogWindow;
class ScriptWindow;
class PlayerWindow;
class RenderWindow;
class ParticlesWindow;
class AudioWindow;
class NetWindow;
struct ScriptSnapshot;
struct PlayerSnapshot;
struct RenderSnapshot;
struct ParticleSnapshot;
struct AudioSnapshot;
struct NetSnapshot;
enum class GameInputMode;
enum class GameWindowRequest;
struct ControlRequest;
struct ControlResult;
struct GameStatusSnapshot;
struct OverlayCamera;
struct EntityMarkersRecord;
struct RaysOverlayRecord;
struct ContactsOverlayRecord;
struct HitboxOverlayRecord;
struct EntityDirectorySnapshot;
struct EntityDetailSnapshot;
struct WeaponDefinitionSnapshot;
struct WeaponLiveSnapshot;
struct WeaponRequest;
struct EnvironmentSnapshot;
struct AiDebugSnapshot;
struct RaysRequest;
struct RaysSnapshot;
struct PhysicsRequest;
struct PhysicsSnapshot;

class GameDevTools {
public:
	GameDevTools();

	ImGuiPass &pass() { return pass_; }
	const ImGuiPass &pass() const { return pass_; }
	StatsWindow &stats_window() { return *stats_window_; }
	EntitiesWindow &entities_window() { return *entities_window_; }
	EntityPropertiesWindow &entity_properties_window() { return *entity_properties_window_; }
	WeaponWindow &weapon_window() { return *weapon_window_; }
	EnvironmentWindow &environment_window() { return *environment_window_; }
	AiWindow &ai_window() { return *ai_window_; }
	RaysWindow &rays_window() { return *rays_window_; }
	PhysicsWindow &physics_window() { return *physics_window_; }
	LogWindow &log_window() { return *log_window_; }
	ScriptWindow &script_window() { return *script_window_; }
	PlayerWindow &player_window() { return *player_window_; }
	RenderWindow &render_window() { return *render_window_; }
	ParticlesWindow &particles_window() { return *particles_window_; }
	AudioWindow &audio_window() { return *audio_window_; }
	NetWindow &net_window() { return *net_window_; }
	void set_game_viewport(GameViewport *viewport);
	void set_game_play_available(bool available);
	void set_game_spectator_state(bool available, bool active);
	void set_game_input_mode(GameInputMode mode);
	void reset_game_input_mode();
	void request_game_escape();
	bool take_game_request(GameWindowRequest &request);

	// The board the Stats window reads (owned by the embedder; may be null).
	void set_frame_stats(FrameStatsBoard *board);
	// The engine log ring the Log window drains (process-wide; may be null).
	void set_log_ring(const io::LogRing *ring);

	// The control board (control_board.h): the debug-control table's catalog,
	// pushed once by the embedder when it lends the table, and the live
	// states of the rows the visible windows want (wanted_control_ids),
	// pushed on kControlStateSeconds while needs_control_states.
	static constexpr double kControlStateSeconds = 0.25;
	ControlBoard &control_board() { return control_board_; }
	void set_control_catalog(std::vector<ControlSpec> catalog);
	void set_control_states(const std::vector<ControlState> &states);
	bool needs_control_states() const;
	void wanted_control_ids(std::vector<const char *> &out) const;

	// The world-space overlays (overlay_canvas.h). The embedder pushes the
	// game camera (needs_overlay_camera: tools open and a layer on) and each
	// layer's record ahead of the layout pass; clear_overlay_records drops
	// them all (the tools closed, the world unloaded).
	void set_overlay_camera(const OverlayCamera &camera);
	bool needs_overlay_camera() const;
	void clear_overlay_records();
	// The Entities layers' markers: wanted while the Selection layer has a
	// selection to mark or the Labels layer is on; the query (anchor, reach,
	// cap, selection) is the tools' policy, the embedder runs it.
	bool needs_entity_markers() const;
	// A paused world's tick stays fixed; selection, layer and camera changes
	// still change which entities the marker query must return.
	bool needs_entity_marker_refresh(uint64_t logic_tick, const world::Vec3 &eye) const;
	world::inspect::EntityMarkerQuery entity_marker_query(const world::Vec3 &eye) const;
	void set_entity_markers(EntityMarkersRecord record);

	// The per-domain windows' records (the same shape: pushed by value on
	// the window's kRefreshSeconds while it shows; an invalid record clears).
	void set_script_snapshot(ScriptSnapshot snapshot);
	bool needs_script_snapshot() const;
	void set_player_snapshot(PlayerSnapshot snapshot);
	bool needs_player_snapshot() const;
	void set_render_snapshot(const RenderSnapshot &snapshot);
	bool needs_render_snapshot() const;
	void set_particle_snapshot(ParticleSnapshot snapshot);
	bool needs_particle_snapshot() const;
	void set_audio_snapshot(AudioSnapshot snapshot);
	bool needs_audio_snapshot() const;
	void set_net_snapshot(NetSnapshot snapshot);
	bool needs_net_snapshot() const;

	// The Game window's status readout, pushed while the tools are open.
	void set_game_status(const GameStatusSnapshot &status);
	bool needs_game_status() const { return pass_.is_open(); }
	GameWindow &game_window() { return *game_window_; }

	// The Entities window's record channel (ADR 0042 d6). The embedder pushes
	// the directory by value (an invalid snapshot clears; it carries the
	// authority fact the edits gate on), gated on needs_entity_directory
	// (pass open && the Entities, Entity Properties or AI window open) so
	// nobody builds snapshots no window would show.
	void set_entity_directory(EntityDirectorySnapshot snapshot);
	bool needs_entity_directory() const;

	// The ONE control-request drain (ADR 0043 d12): every window's
	// debug-control invocations — the Game window's spectator toggle, the
	// Entity Properties actions and attrib toggles, the Environment strip —
	// in queue order (game, entities, environment), for the embedder to hand
	// to the debug-control table by wire id. A window's queue is its own; this
	// only serialises them.
	bool take_control_request(ControlRequest &request);
	// The table's verdict on a drained request: posted to the menu bar's
	// status line ("id: ok" / "id: failed (reason)") and to the Log window
	// with a read's full payload.
	void report_control_result(const ControlResult &result);

	// The selection seam: the shell's world pick (a device event carrying
	// only the engine handle) opens and focuses the Entities window on that
	// row and the Entity Properties window on its card; the embedder pushes
	// the selected row's detail card (the engine card by value, an invalid
	// card clears) gated on needs_entity_detail (pass open && the Properties
	// window open && a selection). kInvalid = nothing selected.
	void select_entity(uint16_t handle);
	void clear_entity_selection();
	uint16_t selected_entity_handle() const;
	void set_entity_detail(EntityDetailSnapshot detail);
	bool needs_entity_detail() const;

	// The Weapon window's record/request channel. The definition (the rows
	// the dope sheet draws) is pushed on a serial bump — an install, an
	// applied edit — while the live record is pushed EVERY frame rather than
	// on the Entities window's 0.5 s cadence: the trace pane is a scope on a
	// 62.5 Hz signal.
	void set_weapon_definition(WeaponDefinitionSnapshot definition);
	void set_weapon_live(WeaponLiveSnapshot live);
	bool needs_weapon_records() const;
	bool take_weapon_request(WeaponRequest &request);

	// The Environment window's record channel (the same shape): the weather
	// page record pushed by value on its cadence while shown; its weather
	// commands leave through take_control_request.
	void set_environment_snapshot(const EnvironmentSnapshot &snapshot);
	bool needs_environment_snapshot() const;

	// The AI window's record channel (the same shape, records-in only): the
	// AI debug join pushed by value on its cadence while shown, and every
	// logic tick while one of its Game-view layers is on (needs_ai_overlay).
	void set_ai_debug(AiDebugSnapshot snapshot);
	bool needs_ai_debug() const;
	bool needs_ai_overlay() const;

	// The Rays window's record/request channel (the same shape): the ray
	// capture's counts + filter state pushed by value on its cadence while
	// shown, and the typed filter/clear requests drained by the embedder into
	// the Simulation ray-debug seam.
	void set_rays_snapshot(const RaysSnapshot &snapshot);
	bool needs_rays_snapshot() const;
	bool take_rays_request(RaysRequest &request);
	// The Rays window's Game-view layer: the filtered ray rows, per logic
	// tick while it is on (needs_rays_snapshot then keeps recording armed).
	bool needs_rays_overlay() const;
	void set_rays_overlay(RaysOverlayRecord record);

	// The Physics window's record/request channel (the same shape): the
	// contact capture's counts + capture state pushed by value on its cadence
	// while shown, and the typed mask/clear/capture requests drained into the
	// Simulation contact-debug seam.
	void set_physics_snapshot(const PhysicsSnapshot &snapshot);
	bool needs_physics_snapshot() const;
	bool take_physics_request(PhysicsRequest &request);
	// The Physics window's Game-view layers: the filtered contact rows (per
	// logic tick; needs_physics_snapshot keeps the capture armed while it is
	// on) and the hit meshes (the hitbox oracle on its own cadence).
	bool needs_contacts_overlay() const;
	void set_contacts_overlay(ContactsOverlayRecord record);
	bool needs_hitbox_overlay() const;
	void set_hitbox_overlay(HitboxOverlayRecord record);

private:
	// Declared before the pass: the windows hold a reference to the board and
	// the pass's destructor still calls their on_visibility.
	ControlBoard control_board_;
	ImGuiPass pass_;
	GameWindow *game_window_ = nullptr;
	StatsWindow *stats_window_ = nullptr;
	EntitiesWindow *entities_window_ = nullptr;
	EntityPropertiesWindow *entity_properties_window_ = nullptr;
	WeaponWindow *weapon_window_ = nullptr;
	EnvironmentWindow *environment_window_ = nullptr;
	AiWindow *ai_window_ = nullptr;
	RaysWindow *rays_window_ = nullptr;
	PhysicsWindow *physics_window_ = nullptr;
	LogWindow *log_window_ = nullptr;
	ScriptWindow *script_window_ = nullptr;
	PlayerWindow *player_window_ = nullptr;
	RenderWindow *render_window_ = nullptr;
	ParticlesWindow *particles_window_ = nullptr;
	AudioWindow *audio_window_ = nullptr;
	NetWindow *net_window_ = nullptr;
};

}  // namespace opennova::devtools
