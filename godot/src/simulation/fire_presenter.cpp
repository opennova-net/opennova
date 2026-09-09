#include "simulation/fire_presenter.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/renderer/tracer_frame.h>

#include "audio/mission_audio.h"
#include "lights/effect_light_director.h"
#include "particle/effect_world.h"
#include "simulation/entity_presenter.h"
#include "simulation/simulation.h"
#include "util/axes.h"

namespace godot {

FirePresenter::FirePresenter(EntityPresenter *p_owner) :
		owner_(p_owner) {}

Simulation *FirePresenter::sim() const {
	return sim_id_.is_valid()
			? Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_))
			: nullptr;
}

MissionAudio *FirePresenter::audio() const {
	return audio_id_.is_valid()
			? Object::cast_to<MissionAudio>(ObjectDB::get_instance(audio_id_))
			: nullptr;
}

EffectWorld *FirePresenter::fx() const {
	return fx_id_.is_valid()
			? Object::cast_to<EffectWorld>(ObjectDB::get_instance(fx_id_))
			: nullptr;
}

EffectLightDirector *FirePresenter::lights() const {
	return lights_id_.is_valid()
			? Object::cast_to<EffectLightDirector>(ObjectDB::get_instance(lights_id_))
			: nullptr;
}

void FirePresenter::free_mesh_instance() {
	if (mesh_instance_id_.is_valid()) {
		Node *instance = Object::cast_to<Node>(ObjectDB::get_instance(mesh_instance_id_));
		if (instance != nullptr) {
			instance->queue_free();
		}
	}
	mesh_instance_id_ = ObjectID();
	mesh_.unref();
}

void FirePresenter::setup(Simulation *p_sim, Node3D *p_container, MissionAudio *p_audio,
		EffectWorld *p_fx, EffectLightDirector *p_lights) {
	sim_id_ = p_sim != nullptr ? p_sim->get_instance_id() : ObjectID();
	audio_id_ = p_audio != nullptr ? p_audio->get_instance_id() : ObjectID();
	fx_id_ = p_fx != nullptr ? p_fx->get_instance_id() : ObjectID();
	lights_id_ = p_lights != nullptr ? p_lights->get_instance_id() : ObjectID();
	// A re-setup replaces the previous tracer geometry instead of stranding it.
	free_mesh_instance();
	if (p_container != nullptr) {
		mesh_.instantiate();
		MeshInstance3D *instance = memnew(MeshInstance3D);
		instance->set_name("FireTracers");
		instance->set_mesh(mesh_);
		instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		// Both families are unshaded vertex-colored, depth-tested (a world object,
		// not an overlay — walls occlude tracers), untextured (the witnessed B=0
		// ribbon writes no UVs). Additive family [orig: ONE:ONE, alpha unused,
		// fog-to-BLACK via SetFogAndBlendMode mode 2 @ CEffectChannel_RenderRibbon]:
		// vertex alpha rides at 1.0 so Godot's SRCALPHA:ONE add equals ONE:ONE;
		// fog off stands in for the fog-to-black leg (tracked in the RE record).
		mat_additive_.instantiate();
		mat_additive_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		mat_additive_->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		mat_additive_->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		mat_additive_->set_blend_mode(BaseMaterial3D::BLEND_MODE_ADD);
		mat_additive_->set_cull_mode(BaseMaterial3D::CULL_DISABLED); // [orig: pass cull-off]
		mat_additive_->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
		// Smoke family (rocket/at4/grenade) [orig: alpha blend + scene fog, mode 0].
		mat_alpha_.instantiate();
		mat_alpha_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		mat_alpha_->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		mat_alpha_->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		mat_alpha_->set_blend_mode(BaseMaterial3D::BLEND_MODE_MIX);
		mat_alpha_->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		p_container->add_child(instance);
		mesh_instance_id_ = instance->get_instance_id();
	}
}

void FirePresenter::teardown() {
	free_mesh_instance();
}

void FirePresenter::present() {
	Simulation *s = sim();
	if (s == nullptr) {
		return;
	}
	std::vector<opennova::world::FirePresentationRow> fires;
	s->drain_fire_presentation_rows(fires);
	present_fires(fires);
	std::vector<opennova::world::ReadyFireSound> fire_sounds;
	s->drain_fire_sounds(fire_sounds);
	present_fire_sounds(fire_sounds);
	std::vector<opennova::world::SoundSlotEvent> slot_sounds;
	s->drain_slot_sounds(slot_sounds);
	present_slot_sounds(slot_sounds);
	std::vector<opennova::world::SoundEmitterEvent> emitters;
	s->drain_sound_emitter_events(emitters);
	present_sound_emitters(emitters);
	draw_tracer_rows(s->get_tracer_trails());
}

// The body slot-sound drain (footsteps/foley/landing thumps/death screams): the
// sim resolves each entity's SndProf.def profile slot to its authored set name
// and emits the (foot-level) position; this plays them full-volume positional
// with NO propagation-delay leg — footsteps play immediately, unlike fire
// [orig: the odd/even-tick consumers call Entity_PlaySound3D_FullVolume
// @ 0x528e20 directly]. The local player's own body sounds DO play (retail
// plays your own steps; only fire has an action-slot presentation to defer to).
// Slots 43/44 (chute flap / freefall) refire every body tick by design; the
// exclusive key folds the refires into one continuous voice (D-SND-10).
void FirePresenter::present_slot_sounds(const std::vector<opennova::world::SoundSlotEvent> &p_events) {
	if (p_events.empty()) {
		return;
	}
	MissionAudio *audio_node = audio();
	if (audio_node == nullptr) {
		return;
	}
	for (const opennova::world::SoundSlotEvent &ev : p_events) {
		if (ev.set_name[0] == '\0') {
			continue;
		}
		const int slot = static_cast<int>(ev.slot);
		const int handle = static_cast<int>(ev.source_handle);
		String key;
		if (slot == SLOT_CHUTE_FLAP || slot == SLOT_FREEFALL) {
			key = vformat("%d:%d", handle, slot);
		}
		// Mission-frame 16.16 -> godot (x, z, -y), the fire drain's mapping.
		const Vector3 pos(static_cast<float>(ev.pos[0]) / 65536.0f,
				static_cast<float>(ev.pos[2]) / 65536.0f,
				static_cast<float>(-ev.pos[1]) / 65536.0f);
        const int source_id = sim() ? sim()->sound_source_bms_id(ev.source_handle) : 0;
		if (audio_node->slot_soundset(String(ev.set_name), pos, key, source_id)) {
			++stat_sounds_;
		}
	}
}

// Entity-attached loop registrations (vehicle idle/drive/reverse today) share
// the native ambient emitter table and its loudest-eight physical pool. The
// audio layer replays the bounded latest intents at their producer ticks before
// advancing to the end of a catch-up frame, preserving the 30-tick keep-alive.
// [orig: SoundEmitter_RegisterSetLayers @0x528340;
// SoundEmitter_UpdateAndMixTop8 @0x5284a0]
void FirePresenter::present_sound_emitters(
		const std::vector<opennova::world::SoundEmitterEvent> &p_events) {
	if (p_events.empty()) {
		return;
	}
	MissionAudio *audio_node = audio();
	if (audio_node == nullptr) {
		return;
	}
	audio_node->apply_sound_emitter_events(p_events);
}

void FirePresenter::present_fires(const std::vector<opennova::world::FirePresentationRow> &p_events) {
	if (p_events.empty()) {
		return;
	}
	EffectWorld *fx_world = fx();
	EffectLightDirector *light_director = lights();
	for (const opennova::world::FirePresentationRow &ev : p_events) {
		// The MF_Light muzzle glow re-arms per shot for EVERY shooter — retail
		// spawns it on both fire arms, the local player's included [orig:
		// WeaponSlot_FireAndSpawnEffects @ 0x53f597 at the fire position;
		// ActionSlot_SpawnEffect @ 0x402080 at the action-transform muzzle;
		// both gate on ammo +36 MF_Light]. Owner = shooter, so the witnessed
		// group gate scopes it to the shooter's own draws.
		if (light_director != nullptr && ev.mf_light != 0) {
			Vector3 glow_pos = mission_to_godot(ev.origin);
			if (ev.adm_arm) {
				const Vector3 glow_anchor = owner_->muzzle_world_for(
						ev.shooter_handle, String::utf8(ev.action_userpoint.c_str()));
				if (glow_anchor.is_finite()) {
					glow_pos = glow_anchor;
				}
			}
			light_director->on_muzzle_fire(ev.shooter_handle, glow_pos);
		}
		// The local player's own fire is presented by the action-slot legs
		// [orig: ActionSlot_ExecuteActionTick @ 0x541A70 routing]; everyone
		// else's rides the ammo-def legs below. (The SOUND legs of every arm
		// run in the sim now — world/fire_sound.h — and arrive through
		// drain_fire_sounds; this drain owns the EFFECT legs.)
		if (ev.is_local_player) {
			continue;
		}
		++stat_fires_;
		Vector3 origin = mission_to_godot(ev.origin);
		String effect = String::utf8(ev.effect.c_str());
		// THE ARM SPLIT. Retail's round-event receive path has two mutually exclusive
		// arms and only one of them is the ammo-def pair. The adm-indexed arm spawns
		// no ammo-def effect: it executes the ADDRESSED def's FIRE action row
		// instead, at that weapon's own userpoint on the gfx3 model.
		// This matters because the wire position is the shooter's EYE — retail sends
		// Position + CameraOffset [orig: Entity_CalcWeaponFirePosition @0x4dc750] — so
		// running the ammo-def leg on this arm draws every remote muzzle flash out of
		// the shooter's face, roughly a metre behind the barrel.
		// [orig: arms @0x42f521 / @0x42f6ce; ammo effect @0x42f6c2;
		//  the fire row @0x42f777 / @0x42f98f]
		if (ev.adm_arm) {
			effect = String::utf8(ev.action_effect.c_str());
			// The anchor: this shooter's held weapon, not the wire point — the
			// rendered gun's own userpoint, which is what retail spawns at (the
			// authority DECISION closing the S12a shadow seam: the rendered-node
			// anchor is permanent, the sim-posed re-derivation is gone). Falling
			// back to the wire eye position would reintroduce the very bug this
			// fixes, so an unresolvable anchor takes the provider's own
			// body-origin fallback — retail's deepest fallback is the entity
			// origin [orig: @0x401867..0x401887].
			const Vector3 anchored = owner_->muzzle_world_for(
					ev.shooter_handle, String::utf8(ev.action_userpoint.c_str()));
			if (anchored.is_finite()) {
				origin = anchored;
			}
		}
		if (fx_world != nullptr && !effect.is_empty()) {
			// The muzzle effect at the fire origin along the fire direction
			// [orig: the 56-B spawn descriptor -> CEffectWorld_SpawnEmitterAtPosition
			// @ 0x5F6DF0; every fire spawns one — no per-shooter guard on this leg].
			fx_world->spawn_effect(effect, origin, mission_to_godot(ev.forward));
			++stat_effects_;
		}
	}
}

// The sim's ready fire sounds: immediate near shots, the adm-arm action-row
// sets, and expired propagation-delayed slots, gated and counted down on the
// logic clock (world/fire_sound.h). Each row plays positionally; the set's
// max-range cull runs in the audio bank (D-AI-8).
// [orig: Entity_PlaySound3D_FullVolume @ 0x528e20 / Sound_TickPendingSlots
//  @ 0x529310]
void FirePresenter::present_fire_sounds(const std::vector<opennova::world::ReadyFireSound> &p_sounds) {
	if (p_sounds.empty()) {
		return;
	}
	MissionAudio *audio_node = audio();
	if (audio_node == nullptr) {
		return;
	}
	for (const opennova::world::ReadyFireSound &row : p_sounds) {
		audio_node->fire_soundset(String::utf8(row.set_name.c_str()), mission_to_godot(row.pos),
				row.source_bms_id);
		++stat_sounds_;
	}
}

// The compiled per-family ribbon strips [orig: CEffectChannel_RenderRibbon
// @ 0x5DB8A0 — the math and the style tables live natively in
// engine/runtime/renderer/tracer_frame.cpp]. This pass drains the sim's trail
// rows, hands them with the camera to the native compile (the row framing
// is [style_id, age, count, then count x (x, y, z, w)] per channel), and
// uploads each family's vertex run verbatim. The clear runs BEFORE the empty
// return: that is also what drops warm_pipelines' strips.
void FirePresenter::draw_tracer_rows(const PackedFloat32Array &p_rows) {
	if (mesh_.is_null()) {
		return;
	}
	mesh_->clear_surfaces();
	if (p_rows.is_empty()) {
		return;
	}
	Vector3 cam = owner_->listener_position();
	if (!cam.is_finite()) {
		cam = Vector3();
	}
	std::vector<opennova::renderer::TracerChannelInput> channels;
	const float *r = p_rows.ptr();
	const int64_t size = p_rows.size();
	int64_t i = 0;
	while (r != nullptr && i + 2 < size) {
		opennova::renderer::TracerChannelInput c;
		c.style_id = static_cast<int>(r[i]);
		c.age = static_cast<int>(r[i + 1]);
		c.count = static_cast<int>(r[i + 2]);
		i += 3;
		if (c.count <= 0 || i + static_cast<int64_t>(c.count) * 4 > size) {
			break;
		}
		c.points = r + i;
		i += static_cast<int64_t>(c.count) * 4;
		channels.push_back(c);
	}
	opennova::renderer::TracerRibbonFrame frame;
	opennova::renderer::compile_tracer_ribbons(channels.data(), channels.size(),
			{static_cast<float>(cam.x), static_cast<float>(cam.y), static_cast<float>(cam.z)},
			frame);
	stat_tracer_peak_ = MAX(stat_tracer_peak_, static_cast<int64_t>(frame.channels));
	emit_strip(frame.additive, mat_additive_);
	emit_strip(frame.alpha, mat_alpha_);
}

// One family's interleaved {x, y, z, r, g, b, a} run as one triangle strip.
void FirePresenter::emit_strip(const std::vector<float> &p_run,
		const Ref<StandardMaterial3D> &p_material) {
	const int64_t verts = static_cast<int64_t>(p_run.size() / 7);
	if (verts < 4) { // fewer than two pairs draws nothing
		return;
	}
	mesh_->surface_begin(Mesh::PRIMITIVE_TRIANGLE_STRIP, p_material);
	for (int64_t v = 0; v < verts; ++v) {
		const float *f = p_run.data() + v * 7;
		mesh_->surface_set_color(Color(f[3], f[4], f[5], f[6]));
		mesh_->surface_add_vertex(Vector3(f[0], f[1], f[2]));
	}
	mesh_->surface_end();
}

void FirePresenter::warm_pipelines(const Vector3 &p_position) {
	if (mesh_.is_null()) {
		return;
	}
	mesh_->clear_surfaces();
	for (const Ref<StandardMaterial3D> &mat : {mat_additive_, mat_alpha_}) {
		if (mat.is_null()) {
			continue;
		}
		mesh_->surface_begin(Mesh::PRIMITIVE_TRIANGLE_STRIP, mat);
		for (int i = 0; i < 4; ++i) {
			mesh_->surface_set_color(Color(1, 1, 1, 0.0f));
			mesh_->surface_add_vertex(p_position + Vector3(0.001f * i, 0, 0));
		}
		mesh_->surface_end();
	}
}

Ref<FirePresentStats> FirePresenter::get_stats() const {
	Ref<FirePresentStats> stats;
	stats.instantiate();
	stats->fires = stat_fires_;
	stats->sounds = stat_sounds_;
	stats->effects = stat_effects_;
	stats->tracer_peak = stat_tracer_peak_;
	return stats;
}

} // namespace godot
