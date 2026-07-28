// SBF audio bank wrapper. The underlying parser is libs/sbf
// (sbf_open_memory), which mirrors the engine's
// AudioVM_OpenContextFile @ 0x00672160 (jointops); the engine reads SBF
// archives via the same header + entry-table layout.

#include "nova_sbf_bank.h"
#include "nova_sbf_audio_stream.h"
#include "util/nova_data_format.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

using namespace godot;

NovaSbfBank::NovaSbfBank() {
	std::memset(&_arc, 0, sizeof(_arc));
}

NovaSbfBank::~NovaSbfBank() {
	if (_opened) {
		sbf_close(&_arc);
		_opened = false;
	}
}

void NovaSbfBank::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_entry_count"), &NovaSbfBank::get_entry_count);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaSbfBank::get_source_path);
	ClassDB::bind_method(D_METHOD("get_entries"), &NovaSbfBank::get_entries);
	ClassDB::bind_method(D_METHOD("has_entry", "name"), &NovaSbfBank::has_entry);
	ClassDB::bind_method(D_METHOD("get_entry_name", "index"), &NovaSbfBank::get_entry_name);
	ClassDB::bind_method(D_METHOD("get_stream", "name"), &NovaSbfBank::get_stream);
	ClassDB::bind_method(D_METHOD("get_stream_at", "index"), &NovaSbfBank::get_stream_at);
	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &NovaSbfBank::load_from_path);
	ClassDB::bind_static_method("NovaSbfBank", D_METHOD("create_empty"), &NovaSbfBank::create_empty);
	ClassDB::bind_method(D_METHOD("get_raw_file_bytes"), &NovaSbfBank::get_raw_file_bytes);
	ClassDB::bind_method(D_METHOD("set_entry_pcm", "index", "samples"), &NovaSbfBank::set_entry_pcm);
	ClassDB::bind_method(D_METHOD("is_dirty"), &NovaSbfBank::is_dirty);
	ClassDB::bind_method(D_METHOD("clear_dirty"), &NovaSbfBank::clear_dirty);
	ClassDB::bind_method(D_METHOD("reorder_entry", "from", "to"), &NovaSbfBank::reorder_entry);
	ClassDB::bind_method(D_METHOD("rename_entry", "index", "name"), &NovaSbfBank::rename_entry);
	ClassDB::bind_method(D_METHOD("add_entry", "name", "samples"), &NovaSbfBank::add_entry);
	ClassDB::bind_method(D_METHOD("delete_entry", "index"), &NovaSbfBank::delete_entry);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path",
								   PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT),
			"", "get_source_path");
}

void NovaSbfBank::load_from_path(const String &p_path) {
	if (_opened) {
		sbf_close(&_arc);
		_opened = false;
	}
	source_path = p_path;
	_file_bytes = PackedByteArray();

	if (!read_nova_payload_file(p_path, _file_bytes)) {
		UtilityFunctions::push_warning("NovaSbfBank: could not open ", p_path);
		return;
	}

	if (_file_bytes.size() <= 0) {
		UtilityFunctions::push_warning("NovaSbfBank: empty file ", p_path);
		return;
	}
	if (sbf_open_memory(&_arc, _file_bytes.ptr(), (size_t)_file_bytes.size()) != 0) {
		UtilityFunctions::push_warning("NovaSbfBank: sbf_open_memory failed for ", p_path);
		_file_bytes = PackedByteArray();
		return;
	}
	_opened = true;
}

Ref<NovaSbfBank> NovaSbfBank::create_empty() {
	Ref<NovaSbfBank> bank;
	bank.instantiate();
	// Start from a zeroed archive: entries == nullptr is the valid empty state
	// (add_entry reallocs from null on the same allocator sbf_close frees with).
	// Stamp a coherent empty SBF header so any header reader sees a real bank,
	// matching the bytes sbf_encode_file would later write.
	std::memset(&bank->_arc, 0, sizeof(bank->_arc));
	bank->_arc.header.magic = SBF_MAGIC;
	bank->_arc.header.version = 0x00000100u;
	bank->_arc.header.flags = 0x00000001u; // byte-paired stereo (matches sbf_encode_file)
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

int NovaSbfBank::get_entry_count() const {
	return _opened ? (int)_arc.header.entry_count : 0;
}

Array NovaSbfBank::get_entries() const {
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

bool NovaSbfBank::has_entry(const StringName &p_name) const {
	if (!_opened) {
		return false;
	}
	String s = String(p_name);
	return sbf_find_by_name(&_arc, s.utf8().get_data()) != nullptr;
}

String NovaSbfBank::get_entry_name(int p_index) const {
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

const SbfRawEntry *NovaSbfBank::raw_entry_at(int p_index) const {
	if (!_opened) {
		return nullptr;
	}
	if (p_index < 0 || (uint32_t)p_index >= _arc.header.entry_count) {
		return nullptr;
	}
	return &_arc.entries[p_index];
}

bool NovaSbfBank::read_file_block(uint64_t p_offset, uint32_t p_size, PackedByteArray &r_block) const {
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

Ref<NovaSbfAudioStream> NovaSbfBank::get_stream(const StringName &p_name) {
	if (!_opened) {
		return Ref<NovaSbfAudioStream>();
	}
	String s = String(p_name);
	const SbfRawEntry *e = sbf_find_by_name(&_arc, s.utf8().get_data());
	if (!e) {
		return Ref<NovaSbfAudioStream>();
	}
	Ref<NovaSbfAudioStream> stream;
	stream.instantiate();
	stream->configure(this, (int)(e - _arc.entries));
	return stream;
}

Ref<NovaSbfAudioStream> NovaSbfBank::get_stream_at(int p_index) {
	if (!_opened) {
		return Ref<NovaSbfAudioStream>();
	}
	if (p_index < 0 || (uint32_t)p_index >= _arc.header.entry_count) {
		return Ref<NovaSbfAudioStream>();
	}
	Ref<NovaSbfAudioStream> stream;
	stream.instantiate();
	stream->configure(this, p_index);
	return stream;
}

Error NovaSbfBank::set_entry_pcm(int p_index, const PackedFloat32Array &p_samples) {
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

Error NovaSbfBank::build_encoded_bytes(PackedByteArray &out) const {
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

Error NovaSbfBank::reorder_entry(int p_from, int p_to) {
	if (!_opened) {
		return ERR_UNCONFIGURED;
	}
	const uint32_t n = _arc.header.entry_count;
	if (p_from < 0 || (uint32_t)p_from >= n) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_to < 0 || (uint32_t)p_to >= n) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_from == p_to) {
		return OK;
	}

	// Move entries[p_from] to slot p_to, shifting the slice in between.
	SbfRawEntry moving = _arc.entries[p_from];
	if (p_from < p_to) {
		// Shift left: indices (from+1 .. to) move down by one to (from .. to-1).
		for (int i = p_from; i < p_to; ++i) {
			_arc.entries[i] = _arc.entries[i + 1];
		}
	} else {
		// Shift right: indices (to .. from-1) move up by one to (to+1 .. from).
		for (int i = p_from; i > p_to; --i) {
			_arc.entries[i] = _arc.entries[i - 1];
		}
	}
	_arc.entries[p_to] = moving;

	// Remap override keys. Build a fresh map so we don't trip over a key we
	// just wrote during in-place shifting.
	HashMap<int, Vector<int16_t>> remapped;
	for (HashMap<int, Vector<int16_t>>::ConstIterator it = _entry_pcm_overrides.begin();
			it != _entry_pcm_overrides.end(); ++it) {
		int k = it->key;
		int new_k;
		if (k == p_from) {
			new_k = p_to;
		} else if (p_from < p_to && k > p_from && k <= p_to) {
			new_k = k - 1;
		} else if (p_from > p_to && k >= p_to && k < p_from) {
			new_k = k + 1;
		} else {
			new_k = k;
		}
		remapped[new_k] = it->value;
	}
	_entry_pcm_overrides = remapped;
	_dirty = true;
	return OK;
}

Error NovaSbfBank::rename_entry(int p_index, const String &p_name) {
	if (!_opened) {
		return ERR_UNCONFIGURED;
	}
	if (p_index < 0 || (uint32_t)p_index >= _arc.header.entry_count) {
		return ERR_INVALID_PARAMETER;
	}
	CharString utf8 = p_name.utf8();
	const char *src = utf8.get_data();
	const int src_len = (int)utf8.length();
	const int copy_len = src_len < (SBF_NAME_SIZE - 1) ? src_len : (SBF_NAME_SIZE - 1);

	std::memset(_arc.entries[p_index].name, 0, SBF_NAME_SIZE);
	if (copy_len > 0 && src != nullptr) {
		std::memcpy(_arc.entries[p_index].name, src, (size_t)copy_len);
	}
	_dirty = true;
	return OK;
}

Error NovaSbfBank::add_entry(const String &p_name, const PackedFloat32Array &p_samples) {
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

Error NovaSbfBank::delete_entry(int p_index) {
	if (!_opened) {
		return ERR_UNCONFIGURED;
	}
	const uint32_t n = _arc.header.entry_count;
	if (p_index < 0 || (uint32_t)p_index >= n) {
		return ERR_INVALID_PARAMETER;
	}

	// Shift entries left.
	for (uint32_t i = (uint32_t)p_index; i + 1 < n; ++i) {
		_arc.entries[i] = _arc.entries[i + 1];
	}
	_arc.header.entry_count = n - 1;

	// Remap override map: drop the deleted key, shift higher keys down by one.
	HashMap<int, Vector<int16_t>> remapped;
	for (HashMap<int, Vector<int16_t>>::ConstIterator it = _entry_pcm_overrides.begin();
			it != _entry_pcm_overrides.end(); ++it) {
		int k = it->key;
		if (k == p_index) {
			continue;
		}
		int new_k = (k > p_index) ? (k - 1) : k;
		remapped[new_k] = it->value;
	}
	_entry_pcm_overrides = remapped;
	_dirty = true;
	return OK;
}
