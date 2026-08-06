#include "audio/nova_ambient_mixer.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "simulation/nova_simulation.h"

#include <utility>
#include <vector>

using namespace godot;

void NovaAmbientMixer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear"), &NovaAmbientMixer::clear);
	ClassDB::bind_method(D_METHOD("set_occlusion_provider", "provider"),
			&NovaAmbientMixer::set_occlusion_provider);
	ClassDB::bind_method(
			D_METHOD("add_marker", "pos", "source_bms_id", "stagger_slot",
					"lifetime_ticks", "slot_keys", "sets"),
			&NovaAmbientMixer::add_marker);
	ClassDB::bind_method(
			D_METHOD("register_emitter", "source_spawn_id", "lane", "pos",
					"source_bms_id", "lifetime_ticks", "pitch_q16",
					"volume_q8_8", "layers"),
			&NovaAmbientMixer::register_emitter);
	ClassDB::bind_method(
			D_METHOD("update_emitter_source", "source_spawn_id", "pos",
					"source_bms_id"),
			&NovaAmbientMixer::update_emitter_source);
	ClassDB::bind_method(D_METHOD("set_time_of_day_hours", "hours"),
			&NovaAmbientMixer::set_time_of_day_hours);
	ClassDB::bind_method(D_METHOD("advance_to_tick", "tick"),
			&NovaAmbientMixer::advance_to_tick);
	ClassDB::bind_method(D_METHOD("advance_seconds", "dt"),
			&NovaAmbientMixer::advance_seconds);
	ClassDB::bind_method(D_METHOD("mix", "listener"), &NovaAmbientMixer::mix);
	ClassDB::bind_method(D_METHOD("mix_v2", "listener"), &NovaAmbientMixer::mix_v2);
	ClassDB::bind_method(D_METHOD("live_slot_count"),
			&NovaAmbientMixer::live_slot_count);
	ClassDB::bind_method(D_METHOD("clock_tick"), &NovaAmbientMixer::clock_tick);
	ClassDB::bind_method(D_METHOD("marker_count"), &NovaAmbientMixer::marker_count);
	ClassDB::bind_static_method("NovaAmbientMixer",
			D_METHOD("calc_distance_volume", "dist_q16", "radius_q16", "vol255",
					"clamp_vol"),
			&NovaAmbientMixer::calc_distance_volume);
	ClassDB::bind_static_method("NovaAmbientMixer",
			D_METHOD("emitter_layer_volume", "dist_q16", "falloff_u", "min_u",
					"vol_byte", "member_vol", "clamp_vol"),
			&NovaAmbientMixer::emitter_layer_volume);
	ClassDB::bind_static_method("NovaAmbientMixer",
			D_METHOD("crossfade_volume_byte", "blend"),
			&NovaAmbientMixer::crossfade_volume_byte);
	ClassDB::bind_static_method("NovaAmbientMixer",
			D_METHOD("time_of_day_region", "hours"),
			&NovaAmbientMixer::time_of_day_region);
}

void NovaAmbientMixer::clear() {
	mixer_.clear();
	sim_ = nullptr;
	duck_ = nullptr;
	provider_id_ = ObjectID();
}

void NovaAmbientMixer::set_occlusion_provider(Object *provider) {
	provider_id_ = provider != nullptr ? provider->get_instance_id() : ObjectID();
	sim_ = nullptr;
	duck_ = nullptr;
}

int NovaAmbientMixer::add_marker(const Vector3 &pos, int64_t source_bms_id,
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

void NovaAmbientMixer::register_emitter(int64_t source_spawn_id, int lane,
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

void NovaAmbientMixer::update_emitter_source(int64_t source_spawn_id,
		const Vector3 &pos, int64_t source_bms_id) {
	const float p[3] = { static_cast<float>(pos.x), static_cast<float>(pos.y),
		static_cast<float>(pos.z) };
	mixer_.update_emitter_source(static_cast<uint64_t>(source_spawn_id), p,
			source_bms_id);
}

void NovaAmbientMixer::set_time_of_day_hours(float hours) {
	mixer_.set_time_of_day_hours(hours);
}

void NovaAmbientMixer::advance_to_tick(int64_t tick) {
	mixer_.advance_to_tick(tick);
}

void NovaAmbientMixer::advance_seconds(float dt) {
	mixer_.advance_seconds(dt);
}

int64_t NovaAmbientMixer::occlusion_trampoline(void *ctx, const float listener[3],
		const float source[3], int64_t dist_q16, int64_t source_id) {
	NovaAmbientMixer *self = static_cast<NovaAmbientMixer *>(ctx);
	const Vector3 l(listener[0], listener[1], listener[2]);
	const Vector3 s(source[0], source[1], source[2]);
	if (self->sim_ != nullptr) {
		return self->sim_->sound_occlusion_distance_q16(l, s, dist_q16,
				static_cast<int>(source_id));
	}
	if (self->duck_ != nullptr) {
		return static_cast<int64_t>(self->duck_->call("sound_occlusion_distance_q16",
				l, s, dist_q16, static_cast<int>(source_id)));
	}
	return dist_q16;
}

PackedFloat32Array NovaAmbientMixer::mix(const Vector3 &listener) {
	return mix_rows(listener, false);
}

PackedFloat32Array NovaAmbientMixer::mix_v2(const Vector3 &listener) {
	return mix_rows(listener, true);
}

PackedFloat32Array NovaAmbientMixer::mix_rows(const Vector3 &listener,
		bool include_pitch) {
	const float l[3] = { static_cast<float>(listener.x),
		static_cast<float>(listener.y), static_cast<float>(listener.z) };
	// Resolve the provider fresh each mix: a freed provider silently degrades to
	// the unoccluded mix instead of dangling.
	Object *provider = ObjectDB::get_instance(provider_id_);
	sim_ = Object::cast_to<NovaSimulation>(provider);
	duck_ = (sim_ == nullptr && provider != nullptr &&
					provider->has_method("sound_occlusion_distance_q16"))
			? provider
			: nullptr;
	const bool has_provider = sim_ != nullptr || duck_ != nullptr;
	const std::vector<opennova::audio::AmbientCandidate> &out = mixer_.mix(
			l, has_provider ? &NovaAmbientMixer::occlusion_trampoline : nullptr, this);
	const int stride = include_pitch ? 6 : 5;
	PackedFloat32Array rows;
	rows.resize(static_cast<int64_t>(out.size()) * stride);
	float *w = rows.ptrw();
	for (const opennova::audio::AmbientCandidate &c : out) {
		*w++ = static_cast<float>(c.candidate_id);
		*w++ = static_cast<float>(c.vol);
		if (include_pitch) {
			*w++ = static_cast<float>(c.pitch_q16);
		}
		*w++ = c.pos[0];
		*w++ = c.pos[1];
		*w++ = c.pos[2];
	}
	return rows;
}

int NovaAmbientMixer::live_slot_count() const {
	return mixer_.live_slot_count();
}

int64_t NovaAmbientMixer::clock_tick() const {
	return mixer_.clock_tick();
}

int NovaAmbientMixer::marker_count() const {
	return mixer_.marker_count();
}

int NovaAmbientMixer::calc_distance_volume(int64_t dist_q16, int64_t radius_q16,
		int vol255, int clamp_vol) {
	return opennova::audio::calc_distance_volume(dist_q16, radius_q16, vol255,
			clamp_vol);
}

int NovaAmbientMixer::emitter_layer_volume(int64_t dist_q16, int falloff_u,
		int min_u, int vol_byte, int member_vol, int clamp_vol) {
	return opennova::audio::emitter_layer_volume(dist_q16, falloff_u, min_u, vol_byte,
			member_vol, clamp_vol);
}

int NovaAmbientMixer::crossfade_volume_byte(float blend) {
	return opennova::audio::crossfade_volume_byte(blend);
}

Dictionary NovaAmbientMixer::time_of_day_region(float hours) {
	const opennova::audio::TimeOfDayRegion tod =
			opennova::audio::time_of_day_region(hours);
	Dictionary d;
	d["region"] = tod.region;
	d["adjacent"] = tod.adjacent;
	d["blend"] = tod.blend;
	return d;
}
