#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <pff/pff.h>

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
	// Normalized-name -> index into entries_, rebuilt lazily on the next find_entry after any model
	// mutation (open/add/remove). Keeps lookups O(1) instead of an O(n) per-call scan, so batch
	// extract over a multi-thousand-entry archive is O(n) rather than O(n^2). Invalidate via
	// invalidate_index() whenever entries_ changes.
	mutable std::unordered_map<std::string, size_t> name_index_;
	mutable bool index_dirty_ = true;
	// Count of entries written un-decoded (decode requested but payload codec failed) by the most
	// recent extract batch / extract_to_status call. Lets the UI warn "N saved as raw".
	mutable int last_undecoded_count_ = 0;

	// Background Save-As job (mirrors NovaTerrainBuildJob). The worker thread is the ONLY reader of
	// source_ while a save runs; the UI disables all other ops, so there is no concurrent access.
	struct SaveState {
		bool running = false;
		bool finished = false;
		int result = 0;             // godot Error (OK == 0)
		uint32_t done = 0;
		uint32_t total = 0;
		std::string message;        // error text for get_save_error()
	};
	mutable std::mutex save_mutex_;
	std::thread save_thread_;
	SaveState save_state_;
	std::vector<PffWriteStreamEntry> save_entries_;  // captured snapshot for the worker
	std::vector<std::string> save_names_;            // keeps save_entries_[i].name alive
	std::string save_out_native_;
	PffFormat save_format_ = PFF_FORMAT_PFF3;

	// Background extract job (mirrors the Save worker). The worker reads source_/entries_ and writes
	// the output files off the main thread; the editor disables every other op while it runs, so the
	// model it reads is stable (the same invariant the save worker relies on). Cancel is checked
	// between entries, so a single large file still finishes before the job stops, but the UI never
	// blocks mid-entry.
	struct ExtractState {
		bool running = false;
		bool finished = false;
		bool cancel_requested = false;
		uint32_t done = 0;
		uint32_t total = 0;
		uint32_t ok = 0;        // extracted and fully decoded
		uint32_t raw = 0;       // extracted but saved raw (decode requested, codec failed)
		uint32_t failed = 0;    // hard failure (unreadable entry or unwritable output)
	};
	struct ExtractJob {
		size_t index = 0;          // index into entries_ (stable for the job's lifetime)
		std::string out_native;    // resolved native output path
	};
	mutable std::mutex extract_mutex_;
	std::thread extract_thread_;
	ExtractState extract_state_;
	std::vector<ExtractJob> extract_jobs_;  // worker-owned snapshot, built on the main thread
	bool extract_decode_ = true;

	uint32_t container_key() const;
	void close_source();
	void build_model_from_source();
	void invalidate_index() { index_dirty_ = true; }
	// Normalize a PFF name the same way the C library does (uppercase + trailing-space trim) so the
	// binding's lookup matches the writer's dedup and the on-disk sort order.
	static std::string normalize_name(const String &name);
	const Entry *find_entry(const String &name) const;
	// out_decoded (optional): set true if the payload codec ran, false if decode was requested but
	// failed and `out` was left as the container-decrypted (raw) fallback. Genuine read failures
	// (unreadable entry) still return false.
	bool read_entry_bytes(const Entry &entry, bool decode, std::vector<uint8_t> &out,
	                      bool *out_decoded = nullptr) const;
	Error do_open(const String &path, bool legacy);
	void join_save_thread();
	void save_worker();
	void join_extract_thread();
	void extract_worker();

	static String to_native_path(const String &path);
	static PffFormat format_from_magic(uint32_t magic);
	// Streaming-writer callback: fills `out` with entry[index]'s stored bytes. ctx is `this`.
	static int read_entry_cb(void *ctx, uint32_t index, uint8_t *out, uint32_t size);
	static void save_progress_cb(void *ctx, uint32_t done, uint32_t total);

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
	// Per-file extract for the editor's batched loop. Returns 0 = extracted (decoded), 1 = extracted
	// but saved raw (decode requested but failed), 2 = hard failure (nothing written).
	int extract_to_status(const String &name, const String &out_path, bool decode) const;
	// Number of files the most recent batch / extract_to_status saved un-decoded.
	int get_last_undecoded_count() const;

	// Non-blocking batch extract. Resolves the job list on the calling (main) thread, then reads,
	// decodes, and writes each file on a background thread. `names` empty means "every entry";
	// output files are written into out_dir by basename. Returns OK if the job started (then poll
	// is_extract_running() and read the counters), else an error. Mirrors save_as_async.
	Error extract_async(const PackedStringArray &names, const String &out_dir, bool decode);
	bool is_extract_running() const;
	bool is_extract_finished() const;
	void request_extract_cancel();          // ask the worker to stop after the current entry
	int get_extract_progress_done() const;  // entries processed so far (incl. failures)
	int get_extract_progress_total() const;
	int get_extract_ok_count() const;       // extracted and fully decoded
	int get_extract_raw_count() const;       // extracted but saved raw (decode failed)
	int get_extract_failed_count() const;    // hard failures (unreadable / unwritable)
	void wait_for_extract_completion();      // joins the worker (main thread)

	// add stores the file's bytes verbatim (optionally container-XOR-encrypted); marks dirty.
	Error add_file_from_disk(const String &src_path, const String &store_name, bool encrypt);
	Error remove_entries(const PackedStringArray &names);
	bool is_dirty() const;

	// Writes a NEW archive (never the source). Preserves the source container format, PFF3 default.
	Error save_as(const String &out_path);

	// Non-blocking Save-As: validates + snapshots on the calling (main) thread, then writes on a
	// background thread. Returns OK if the job started (poll is_save_finished()), else an error.
	Error save_as_async(const String &out_path);
	bool is_save_running() const;
	bool is_save_finished() const;
	int get_save_progress_done() const;
	int get_save_progress_total() const;
	int get_save_result() const;        // godot Error of the finished save
	String get_save_error() const;
	void wait_for_save_completion();     // joins the worker; clears dirty on success (main thread)
};

} // namespace godot
