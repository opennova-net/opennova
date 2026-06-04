#include "pff/nova_pff_archive.h"

#include <gameprofile/gameprofile.h>
#include <vfs/vfs_decode.h>

#include <godot_cpp/classes/project_settings.hpp>

#include <cstdio>
#include <cstring>

using namespace godot;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

String NovaPffArchive::to_native_path(const String &path) {
	if (path.begins_with("res://") || path.begins_with("user://")) {
		ProjectSettings *settings = ProjectSettings::get_singleton();
		if (settings != nullptr) {
			return settings->globalize_path(path);
		}
	}
	return path;
}

PffFormat NovaPffArchive::format_from_magic(uint32_t magic) {
	if (magic == PFF_MAGIC_PFF4) {
		return PFF_FORMAT_PFF4;
	}
	if (magic == PFF_MAGIC_BHD) {
		return PFF_FORMAT_BHD;
	}
	return PFF_FORMAT_PFF3; // PFF3 and legacy (magic 0) both resave as PFF3
}

uint32_t NovaPffArchive::container_key() const {
	const NovaGameProfile *p = gameprofile_by_id(game_id_);
	return p ? p->container_key : 0x0312A4CEu;
}

void NovaPffArchive::close_source() {
	if (source_open_) {
		pff_close(&source_);
		source_open_ = false;
	}
}

void NovaPffArchive::build_model_from_source() {
	entries_.clear();
	entries_.reserve(source_.entry_count);
	for (uint32_t i = 0; i < source_.entry_count; ++i) {
		const PffEntry &pe = source_.entries[i];
		Entry e;
		size_t len = 0;
		while (len < PFF_NAME_SIZE && pe.filename[len] != '\0') {
			++len;
		}
		e.name.assign(pe.filename, len);
		e.size = pe.size;
		e.flags = pe.flags;
		e.timestamp = pe.timestamp;
		e.checksum = pe.checksum;
		e.added = false;
		e.src = &pe;
		entries_.push_back(std::move(e));
	}
}

const NovaPffArchive::Entry *NovaPffArchive::find_entry(const String &name) const {
	const String wanted = name.strip_edges().to_lower();
	for (const Entry &e : entries_) {
		if (String(e.name.c_str()).to_lower() == wanted) {
			return &e;
		}
	}
	return nullptr;
}

bool NovaPffArchive::read_entry_bytes(const Entry &entry, bool decode, std::vector<uint8_t> &out,
                                      bool *out_decoded) const {
	if (out_decoded != nullptr) {
		*out_decoded = false;
	}
	out.clear();
	if (entry.size) {
		out.resize(entry.size);
		if (entry.added) {
			if (entry.data.size() < entry.size) {
				return false;
			}
			memcpy(out.data(), entry.data.data(), entry.size);
		} else {
			if (entry.src == nullptr) {
				return false;
			}
			if (pff_extract_raw(&source_, entry.src, out.data(), out.size()) != 0) {
				return false;
			}
		}
	}
	if (decode) {
		// Container-decrypt in place; `out` is now the stored-but-container-decrypted bytes — our
		// raw fallback if the payload codec can't handle this file.
		if ((entry.flags & PFF_FLAG_ENCRYPTED) && !out.empty()) {
			pff_container_xor(out.data(), out.size(), container_key());
		}
		// Decode into a COPY: vfs_decode_payload runs SCR then BFC1 and only swaps on success, so a
		// SCR-ok/BFC1-fail would leave a half-transformed buffer. On failure we keep `out` as the
		// container-decrypted fallback (so the file still extracts) and report it via out_decoded.
		std::vector<uint8_t> decoded(out);
		if (opennova::vfs_decode_payload(decoded)) {
			out.swap(decoded);
			if (out_decoded != nullptr) {
				*out_decoded = true;
			}
		}
	}
	return true;
}

int NovaPffArchive::read_entry_cb(void *ctx, uint32_t index, uint8_t *out, uint32_t size) {
	NovaPffArchive *self = static_cast<NovaPffArchive *>(ctx);
	if (index >= self->entries_.size()) {
		return -1;
	}
	const Entry &e = self->entries_[index];
	if (size == 0) {
		return 0;
	}
	if (e.added) {
		if (e.data.size() < size) {
			return -1;
		}
		memcpy(out, e.data.data(), size);
		return 0;
	}
	if (e.src == nullptr) {
		return -1;
	}
	return pff_extract_raw(&self->source_, e.src, out, size); // verbatim stored (ciphertext) bytes
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NovaPffArchive::NovaPffArchive() {}

NovaPffArchive::~NovaPffArchive() {
	// Join the save worker BEFORE freeing the source handle / model vectors it reads.
	join_save_thread();
	close_source();
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

void NovaPffArchive::_bind_methods() {
	ClassDB::bind_static_method("NovaPffArchive", D_METHOD("list_games"), &NovaPffArchive::list_games);
	ClassDB::bind_method(D_METHOD("open", "path"), &NovaPffArchive::open);
	ClassDB::bind_method(D_METHOD("open_legacy", "path"), &NovaPffArchive::open_legacy);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaPffArchive::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaPffArchive::get_last_error);
	ClassDB::bind_method(D_METHOD("set_game", "game_id"), &NovaPffArchive::set_game);
	ClassDB::bind_method(D_METHOD("get_game"), &NovaPffArchive::get_game);
	ClassDB::bind_method(D_METHOD("get_entry_count"), &NovaPffArchive::get_entry_count);
	ClassDB::bind_method(D_METHOD("has_file", "name"), &NovaPffArchive::has_file);
	ClassDB::bind_method(D_METHOD("get_entries"), &NovaPffArchive::get_entries);
	ClassDB::bind_method(D_METHOD("read_entry", "name", "decode"), &NovaPffArchive::read_entry, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("extract_to", "name", "out_path", "decode"), &NovaPffArchive::extract_to, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("extract_selected", "names", "out_dir", "decode"), &NovaPffArchive::extract_selected, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("extract_all", "out_dir", "decode"), &NovaPffArchive::extract_all, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("extract_to_status", "name", "out_path", "decode"), &NovaPffArchive::extract_to_status, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_last_undecoded_count"), &NovaPffArchive::get_last_undecoded_count);
	ClassDB::bind_method(D_METHOD("add_file_from_disk", "src_path", "store_name", "encrypt"), &NovaPffArchive::add_file_from_disk, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("remove_entries", "names"), &NovaPffArchive::remove_entries);
	ClassDB::bind_method(D_METHOD("is_dirty"), &NovaPffArchive::is_dirty);
	ClassDB::bind_method(D_METHOD("save_as", "out_path"), &NovaPffArchive::save_as);
	ClassDB::bind_method(D_METHOD("save_as_async", "out_path"), &NovaPffArchive::save_as_async);
	ClassDB::bind_method(D_METHOD("is_save_running"), &NovaPffArchive::is_save_running);
	ClassDB::bind_method(D_METHOD("is_save_finished"), &NovaPffArchive::is_save_finished);
	ClassDB::bind_method(D_METHOD("get_save_progress_done"), &NovaPffArchive::get_save_progress_done);
	ClassDB::bind_method(D_METHOD("get_save_progress_total"), &NovaPffArchive::get_save_progress_total);
	ClassDB::bind_method(D_METHOD("get_save_result"), &NovaPffArchive::get_save_result);
	ClassDB::bind_method(D_METHOD("get_save_error"), &NovaPffArchive::get_save_error);
	ClassDB::bind_method(D_METHOD("wait_for_save_completion"), &NovaPffArchive::wait_for_save_completion);
}

// ---------------------------------------------------------------------------
// Profile
// ---------------------------------------------------------------------------

Array NovaPffArchive::list_games() {
	Array out;
	for (int i = 0; i < gameprofile_count(); ++i) {
		const NovaGameProfile *p = gameprofile_at(i);
		if (p == nullptr) {
			continue;
		}
		Dictionary d;
		d["id"] = p->id;
		d["name"] = String(p->display_name);
		out.push_back(d);
	}
	return out;
}

void NovaPffArchive::set_game(int game_id) {
	if (gameprofile_by_id(game_id) != nullptr) {
		game_id_ = game_id;
	}
}

int NovaPffArchive::get_game() const {
	return game_id_;
}

// ---------------------------------------------------------------------------
// Open / inspect
// ---------------------------------------------------------------------------

Error NovaPffArchive::do_open(const String &path, bool legacy) {
	last_error_ = String();
	const String native = to_native_path(path).strip_edges();
	if (native.is_empty()) {
		last_error_ = "Archive path is empty";
		return ERR_INVALID_PARAMETER;
	}
	close_source();
	entries_.clear();
	const int rc = legacy
			? pff_open_legacy(&source_, native.utf8().get_data())
			: pff_open(&source_, native.utf8().get_data());
	if (rc != 0) {
		last_error_ = "Not a readable PFF archive: " + native;
		return ERR_FILE_UNRECOGNIZED;
	}
	source_open_ = true;
	source_path_ = native.utf8().get_data();
	source_format_ = format_from_magic(source_.header.magic);
	build_model_from_source();
	dirty_ = false;
	return OK;
}

Error NovaPffArchive::open(const String &path) {
	return do_open(path, false);
}

Error NovaPffArchive::open_legacy(const String &path) {
	return do_open(path, true);
}

String NovaPffArchive::get_source_path() const {
	return String(source_path_.c_str());
}

String NovaPffArchive::get_last_error() const {
	return last_error_;
}

int NovaPffArchive::get_entry_count() const {
	return static_cast<int>(entries_.size());
}

bool NovaPffArchive::has_file(const String &name) const {
	return find_entry(name) != nullptr;
}

Array NovaPffArchive::get_entries() const {
	Array out;
	for (const Entry &e : entries_) {
		Dictionary d;
		d["name"] = String(e.name.c_str());
		d["size"] = static_cast<int64_t>(e.size);
		d["encrypted"] = (e.flags & PFF_FLAG_ENCRYPTED) != 0;
		out.push_back(d);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Extract (one-way export)
// ---------------------------------------------------------------------------

PackedByteArray NovaPffArchive::read_entry(const String &name, bool decode) const {
	PackedByteArray out;
	const Entry *e = find_entry(name);
	if (e == nullptr) {
		last_error_ = "No such entry: " + name;
		return out;
	}
	std::vector<uint8_t> bytes;
	if (!read_entry_bytes(*e, decode, bytes)) {
		last_error_ = "Failed to read or decode: " + name;
		return out;
	}
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

Error NovaPffArchive::extract_to(const String &name, const String &out_path, bool decode) const {
	const Entry *e = find_entry(name);
	if (e == nullptr) {
		last_error_ = "No such entry: " + name;
		return ERR_DOES_NOT_EXIST;
	}
	std::vector<uint8_t> bytes;
	bool decoded = false;
	if (!read_entry_bytes(*e, decode, bytes, &decoded)) {
		last_error_ = "Failed to read: " + name;
		return ERR_INVALID_DATA;
	}
	const String native = to_native_path(out_path);
	FILE *f = fopen(native.utf8().get_data(), "wb");
	if (f == nullptr) {
		last_error_ = "Cannot write file: " + out_path;
		return ERR_CANT_CREATE;
	}
	bool ok = true;
	if (!bytes.empty()) {
		ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	}
	fclose(f);
	if (!ok) {
		last_error_ = "Short write: " + out_path;
		return ERR_FILE_CANT_WRITE;
	}
	// Decode was asked for but the codec couldn't handle it: we wrote the raw stored bytes instead.
	if (decode && !decoded) {
		++last_undecoded_count_;
	}
	return OK;
}

int NovaPffArchive::extract_to_status(const String &name, const String &out_path, bool decode) const {
	const int before = last_undecoded_count_;
	const Error rc = extract_to(name, out_path, decode);
	if (rc != OK) {
		return 2; // hard failure, nothing written
	}
	return (last_undecoded_count_ > before) ? 1 : 0; // 1 = saved raw, 0 = decoded ok
}

Error NovaPffArchive::extract_selected(const PackedStringArray &names, const String &out_dir, bool decode) const {
	last_undecoded_count_ = 0;
	Error last = OK;
	for (int i = 0; i < names.size(); ++i) {
		const String name = names[i];
		const String out_path = out_dir.path_join(String(name).get_file());
		const Error rc = extract_to(name, out_path, decode);
		if (rc != OK) {
			last = rc;
		}
	}
	return last;
}

Error NovaPffArchive::extract_all(const String &out_dir, bool decode) const {
	last_undecoded_count_ = 0;
	Error last = OK;
	for (const Entry &e : entries_) {
		const String name(e.name.c_str());
		const Error rc = extract_to(name, out_dir.path_join(name), decode);
		if (rc != OK) {
			last = rc;
		}
	}
	return last;
}

// ---------------------------------------------------------------------------
// Edit
// ---------------------------------------------------------------------------

Error NovaPffArchive::add_file_from_disk(const String &src_path, const String &store_name, bool encrypt) {
	last_error_ = String();
	const String clean_name = store_name.strip_edges();
	std::string name = clean_name.utf8().get_data();
	if (name.empty()) {
		last_error_ = "Store name is empty";
		return ERR_INVALID_PARAMETER;
	}
	if (name.size() > PFF_NAME_SIZE) {
		last_error_ = "Name too long (max 16 characters): " + clean_name;
		return ERR_INVALID_PARAMETER;
	}
	if (find_entry(clean_name) != nullptr) {
		last_error_ = "An entry named '" + clean_name + "' already exists";
		return ERR_ALREADY_EXISTS;
	}

	const String native = to_native_path(src_path);
	FILE *f = fopen(native.utf8().get_data(), "rb");
	if (f == nullptr) {
		last_error_ = "Cannot open file: " + src_path;
		return ERR_CANT_OPEN;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		last_error_ = "Cannot size file: " + src_path;
		return ERR_FILE_CANT_READ;
	}

	Entry e;
	e.name = name;
	e.added = true;
	e.src = nullptr;
	e.timestamp = 0;
	e.checksum = 0;
	e.data.resize(static_cast<size_t>(n));
	if (n > 0 && fread(e.data.data(), 1, static_cast<size_t>(n), f) != static_cast<size_t>(n)) {
		fclose(f);
		last_error_ = "Read failed: " + src_path;
		return ERR_FILE_CANT_READ;
	}
	fclose(f);

	if (encrypt && !e.data.empty()) {
		pff_container_xor(e.data.data(), e.data.size(), container_key());
		e.flags = PFF_FLAG_ENCRYPTED;
	} else {
		e.flags = 0;
	}
	e.size = static_cast<uint32_t>(e.data.size());
	entries_.push_back(std::move(e));
	dirty_ = true;
	return OK;
}

Error NovaPffArchive::remove_entries(const PackedStringArray &names) {
	last_error_ = String();
	int removed = 0;
	for (int i = 0; i < names.size(); ++i) {
		const String wanted = names[i].strip_edges().to_lower();
		for (size_t j = 0; j < entries_.size();) {
			if (String(entries_[j].name.c_str()).to_lower() == wanted) {
				entries_.erase(entries_.begin() + j);
				++removed;
			} else {
				++j;
			}
		}
	}
	if (removed > 0) {
		dirty_ = true;
	}
	return OK;
}

bool NovaPffArchive::is_dirty() const {
	return dirty_;
}

int NovaPffArchive::get_last_undecoded_count() const {
	return last_undecoded_count_;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

Error NovaPffArchive::save_as(const String &out_path) {
	last_error_ = String();
	const String native = to_native_path(out_path).strip_edges();
	if (native.is_empty()) {
		last_error_ = "Output path is empty";
		return ERR_INVALID_PARAMETER;
	}
	if (!source_path_.empty()) {
		const String src(source_path_.c_str());
		if (native.to_lower().replace("\\", "/") == src.to_lower().replace("\\", "/")) {
			last_error_ = "Refusing to overwrite the source archive; choose a different file.";
			return ERR_ALREADY_IN_USE;
		}
	}

	const uint32_t n = static_cast<uint32_t>(entries_.size());
	std::vector<PffWriteStreamEntry> se(n);
	std::vector<std::string> names(n); // keep the name storage alive across the write
	for (uint32_t i = 0; i < n; ++i) {
		names[i] = entries_[i].name;
		se[i].name = names[i].c_str();
		se[i].size = entries_[i].size;
		se[i].flags = entries_[i].flags;
		se[i].timestamp = entries_[i].timestamp;
		se[i].checksum = entries_[i].checksum;
	}

	const int rc = pff_write_archive_streamed(native.utf8().get_data(), source_format_,
			n ? se.data() : nullptr, n, &NovaPffArchive::read_entry_cb, this);
	if (rc != PFF_WRITE_OK) {
		switch (rc) {
			case PFF_WRITE_ERR_NAME_LEN: last_error_ = "An entry name exceeds 16 characters"; break;
			case PFF_WRITE_ERR_NAME_EMPTY: last_error_ = "An entry has an empty name"; break;
			case PFF_WRITE_ERR_DUP_NAME: last_error_ = "Two entries share the same name"; break;
			default: last_error_ = "Failed to write archive: " + out_path; break;
		}
		return ERR_CANT_CREATE;
	}
	dirty_ = false;
	return OK;
}

// ---------------------------------------------------------------------------
// Async Save (background thread; mirrors NovaTerrainBuildJob)
// ---------------------------------------------------------------------------

void NovaPffArchive::join_save_thread() {
	if (save_thread_.joinable()) {
		save_thread_.join();
	}
}

void NovaPffArchive::save_progress_cb(void *ctx, uint32_t done, uint32_t total) {
	NovaPffArchive *self = static_cast<NovaPffArchive *>(ctx);
	std::lock_guard<std::mutex> lock(self->save_mutex_);
	self->save_state_.done = done;
	self->save_state_.total = total;
}

// Runs on the worker thread. The ONLY reader of source_ while a save is in flight (the editor
// disables every other op via _set_busy, so there is no concurrent main-thread read).
void NovaPffArchive::save_worker() {
	int rc;
	try {
		rc = pff_write_archive_streamed_progress(
				save_out_native_.c_str(), save_format_,
				save_entries_.empty() ? nullptr : save_entries_.data(),
				static_cast<uint32_t>(save_entries_.size()),
				&NovaPffArchive::read_entry_cb, this,
				&NovaPffArchive::save_progress_cb, this);
	} catch (...) {
		rc = -1000; // unexpected exception
	}
	std::lock_guard<std::mutex> lock(save_mutex_);
	if (rc == PFF_WRITE_OK) {
		save_state_.result = OK;
		save_state_.message.clear();
	} else {
		save_state_.result = ERR_CANT_CREATE;
		switch (rc) {
			case PFF_WRITE_ERR_NAME_LEN: save_state_.message = "An entry name exceeds 16 characters"; break;
			case PFF_WRITE_ERR_NAME_EMPTY: save_state_.message = "An entry has an empty name"; break;
			case PFF_WRITE_ERR_DUP_NAME: save_state_.message = "Two entries share the same name"; break;
			case -1000: save_state_.message = "Internal error while saving"; break;
			default: save_state_.message = "Failed to write archive"; break;
		}
	}
	save_state_.running = false;
	save_state_.finished = true;
}

Error NovaPffArchive::save_as_async(const String &out_path) {
	last_error_ = String();
	join_save_thread(); // never start a second save over a running one
	const String native = to_native_path(out_path).strip_edges();
	if (native.is_empty()) {
		last_error_ = "Output path is empty";
		return ERR_INVALID_PARAMETER;
	}
	if (!source_path_.empty()) {
		const String src(source_path_.c_str());
		if (native.to_lower().replace("\\", "/") == src.to_lower().replace("\\", "/")) {
			last_error_ = "Refusing to overwrite the source archive; choose a different file.";
			return ERR_ALREADY_IN_USE;
		}
	}

	// Snapshot the model into worker-owned buffers, on the main thread.
	const uint32_t n = static_cast<uint32_t>(entries_.size());
	save_names_.assign(n, std::string());
	save_entries_.assign(n, PffWriteStreamEntry());
	for (uint32_t i = 0; i < n; ++i) {
		save_names_[i] = entries_[i].name;
		save_entries_[i].name = save_names_[i].c_str();
		save_entries_[i].size = entries_[i].size;
		save_entries_[i].flags = entries_[i].flags;
		save_entries_[i].timestamp = entries_[i].timestamp;
		save_entries_[i].checksum = entries_[i].checksum;
	}
	save_out_native_ = native.utf8().get_data();
	save_format_ = source_format_;

	{
		std::lock_guard<std::mutex> lock(save_mutex_);
		save_state_ = SaveState();
		save_state_.running = true;
		save_state_.total = n;
	}
	save_thread_ = std::thread(&NovaPffArchive::save_worker, this);
	return OK;
}

bool NovaPffArchive::is_save_running() const {
	std::lock_guard<std::mutex> lock(save_mutex_);
	return save_state_.running;
}

bool NovaPffArchive::is_save_finished() const {
	std::lock_guard<std::mutex> lock(save_mutex_);
	return save_state_.finished;
}

int NovaPffArchive::get_save_progress_done() const {
	std::lock_guard<std::mutex> lock(save_mutex_);
	return static_cast<int>(save_state_.done);
}

int NovaPffArchive::get_save_progress_total() const {
	std::lock_guard<std::mutex> lock(save_mutex_);
	return static_cast<int>(save_state_.total);
}

int NovaPffArchive::get_save_result() const {
	std::lock_guard<std::mutex> lock(save_mutex_);
	return save_state_.result;
}

String NovaPffArchive::get_save_error() const {
	std::lock_guard<std::mutex> lock(save_mutex_);
	return String(save_state_.message.c_str());
}

void NovaPffArchive::wait_for_save_completion() {
	join_save_thread();
	bool ok;
	{
		std::lock_guard<std::mutex> lock(save_mutex_);
		ok = save_state_.finished && save_state_.result == OK;
	}
	if (ok) {
		dirty_ = false; // cleared on the main thread only
	}
}
