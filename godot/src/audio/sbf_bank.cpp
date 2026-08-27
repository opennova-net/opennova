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

#include <cstring>

using namespace godot;

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
	// _opened lights up the mutation API; _dirty steers the saver to the
	// re-encode (build_encoded_bytes) path rather than raw passthrough.
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

Error SbfBank::set_entry_pcm(int p_index, const PackedFloat32Array &p_samples) {
	if (!_opened) {
		return ERR_UNCONFIGURED;
	}
	if (p_index < 0 || (uint32_t)p_index >= _arc.header.entry_count) {
		return ERR_INVALID_PARAMETER;
	}

	// Convert float [-1, 1] to int16 with clamp; the encoder consumes int16
	// and re-derives the per-chunk scale via sbf_pick_scale.
	const int n = p_samples.size();
	Vector<int16_t> int16_samples;
	int16_samples.resize(n);
	int16_t *dst = int16_samples.ptrw();
	for (int i = 0; i < n; ++i) {
		float f = p_samples[i];
		if (f < -1.0f) {
			f = -1.0f;
		}
		if (f > 1.0f) {
			f = 1.0f;
		}
		dst[i] = (int16_t)(f * 32767.0f);
	}

	_entry_pcm_overrides[p_index] = int16_samples;
	_dirty = true;
	return OK;
}

Error SbfBank::build_encoded_bytes(PackedByteArray &out) const {
	if (!_opened) {
		return ERR_UNCONFIGURED;
	}

	const uint32_t n = _arc.header.entry_count;

	// Per-entry PCM int16 storage: override or decoded original.
	Vector<Vector<int16_t>> entries_pcm;
	entries_pcm.resize(n);
	Vector<String> names;
	names.resize(n);

	for (uint32_t i = 0; i < n; ++i) {
		char name_buf[SBF_NAME_SIZE + 1] = { 0 };
		std::memcpy(name_buf, _arc.entries[i].name, SBF_NAME_SIZE);
		names.write[i] = String(name_buf);

		const HashMap<int, Vector<int16_t>>::ConstIterator it = _entry_pcm_overrides.find((int)i);
		if (it != _entry_pcm_overrides.end()) {
			entries_pcm.write[i] = it->value;
			continue;
		}

		// No override: decode original chunk bytes from the in-memory archive.
		// sbf_open_memory leaves the archive in memory mode; sbf_read_raw
		// only works for file-backed archives, so we copy the raw bytes
		// directly from _file_bytes via the entry's data_offset/total_size.
		const SbfRawEntry &entry = _arc.entries[i];
		const int total_bytes = (int)entry.total_size;
		if (total_bytes <= 0) {
			entries_pcm.write[i] = Vector<int16_t>();
			continue;
		}
		const int64_t off = (int64_t)entry.data_offset;
		if (off < 0 || off + total_bytes > _file_bytes.size()) {
			return ERR_FILE_CORRUPT;
		}
		const uint8_t *raw = _file_bytes.ptr() + off;
		// Decode capacity: at most one int16 per audio byte (less for the
		// 8-byte chunk headers, but using total_bytes is a safe upper bound).
		Vector<int16_t> dec;
		dec.resize(total_bytes);
		const int got = sbf_decode_all(raw, (size_t)total_bytes, dec.ptrw(), dec.size());
		if (got < 0) {
			return ERR_FILE_CORRUPT;
		}
		dec.resize(got);
		entries_pcm.write[i] = dec;
	}

	// Marshal into the C-style argument arrays expected by sbf_encode_file.
	// CharString instances must outlive the const char* pointers.
	Vector<CharString> name_storage;
	name_storage.resize(n);
	Vector<const char *> name_ptrs;
	name_ptrs.resize(n);
	Vector<const int16_t *> pcm_ptrs;
	pcm_ptrs.resize(n);
	Vector<size_t> pcm_counts;
	pcm_counts.resize(n);
	for (uint32_t i = 0; i < n; ++i) {
		name_storage.write[i] = names[i].utf8();
		name_ptrs.write[i] = name_storage[i].get_data();
		pcm_ptrs.write[i] = entries_pcm[i].ptr();
		pcm_counts.write[i] = (size_t)entries_pcm[i].size();
	}

	uint8_t *buf = nullptr;
	size_t buf_size = 0;
	const int rc = sbf_encode_file(name_ptrs.ptr(), n,
			pcm_ptrs.ptr(), pcm_counts.ptr(),
			&buf, &buf_size);
	if (rc != 0 || buf == nullptr) {
		if (buf) {
			sbf_free(buf);
		}
		return ERR_BUG;
	}

	out.resize((int)buf_size);
	std::memcpy(out.ptrw(), buf, buf_size);
	sbf_free(buf);
	return OK;
}

// --- Phase E: bank-edit ops ----------------------------------------------
//
// These mutate the in-memory entry table + override map. The saver's
// re-encode path (build_encoded_bytes) walks _arc.header.entry_count and
// reads the override map keyed by current index, so swapping/shifting
// requires both the entries[] array and the override map to stay in sync.

Error SbfBank::add_entry(const String &p_name, const PackedFloat32Array &p_samples) {
	if (!_opened) {
		return ERR_UNCONFIGURED;
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

	// total_size / block_size: build_encoded_bytes recomputes layout from the
	// override-derived chunk count, so a placeholder consistent with the
	// number of chunks here is enough. block_size = SBF_CHUNK_TOTAL is the
	// invariant every observed SBF holds.
	const int sample_count = p_samples.size();
	int chunk_count = 0;
	if (sample_count > 0) {
		chunk_count = (sample_count + SBF_CHUNK_AUDIO - 1) / SBF_CHUNK_AUDIO;
	}
	slot.block_size = SBF_CHUNK_TOTAL;
	slot.total_size = (uint32_t)(chunk_count * SBF_CHUNK_TOTAL);
	slot.sample_length_hint = 0;
	slot.data_offset = 0; // re-encoder fills this when building bytes.

	_arc.header.entry_count = new_n;

	// Convert float -> int16 and store as override at the new slot's index.
	Vector<int16_t> int16_samples;
	int16_samples.resize(sample_count);
	int16_t *dst = int16_samples.ptrw();
	for (int i = 0; i < sample_count; ++i) {
		float f = p_samples[i];
		if (f < -1.0f) {
			f = -1.0f;
		}
		if (f > 1.0f) {
			f = 1.0f;
		}
		dst[i] = (int16_t)(f * 32767.0f);
	}
	_entry_pcm_overrides[(int)n] = int16_samples;
	_dirty = true;
	return OK;
}

Error SbfBank::save_to_path(const String &p_path) {
	if (!is_dirty()) {
		// Lossless passthrough: copy original source bytes through.
		PackedByteArray bytes = get_raw_file_bytes();
		if (bytes.is_empty()) return ERR_FILE_CANT_OPEN;
		Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::WRITE);
		if (fa.is_null()) return ERR_CANT_OPEN;
		fa->store_buffer(bytes);
		return OK;
	}
	// Re-encode path (Phase D / SBF F2): walk current entry table + override
	// PCM map, rebuild the byte stream from scratch.
	PackedByteArray out_bytes;
	Error err = build_encoded_bytes(out_bytes);
	if (err != OK) return err;
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::WRITE);
	if (fa.is_null()) return ERR_CANT_OPEN;
	fa->store_buffer(out_bytes);
	clear_dirty();
	return OK;
}
