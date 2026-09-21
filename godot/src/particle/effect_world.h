#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

#include "particle/effect_group_report.h"
#include "particle/effect_load_report.h"
#include "particle/effect_scene.h"
#include "particle/effect_spawn_records.h"
#include "particle/particle_file.h"

namespace opennova::particle {
class ParticleForceField;
}

namespace godot {

class Camera3D;
class ParticleRenderer;
class ResourceRoot;

// World-facing owner for the portable effect scene and draw-list renderer
// (the former effect_world.gd, ADR 0043 d9). Effects, emitters, and
// particles are values owned by EffectScene; this node owns one
// ParticleRenderer for both render domains. Device only: the Variant
// slot/owner keys the game hands it are interned to native tokens here, the
// per-frame owner-pose sync polls the game's resolver, and the debug read
// model joins the scene's snapshot to the renderer's bounds.
class EffectWorld : public Node3D {
	GDCLASS(EffectWorld, Node3D)

public:
	// Re-exports of the vocabulary the portable effect scene owns: callers
	// keep the EffectWorld.* spelling.
	enum {
		PARTICLE_FLAG_FOREVER_EMIT = opennova::particle::particle_flag::ForeverEmit,
		ADMISSION_ALWAYS = EffectScene::ADMISSION_ALWAYS,
		ADMISSION_REPLACE_OWNED = EffectScene::ADMISSION_REPLACE_OWNED,
		ADMISSION_SUPPRESS_WHILE_OWNED = EffectScene::ADMISSION_SUPPRESS_WHILE_OWNED,
        ADMISSION_STORE_OWNED = EffectScene::ADMISSION_STORE_OWNED,
		BINDING_WORLD = EffectScene::BINDING_WORLD,
		BINDING_FOLLOW_OWNER = EffectScene::BINDING_FOLLOW_OWNER,
		RENDER_DOMAIN_WORLD = EffectScene::RENDER_DOMAIN_WORLD,
		RENDER_DOMAIN_FIRST_PERSON = EffectScene::RENDER_DOMAIN_FIRST_PERSON,
		KILL_PLANE_DISABLED = EffectScene::KILL_PLANE_DISABLED,
	};

	EffectWorld();

	// Retire compositor callbacks and their RenderingDevice resources while
	// the mission viewport and RenderingServer targets they reference are
	// still live. ParticleRenderer's EXIT_TREE hook is only an idempotent
	// fallback: queued mission teardown can otherwise reach it after those
	// target RIDs are gone.
	void release_runtime_renderer_resources();
	void set_environment_source(Node *p_source);
	int file_count() const;
	// The loaded particle files (a copy of the list; the probes describe an
	// effect's authored definition through it).
	TypedArray<ParticleFile> get_files() const;
	int effect_count() const;
	int live_group_count() const;
	int active_entry_count() const;
	int interned_count() const;
	void set_particles_hidden(bool p_hidden);
	bool are_particles_hidden() const;
	Callable get_texture_provider() const;
	// The per-frame owner-pose resolver: called with the owner key, it
	// answers a Transform3D (the full pose), a Vector3 (a translation over
	// the cached basis) or null (the owner is gone: its groups detach).
	void set_owner_position_provider(const Callable &p_provider);
	void set_water_plane(float p_value, Camera3D *p_reflection_camera);
	// The camera of a second view of this world's scene (the weapon Inset pass),
	// or null while none renders: the renderer compiles the world's particles
	// for it too (ParticleRenderer::set_second_scene_camera). The device owner
	// of that view hands it in every frame; clear_world() drops it.
	void set_second_scene_camera(Camera3D *p_camera);
	Camera3D *get_second_scene_camera() const;

	// Loads every mounted .ptl AND the active gore set in VFS order, then
	// opens the catalog once. The gore set is a second extension carrying
	// the same grammar — `.ptu` (US) or `.ptg` (German) — and the
	// impact-effect definitions that ride only there: Effect_AmHitBody /
	// Effect_SGvBody (the blood puffs) and the Effect_FX50Cal* impact
	// family. Skipping it does not fail loudly: an unknown effect name
	// interns as an invisible `stockeffect` clone (D-PTL-8), so every flesh
	// hit silently renders nothing.
	// [orig: CEffectSystem_Init @ 0x5f6070 scans loose `ptl\*.ptl` @0x5f6228
	//  then `ptl\*<ext>` @0x5f6356, and matches each archive entry against
	//  ".ptl" OR the selected extension @0x5f64f3 — both legs parse through
	//  CEffectWorld_ParseSectionCallback @ 0x5ecb40.]
	// Order: the catalog registers EVERY particledef across ALL documents
	// before it resolves ANY effectdef, so cross-file pdefs resolve
	// regardless of file order; order only picks which duplicate id wins
	// (first registration). `.ptl` before the gore set reproduces retail's
	// outcome on the shipped data, where the joAmmoHit.ptl copies of
	// AmHitBody_Mist4G/BloodAltG sort ahead of US_BLOOD.PTU.
	int load_from_resource_root(const Ref<ResourceRoot> &p_root);
	// The in-memory entry (the GUT fixtures author documents directly).
	void load_particle_file(const Ref<ParticleFile> &p_file);
	void clear_world();
	// Clear live simulation values while preserving the compiled catalog,
	// native definition identity, interned handles, texture atlas, and
	// renderer pipelines. Mission Stop uses this instead of clear_world() so
	// the next Play reuses every load-time warm result.
	void reset_runtime_state();
	int64_t intern_effect(const String &p_name);
    void spawn_script_effect(const opennova::world::ScriptEffectEvent &event, uint32_t age_ticks);
	String effect_name_for_handle(int64_t p_handle) const;

	// The effect pose for an authored forward vector: Z along the forward,
	// an up hint of UP (RIGHT when the forward is vertical). Shared with the
	// static item-effect sources (ItemEffectDirector).
	static Transform3D forward_pose(const Vector3 &p_position, const Vector3 &p_forward);
	static Transform3D descriptor_pose(const Vector3 &p_position, const Vector3 &p_orientation);

	// Deep spawn seam. Admission, binding, and render domain are explicit
	// values; the options' slot_key/owner_key Variants are interned to
	// stable native tokens.
	Ref<EffectSpawnReceipt> spawn_effect_request(const String &p_name,
			const Transform3D &p_transform,
			const Ref<EffectSpawnOptions> &p_options = Ref<EffectSpawnOptions>());
	// Load-time warm pass: spawn every catalog effect once at `position` so
	// the lazily-deferred one-times (texture resolves, the renderer's
	// first-draw pipeline compiles) are paid behind the loading screen
	// instead of as a ~90 ms hitch on the player's first live shot. Retail
	// pays this at load — CEffectSystem_Init loads every .ptl AND its
	// textures up front [orig: @ 0x5f6070 <- Game_StartMission @ 0x524980];
	// the deferred-resolve shell path is what made first fire hitch. The
	// caller renders a frame or two (advancing the fixed tick so fresh
	// emitters actually emit and draw), then clears the warm spawns via
	// reset_runtime_state(). Returns the spawn count.
	int warm_all_effects(const Vector3 &p_position);
	// Compile and submit the latest scene snapshot synchronously. The normal
	// process path calls the renderer every frame; mission loading uses this
	// seam between fixed warm ticks and forced draws so freshly emitted
	// values are in the submitted draw list immediately. Returns the number
	// of material runs.
	int64_t render_now(int64_t p_time_ms);
	// The renderer's full draw-list diagnostics (world + first-person lists,
	// backends, atlas pages/entries) for the F3 Particles page — the one
	// Dictionary report this facade keeps (the allowlisted transport edge).
	Dictionary get_debug_draw_list_report();

	int64_t spawn_effect_transient(const String &p_name, const Vector3 &p_position,
			const Vector3 &p_orientation = Vector3(), int p_initial_age_ticks = 0,
			int p_render_domain = RENDER_DOMAIN_WORLD, int64_t p_source_tick = 0,
			int64_t p_source_order = 0);
	int64_t spawn_effect(const String &p_name, const Vector3 &p_position,
			const Vector3 &p_orientation = Vector3());
	Ref<EffectSpawnReceipt> spawn_effect_owned_request(const Variant &p_owner_key,
			const String &p_name, const Vector3 &p_position,
			const Vector3 &p_orientation = Vector3());
	int64_t spawn_effect_owned(const Variant &p_owner_key, const String &p_name,
			const Vector3 &p_position, const Vector3 &p_orientation = Vector3());
	Ref<EffectSpawnReceipt> spawn_effect_attached_request(const Variant &p_owner_key,
			const String &p_name, const Transform3D &p_initial_transform,
			const Vector3 &p_local_pos, const Vector3 &p_local_dir);
	int64_t spawn_effect_attached(const Variant &p_owner_key, const String &p_name,
			const Transform3D &p_initial_transform, const Vector3 &p_local_pos,
			const Vector3 &p_local_dir);
	int64_t spawn_effect_unless_alive(const Variant &p_owner_key, const String &p_name,
			const Vector3 &p_position, const Vector3 &p_orientation = Vector3());
	bool spawn_effect_by_handle(int64_t p_handle, const Vector3 &p_position,
			const Vector3 &p_orientation = Vector3());
    std::shared_ptr<opennova::particle::EffectScene> shared_native_scene() const;
	void stop_group(int64_t p_group_id);
	// The rotor-wash re-trigger: every child emitter of a live group spawns
	// one particle at `p_position` along `p_forward`, bound to `p_force_zone`
	// (the retail dword_29D6BB0 spawn window). False for a dead group, which
	// is retail's freed-slot no-op. The witness is cited on the portable
	// particle::EffectScene::trigger_group_children this forwards to.
	bool trigger_group_children(int64_t p_group_id, const Vector3 &p_position,
			const Vector3 &p_forward, int p_force_zone);
	// Update the two live arguments supplied by moving bone-trail effects:
	// emission-rate control and vertical/camera-offset control. Inputs remain
	// unclamped because the portable scene owns the retail formulas.
	bool set_group_parameters(int64_t p_group_id, float p_rate_control,
			float p_offset_control);
	// Releases the script/native binding identity for an owner whose
	// lifecycle is complete. Call after stopping its group: generation-scoped
	// owners (flying rounds, debris pieces) otherwise accumulate one slot
	// key, owner key, and cached pose for every lifetime until the whole
	// mission is reset.
	void release_effect_binding(const Variant &p_owner_key);
	// True while this owner key holds a live binding identity (slot token,
	// owner token, and reverse lookup). Leak-check seam for generation-scoped
	// owners (flying rounds, debris pieces) whose keys must not accumulate
	// across lifetimes (ADR 0017: typed scalars, not a Dictionary report).
	bool has_owner_binding(const Variant &p_owner_key) const;
	// True while this owner key's binding also carries a cached native pose.
	// A rejected ReplaceOwned spawn allocates its identities but never seeds
	// one.
	bool has_cached_owner_pose(const Variant &p_owner_key) const;
	// True when every owner-binding table is empty, i.e. no retired owner
	// key is still holding a slot token, owner token, reverse lookup, or
	// cached pose.
	bool has_no_owner_bindings() const;
	// The mission header's wind, the GLOBALWIND drift every tick
	// (particle::mission_wind_vector). GameWorld feeds it at effect-world start.
	void set_mission_wind(int p_wind_speed, int p_wind_direction_degrees);
	// The only simulation clock. Callers feed fixed mission ticks (1 / 62.5 s).
	void advance_fixed_tick(double p_delta);
	// The same clock with the simulation's borrowed force field (the
	// helicopter focal-wind pool); null leaves ordinary effects unchanged.
	void advance_simulation_tick(double p_delta,
			const opennova::particle::ParticleForceField *p_forces);
	// Explicit GameWorld device leg. Attachment poses and the
	// immutable draw list are refreshed once at the pipeline's chosen point;
	// particles never advance on render delta.
	void render_frame(int64_t p_time_ms);
	// Value-only F3 read model (particle/effect_group_report.h). Emitter ids
	// join portable simulation values to the renderer's draw list bounds; no
	// particle/render Nodes escape this facade. Hidden particles report
	// nothing unless `include_hidden` (the GUT pins over the retail master
	// switch read the suppressed groups through it); each row carries the
	// spawn's `owner_key` (null for unowned groups).
	TypedArray<EffectGroupReport> get_debug_group_report(bool p_include_hidden = false);
	// Unresolved names describe the loaded atlas catalog as a whole, not any
	// one live group; keeping them here prevents one unrelated missing frame
	// from falsely implicating every effect.
	PackedStringArray get_unresolved_texture_names();

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	ParticleRenderer *_renderer() const;
	ParticleRenderer *_ensure_renderer();
	Node *_environment_source() const;
	Camera3D *_reflection_camera() const;
	void _open_files();
	int64_t _allocate_token();
	int64_t _slot_token_for(const Variant &p_key);
	int64_t _owner_token_for(const Variant &p_key);
	void _seed_owner_pose(int64_t p_owner_token, const Transform3D &p_transform);
	Ref<EffectSpawnReceipt> _disabled_receipt() const;
	void _sync_owner_poses(bool p_refresh_frame);

	TypedArray<ParticleFile> files_;
	Ref<EffectLoadReport> load_report_;
	Ref<EffectScene> scene_;
	ObjectID renderer_id_;
	Ref<ResourceRoot> root_;
	Callable texture_provider_;
	String texture_dir_;
	ObjectID environment_source_id_;
	Callable owner_position_provider_;
	float water_height_ = 0.0f;
	ObjectID reflection_camera_id_;
	ObjectID second_scene_camera_id_;
	bool particles_disabled_ = false;

	// Keys never become native tokens by hashing. A shared monotonic
	// allocator and separate maps give the same Variant distinct slot and
	// owner identities.
	HashMap<Variant, int64_t, VariantHasher, VariantComparator> slot_tokens_;
	HashMap<Variant, int64_t, VariantHasher, VariantComparator> owner_tokens_;
	HashMap<int64_t, Variant> owner_keys_by_token_;
	HashMap<int64_t, Transform3D> owner_pose_cache_;
	// The one pose-update batch, cleared and refilled per push so the
	// per-frame owner sync allocates nothing once warm.
	EffectOwnerPoseBatch owner_pose_batch_;
	int64_t next_token_ = 1;
};

} // namespace godot
