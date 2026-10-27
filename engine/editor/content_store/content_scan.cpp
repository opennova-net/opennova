#include <editor/content_store/content_scan.h>

#include <algorithm>
#include <system_error>
#include <unordered_map>

#include <base/io/file_io.h>
#include <base/io/file_time.h>
#include <base/io/hash.h>
#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/assets/project_scan.h>
#include <editor/assets/project_walk.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// How much of a file one read takes (a block a step's budget spends at least).
constexpr size_t kHashBlock = 1 << 20;

} // namespace

bool content_skips_file(std::string_view name) {
	const std::string lower = strutil::to_lower(name);
	if (lower == "thumbs.db" || lower == "desktop.ini" || lower == ".ds_store")
		return true;
	if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".tmp") == 0)
		return true;
	// Blender's backups: <name>.blend1, .blend2, ... (its Save Versions).
	const size_t dot = lower.rfind(".blend");
	if (dot == std::string::npos || dot + 6 == lower.size())
		return false;
	return std::all_of(lower.begin() + static_cast<std::ptrdiff_t>(dot + 6), lower.end(),
			[](char c) { return c >= '0' && c <= '9'; });
}

ContentScan::ContentScan(const ProjectPaths &paths, std::string export_dir) :
		paths_(paths),
		content_(ContentPaths::for_project(paths)),
		root_(system_path(paths.root)),
		export_dir_(export_dir.empty() ? fs::path() : system_path(export_dir)) {}

void ContentScan::load_cache() {
	std::string message;
	std::error_code ec;
	if (!fs::is_regular_file(system_path(content_.hashes_file), ec) ||
			!io::read_file_text(content_.hashes_file, cache_text_, message))
		return;
	io::JsonValue json;
	if (!io::json_parse(cache_text_, json, message) || !json.is_object() ||
			json.get_int("schema_version", -1) != kContentHashCacheSchemaVersion)
		return;
	const io::JsonValue *files = json.get("files");
	if (!files || !files->is_array())
		return;
	for (const io::JsonValue &item : files->array) {
		if (!item.is_object())
			continue;
		const std::string path = item.get_string("path", "");
		Cached cached;
		uint64_t modified = 0;
		cached.size = uint64_t(item.get_number("size", 0));
		cached.sha256 = item.get_string("sha256", "");
		if (path.empty() || !io::parse_hex64(item.get_string("modified", ""), modified) ||
				!project::is_sha256_hex(cached.sha256))
			continue;
		cached.modified = int64_t(modified);
		cache_[path] = std::move(cached);
	}
}

bool ContentScan::hash_next(uint64_t &spent) {
	const Listed &file = listed_[next_];
	if (!open_) {
		const auto cached = cache_.find(file.path);
		if (cached != cache_.end() && cached->second.size == file.size &&
				cached->second.modified == file.modified &&
				io::file_stamp_settled(file.modified, pass_began_)) {
			seen_[file.path] = cached->second;
			files_.push_back({ file.path, cached->second.sha256, file.size });
			bytes_hashed_ += file.size;
			spent += kWalkEntryCost;
			return true;
		}
		reading_ = std::ifstream(file.system, std::ios::binary);
		if (!reading_) {
			problems_.push_back(file.path + ": cannot be read");
			unread_.push_back(file.path);
			bytes_hashed_ += file.size;
			spent += kWalkEntryCost;
			return true;
		}
		hashing_.reset();
		open_ = true;
	}
	block_.resize(kHashBlock);
	reading_.read(block_.data(), static_cast<std::streamsize>(block_.size()));
	const std::streamsize got = reading_.gcount();
	if (got > 0) {
		hashing_.update(block_.data(), static_cast<size_t>(got));
		bytes_hashed_ += uint64_t(got);
		spent += uint64_t(got);
	}
	if (reading_.bad()) {
		problems_.push_back(file.path + ": cannot be read");
		unread_.push_back(file.path);
		reading_.close();
		open_ = false;
		return true;
	}
	if (!reading_.eof())
		return false;
	reading_.close();
	open_ = false;
	// What was read is the file as it is now: a file rewritten while it was read is read again by
	// the next pass (its last write moved, so the cache's entry is not the one kept).
	const std::string sha = io::sha256_hex(hashing_.finish());
	std::error_code ec;
	const uint64_t size_now = fs::file_size(file.system, ec);
	const uint64_t size = ec ? file.size : size_now;
	seen_[file.path] = { size, file.modified, sha };
	files_.push_back({ file.path, sha, size });
	spent += kWalkEntryCost;
	return true;
}

void ContentScan::finish() {
	// Two paths one file on Windows and macOS: the second (in path order) is no file a store takes.
	std::sort(files_.begin(), files_.end(),
			[](const project::ProjectFile &a, const project::ProjectFile &b) {
				return a.path < b.path;
			});
	std::unordered_map<std::string, std::string> folded;
	std::vector<project::ProjectFile> kept;
	kept.reserve(files_.size());
	for (project::ProjectFile &file : files_) {
		const auto [at, added] = folded.emplace(project::folded_path(file.path), file.path);
		if (!added) {
			problems_.push_back(
					file.path + ": one file with " + at->second + " on Windows and macOS");
			unread_.push_back(file.path);
			continue;
		}
		kept.push_back(std::move(file));
	}
	files_ = std::move(kept);

	// The cache: the files of this pass alone, each one whose last write had settled.
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kContentHashCacheSchemaVersion));
	io::JsonValue list = io::JsonValue::make_array();
	for (const auto &[path, cached] : seen_) {
		if (!io::file_stamp_settled(cached.modified, pass_began_))
			continue;
		io::JsonValue item = io::JsonValue::make_object();
		item.set("path", io::JsonValue::make_string(path));
		item.set("size", io::JsonValue::make_number(double(cached.size)));
		item.set("modified", io::JsonValue::make_string(io::hex64(uint64_t(cached.modified))));
		item.set("sha256", io::JsonValue::make_string(cached.sha256));
		list.push(std::move(item));
	}
	json.set("files", std::move(list));
	const std::string text = io::json_write(json);
	if (text != cache_text_) {
		std::error_code ec;
		fs::create_directories(system_path(content_.dir), ec);
		std::string message;
		io::write_file_atomic(
				content_.hashes_file, text, message); // a cache not written is only slower
	}
	phase_ = Phase::Done;
}

bool ContentScan::step(uint64_t budget) {
	uint64_t spent = 0;
	do {
		switch (phase_) {
			case Phase::Start: {
				pass_began_ = io::file_clock_now_ticks();
				load_cache();
				// A folder it may not enter stops the walk (incomplete), never one passed over in
				// silence (skip_permission_denied) whose files a push would send as deleted.
				std::error_code ec;
				walk_ = fs::recursive_directory_iterator(root_, ec);
				if (ec) {
					incomplete_ = "the folder " + paths_.root + " cannot be read: " + ec.message();
					problems_.push_back(incomplete_);
					phase_ = Phase::Done;
					return true;
				}
				phase_ = Phase::Listing;
				break;
			}
			case Phase::Listing: {
				if (walk_ == fs::recursive_directory_iterator()) {
					std::sort(listed_.begin(), listed_.end(),
							[](const Listed &a, const Listed &b) { return a.path < b.path; });
					for (const Listed &file : listed_)
						bytes_to_hash_ += file.size;
					phase_ = Phase::Hashing;
					break;
				}
				spent += kWalkEntryCost;
				std::error_code ec;
				const fs::directory_entry &entry = *walk_;
				const fs::path path = entry.path();
				// An entry whose type, place or size cannot be told (in a folder it may list but
				// not look into) is a file the folder may hold that the list lacks: the scan is
				// incomplete, so no comparison takes it for deleted. One gone since it was listed
				// (or a link to nothing) is no file at all.
				const auto unlisted = [this](const std::string &name,
											  const std::error_code &why) {
					if (why == std::errc::no_such_file_or_directory ||
							why == std::errc::not_a_directory)
						return;
					const std::string line = name + " could not be listed: " + why.message();
					if (incomplete_.empty())
						incomplete_ = line;
					problems_.push_back(line);
				};
				const bool directory = entry.is_directory(ec);
				if (ec) {
					unlisted(utf8_of(path), ec);
				} else if (directory) {
					const bool skipped = export_dir_.empty() ? is_dot_directory(path)
															 : walk_skips_folder(path, export_dir_);
					if (skipped)
						walk_.disable_recursion_pending();
				} else if (const bool regular = entry.is_regular_file(ec); ec) {
					unlisted(utf8_of(path), ec);
				} else if (regular && !content_skips_file(utf8_of(path.filename()))) {
					Listed file;
					file.path = utf8_of(fs::relative(path, root_, ec));
					file.system = path;
					std::string why;
					if (ec) {
						unlisted(utf8_of(path), ec);
					} else if (!project::is_project_path(file.path, why)) {
						problems_.push_back(file.path + ": " + why);
					} else {
						file.size = entry.file_size(ec);
						if (ec) {
							unlisted(file.path, ec);
						} else {
							file.modified = io::file_modified_ticks(path);
							listed_.push_back(std::move(file));
						}
					}
				}
				walk_.increment(ec);
				if (ec) {
					incomplete_ = "the walk stopped in " + utf8_of(path) + ": " + ec.message();
					problems_.push_back(incomplete_);
					walk_ = fs::recursive_directory_iterator();
				}
				break;
			}
			case Phase::Hashing: {
				if (next_ == listed_.size()) {
					finish();
					return true;
				}
				if (hash_next(spent))
					++next_;
				break;
			}
			case Phase::Done:
				return true;
		}
	} while (spent < budget);
	return done();
}

ContentScan scan_content(const ProjectPaths &paths, const std::string &export_dir) {
	ContentScan scan(paths, export_dir);
	while (!scan.step(UINT64_MAX)) {
	}
	return scan;
}

} // namespace opennova::editor
