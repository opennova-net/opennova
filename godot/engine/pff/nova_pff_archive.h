#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <pff/pff.h>

#include <string>
#include <vector>

namespace godot {

// Editable single-archive PFF tool, exposed to the OpenNova Editor. Wraps the libs/pff reader +
// streaming writer + the payload decode layer + the game-profile table. A "game" choice drives
// both the container key (for encrypting added files) and the payload codec (for decoding on
// extract). Read-only resolution still belongs to NovaResourceRoot; this class is the authoring
// surface. The model holds the directory plus pending add/delete ops, never the whole archive in
// RAM: retained payloads stream from the still-open source on Save-As, added payloads live in the
// entry. Save-As only — the source file is never overwritten.
class NovaPffArchive : public RefCounted {
	GDCLASS(NovaPffArchive, RefCounted)

public:
	// One entry in the current (post-edit) model.
	struct Entry {
		std::string name;                 // original-case logical name (<= PFF_NAME_SIZE)
		uint32_t size = 0;
		uint32_t flags = 0;               // bit 0 = PFF_FLAG_ENCRYPTED
		uint32_t timestamp = 0;
		uint32_t checksum = 0;
		bool added = false;               // true: bytes in `data`; false: read from `src`
		const PffEntry *src = nullptr;    // retained entries: points into source_.entries
		std::vector<uint8_t> data;        // added entries: the exact stored bytes
	};

private:
	PffArchive source_{};                 // open read handle (zero-inited until open())
	bool source_open_ = false;
	std::string source_path_;
	PffFormat source_format_ = PFF_FORMAT_PFF3;
	int game_id_ = 0;                     // NOVA_GAME_JO
	bool dirty_ = false;
	mutable String last_error_;
	std::vector<Entry> entries_;

	uint32_t container_key() const;
	void close_source();
	void build_model_from_source();
	const Entry *find_entry(const String &name) const;
	bool read_entry_bytes(const Entry &entry, bool decode, std::vector<uint8_t> &out) const;
	Error do_open(const String &path, bool legacy);

	static String to_native_path(const String &path);
	static PffFormat format_from_magic(uint32_t magic);
	// Streaming-writer callback: fills `out` with entry[index]'s stored bytes. ctx is `this`.
	static int read_entry_cb(void *ctx, uint32_t index, uint8_t *out, uint32_t size);

protected:
	static void _bind_methods();

public:
	NovaPffArchive();
	~NovaPffArchive();

	// The games the tool can target: [{id:int, name:String}, ...] from the gameprofile table.
	static Array list_games();

	Error open(const String &path);
	Error open_legacy(const String &path);
	String get_source_path() const;
	String get_last_error() const;

	void set_game(int game_id);
	int get_game() const;

	int get_entry_count() const;
	bool has_file(const String &name) const;
	// [{name:String, size:int, encrypted:bool}, ...] for the current model.
	Array get_entries() const;

	// decode=true fully decodes (container-XOR if flagged, then SCR + BFC1); decode=false returns
	// the raw stored bytes. Decoded bytes are an export only — they never re-enter the archive.
	PackedByteArray read_entry(const String &name, bool decode) const;
	Error extract_to(const String &name, const String &out_path, bool decode) const;
	Error extract_selected(const PackedStringArray &names, const String &out_dir, bool decode) const;
	Error extract_all(const String &out_dir, bool decode) const;

	// add stores the file's bytes verbatim (optionally container-XOR-encrypted); marks dirty.
	Error add_file_from_disk(const String &src_path, const String &store_name, bool encrypt);
	Error remove_entries(const PackedStringArray &names);
	bool is_dirty() const;

	// Writes a NEW archive (never the source). Preserves the source container format, PFF3 default.
	Error save_as(const String &out_path);
};

} // namespace godot
