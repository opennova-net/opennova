#include "audio/sound_bank.h"

#include "audio/audio_out.h"
#include "lwf/lwf_data.h"
#include "resource_index/resource_root.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <base/gameprofile/resource_missing.h>
#include <base/io/fixed.h>
#include <formats/lwf/wav_pcm.h>

#include <cmath>

using namespace godot;

namespace {

const opennova::lwf::File &empty_bank() {
	static const opennova::lwf::File empty;
	return empty;
}

} // namespace

void SoundBank::_bind_methods() {
	ClassDB::bind_static_method("SoundBank", D_METHOD("create", "resource_root"), &SoundBank::create);
	ClassDB::bind_method(D_METHOD("set_occlusion_provider", "provider"), &SoundBank::set_occlusion_provider);
	ClassDB::bind_method(D_METHOD("set_occlusion_override", "override"), &SoundBank::set_occlusion_override);
	ClassDB::bind_method(D_METHOD("add_bank", "lwf"), &SoundBank::add_bank);
	ClassDB::bind_method(D_METHOD("has_set", "name"), &SoundBank::has_set);
	ClassDB::bind_method(D_METHOD("get_set_names"), &SoundBank::get_set_names);
	BIND_CONSTANT(VOLUME_BYTE_MAX);
}

Ref<SoundBank> SoundBank::create(const Ref<ResourceRoot> &p_resource_root) {
	Ref<SoundBank> bank;
	bank.instantiate();
	bank->resource_root_ = p_resource_root;
	return bank;
}

void SoundBank::set_occlusion_provider(const Ref<Simulation> &p_provider) {
	occlusion_provider_id_ = p_provider.is_valid() ? ObjectID(p_provider->get_instance_id()) : ObjectID();
}

void SoundBank::set_occlusion_override(const Callable &p_override) {
	occlusion_override_ = p_override;
}

void SoundBank::add_bank(const Ref<LwfData> &p_lwf) {
	if (p_lwf.is_null() || !p_lwf->is_loaded()) {
		return;
	}
	const int32_t bank_index = static_cast<int32_t>(banks_.size());
	banks_.push_back(p_lwf->engine_file());
	index_.add_bank(bank_index, banks_.back());
}

bool SoundBank::has_set(const String &p_name) const {
	return _find_set(p_name).valid();
}

PackedStringArray SoundBank::get_set_names() const {
	PackedStringArray out;
	for (const std::string &name : index_.names()) {
		out.push_back(opennova::to_gd(name));
	}
	return out;
}

std::vector<opennova::audio::AmbientLayerRow> SoundBank::ambient_layer_rows(
		const std::string &p_name) const {
	const opennova::audio::SetLocation loc = index_.find(p_name);
	if (!loc.valid()) {
		return {};
	}
	const opennova::lwf::File &bank = _bank_at(loc);
	const opennova::lwf::Multi &set = bank.multis[static_cast<size_t>(loc.set)];
	const auto has_wave = [this](const std::string &p_wave) {
		return resource_root_.is_valid() &&
				resource_root_->has_file(opennova::to_gd(p_wave).get_file());
	};
	return opennova::audio::ambient_layer_rows(bank, set, has_wave);
}

opennova::audio::MixWaveRef SoundBank::resolve_wave(const String &p_wav_path) {
	if (p_wav_path.is_empty()) {
		return nullptr;
	}
	const std::string name = opennova::to_std(p_wav_path.get_file().to_lower());
	if (const auto cached = waves_.find(name); cached != waves_.end()) {
		return cached->second;
	}
	opennova::audio::MixWaveRef wave;
	if (resource_root_.is_valid()) {
		const PackedByteArray bytes = resource_root_->read_file(p_wav_path.get_file());
		opennova::lwf::WavPcm pcm;
		std::string error;
		if (!bytes.is_empty()) {
			if (opennova::lwf::wav_decode_pcm16(bytes.ptr(), static_cast<size_t>(bytes.size()), pcm, error)) {
				wave = opennova::audio::mix_wave_from_pcm(pcm);
			} else {
				UtilityFunctions::push_warning("SoundBank: ", p_wav_path.get_file(), ": ", opennova::to_gd(error));
			}
		} else if (!resource_root_->has_file(p_wav_path.get_file())) {
			// A wave a bank's member names that is not there: the log line the editor's Play reads back into
			// a Problems row on the bank naming it (ADR 0046 DI-27).
			ResourceRoot::report_missing(opennova::gameprofile::resource_kind::kWave, p_wav_path.get_file(), String(),
					"the member that names it plays nothing");
		}
	}
	waves_[name] = wave;
	return wave;
}

int64_t SoundBank::occlusion_trampoline(void *p_ctx, const float p_listener[3],
		const float p_source[3], int64_t p_dist_q16, int64_t p_source_id) {
	FireContext *ctx = static_cast<FireContext *>(p_ctx);
	const Vector3 listener(p_listener[0], p_listener[1], p_listener[2]);
	const Vector3 source(p_source[0], p_source[1], p_source[2]);
	if (ctx->sim != nullptr) {
		return ctx->sim->sound_occlusion_distance_q16(listener, source, p_dist_q16,
				static_cast<int>(p_source_id));
	}
	if (ctx->override != nullptr && ctx->override->is_valid()) {
		return static_cast<int64_t>(ctx->override->call(listener, source, p_dist_q16,
				static_cast<int>(p_source_id)));
	}
	return p_dist_q16;
}

uint8_t SoundBank::listener_view_flags() const {
	const auto *sim = Object::cast_to<Simulation>(ObjectDB::get_instance(occlusion_provider_id_));
	return sim != nullptr ? sim->sound_listener_view_flags() : opennova::audio::kListenerViewStartup;
}

bool SoundBank::play_oneshot_3d(const Vector3 &p_world_pos, const String &p_name,
		const Vector3 &p_listener_pos, const Hearing &p_hearing, int p_source_bms_id, int p_sound_id) {
	const opennova::audio::SetLocation loc = _find_set(p_name);
	if (!loc.valid()) {
		_note_missing_set(p_name);
		return false;
	}
	const opennova::lwf::File &bank = _bank_at(loc);
	const bool has_listener = p_listener_pos.is_finite();
	const float world[3] = { static_cast<float>(p_world_pos.x), static_cast<float>(p_world_pos.y),
		static_cast<float>(p_world_pos.z) };
	const float listener[3] = { static_cast<float>(p_listener_pos.x),
		static_cast<float>(p_listener_pos.y), static_cast<float>(p_listener_pos.z) };
	FireContext ctx;
	ctx.sim = Object::cast_to<Simulation>(ObjectDB::get_instance(occlusion_provider_id_));
	ctx.override = &occlusion_override_;
	const bool has_provider = ctx.sim != nullptr || occlusion_override_.is_valid();
	const int32_t emitter_volume =
			opennova::audio::scaled_volume(opennova::audio::kVolumeByteMax, AudioOut::options().sound_fx());
	const opennova::audio::OneshotPlan plan = opennova::audio::plan_oneshot_3d(bank, loc, world,
			listener, has_listener, p_source_bms_id, static_cast<uint32_t>(p_sound_id),
			has_provider ? &SoundBank::occlusion_trampoline : nullptr, &ctx, selector_, listener_view_flags(),
			emitter_volume);
	// The bearing pan from the sound layer's listener, the source in the mission frame's 16.16
	// units (presentation (x, y, z) is mission (x, -z, y)).
	const uint8_t pan = p_hearing.listener == nullptr ? 0
			: p_hearing.listener->pan_of(int32_t(world[0] * opennova::io::kFp16One),
					int32_t(-world[2] * opennova::io::kFp16One), p_hearing.ridden);
	return _play_plan(bank, plan, pan);
}

bool SoundBank::play_oneshot_at_distance(const String &p_name, int64_t p_dist_q16, uint8_t p_bearing) {
	const opennova::audio::SetLocation loc = _find_set(p_name);
	if (!loc.valid()) {
		_note_missing_set(p_name);
		return false;
	}
	const opennova::lwf::File &bank = _bank_at(loc);
	// The emitter volume the option word (runtime/audio/sound_options.h).
	const auto plan = opennova::audio::plan_oneshot_at_distance(bank, loc, p_dist_q16, selector_,
			listener_view_flags(), true, AudioOut::options().sound_fx());
	return _play_plan(bank, plan, p_bearing);
}

bool SoundBank::play_interface_oneshot(const String &p_name) {
	const auto loc = _find_set(p_name);
	if (!loc.valid()) {
		_note_missing_set(p_name);
		return false;
	}
	const auto &bank = _bank_at(loc);
	// Distance 0, pan 0, the option word's emitter volume (runtime/audio/sound_options.h).
	const auto plan = opennova::audio::plan_oneshot_at_distance(bank, loc, 0, selector_,
			listener_view_flags(), true, AudioOut::options().sound_fx());
	return _play_plan(bank, plan, 0);
}

bool SoundBank::_play_plan(const opennova::lwf::File &p_bank, const opennova::audio::OneshotPlan &p_plan,
		uint8_t p_pan) {
	return opennova::audio::open_oneshot_plan(AudioOut::mixer(), p_plan, p_pan,
			[this, &p_bank](const opennova::audio::OneshotVoice &voice) {
				return resolve_wave(_member_wav_path(p_bank, p_bank.sndparms[voice.sndparm]));
			});
}

std::optional<opennova::world::ScriptVoiceChannel::SetSelection>
SoundBank::select_radio_set(const std::string &name, uint8_t listener_view_flags,
		bool p_bank_member) {
	const auto loc = index_.find(name);
	if (!loc.valid()) return std::nullopt;
	const auto &bank = _bank_at(loc);
	return p_bank_member
			? opennova::audio::select_entity_voice(bank, loc, selector_, listener_view_flags)
			: opennova::audio::select_radio_voice(bank, loc, selector_, listener_view_flags);
}

// --- Internals ---

opennova::audio::SetLocation SoundBank::_find_set(const String &p_name) const {
	return index_.find(opennova::to_std(p_name));
}

void SoundBank::_note_missing_set(const String &p_name) const {
	if (p_name.is_empty() || banks_.empty()) {
		return;
	}
	ResourceRoot::report_missing(opennova::gameprofile::resource_kind::kSound, p_name, String(),
			"no sound bank the game loaded holds it, so it plays nothing");
}

const opennova::lwf::File &SoundBank::_bank_at(const opennova::audio::SetLocation &p_loc) const {
	if (!p_loc.valid() || static_cast<size_t>(p_loc.bank) >= banks_.size()) {
		return empty_bank();
	}
	return banks_[static_cast<size_t>(p_loc.bank)];
}

String SoundBank::_member_wav_path(const opennova::lwf::File &p_bank,
		const opennova::lwf::Sndparm &p_member) {
	if (p_member.single_index >= p_bank.singles.size()) {
		return String();
	}
	return opennova::to_gd(p_bank.singles[p_member.single_index].path);
}
