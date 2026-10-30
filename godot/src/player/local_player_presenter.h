#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

#include "mnu/controls_model.h"
#include "object/object_model.h"
#include "player/gameplay_camera.h"
#include "player/local_player_visuals.h"
#include "player/player_input_router.h"
#include "player/player_move_intent.h"
#include "player/player_viewmodel_rig.h"
#include "player/player_weapon_effects.h"
#include "simulation/inmatch_session_values.h"
#include "simulation/person_overlay_models.h"
#include "simulation/player_aim_overlay.h"
#include "simulation/player_local_view.h"
#include "simulation/player_weapon_event.h"

#include <runtime/world/player_view.h>

namespace godot {

class Simulation;

// The local player's presenter (the former local_player_presenter.gd, ADR
// 0043 slice G8): the game shell's "LocalPlayerPresenter" node (probes locate
// it by name) that places the camera/avatar nodes from the sim's view
// snapshot and owns the three collaborators for one setup -> teardown span --
// PlayerInputRouter (raw input sampling: trigger edges, the RMB toggle
// REQUEST, movement, gameplay keys), PlayerViewmodelRig (the FP viewmodel
// node/parts, the renderfov render pass, the weapon.def placement) and
// PlayerWeaponEffects (the weapon EVENT presentation: the batch consume, the
// FSM event clips on BOTH viewmodel parts, the muzzle/shell userpoint
// resolution, the owner-bound effect anchors). The local-player state the
// world keeps (the equipped def, the spawn loadout, the model builders) is
// LocalPlayerVisuals, reached through the world's public surface.
//
// Faithful first-person camera. The on-foot eye is Position + CameraOffset,
// where the local player's CameraOffset is the POSED HEAD BONE minus
// Position -- the eye follows the animation (stand/crouch/prone/jump all
// move it) [orig: the local bone path @0x4b6bb3 stores head-Position into
// CameraOffset(+0x6C); the on-foot person camera leg adds it @0x437f9c].
// +1.0 is the witnessed NON-person fallback bump (engine: runtime/world/player_view.cpp), kept for
// the no-skeleton case. F4 swaps to the mode-1 chase camera (engine: runtime/world/player_view.cpp). Mouse look is SIM-owned: the
// cursor offset rides the frame input to the session's mouse pump, whose axis
// words feed the witnessed integer pipeline (sensitivity<<11, scoped zoom
// reduction, +-80 deg pitch clamp
// with the +40 deg up-limit while prone) (engine: runtime/world/local_player.cpp). Calibration values live at engine world/player_view.h
// (Simulation re-exports).
//
// The head is bone INDEX 14 (.bad row "BN15 Head") -- the rig is index-driven
// and the model bone order IS the BN order [world-wac-ai-re §14.2]; the
// original reads the head row of its bone-matrix array, never a name (engine: runtime/world/infantry.cpp).
//
// The FP eye pull-back (-0x3000 along the view forward), the eye floor, and
// the non-person bump compose in the SIM's camera pose now
// (world/player_view.h kFpEyePullback/kEyeMinAbovePosition/kNonPersonEyeBump);
// the constants remain for this presenter's OWN eye sampler (eye_position --
// the aim-ray and 3P-anchor legs read the raw head anchor, not the composed
// camera). The chase-camera composition -- the reset distance/orbit, the
// pivot nudge, and the collision march's no-collision landing -- runs in the
// SIM per drain (world/player_view.h, S8: player_view_compose_camera +
// player_view_tp_effective_distance); this presenter stamps the composed
// pose. Orbit keys (view bits 0x10/0x40 -> orbit_yaw +-0x1000000/tick
// @0x437c1b), the zoom keys (view actions 409/410 @0x49c1c5..0x49c23f), the
// march's bone-collision FORCES (@0x4382d9), and the dead-target 10.0 -> 3.0
// distance ease @0x437cc0 are tracked deferrals (net-re section 5.39).
//
// The equipped-weapon FSM view + the sim-owned view state (net-re
// §5.62/§5.41): the FSM and the VIEW STATE both tick in the sim at 62.5 Hz
// (engine/runtime/world weapon_fsm + player_view; ADR 0016 -- policy, state,
// and cadence live in the engine): the ADS engaged bit + 15-step ease, the
// fov policy, and the 3P anchor chase arrive as a PlayerLocalView snapshot
// each frame.
class LocalPlayerPresenter : public Node3D {
	GDCLASS(LocalPlayerPresenter, Node3D)

public:
	LocalPlayerPresenter();

	// `fly_camera`: the shell's gameplay-lock seam -- the game passes its
	// FlyCamera (the same node as `camera`); previews that fly no camera pass
	// null and the lock is a no-op. `controls`: the live binding table the
	// shell's ControlsBindings singleton owns (ControlsBindings.model()); null
	// reads every token released (the presenter test injects its intent).
	void setup(Node *p_world, Camera3D *p_camera, GameplayCamera *p_fly_camera = nullptr,
			const Ref<ControlsModel> &p_controls = Ref<ControlsModel>());
	void teardown();
	// The mission start's selection of the session's aspect mode (GameWorld's start
	// copies): the stretched target for that mode is made and held ahead of the
	// first live frame, as retail's backbuffer stands before it (the definition
	// carries the witness).
	void prepare_view_projection(int p_aspect_mode);

	// Drop the built FP viewmodel so the next update pass rebuilds
	// gun/arms/FSM from the (changed) equipped weapon -- the armory ACCEPT
	// re-mount [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 tail ->
	// Player_MountWeaponSlot @0x4dfa40].
	void refresh_viewmodel();
	// How many times the FP viewmodel was dropped for a rebuild (read seam).
	int viewmodel_generation() const;
	// Scripted movement (probes, automation, tests): null = poll the bindings.
	void set_input_override(const Ref<PlayerMoveIntent> &p_intent);
	// The HUD's key toggles (the F9 Emotes / F10 Radio menus the input
	// router's digit arms read and close); the HUD presenter hands its
	// process-lifetime instance over at setup.
	void set_hud_toggles(const Ref<HudToggles> &p_toggles);
	// The HUD toggle rows' event words of the input frames since the last
	// read, in their order (PlayerInputRouter::take_hud_toggle_events); the
	// HUD presenter applies them.
	PackedInt64Array take_hud_toggle_events();
	// The HUD's chat line and the gametext its prompts read, whose talk rows
	// the input router polls in every input frame
	// (PlayerInputRouter::set_hud_chat); the HUD presenter hands them over.
	void set_hud_chat(const Ref<HudChatEntry> &p_chat, const Ref<RtxtStringFile> &p_gametext);

	// A view action (Simulation::VIEW_ACTION_*: view1st F2, viewwithgun F3,
	// viewchase F4): view1st/viewwithgun select first person, viewchase the
	// chase, and each writes its own BMS input-action bit (engine:
	// world/player_view.h player_view_apply_view_action). The preference is
	// effective only in a control seat: the sim's arbiter resolves the camera mode every tick from
	// the preference and the seat, so on foot the preference changes nothing
	// visible (engine: runtime/world/player_view.h). The input router polls the rows
	// in each input frame; GameHudPresenter applies their events (it owns the FP-gun
	// bit two of them also write) and calls this.
	void apply_view_action(int p_action);
	// The debug menu's on-foot third person (the F3 Player page): stock JO
	// never resolves the chase outside a control seat (net-re §5.39 -- the
	// per-frame arbiter), so this is the onhook debug patch's affordance,
	// pushed into the sim's arbiter as its one override. Never a gameplay key.
	void set_debug_third_person(bool p_enabled);
	bool is_debug_third_person() const { return debug_third_person_; }
	// The RESOLVED camera mode, mirrored from the sim's view snapshot every
	// tick (engine: runtime/world/local_player_view.cpp). Nothing on the shell writes it
	// directly.
	bool is_third_person() const { return third_person_; }
	// The last presented snapshot: the frame the camera displays, composed
	// once per display frame by after_world_tick.
	Ref<PlayerLocalView> presented_view() const { return view_; }

	// Debug experiments (the dev tools' Player controls): keep the FP arms
	// drawn in every camera mode, and/or draw the player's own body in first
	// person -- the "see our feet" probe (the §14 aim overlay bends the spine
	// away from the eye, so looking down shows your legs; the head/shoulders
	// clip the near plane: the original draws no first-person body, so nothing
	// folds those bones here (ObjectModel::set_cut_sections would)).
	void set_debug_force_viewmodel(bool p_enabled) { debug_force_viewmodel_ = p_enabled; }
	bool is_debug_force_viewmodel() const { return debug_force_viewmodel_; }
	void set_debug_body_in_first_person(bool p_enabled) { debug_body_in_first_person_ = p_enabled; }
	bool is_debug_body_in_first_person() const { return debug_body_in_first_person_; }
	// The showhud bit-0 FP-gun gate, forwarded to the rig's visibility
	// decision. GameHudPresenter owns the showhud flag cycle and pushes the bit
	// here. (engine: runtime/hud/hud_config_tokens.h)
	void set_fp_gun_visible(bool p_visible);

	// W4-2-style justified accessors: PlayerWeaponEffects resolves action
	// userpoints and effect anchors against the presentation nodes (the FP
	// viewmodel parts -- rig-owned, delegated here -- the 3P gun, the camera).
	// These expose exactly the state it reads, so no cross-object private
	// access crosses the seam.
	ObjectModel *avatar() const;
	TypedArray<ObjectModel> vm_parts() const;
	Node3D *viewmodel() const;
	// The 3P gun; a SIBLING of the avatar (see LocalPlayerVisuals).
	ObjectModel *held_weapon() const;
	// The avatar's item overlays (canopy, goggles, binoculars, carried
	// object); siblings of the avatar like the held weapon.
	Ref<PersonOverlayModels> person_overlays() const { return person_overlays_; }
	Camera3D *camera() const;
	// The view block this frame's compose left (engine world/local_player_view.h
	// LocalPlayerViewFrame::view_block, which carries the witness): the camera's
	// stamped pose without the main scene's optics offsets, the eye the main
	// scene's water mirror collects from. False with no composed pose.
	bool get_view_block_transform(Transform3D &r_out) const;
	// THE FRAME'S PROJECTION over the surface (the engine's world::view_projection
	// for the session aspect mode): the horizontal fov is the policy fov in
	// every mode and the vertical half-extent follows the SELECTED ratio, so a
	// mode whose ratio differs from the surface's draws non-square pixels.
	// Godot couples a Camera3D's two fovs through its viewport aspect, so such
	// a mode renders the world through a SubViewport target of the selected
	// aspect (its camera at the horizontal fov, KEEP_WIDTH) that a full-surface
	// blit stretches onto the window -- the retail anamorphic fill; a native or
	// matched mode draws the surface directly. The NVG raster draws the 512
	// square at the frame's frustum instead, through the matrix it serves to
	// TargetProjectionXrInterface (which this returns while it is served).
	// Anything that projects world points onto the surface (HUD labels, tags,
	// picks) must read THIS frame (screen_projection for the HUD): while the
	// target is live the gameplay camera only carries a CULLING SUPERSET of the
	// frustum (the cullers that read it must never clip what the target draws).
	// Every camera here draws through the frame's D3D9 raster shift (the
	// engine's ViewProjection::raster_shift, render/d3d9_raster_device.h), and
	// this is the projection the raster draws with, the shift included.
	Projection view_projection() const;
	// The frame's projection without that raster shift: the original's screen
	// projection, which maps a world point to the window coordinate its 2D draws
	// at (D3D9's, pixel centres on the integers). The HUD's world anchors (attach
	// labels, friendly tags, the third-person aim point) project through it and
	// draw under the overlay's d3d9_screen_to_canvas, as the original's do; the
	// water's strip march and mirror take it too (Water::_drawing_view).
	Projection screen_projection() const;
	// The camera drawing the world while the stretched target is live (null
	// when the surface draws directly), and that target.
	Camera3D *projection_camera() const;
	SubViewport *projection_viewport() const;
	// The vertical stretch of the frame onto the surface (1 = none).
	float projection_scale_y() const { return projection_scale_y_; }
	// True while the NVG composite is up: the world renders as the NVG scene,
	// through the target at world::nvg_view_projection's raster (the 512
	// square at the frame's frustum, its texels non-square, or at the Scoped
	// arm's square frustum), the surface's own 3D pass off -- one world render
	// a frame.
	bool is_nvg_raster_active() const { return nvg_raster_active_; }
	// The reveal's edge (GameWorld::set_presentation_held(false)): a stretched view this frame's local
	// view held under the loading presentation draws through its target again at once, at the pose
	// that step placed, so the revealed frame is the stretched picture. A joiner's admission drops
	// the hold in the network leg, past the local view.
	void resume_view_projection();
	// The FP viewmodel owner (tests and probes inspect the projection feed and
	// sweep the placement tunables through it).
	Ref<PlayerViewmodelRig> viewmodel_rig() const { return viewmodel_rig_; }
	// A world-only render capture hides the FP gun for its duration (the
	// shell's capture session latches it; the rig's per-frame submission gate
	// ANDs it in).
	void set_viewmodel_capture_hidden(bool p_hidden);
	// Re-place the FP viewmodel against the camera's current pose without a
	// frame (the frozen-shell fixture capture moves the camera after freezing).
	void restamp_viewmodel_at_camera();
	// The world this presenter was set up on (null before setup).
	Node *world() const;

	Ref<MissionFrameInput> before_world_tick(double p_delta, bool p_capture_mouse = false,
			bool p_gameplay_input_active = true, bool p_menu_open = false);
	void after_world_tick();
	// Present one simulation tick's weapon batch before EffectWorld advances
	// that same tick (the ex fixed-tick consumer: GameWorld's effect fan-out calls it
	// directly through the world's local view presenter). Updating only the
	// local camera/avatar/viewmodel here gives action user points their
	// production-tick pose; mission/vehicle Nodes retain the
	// render-frame-batched present path that prevents 00TRa transform flicker.
	// Bound: a test runs one fixed tick's weapon drain (the live action handles'
	// re-pose, PlayerWeaponEffects::follow_action_effects) past a pose it set.
	void present_fixed_weapon_tick(const TypedArray<PlayerWeaponEvent> &p_events);
	// How many fixed-tick batches the router handed this presenter (a read
	// seam: the world tests pin the fixed-tick order through it).
	int fixed_weapon_batches_consumed() const { return fixed_weapon_batches_consumed_; }

	// The shell consumed the live USE-ITEM hold for a chord of its own (the
	// debug pick rides Shift+F6; the tools window opening mid-hold): the
	// router's release edge then runs no mount toggle.
	void consume_use_hold();
	// The USE hold the HUD's special-key chain reads (PlayerInputRouter::is_use_held).
	bool is_use_held() const { return input_router_.is_use_held(); }
	// One key-down through the special-key chain (PlayerInputRouter::special_key).
	Ref<SpecialKeyOutcome> special_key(const Ref<Simulation> &p_session, int p_vk) {
		return input_router_.special_key(p_session, p_vk);
	}
	// The special-key chain's deploy pick into the input frame's event list
	// (PlayerInputRouter::queue_pick_event), and the shell's arms for the escape row's and
	// the picks' records (PlayerInputRouter::set_record_leg).
	void queue_pick_event(int p_param) { input_router_.queue_pick_event(p_param); }
	void set_record_leg(const Callable &p_leg) { input_router_.set_record_leg(p_leg); }
	// The held USE row's armory arm took its open this frame (PlayerInputRouter::
	// take_armory_request); reading clears it.
	bool take_use_item_armory_request();
	// The vehicle-loadout arm's open (PlayerInputRouter::take_vehicle_menu_request);
	// reading clears it.
	bool take_use_item_vehicle_menu_request();
	// A menu close's input reset (PlayerInputRouter::reset_input_state).
	void reset_input_state();
	// The device's key queue the router owns and the shell's key-down feed
	// (PlayerInputRouter::key_events / push_key_event).
	Ref<KeyEventQueue> get_key_events() { return input_router_.key_events(); }
	void push_key_event(const Ref<InputEventKey> &p_event) {
		input_router_.push_key_event(p_event);
	}
	// One row event the keyboard pass's scan queued (PlayerInputRouter::queue_row_event).
	void queue_row_event(int p_code, bool p_signed, int p_key) {
		input_router_.queue_row_event(p_code, p_signed, p_key);
	}
	// The keyboard pass's release pass (PlayerInputRouter::run_key_release_pass).
	void run_key_release_pass() { input_router_.run_key_release_pass(); }
	// The shell's keyboard pass, run at each update's input-frame head
	// (PlayerInputRouter::set_keyboard_pass).
	void set_keyboard_pass(const Callable &p_pass) { input_router_.set_keyboard_pass(p_pass); }
	// The shell's menu-screen bit, read at each update's input-frame head
	// (PlayerInputRouter::set_menu_screen_probe).
	void set_menu_screen_probe(const Callable &p_probe) {
		input_router_.set_menu_screen_probe(p_probe);
	}
	// A pass-through screen's close: the next input frame takes the menu
	// cursor's offset (PlayerInputRouter::close_pass_through_screen).
	void close_pass_through_screen();
	// A game screen frees the captured cursor at the re-centre point
	// (PlayerInputRouter::release_capture_for_screen).
	void release_capture_for_screen();
	// The process's keyboard-lock legs and the Escape key's Scroll Lock turn-off
	// (PlayerInputRouter::start_process_num_lock and its siblings).
	static void start_process_num_lock();
	static void start_process_scroll_lock();
	static void exit_process_key_locks();
	void owe_scroll_lock_turn_off();
	// Mouse-look rides the input router: the motion events' cursor offset,
	// which the session's mouse pump spreads into the axis words the SIM's
	// witnessed integer pipeline scales (engine: runtime/world/player_look.h).
	bool handle_input(const Ref<InputEvent> &p_event, bool p_active);

	bool has_player() const;
	bool is_local_spectator() const;
	// The model lifetime around the input router's before-tick sample: build
	// the avatar when missing and let the rig (re)build the FP viewmodel.
	void ensure_models();
	void clear_models();
	// The avatar and what it carries (the held gun, the person overlays) go;
	// ensure_models builds the next.
	void release_avatar();
	void set_fly_camera_locked(bool p_locked);

	// The crosshair's witnessed anchor. First person PINS the exact screen
	// center -- the original never projects there [orig: HUD_DrawCrosshair
	// @0x592640 -- 1P local takes screen_w/2, screen_h/2 @0x5928a0/@0x5928ae];
	// third person / spectate projects the aim ray's far point through the live
	// camera [orig: the else branch @0x592910 -- Entity_BuildCameraView(entity,
	// 1, 1, 65536000 = 1000.0 q16) transformed + frustum-clipped
	// @0x592932..3c; Viewport_ScreenToVirtual @0x5d2c70]. Vector2.INF = "no
	// projection" (1P pin, no camera/player, or the far point behind the
	// camera) -- the HUD falls back to the design center.
	Vector2 aim_screen_point() const;
    // Read-only render-pose diagnostics (Vector3.INF when no skeleton exists).
    // Gameplay eyes come from the current simulation CameraOffset.
	Vector3 avatar_root_world() const;
	Vector3 avatar_head_world() const;

	// --- C++-only seams for the collaborators ---
	Ref<Simulation> sim() const;
	Ref<LocalPlayerVisuals> visuals() const { return visuals_; }
	PlayerWeaponEffects *weapon_effects() const { return weapon_effects_.ptr(); }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void refresh_camera_mode();
	void set_avatar_transform(ObjectModel *p_avatar, const Transform3D &p_next);
	void reset_state();
	void set_world_nvg_view(bool p_active, int p_gain);
	Vector2 aim_angles_deg() const;
	Vector3 eye_position(const Vector3 &p_pos) const;
	void update_held_weapon(const Ref<PlayerAimOverlay> &p_overlay);
	void update_person_overlays();
	// Whether the avatar draws this frame: a view collected it (its occlusion
	// claims, ObjectModel::is_view_collected) or a render slot captured it.
	static bool avatar_drawn(const ObjectModel *p_body);
	void update_player_camera();
	void stamp_camera_pose();
	void update_model_lighting_context();
	static void set_model_lighting_context(ObjectModel *p_model, bool p_interior, float p_transfer,
			float p_effect_scale);
	void update_scope_camera();
	// The vehicle first-person swap, local-view half: this frame's engine
	// verdict (the view snapshot's virtual-display triple) handed to the entity
	// presenter, which owns the carrier nodes and draws the display model in
	// the hidden hull's place. `p_live` false feeds the inactive frame.
	void feed_virtual_display(bool p_live);
	void update_view_projection(const opennova::world::ViewProjection &p_projection,
			bool p_nvg_raster);
	// The target, its camera and its blit, live (the caller holds it or sizes it).
	SubViewport *make_view_projection_target();
	// The load's one forced frame through the held target (GameWorld's
	// hud_pipeline_warm edges, connected by setup, dropped by teardown).
	bool draw_held_view_projection(bool p_drawing);
	void disconnect_forced_frame();
	// An exit from live play keeps the target (engine world/player_view.h
	// ViewProjection: retail's backbuffer is made once): it stops drawing, the
	// blit hides and the surface draws its own 3D again; the next live frame
	// draws into it with its render buffers kept.
	void hold_view_projection();
	void release_view_projection();
	void update_avatar(const Vector3 &p_pos);
	GameplayCamera *fly_camera() const;

	ObjectID world_id_;
	ObjectID fly_camera_id_;
	ObjectID camera_id_;
	bool third_person_ = false;
	ObjectID avatar_id_;
	// The avatar's world transform as update_avatar composed it from the sim (its
	// position, its body frame): the frame its attach legs place by.
	Transform3D avatar_world_;
	int avatar_type_id_ = 0; // the local body's type word the avatar was built for
	ObjectID held_weapon_id_; // the 3P gun; a SIBLING of the avatar (see LocalPlayerVisuals)
	String held_weapon_graphic_; // the gfx3 the live node was built from
	Ref<PersonOverlayModels> person_overlays_;
	Ref<PlayerWeaponEffects> weapon_effects_;
	PlayerInputRouter input_router_;
	Ref<PlayerViewmodelRig> viewmodel_rig_;
	Ref<LocalPlayerVisuals> visuals_;
	Ref<PlayerLocalView> view_; // the sim's per-tick view snapshot (null = no sim)
	float camera_saved_fov_ = -1.0f;
	int camera_saved_keep_aspect_ = -1;
	int camera_saved_projection_ = -1;
	// The D3D9 raster shift the frame's cameras draw through (view_projection
	// carries it, screen_projection takes it back out).
	opennova::renderer::NdcShift raster_shift_;
	// The stretched-mode target (view_projection): the SubViewport, its camera,
	// the blit CanvasLayer, and the surface viewport whose own 3D draw it
	// replaced while live.
	ObjectID projection_viewport_id_;
	ObjectID projection_camera_id_;
	ObjectID projection_blit_layer_id_;
	ObjectID projection_surface_id_;
	// The same target while held (hold_view_projection): no reader sees it.
	ObjectID held_projection_viewport_id_;
	ObjectID held_projection_camera_id_;
	ObjectID held_projection_blit_layer_id_;
	float projection_scale_y_ = 1.0f;
	bool nvg_raster_active_ = false;
	// The projection this frame's local view asked for while the loading presentation held the
	// world's draw (stretched or the NVG raster), for the reveal's resume_view_projection.
	opennova::world::ViewProjection held_frame_projection_;
	bool held_frame_nvg_raster_ = false;
	bool held_frame_pending_ = false;
	// The NVG raster's frame frustum as served to TargetProjectionXrInterface
	// (the matrix the target draws with while nvg_raster_served_).
	Projection nvg_raster_projection_;
	bool nvg_raster_served_ = false;
	bool debug_force_viewmodel_ = false;
	bool debug_body_in_first_person_ = false;
	bool debug_third_person_ = false;
	int64_t camera_saved_cull_mask_ = -1;
	int fixed_weapon_batches_consumed_ = 0;
};

} // namespace godot
