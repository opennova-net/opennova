// SBF audio bank wrapper. The underlying parser is engine/formats/sbf
// (sbf_open_memory), which mirrors the engine's
// AudioVM_OpenContextFile @ 0x00672160 (jointops); the engine reads SBF
// archives via the same header + entry-table layout.

#include "audio/sbf_bank.h"
#include "audio/sbf_audio_stream.h"
#include "util/data_format.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace godot;
using namespace opennova::sbf;

SbfBank::SbfBank() {
	std::memset(&_arc, 0, sizeof(_arc));
}

SbfBank::~SbfBank() {
	if (_opened) {
		sbf_close(&_arc);
		_opened = false;
	}
}

void SbfBank::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_entry_count"), &SbfBank::get_entry_count);
	ClassDB::bind_method(D_METHOD("get_source_path"), &SbfBank::get_source_path);
	ClassDB::bind_method(D_METHOD("get_entries"), &SbfBank::get_entries);
	ClassDB::bind_method(D_METHOD("has_entry", "name"), &SbfBank::has_entry);
	ClassDB::bind_method(D_METHOD("get_entry_name", "index"), &SbfBank::get_entry_name);
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &SbfBank::load_from_path);
	ClassDB::bind_static_method("SbfBank", D_METHOD("create_empty"), &SbfBank::create_empty);
	ClassDB::bind_method(D_METHOD("get_raw_file_bytes"), &SbfBank::get_raw_file_bytes);
	ClassDB::bind_method(D_METHOD("set_entry_pcm", "index", "samples"), &SbfBank::set_entry_pcm);
	ClassDB::bind_method(D_METHOD("is_dirty"), &SbfBank::is_dirty);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &SbfBank::save_to_path);
	ClassDB::bind_method(D_METHOD("add_entry", "name", "samples"), &SbfBank::add_entry);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path",
								   PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT),
			"", "get_source_path");
}

void SbfBank::load_from_path(const String &p_path) {
	if (_opened) {
		sbf_close(&_arc);
		_opened = false;
	}
	source_path = p_path;
	_file_bytes = PackedByteArray();
	_model = SbfFile();
	_model_read = false;
	_modeled = false;
	_model_error.clear();
	_dirty = false;

	if (!read_nova_payload_file(p_path, _file_bytes)) {
		UtilityFunctions::push_warning("SbfBank: could not open ", p_path);
		return;
	}

	if (_file_bytes.size() <= 0) {
		UtilityFunctions::push_warning("SbfBank: empty file ", p_path);
		return;
	}
	if (sbf_open_memory(&_arc, _file_bytes.ptr(), (size_t)_file_bytes.size()) != 0) {
		UtilityFunctions::push_warning("SbfBank: sbf_open_memory failed for ", p_path);
		_file_bytes = PackedByteArray();
		return;
	}
	_opened = true;
}

bool SbfBank::_ensure_model() const {
	if (!_model_read) {
		_model_read = true;
		_model = SbfFile();
		_modeled = sbf_read_bank(_file_bytes.ptr(), (size_t)_file_bytes.size(), _model, _model_error);
	}
	if (!_modeled) {
		UtilityFunctions::push_warning("SbfBank: ", source_path, " has no model to edit or write: ", _model_error.c_str());
	}
	return _modeled;
}

Ref<SbfBank> SbfBank::create_empty() {
	Ref<SbfBank> bank;
	bank.instantiate();
	// Start from a zeroed archive: entries == nullptr is the valid empty state
	// (add_entry reallocs from null on the same allocator sbf_close frees with).
	// Stamp a coherent empty SBF header so any header reader sees a real bank,
	// matching the bytes sbf_encode_file would later write.
	std::memset(&bank->_arc, 0, sizeof(bank->_arc));
	bank->_arc.header.magic = SBF_MAGIC;
	bank->_arc.header.version = SBF_VERSION_DEFAULT;
	bank->_arc.header.flags = SBF_FLAGS_BYTE_PAIRED_STEREO; // matches sbf_encode_file
	bank->_arc.header.index_offset = SBF_HEADER_SIZE;
	bank->_arc.header.entry_count = 0;
	bank->_arc.entries = nullptr;
	bank->source_path = String();
	bank->_file_bytes = PackedByteArray();
	// The model of a bank made in code: no stream.
	bank->_model = SbfFile();
	bank->_model_read = true;
	bank->_modeled = true;
	// _opened lights up the mutation API; a new bank is unsaved.
	bank->_opened = true;
	bank->_dirty = true;
	return bank;
}

int SbfBank::get_entry_count() const {
	return _opened ? (int)_arc.header.entry_count : 0;
}

Array SbfBank::get_entries() const {
	Array out;
	if (!_opened) {
		return out;
	}
	for (uint32_t i = 0; i < _arc.header.entry_count; ++i) {
		const SbfRawEntry &e = _arc.entries[i];
		// SbfRawEntry.name is null-padded to SBF_NAME_SIZE; copy into a
		// guarded buffer so String() sees a real terminator.
		char name_buf[SBF_NAME_SIZE + 1] = { 0 };
		std::memcpy(name_buf, e.name, SBF_NAME_SIZE);

		Dictionary d;
		d["name"] = String(name_buf);
		d["total_size"] = (int64_t)e.total_size;
		d["block_size"] = (int64_t)e.block_size;
		d["sample_length_hint"] = (int64_t)e.sample_length_hint;
		out.append(d);
	}
	return out;
}

bool SbfBank::has_entry(const StringName &p_name) const {
	if (!_opened) {
		return false;
	}
	String s = String(p_name);
	return sbf_find_by_name(&_arc, s.utf8().get_data()) != nullptr;
}

String SbfBank::get_entry_name(int p_index) const {
	if (!_opened) {
		return String();
	}
	if (p_index < 0 || (uint32_t)p_index >= _arc.header.entry_count) {
		return String();
	}
	char name_buf[SBF_NAME_SIZE + 1] = { 0 };
	std::memcpy(name_buf, _arc.entries[p_index].name, SBF_NAME_SIZE);
	return String(name_buf);
}

const SbfRawEntry *SbfBank::raw_entry_at(int p_index) const {
	if (!_opened) {
		return nullptr;
	}
	if (p_index < 0 || (uint32_t)p_index >= _arc.header.entry_count) {
		return nullptr;
	}
	return &_arc.entries[p_index];
}

bool SbfBank::read_file_block(uint64_t p_offset, uint32_t p_size, PackedByteArray &r_block) const {
	r_block = PackedByteArray();
	if (p_size == 0) {
		return true;
	}
	if (p_offset > static_cast<uint64_t>(_file_bytes.size())) {
		return false;
	}
	const uint64_t available = static_cast<uint64_t>(_file_bytes.size()) - p_offset;
	if (static_cast<uint64_t>(p_size) > available) {
		return false;
	}
	r_block.resize(static_cast<int64_t>(p_size));
	std::memcpy(r_block.ptrw(), _file_bytes.ptr() + p_offset, p_size);
	return true;
}

Ref<SbfAudioStream> SbfBank::get_stream_at(int p_index) {
	if (!_opened) {
		return Ref<SbfAudioStream>();
	}
	if (p_index < 0 || (uint32_t)p_index >= _arc.header.entry_count) {
		return Ref<SbfAudioStream>();
	}
	Ref<SbfAudioStream> stream;
	stream.instantiate();
	stream->configure(this, p_index);
	return stream;
}

namespace {

// Floats in [-1, 1] as the encoder's int16, clamped.
std::vector<int16_t> int16_from(const PackedFloat32Array &p_samples) {
	std::vector<int16_t> out((size_t)p_samples.size());
	for (int64_t i = 0; i < p_samples.size(); ++i) {
		float f = p_samples[i];
		if (f < -1.0f) {
			f = -1.0f;
		}
		if (f > 1.0f) {
			f = 1.0f;
		}
		out[(size_t)i] = (int16_t)(f * 32767.0f);
	}
	return out;
}

} // namespace

Error SbfBank::set_entry_pcm(int p_index, const PackedFloat32Array &p_samples) {
	if (!_opened) {
		return ERR_UNCONFIGURED;
	}
	if (p_index < 0 || (uint32_t)p_index >= _arc.header.entry_count) {
		return ERR_INVALID_PARAMETER;
	}
	if (!_ensure_model() || (size_t)p_index >= _model.streams.size()) {
		return ERR_UNAVAILABLE;
	}
	// The stream's chunks encoded from the samples; its name and block size stay.
	const std::vector<int16_t> pcm = int16_from(p_samples);
	SbfStream &stream = _model.streams[(size_t)p_index];
	stream.chunks = sbf_encode_stream(stream.name, pcm.data(), pcm.size()).chunks;
	_dirty = true;
	return OK;
}

Error SbfBank::build_encoded_bytes(PackedByteArray &out) const {
	if (!_opened) {
		return ERR_UNCONFIGURED;
	}
	if (!_ensure_model()) {
		return ERR_UNAVAILABLE;
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!sbf_write_bank(_model, bytes, error)) {
		UtilityFunctions::push_warning("SbfBank: the bank cannot be written: ", error.c_str());
		return ERR_INVALID_DATA;
	}
	out = to_packed_bytes(bytes);
	return OK;
}

// --- Bank-edit ops ----------------------------------------------------------
//
// These mutate the in-memory entry table (the names and sizes the bank lists) and
// the bank's model (what the saver writes), the two kept in the same order.

Error SbfBank::add_entry(const String &p_name, const PackedFloat32Array &p_samples) {
	if (!_opened) {
		return ERR_UNCONFIGURED;
	}
	if (!_ensure_model()) {
		return ERR_UNAVAILABLE;
	}
	const uint32_t n = _arc.header.entry_count;
	const uint32_t new_n = n + 1;

	// realloc the libsbf-owned entries array. malloc/realloc were used by
	// sbf_open_memory so we stay on the same allocator.
	SbfRawEntry *grown = (SbfRawEntry *)std::realloc(
			_arc.entries, (size_t)new_n * sizeof(SbfRawEntry));
	if (grown == nullptr) {
		return ERR_OUT_OF_MEMORY;
	}
	_arc.entries = grown;

	SbfRawEntry &slot = _arc.entries[n];
	std::memset(&slot, 0, sizeof(SbfRawEntry));

	// Copy name (null-padded, max SBF_NAME_SIZE-1 to keep terminator safe).
	CharString utf8 = p_name.utf8();
	const char *src = utf8.get_data();
	const int src_len = (int)utf8.length();
	const int copy_len = src_len < (SBF_NAME_SIZE - 1) ? src_len : (SBF_NAME_SIZE - 1);
	if (copy_len > 0 && src != nullptr) {
		std::memcpy(slot.name, src, (size_t)copy_len);
	}

	// total_size / block_size: the writer lays the bank out from the model, so the
	// listed sizes only need to agree with the stream's chunk count. block_size =
	// SBF_CHUNK_TOTAL is the invariant every observed SBF holds.
	const int sample_count = p_samples.size();
	int chunk_count = 0;
	if (sample_count > 0) {
		chunk_count = (sample_count + SBF_CHUNK_AUDIO - 1) / SBF_CHUNK_AUDIO;
	}
	slot.block_size = SBF_CHUNK_TOTAL;
	slot.total_size = (uint32_t)(chunk_count * SBF_CHUNK_TOTAL);
	slot.sample_length_hint = 0;
	slot.data_offset = 0; // the writer lays the bank out.

	_arc.header.entry_count = new_n;

	// The stream in the model: the slot's name, its chunks encoded from the samples.
	const std::vector<int16_t> pcm = int16_from(p_samples);
	_model.streams.push_back(sbf_encode_stream(std::string(slot.name, (size_t)copy_len), pcm.data(), pcm.size()));
	_dirty = true;
	return OK;
}

Error SbfBank::save_to_path(const String &p_path) {
	// The bank from its model alone, edited or not (ADR 0003).
	PackedByteArray out_bytes;
	Error err = build_encoded_bytes(out_bytes);
	if (err != OK) return err;
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::WRITE);
	if (fa.is_null()) return ERR_CANT_OPEN;
	fa->store_buffer(out_bytes);
	clear_dirty();
	return OK;
}
