#include "audio/ambient_mixer.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/audio/bank_chain.h>
#include "simulation/simulation.h"

#include <string>
#include <utility>
#include <vector>

using namespace godot;

void AmbientMixer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear"), &AmbientMixer::clear);
	ClassDB::bind_method(D_METHOD("set_occlusion_provider", "provider"),
			&AmbientMixer::set_occlusion_provider);
	ClassDB::bind_method(D_METHOD("set_occlusion_override", "override"),
			&AmbientMixer::set_occlusion_override);
	ClassDB::bind_method(
			D_METHOD("add_marker", "pos", "source_bms_id", "stagger_slot",
					"lifetime_ticks", "slot_keys", "sets"),
			&AmbientMixer::add_marker);
	ClassDB::bind_method(
			D_METHOD("register_emitter", "source_spawn_id", "lane", "pos",
					"source_bms_id", "lifetime_ticks", "pitch_q16",
					"volume_q8_8", "layers"),
			&AmbientMixer::register_emitter);
	ClassDB::bind_method(
			D_METHOD("update_emitter_source", "source_spawn_id", "pos",
					"source_bms_id"),
			&AmbientMixer::update_emitter_source);
	ClassDB::bind_method(D_METHOD("set_time_of_day_hours", "hours"),
			&AmbientMixer::set_time_of_day_hours);
	ClassDB::bind_method(D_METHOD("advance_to_tick", "tick"),
			&AmbientMixer::advance_to_tick);
	ClassDB::bind_method(D_METHOD("advance_seconds", "dt"),
			&AmbientMixer::advance_seconds);
	ClassDB::bind_method(D_METHOD("mix", "listener"), &AmbientMixer::mix);
	ClassDB::bind_method(D_METHOD("clock_tick"), &AmbientMixer::clock_tick);
	ClassDB::bind_method(D_METHOD("marker_count"), &AmbientMixer::marker_count);
	ClassDB::bind_static_method("AmbientMixer",
			D_METHOD("calc_distance_volume", "dist_q16", "radius_q16", "vol255",
					"clamp_vol"),
			&AmbientMixer::calc_distance_volume);
	ClassDB::bind_static_method("AmbientMixer",
			D_METHOD("emitter_layer_volume", "dist_q16", "falloff_u", "min_u",
					"vol_byte", "member_vol", "clamp_vol"),
			&AmbientMixer::emitter_layer_volume);
	ClassDB::bind_static_method("AmbientMixer",
			D_METHOD("oneshot_layer_volume", "dist_q16", "min_q16",
					"falloff_q16", "member_vol", "clamp_vol"),
			&AmbientMixer::oneshot_layer_volume);
	ClassDB::bind_static_method("AmbientMixer",
			D_METHOD("crossfade_volume_byte", "blend"),
			&AmbientMixer::crossfade_volume_byte);
	ClassDB::bind_static_method("AmbientMixer",
			D_METHOD("time_of_day_region", "hours"),
			&AmbientMixer::time_of_day_region);
	ClassDB::bind_static_method("AmbientMixer",
			D_METHOD("global_bank_chain", "expansion_name"),
			&AmbientMixer::global_bank_chain);
}

PackedStringArray AmbientMixer::global_bank_chain(
		const String &expansion_name) {
	PackedStringArray out;
	for (const std::string &name : opennova::audio::global_bank_chain(
			std::string(expansion_name.utf8().get_data())))
		out.append(String(name.c_str()));
	return out;
}

void AmbientMixer::clear() {
	mixer_.clear();
	sim_ = nullptr;
	occlusion_override_ = Callable();
	provider_id_ = ObjectID();
}

void AmbientMixer::set_occlusion_provider(Simulation *provider) {
	provider_id_ = provider != nullptr ? provider->get_instance_id() : ObjectID();
	sim_ = nullptr;
}

void AmbientMixer::set_occlusion_override(const Callable &override) {
	occlusion_override_ = override;
}

int AmbientMixer::add_marker(const Vector3 &pos, int64_t source_bms_id,
		int stagger_slot, int lifetime_ticks, const PackedInt32Array &slot_keys,
		const Array &sets) {
	std::vector<std::vector<opennova::audio::AmbientMixer::LayerDesc>> native_sets;
	native_sets.reserve(static_cast<size_t>(sets.size()));
	for (int si = 0; si < sets.size(); ++si) {
		const PackedInt32Array layers = sets[si];
		std::vector<opennova::audio::AmbientMixer::LayerDesc> native_layers;
		native_layers.reserve(static_cast<size_t>(layers.size() / 5));
		for (int base = 0; base + 5 <= layers.size(); base += 5) {
			opennova::audio::AmbientMixer::LayerDesc ld;
			ld.candidate_id = layers[base + 0];
			ld.falloff_u = layers[base + 1];
			ld.min_u = layers[base + 2];
			ld.member_vol = layers[base + 3];
			ld.clamp_vol = layers[base + 4];
			native_layers.push_back(ld);
		}
		native_sets.push_back(std::move(native_layers));
	}
	int32_t keys[4] = { -1, -1, -1, -1 };
	for (int i = 0; i < 4 && i < slot_keys.size(); ++i) {
		keys[i] = slot_keys[i];
	}
	const float p[3] = { static_cast<float>(pos.x), static_cast<float>(pos.y),
		static_cast<float>(pos.z) };
	return mixer_.add_marker(p, source_bms_id, stagger_slot, lifetime_ticks, keys,
			std::move(native_sets));
}

void AmbientMixer::register_emitter(int64_t source_spawn_id, int lane,
		const Vector3 &pos, int64_t source_bms_id, int lifetime_ticks,
		int pitch_q16, int volume_q8_8, const PackedInt32Array &layers) {
	std::vector<opennova::audio::AmbientMixer::LayerDesc> native_layers;
	native_layers.reserve(static_cast<size_t>(layers.size() / 5));
	for (int base = 0; base + 5 <= layers.size(); base += 5) {
		opennova::audio::AmbientMixer::LayerDesc ld;
		ld.candidate_id = layers[base + 0];
		ld.falloff_u = layers[base + 1];
		ld.min_u = layers[base + 2];
		ld.member_vol = layers[base + 3];
		ld.clamp_vol = layers[base + 4];
		native_layers.push_back(ld);
	}
	const float p[3] = { static_cast<float>(pos.x), static_cast<float>(pos.y),
		static_cast<float>(pos.z) };
	mixer_.register_emitter(static_cast<uint64_t>(source_spawn_id), lane, p,
			source_bms_id, lifetime_ticks, pitch_q16, volume_q8_8,
			std::move(native_layers));
}

void AmbientMixer::update_emitter_source(int64_t source_spawn_id,
		const Vector3 &pos, int64_t source_bms_id) {
	const float p[3] = { static_cast<float>(pos.x), static_cast<float>(pos.y),
		static_cast<float>(pos.z) };
	mixer_.update_emitter_source(static_cast<uint64_t>(source_spawn_id), p,
			source_bms_id);
}

void AmbientMixer::set_time_of_day_hours(float hours) {
	mixer_.set_time_of_day_hours(hours);
}

void AmbientMixer::advance_to_tick(int64_t tick) {
	mixer_.advance_to_tick(tick);
}

void AmbientMixer::advance_seconds(float dt) {
	mixer_.advance_seconds(dt);
}

int64_t AmbientMixer::occlusion_trampoline(void *ctx, const float listener[3],
		const float source[3], int64_t dist_q16, int64_t source_id) {
	AmbientMixer *self = static_cast<AmbientMixer *>(ctx);
	const Vector3 l(listener[0], listener[1], listener[2]);
	const Vector3 s(source[0], source[1], source[2]);
	if (self->sim_ != nullptr) {
		return self->sim_->sound_occlusion_distance_q16(l, s, dist_q16,
				static_cast<int>(source_id));
	}
	if (self->occlusion_override_.is_valid()) {
		return static_cast<int64_t>(self->occlusion_override_.call(l, s, dist_q16,
				static_cast<int>(source_id)));
	}
	return dist_q16;
}

PackedFloat32Array AmbientMixer::mix(const Vector3 &listener) {
	const float l[3] = { static_cast<float>(listener.x),
		static_cast<float>(listener.y), static_cast<float>(listener.z) };
	// Resolve the provider fresh each mix: a freed provider silently degrades to
	// the unoccluded mix instead of dangling.
	sim_ = Object::cast_to<Simulation>(ObjectDB::get_instance(provider_id_));
	const bool has_provider = sim_ != nullptr || occlusion_override_.is_valid();
	const std::vector<opennova::audio::AmbientCandidate> &out = mixer_.mix(
			l, has_provider ? &AmbientMixer::occlusion_trampoline : nullptr, this);
	PackedFloat32Array rows;
	rows.resize(static_cast<int64_t>(out.size()) * 6);
	float *w = rows.ptrw();
	for (const opennova::audio::AmbientCandidate &c : out) {
		*w++ = static_cast<float>(c.candidate_id);
		*w++ = static_cast<float>(c.vol);
		*w++ = static_cast<float>(c.pitch_q16);
		*w++ = c.pos[0];
		*w++ = c.pos[1];
		*w++ = c.pos[2];
	}
	return rows;
}

int AmbientMixer::live_slot_count() const {
	return mixer_.live_slot_count();
}

int64_t AmbientMixer::clock_tick() const {
	return mixer_.clock_tick();
}

int AmbientMixer::marker_count() const {
	return mixer_.marker_count();
}

int AmbientMixer::calc_distance_volume(int64_t dist_q16, int64_t radius_q16,
		int vol255, int clamp_vol) {
	return opennova::audio::calc_distance_volume(dist_q16, radius_q16, vol255,
			clamp_vol);
}

int AmbientMixer::emitter_layer_volume(int64_t dist_q16, int falloff_u,
		int min_u, int vol_byte, int member_vol, int clamp_vol) {
	return opennova::audio::emitter_layer_volume(dist_q16, falloff_u, min_u, vol_byte,
			member_vol, clamp_vol);
}

int AmbientMixer::oneshot_layer_volume(int64_t dist_q16, int64_t min_q16,
		int64_t falloff_q16, int member_vol, int clamp_vol) {
	return opennova::audio::oneshot_layer_volume(dist_q16, min_q16, falloff_q16,
			member_vol, clamp_vol);
}

int AmbientMixer::crossfade_volume_byte(float blend) {
	return opennova::audio::crossfade_volume_byte(blend);
}

Dictionary AmbientMixer::time_of_day_region(float hours) {
	const opennova::audio::TimeOfDayRegion tod =
			opennova::audio::time_of_day_region(hours);
	Dictionary d;
	d["region"] = tod.region;
	d["adjacent"] = tod.adjacent;
	d["blend"] = tod.blend;
	return d;
}
