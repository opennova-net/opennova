#include <editor/import/import_plan.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <system_error>
#include <utility>

#include <base/gameprofile/gameprofile.h>
#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/assets/player_files.h>
#include <editor/graph/graph_names.h>
#include <editor/import/converter.h>
#include <editor/import/importer.h>
#include <editor/import/mission_fixed_files.h>
#include <editor/model/diagnostic.h>
#include <editor/project/expansion_files.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirements.h>
#include <formats/mns/mns.h>
#include <runtime/menu/menu_style.h>

namespace fs = std::filesystem;

namespace opennova::editor {

using graph_names::is_style_reference;
using graph_names::key;
using graph_names::style_variable;
using graph_names::symbol_name;

bool references_unread(AssetKind kind) {
	return asset_kind_row(kind).names_files && !graph_reads_kind(kind);
}

bool ImportOrigin::open(Kind kind, const std::string &path, const ProjectDocument &document, std::string &error) {
	kind_ = kind;
	path_ = path;
	names_.clear();
	kinds_.clear();
	vfs_.clear();
	if (kind == Kind::Folder) {
		std::error_code ec;
		fs::directory_iterator it(system_path(path), ec);
		if (ec) {
			error = "The folder " + path + " could not be listed: " + ec.message() + ".";
			return false;
		}
		for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
			std::error_code status;
			if (!it->is_regular_file(status)) continue;
			// Its name as UTF-8, whatever the code page (project_files.h, utf8_of).
			const std::string name = utf8_of(it->path().filename());
			names_.emplace(normalized_logical_name(name), name);
		}
		return true;
	}
	if (kind == Kind::GameInstall) {
		// The install as the project imports it (assets/install_view.h): each file by the name the
		// project gets.
		if (!install_.open(install_spec(path, document), error)) return false;
		for (const InstallFile &file : install_.files()) names_.emplace(normalized_logical_name(file.name), file.name);
		return true;
	}
	vfs_.set_scr_policy(gameprofile::gameprofile_scr_policy_for_code(document.target_game.c_str()));
	if (!vfs_.set_primary_archive(path)) {
		error = "Could not open archive: " + path;
		return false;
	}
	for (const VfsFileLocation &file : vfs_.list_files())
		if (!strutil::ends_with_icase(file.logical_name, ".pff")) // the archives themselves
			names_.emplace(normalized_logical_name(file.logical_name), file.logical_name);
	return true;
}

std::string ImportOrigin::find(const std::string &name) const {
	const auto found = names_.find(normalized_logical_name(name));
	return found == names_.end() ? std::string() : found->second;
}

bool ImportOrigin::read(const std::string &name, std::vector<uint8_t> &out) const {
	if (kind_ == Kind::GameInstall) {
		const InstallFile *file = install_.find(name);
		return file && install_.read(*file, out);
	}
	if (kind_ == Kind::Archive) return read_served(vfs_, name, out);
	std::string error;
	return read_file_bytes(join_path(path_, name), out, error);
}

uint64_t ImportOrigin::size(const std::string &name) const {
	const std::string spelling = find(name);
	if (spelling.empty()) return 0;
	if (kind_ == Kind::GameInstall) {
		const InstallFile *file = install_.find(spelling);
		return file ? install_.size(*file) : 0;
	}
	if (kind_ == Kind::Archive) {
		uint64_t stored = 0;
		return vfs_.file_size(spelling, stored) ? stored : 0;
	}
	std::error_code ec;
	const auto on_disk = fs::file_size(system_path(join_path(path_, spelling)), ec);
	return ec ? 0 : static_cast<uint64_t>(on_disk);
}

std::vector<std::string> ImportOrigin::files_of_kind(AssetKind kind) const {
	std::vector<std::string> out;
	for (const auto &entry : names_)
		if (classify_asset(entry.second, nullptr) == kind) out.push_back(entry.second);
	return out;
}

AssetKind ImportOrigin::file_kind(const std::string &name) const {
	const std::string spelling = find(name);
	if (spelling.empty()) return AssetKind::Unknown;
	const std::string normalized = normalized_logical_name(spelling);
	const auto cached = kinds_.find(normalized);
	if (cached != kinds_.end()) return cached->second;
	AssetKind kind = classify_asset(spelling, nullptr);
	if (kind == AssetKind::Unknown || asset_classification_needs_bytes(spelling)) {
		std::vector<uint8_t> bytes;
		if (read(spelling, bytes)) kind = classify_asset(spelling, &bytes);
	}
	kinds_.emplace(normalized, kind);
	return kind;
}

ImportChoice ImportOrigin::source(const std::string &name) const {
	ImportChoice out;
	if (kind_ == Kind::Folder) {
		out.path = join_path(path_, name);
		out.native = true;
	} else if (kind_ == Kind::GameInstall) {
		const InstallFile *file = install_.find(name);
		out = file ? install_choice(path_, *file) : ImportChoice{ path_, name, true };
	} else {
		out.path = path_;
		out.entry = name;
	}
	return out;
}

std::string ImportOrigin::words() const {
	switch (kind_) {
	case Kind::Folder: return "the folder " + path_;
	case Kind::Archive: return "the archive " + path_;
	case Kind::GameInstall: return "the game install";
	}
	return path_;
}

namespace {

using Exists = std::function<bool(const std::string &)>;

// The first name a reference's loader reads that `exists` has; "" for none.
std::string first_candidate(const ImportNeed &need, const Exists &exists) {
	for (const std::string &candidate : reference_file_candidates(need.reference, need.name, need.loader_arg, exists))
		if (exists(candidate)) return candidate;
	return std::string();
}

// Two sources of one file: the same member of the same archive or install, or the same
// file of the same folder (names without case, as the folder's listing compares them).
bool same_file(const ImportChoice &a, const ImportChoice &b) {
	if (a.install != b.install || a.entry.empty() != b.entry.empty()) return false;
	if (!a.entry.empty()) return path_of(a.path) == path_of(b.path) && key(a.entry) == key(b.entry);
	const fs::path x = path_of(a.path), y = path_of(b.path);
	return x.parent_path() == y.parent_path() && key(utf8_of(x.filename())) == key(utf8_of(y.filename()));
}

// A file the walk reads the references of: its row, the place it came from, its bytes when
// they are in hand already (a loose source's, a converter's output: `loaded`; any other file's
// are read when its references are followed), and what they reference once read.
struct PlanNode {
	size_t row = 0;
	const ImportOrigin *origin = nullptr;
	std::vector<uint8_t> bytes;
	bool loaded = false;
	enum class Read { Not, Done, Failed } read = Read::Not;
	Extracted content;
};

// What one source or file costs a step besides the bytes it reads: placing its row resolves its
// destination on the disk (check_project_file_name), so a listing of an install's nine thousand
// names, which reads none of them, still steps a few files at a time (16 to the editor's 64 KiB
// step, as a scan's directory entries).
constexpr uint64_t kItemCost = 4096;

} // namespace

class ImportPlanner::Walk {
public:
	Walk(std::vector<ImportChoice> sources, bool with_dependencies, const ProjectPaths &paths,
	     const ProjectDocument &document, const AssetScan &scan, const AssetGraph &graph, std::string retail_directory,
	     size_t cap, std::shared_ptr<const ImportOrigin> install_mounted)
	    : sources_(std::move(sources)),
	      with_dependencies_(with_dependencies),
	      retail_directory_(std::move(retail_directory)),
	      paths_(paths),
	      document_(document),
	      scan_(scan),
	      graph_(graph),
	      cap_(cap) {
		if (install_mounted && !retail_directory_.empty())
			adopt(ImportOrigin::Kind::GameInstall, retail_directory_, std::move(install_mounted));
	}

	// One step (ImportPlanner::step): the install mounted, a step of its own; then sources and
	// queued files while the step's bytes and items last.
	bool step(uint64_t bytes) {
		cost_ = 0;
		for (size_t items = 0; phase_ != Phase::Done && (items == 0 || cost_ < bytes); ++items) {
			cost_ += kItemCost;
			switch (phase_) {
			case Phase::Install:
				if (with_dependencies_ && !retail_directory_.empty()) {
					std::string error;
					const ImportOrigin *install = origin(ImportOrigin::Kind::GameInstall, retail_directory_, error);
					if (!install)
						plan_.diagnostics.push_back(
						        make_finding(CoreFinding::ImportInstall, DiagnosticSeverity::Warning,
						                     error + " The files the import needs are not looked for there."));
					set_install(install);
				}
				phase_ = Phase::Sources;
				return false;
			case Phase::Sources:
				if (next_source_ < sources_.size() && !plan_.truncated) {
					add_source(sources_[next_source_++], with_dependencies_);
					break;
				}
				phase_ = with_dependencies_ && !plan_.truncated ? Phase::Mission : Phase::Done;
				break;
			case Phase::Mission:
				// A planned mission's own files and the game's: one mission a step.
				if (next_mission_ < missions_.size() && !plan_.truncated) {
					add_mission_files(missions_[next_mission_++]);
					break;
				}
				phase_ = Phase::Stylesheets;
				break;
			case Phase::Stylesheets:
				read_stylesheets();
				sheets_dirty_ = false;
				phase_ = Phase::Follow;
				break;
			case Phase::Follow:
				if (!queue_.empty() && !plan_.truncated) {
					PlanNode node = std::move(queue_.front());
					queue_.pop_front();
					if (again_queued_ > 0 && followed_.count(node.row)) --again_queued_;
					follow(node);
					break;
				}
				queue_.clear();
				again_queued_ = 0;
				phase_ = Phase::Symbols;
				break;
			case Phase::Symbols:
				// The symbol references of every file followed, each to the file that defines it:
				// one a step; a file it brings is followed in turn (a round of Follow), with the
				// stylesheets read again when it brought one.
				if (next_symbol_ < symbols_.size() && !plan_.truncated) {
					follow_symbol(symbols_[next_symbol_++]);
					break;
				}
				if (!queue_.empty() && !plan_.truncated) {
					if (sheets_dirty_) {
						sheets_dirty_ = false;
						read_stylesheets();
						requeue_menus();
					}
					phase_ = Phase::Follow;
					break;
				}
				settle();
				phase_ = Phase::Done;
				break;
			case Phase::Done: break;
			}
		}
		known_ = std::max(known_, files_ + (sources_.size() - next_source_));
		return phase_ == Phase::Done;
	}

	bool done() const { return phase_ == Phase::Done; }
	size_t files_known() const { return known_; }
	// The files followed: a menu queued again after a stylesheet came is done already, so the count
	// never falls (review F13).
	size_t files_done() const { return files_ - std::min(files_, queue_.size() - again_queued_); }
	ImportPlan take() { return std::move(plan_); }

private:
	enum class Phase : uint8_t { Install, Sources, Mission, Stylesheets, Follow, Symbols, Done };

	// A symbol reference of a planned file, kept for the Symbols phase (every file followed
	// first, so a planned file that defines the name is known).
	struct SymbolUse {
		std::string file;
		GraphEdge edge;
	};
	// A file of a place read for the names it defines (a candidate for a symbol): each symbol's
	// key and scope.
	struct Definitions {
		std::vector<std::pair<std::string, std::string>> symbols; // (GraphIndex::key_of, scope)
	};

	// The place of a kind at a path, opened once; null (with `error`) when it does not open.
	const ImportOrigin *origin(ImportOrigin::Kind kind, const std::string &path, std::string &error) {
		auto found = origins_.find({kind, path});
		if (found == origins_.end()) {
			Opened opened;
			auto made = std::make_shared<ImportOrigin>();
			if (made->open(kind, path, document_, opened.error)) opened.origin = std::move(made);
			found = origins_.emplace(std::make_pair(kind, path), std::move(opened)).first;
		}
		error = found->second.error;
		return found->second.origin.get();
	}

	void set_install(const ImportOrigin *install) { install_ = install; }
	// The place of a kind at a path, opened already (the game install a caller mounted).
	void adopt(ImportOrigin::Kind kind, const std::string &path, std::shared_ptr<const ImportOrigin> origin) {
		origins_[{kind, path}] = Opened{std::move(origin), std::string()};
	}

	// A selected source, as import_assets takes it; what a converter makes of it, each output a
	// row (the first file of a name only is walked). A file of an archive or of the install is
	// looked up in its listing and read only for a converter, which makes its outputs of the
	// bytes (any other is read when its references are followed, and never when the graph does
	// not read it); a loose file is read, as its kind and its importer are asked of its bytes.
	// The cap stops it before it is read, and takes a converter's outputs whole or not at all.
	void add_source(const ImportChoice &source, bool walk) {
		if (files_ >= cap_) {
			plan_.truncated = true;
			return;
		}
		const std::string name = source.name();
		const auto fail = [&](CoreFinding code, const std::string &message) {
			plan_.diagnostics.push_back(make_finding(code, DiagnosticSeverity::Error, message, name));
		};
		const Converter *converter = converter_for(name);
		const ImportOrigin *from = nullptr;
		std::string found_in, error;
		std::vector<uint8_t> bytes;
		bool loaded = false; // the source's bytes are in hand
		if (source.install || !source.entry.empty()) {
			from = origin(source.install ? ImportOrigin::Kind::GameInstall : ImportOrigin::Kind::Archive, source.path, error);
			if (!from) {
				fail(source.install ? CoreFinding::ImportInstall : CoreFinding::ImportArchive, error);
				return;
			}
			// By the name the project gets (an install's file under the project's expansion's name: `as`).
			if (from->find(name).empty() || (converter && !from->read(name, bytes))) {
				fail(CoreFinding::ImportRead, source.install ? "The game data has no file named " + name + "."
				                                  : "Could not read " + name + " from " + source.path);
				return;
			}
			loaded = converter != nullptr;
			cost_ += bytes.size();
			found_in = from->words();
		} else {
			std::string io_error;
			if (!read_file_bytes(source.path, bytes, io_error)) {
				fail(CoreFinding::ImportRead, io_error);
				return;
			}
			loaded = true;
			cost_ += bytes.size();
			// The folder it sits in (a bare name's is the working folder): where the files it
			// names are looked for first.
			std::string folder = utf8_of(path_of(source.path).parent_path());
			if (folder.empty()) {
				std::error_code ec;
				folder = utf8_of(fs::current_path(ec));
			}
			found_in = "the folder " + folder;
			if (walk) {
				from = origin(ImportOrigin::Kind::Folder, folder, error);
				if (!from)
					plan_.diagnostics.push_back(make_finding(CoreFinding::ImportFolder, DiagnosticSeverity::Warning,
					                                         error + " The files " + name +
					                                                 " names are not looked for there.",
					                                         name));
			}
		}
		// A converter's outputs are made in memory: the textures an .o3d names are its model's
		// references, followed like any other (a row found, or one not found).
		std::vector<ImportOutput> outputs;
		if (converter) {
			ImportProduct product;
			converter->run(name, bytes, product);
			bool broken = false;
			for (Diagnostic &d : product.diagnostics) {
				broken = broken || d.severity == DiagnosticSeverity::Error;
				plan_.diagnostics.push_back(std::move(d));
			}
			if (broken) return;
			outputs = std::move(product.outputs);
		} else {
			outputs.push_back({name, std::move(bytes)});
		}
		// The files one source makes come together (import_assets writes them all): the cap
		// takes all of them or none.
		if (files_ + outputs.size() > cap_) {
			plan_.truncated = true;
			return;
		}
		for (ImportOutput &output : outputs) {
			ImportPlanRow row;
			row.state = ImportPlanRow::State::Selected;
			row.selected = true;
			row.source = source;
			row.name = output.name;
			// An author's file an importer converts becomes an import source, its record written with
			// it (import_assets); the game's own file (an archive's, the install's, one copied native)
			// gets no record and is the kind its name and bytes give: a PNG of it the texture the game
			// loads.
			const bool authored = !source.install && source.entry.empty() && !source.native;
			row.kind = authored && importer_for(output.name) ? AssetKind::ImportSource
			           : loaded                              ? classify_asset(output.name, &output.bytes)
			                                                 : from->file_kind(name);
			row.size = loaded ? output.bytes.size() : from->size(name);
			row.made_from = converter ? name : std::string();
			row.found_in = found_in;
			const bool first = !provided_.count(key(output.name));
			if (first)
				provided_[key(output.name)] = {plan_.rows.size(), row.kind};
			else
				row.problem = (converter ? "More than one selected file makes " : "More than one selected file has the name ") +
				              output.name + ".";
			place(row);
			// A file of a name the project holds is left as it is unless the import is asked to replace
			// it (review F2): marked, and not taken by default.
			if (scan_.find(output.name)) {
				row.held = true;
				row.selected = false;
			}
			plan_.rows.push_back(std::move(row));
			++files_;
			const size_t index = plan_.rows.size() - 1;
			if (converter) made_[index] = std::make_shared<const std::vector<uint8_t>>(output.bytes);
			if (walk && first) {
				planned(index, from);
				queue(index, from, std::move(output.bytes), loaded);
			}
		}
	}

	// A file the plan took, whatever took it: a mission (its .bms) brings its own files and the
	// game's (Phase::Mission); a shell stylesheet changes what a %NAME% expands to.
	void planned(size_t index, const ImportOrigin *from) {
		const ImportPlanRow &row = plan_.rows[index];
		if (row.kind == AssetKind::Mission && strutil::ends_with_icase(row.name, ".bms")) missions_.push_back({index, from});
		if (row.kind == AssetKind::MenuStyle && menu::is_shell_stylesheet(row.name)) sheets_dirty_ = true;
	}

	// The file `name` of the places, as a found row the plan takes (its row index), or none: the
	// project's scan has it (nothing to bring), the plan has it (that row), else the origin the
	// wanting file came from, then the install; `need` says what wanted it.
	bool bring(const ImportOrigin *own, const std::string &name, const ImportNeed &need, size_t *row_out = nullptr) {
		if (row_out) *row_out = SIZE_MAX;
		if (scan_.find(name)) return true;
		const auto taken = provided_.find(key(name));
		if (taken != provided_.end()) {
			if (row_out) *row_out = taken->second.row;
			return true;
		}
		const ImportOrigin *install = install_ != own ? install_ : nullptr;
		const ImportOrigin *from = own && !own->find(name).empty() ? own : nullptr;
		if (!from && install && !install->find(name).empty()) from = install;
		if (!from) return false;
		if (files_ >= cap_) {
			plan_.truncated = true;
			return false;
		}
		const std::string spelling = from->find(name);
		ImportPlanRow row;
		row.state = ImportPlanRow::State::Found;
		row.selected = true;
		row.source = from->source(spelling);
		row.name = spelling;
		row.kind = from->file_kind(spelling);
		row.size = from->size(spelling);
		row.found_in = from->words();
		row.needed_by = need;
		place(row);
		row.selected = row.problem.empty();
		const size_t index = plan_.rows.size();
		plan_.rows.push_back(std::move(row));
		provided_[key(spelling)] = {index, plan_.rows[index].kind};
		++files_;
		if (from == own && install && !install->find(name).empty()) add_rival(index, install, install->find(name));
		planned(index, from);
		queue(index, from);
		if (row_out) *row_out = index;
		return true;
	}

	// The files the game opens by a fixed literal on the way into a mission, once whatever the
	// missions' number (ADR 0046 S14): the manifest's boot, menu and mission rows (a Required one
	// found nowhere is not found, an optional one no row), then mission_fixed_files(), the fixed names a
	// running mission opens, none ever not found (the game goes on without each). Each found is followed like any other file. The files the game finds by
	// the mission's own name are its edges (documents/mission_file_set.h), which the walk follows.
	void add_mission_files(const std::pair<size_t, const ImportOrigin *> &mission) {
		const std::string file = plan_.rows[mission.first].name;
		const ImportOrigin *own = mission.second;
		if (!manifest_done_) {
			manifest_done_ = true;
			using namespace opennova::gameprofile;
			const int count = gameprofile_required_resource_count();
			for (int i = 0; i < count; ++i) {
				const RequiredResource *resource = gameprofile_required_resource_at(i);
				// An expansion's own file, for a project that builds as one (ADR 0046 S16): by the name
				// its expansion forms, the install's copy of it listed under that name (install_view.h);
				// every row optional, the version text the build's own.
				if (resource->flags & RES_F_EXPANSION) {
					const ExpansionFileRow *expansion_file = document_.expansion.standalone()
					        ? nullptr : expansion_file_row_for_manifest_role(resource->role);
					if (!expansion_file || expansion_file->fixed) continue;
					const std::string name = expansion_file_name(*expansion_file, document_.expansion.name);
					bring(own, name, ImportNeed{file, std::string(),
					                            std::string("the game, ") + requirement_phase_label(resource->phase),
					                            ReferenceKind::None, name, -1});
					if (plan_.truncated) return;
					continue;
				}
				// A pattern, a boot archive, or the player's own file (a save, a configuration, the
				// stored credentials: never a resource the game is made of, and in a game's folder
				// beside the missions found loose there) is never brought.
				if (resource->flags & (RES_F_PATTERN | RES_F_PFF_TABLE_ANY | RES_F_PLAYER_FILE)) continue;
				const ImportNeed need{file, std::string(),
				                      std::string("the game, ") + requirement_phase_label(resource->phase),
				                      ReferenceKind::None, resource->name, -1};
				if (bring(own, resource->name, need)) continue;
				if (plan_.truncated) return;
				if (resource->severity != RES_OPTIONAL) not_found(need, expected_asset_kind_for_required_name(resource->name));
			}
			for (const MissionFixedFile &fixed : mission_fixed_files()) {
				const ImportNeed need{file, std::string(), "the game, " + fixed.what, ReferenceKind::None, fixed.name, -1};
				bring(own, fixed.name, need);
				if (plan_.truncated) return;
			}
		}
	}

	// The menus followed so far, followed again (a stylesheet the walk brought changed what their
	// %NAME%s expand to): each read again.
	void requeue_menus() {
		for (const auto &menu : menus_) queue(menu.first, menu.second);
		again_queued_ += menus_.size();
	}

	// What a planned file defines, once extracted: the symbol references of other files resolve
	// against it (follow_symbol).
	void index_symbols(const PlanNode &node) {
		for (const GraphSymbol &symbol : node.content.symbols) {
			if (symbol.inert) continue;
			planned_symbols_[GraphIndex::key_of(symbol.kind, symbol.name, std::string())].push_back(symbol.scope);
		}
	}

	// Whether a planned file defines a symbol of `kind` named `name` where `scope` reads it.
	bool planned_defines(ReferenceKind kind, const std::string &name, const std::string &scope) const {
		const auto found = planned_symbols_.find(GraphIndex::key_of(kind, symbol_name(kind, name), std::string()));
		if (found == planned_symbols_.end()) return false;
		for (const std::string &defined_in : found->second)
			if (scope_matches(defined_in, scope)) return true;
		return false;
	}

	// What a file of a place defines, read once per place and file (a catalog's names, a
	// particle file's effects, a stylesheet's variables); null when it does not read.
	const Definitions *definitions_of(const ImportOrigin *from, const std::string &spelling) {
		const std::string cache_key = from->path() + '\n' + key(spelling);
		auto found = read_definitions_.find(cache_key);
		if (found != read_definitions_.end()) return found->second ? &*found->second : nullptr;
		std::unique_ptr<Definitions> made;
		std::vector<uint8_t> bytes;
		Extracted content;
		Diagnostic error;
		const AssetKind kind = from->file_kind(spelling);
		if (graph_reads_kind(kind) && from->read(spelling, bytes)) {
			cost_ += bytes.size();
			if (extract_from_bytes(spelling, kind, bytes, document_.target_game, content, error)) {
				made = std::make_unique<Definitions>();
				for (const GraphSymbol &symbol : content.symbols)
					if (!symbol.inert)
						made->symbols.push_back({GraphIndex::key_of(symbol.kind, symbol.name, std::string()), symbol.scope});
			}
		}
		const Definitions *out = made.get();
		read_definitions_.emplace(cache_key, std::move(made));
		return out;
	}

	bool defines(const Definitions &definitions, ReferenceKind kind, const std::string &name, const std::string &scope) const {
		const std::string wanted = GraphIndex::key_of(kind, symbol_name(kind, name), std::string());
		for (const auto &symbol : definitions.symbols)
			if (symbol.first == wanted && scope_matches(symbol.second, scope)) return true;
		return false;
	}

	// The files of a place a symbol of `kind` could be defined in, in the order looked: the file
	// its scope names where the kind's row says so (a string id's table, a screen's menu, a user
	// point's model; none when the scope names no file), else the files of the kind the row says
	// defines it (a style variable's, the two shell stylesheets the game reads).
	std::vector<std::string> defining_candidates(const ImportOrigin *from, ReferenceKind kind, const std::string &scope) {
		std::vector<std::string> out;
		if (!from) return out;
		const ReferenceKindRow &row = reference_row(kind);
		if (row.scope_names_file) {
			const std::string named = scope.substr(0, scope.find('/'));
			if (!named.empty()) {
				const std::string spelling = from->find(named);
				if (!spelling.empty()) out.push_back(spelling);
			}
			return out;
		}
		if (row.resolution == ReferenceResolution::StyleVariable) {
			for (const char *sheet : {"menu_style.mns", "brand.mns"}) {
				const std::string spelling = from->find(sheet);
				if (!spelling.empty()) out.push_back(spelling);
			}
			return out;
		}
		if (row.defined_in == AssetKind::Unknown) return out;
		// A place's files of a kind, listed once per place and kind (an install has nine thousand
		// names to type; a mission's items name hundreds of effects).
		const std::string cache_key = from->path() + '\n' + asset_kind_token(row.defined_in);
		auto listed = files_by_kind_.find(cache_key);
		if (listed == files_by_kind_.end())
			listed = files_by_kind_.emplace(cache_key, from->files_of_kind(row.defined_in)).first;
		return listed->second;
	}

	// A symbol reference followed to the file that defines it (import_plan.h): nothing when the
	// project or a planned file defines it; else the first file of the wanting file's origin, then
	// of the install, that does, planned and queued, or counted as shadowed where the project holds a
	// file of that name (its own copy, which lacks it, is kept); else counted as undefined.
	// The lookup is the graph's (AssetGraph::resolve): the scope it starts at (AssetGraph::
	// lookup_scope: the edge's own; its alternate table's where the project lacks the own table, the
	// own one tried first, which the import may bring; any table, nothing after, where the project
	// lacks its owner), then each scope it tries after (a mission script's text key: GAMETEXT.BIN), the
	// value then its fallback in each.
	void follow_symbol(const SymbolUse &use) {
		const GraphEdge &edge = use.edge;
		if (graph_.resolve(edge) == ReferenceStatus::Present) return;
		// A style variable is written as its %NAME% and defined by its NAME (graph_names).
		const bool variable = reference_row(edge.kind).spell == NameSpelling::StyleVariable && is_style_reference(edge.value);
		std::vector<std::string> names{variable ? style_variable(edge.value) : edge.value};
		if (!edge.fallback.empty()) names.push_back(edge.fallback);
		const std::string first = graph_.lookup_scope(edge);
		const bool owned = edge.scope_owner.empty() || graph_.has_file(edge.scope_owner);
		std::vector<const std::string *> scopes;
		if (owned && first != edge.scope) scopes.push_back(&edge.scope);
		scopes.push_back(&first);
		if (owned)
			for (const std::string &scope : edge.scopes_after) scopes.push_back(&scope);
		const ImportNeed need{use.file, edge.record, edge.field, edge.kind, edge.value, edge.loader_arg};
		const ImportOrigin *own = origin_of(use.file);
		const ImportOrigin *install = install_ != own ? install_ : nullptr;
		for (const std::string *scope : scopes) {
			for (const std::string &name : names) {
				if (planned_defines(edge.kind, name, *scope)) return;
				for (const ImportOrigin *from : {own, install}) {
					if (!from) continue;
					for (const std::string &candidate : defining_candidates(from, edge.kind, *scope)) {
						const Definitions *definitions = definitions_of(from, candidate);
						if (!definitions || !defines(*definitions, edge.kind, name, *scope)) continue;
						// A file of the name the project holds, its own copy, lacks it (the graph found
						// it nowhere): the import keeps that file, so the place's copy is not brought and
						// the name stays undefined where the game reads it (review F6).
						if (scan_.find(candidate)) {
							count_into(plan_.shadowed, edge.kind, from->file_kind(candidate), use.file);
							return;
						}
						// Planned already (brought for an earlier symbol, not yet followed): it
						// defines this one too.
						if (!provided_.count(key(candidate))) bring(from, candidate, need);
						return;
					}
				}
			}
		}
		count_into(plan_.undefined, edge.kind, AssetKind::Unknown, use.file);
	}

	// One more reference of a kind in a list kept once per kind (ImportPlan::undefined, shadowed).
	static void count_into(std::vector<ImportNotFollowed> &list, ReferenceKind reference, AssetKind kind,
	                       const std::string &file) {
		for (ImportNotFollowed &entry : list)
			if (entry.reference == reference) {
				++entry.count;
				return;
			}
		list.push_back({reference, kind, 1, file});
	}

	// The place a planned file came from (by its name); null for one the plan does not hold.
	const ImportOrigin *origin_of(const std::string &file) const {
		const auto found = origins_by_file_.find(key(file));
		return found == origins_by_file_.end() ? nullptr : found->second;
	}

	// The variables a %NAME% expands through once the import is in: the shell's own load of
	// menu_style.mns, then brand.mns over it [orig: Menu_InitShellResources @ 0x552500]
	// (menu::load_shell_style), each the selection's copy where it brings one, else the
	// project's.
	void read_stylesheets() {
		const int policy = gameprofile::gameprofile_scr_policy_for_code(document_.target_game.c_str());
		const menu::ShellStyle style = menu::load_shell_style([&](const std::string &name, std::string &text) {
			std::vector<uint8_t> bytes;
			if (!stylesheet_bytes(name, bytes)) return false;
			vfs_decode_payload(bytes, policy);
			text.assign(bytes.begin(), bytes.end());
			return true;
		});
		sheet_ = style.list.sheet();
	}

	struct Opened {
		std::shared_ptr<const ImportOrigin> origin;
		std::string error;
	};
	// A file the plan takes, by its normalized name: its row and what it is to the engine.
	struct Provided {
		size_t row = 0;
		AssetKind kind = AssetKind::Unknown;
	};
	// A name looked for in vain: its row, and every distinct lookup that wanted it (the same
	// name read by different rules can want different files).
	struct Missing {
		size_t row = 0;
		std::vector<ImportNeed> needs;
	};

	// A planned file whose references the walk follows: with its bytes when they are in hand
	// (`loaded`), else read when its turn comes.
	void queue(size_t row, const ImportOrigin *from, std::vector<uint8_t> bytes = {}, bool loaded = false) {
		PlanNode node;
		node.row = row;
		node.origin = from;
		node.bytes = std::move(bytes);
		node.loaded = loaded;
		origins_by_file_[key(plan_.rows[row].name)] = from;
		queue_.push_back(std::move(node));
	}

	// Where import_assets writes the file, and why the project could not take it.
	void place(ImportPlanRow &row) const {
		row.destination = import_destination(scan_, row.name, row.kind);
		if (!row.problem.empty()) return;
		FileNameProblem problem = FileNameProblem::None;
		std::string message;
		// The player's or this machine's own file is never the project's (assets/player_files.h).
		const std::string player = player_file_words(row.name);
		if (!player.empty())
			row.problem = row.name + " is " + player + ": an import never takes the player's own files.";
		else if (row.kind == AssetKind::Unknown || row.kind == AssetKind::Archive)
			row.problem = "Unsupported asset type: " + row.name;
		else if (!check_project_file_name(paths_.root, utf8_of(path_of(row.destination).parent_path()), row.name,
		                                  row.kind, problem, message))
			row.problem = message;
	}

	// A stylesheet the shell reads by name, as the project holds it once the import is in: the
	// plan's copy (chosen, or brought by the walk), else the project's file of the name.
	bool stylesheet_bytes(const std::string &name, std::vector<uint8_t> &bytes) {
		const auto taken = provided_.find(key(name));
		if (taken != provided_.end() && taken->second.kind == AssetKind::MenuStyle) return read_row(taken->second.row, bytes);
		const AssetEntry *asset = scan_.find(name);
		if (!asset || asset->kind != AssetKind::MenuStyle) return false;
		std::string error;
		return read_file_bytes(join_path(paths_.root, asset->relative_path), bytes, error);
	}

	// A planned row's bytes as the import writes them.
	bool read_row(size_t row, std::vector<uint8_t> &out) {
		const auto made = made_.find(row);
		if (made != made_.end()) {
			out = *made->second;
			return true;
		}
		const ImportChoice &source = plan_.rows[row].source;
		std::string error;
		bool read = false;
		if (!source.install && source.entry.empty()) {
			read = read_file_bytes(source.path, out, error);
		} else {
			const ImportOrigin *from = origin(source.install ? ImportOrigin::Kind::GameInstall : ImportOrigin::Kind::Archive,
			                                  source.path, error);
			read = from && from->read(source.name(), out);
		}
		if (read) cost_ += out.size();
		return read;
	}

	// A node's references and symbols, read once (its bytes read now when they were not in hand);
	// false (with one warning) when they cannot be.
	bool extract(PlanNode &node) {
		if (node.read != PlanNode::Read::Not) return node.read == PlanNode::Read::Done;
		const ImportPlanRow &row = plan_.rows[node.row];
		Diagnostic error;
		const bool have = node.loaded || read_row(node.row, node.bytes);
		node.loaded = true;
		if (!have) error.message = "The file could not be read";
		if (!have || !extract_from_bytes(row.name, row.kind, node.bytes, document_.target_game, node.content, error)) {
			node.read = PlanNode::Read::Failed;
			std::string reason = error.message.empty() ? std::string("The file could not be read.") : error.message;
			if (reason.back() != '.') reason += '.';
			plan_.diagnostics.push_back(make_finding(CoreFinding::ImportUnreadable, DiagnosticSeverity::Warning,
			                                         reason + " The files it names are not looked for.", row.name));
			return false;
		}
		node.read = PlanNode::Read::Done;
		return true;
	}

	// One kind not followed, counted where it was met first.
	void note(ReferenceKind reference, AssetKind kind, const std::string &file) {
		for (ImportNotFollowed &entry : plan_.not_followed)
			if (entry.reference == reference && entry.kind == kind) {
				++entry.count;
				return;
			}
		plan_.not_followed.push_back({reference, kind, 1, file});
	}

	// A %NAME% through the stylesheets the project reads once the import is in; a plain name
	// as it is. One they do not define stays a %NAME%.
	std::string resolve_name(const std::string &value) const {
		if (!is_style_reference(value)) return value;
		const std::string variable = style_variable(value);
		return sheet_.has(variable) ? sheet_.get(variable) : value;
	}

	// The planned file a reference loads: the first name its loader reads that the plan takes
	// as a file of a kind that serves it (file_serves_reference: the kind it loads, or for a chunk
	// row a material chunk); null for none.
	const Provided *provided_for(const ImportNeed &need) const {
		const std::string file = first_candidate(need, [this, &need](const std::string &name) {
			const auto found = provided_.find(key(name));
			return found != provided_.end() && file_serves_reference(found->second.kind, need.reference, need.loader_arg);
		});
		return file.empty() ? nullptr : &provided_.at(key(file));
	}

	// The origin's spelling of the file a reference loads from it: the first name its loader
	// reads that the origin has as a file of a kind that serves it, a name the plan takes as
	// another kind passed over (the project holds one file of a name); "" for none.
	std::string look(const ImportOrigin *origin, const ImportNeed &need) const {
		if (!origin) return std::string();
		const std::string file = first_candidate(need, [this, origin, &need](const std::string &name) {
			if (!file_serves_reference(origin->file_kind(name), need.reference, need.loader_arg)) return false;
			const auto taken = provided_.find(key(name));
			return taken == provided_.end() ||
			       file_serves_reference(taken->second.kind, need.reference, need.loader_arg);
		});
		return file.empty() ? std::string() : origin->find(file);
	}

	// Another place's file for a reference the planned row serves, reported beside it (once,
	// and never the planned file itself), with whether the bytes differ.
	void add_rival(size_t row, const ImportOrigin *origin, const std::string &spelling) {
		if (!origin || spelling.empty()) return;
		const ImportChoice source = origin->source(spelling);
		if (same_file(plan_.rows[row].source, source)) return;
		for (const ImportRival &rival : plan_.rows[row].rivals)
			if (same_file(rival.source, source)) return;
		ImportRival rival;
		rival.name = spelling;
		rival.found_in = origin->words();
		rival.source = source;
		std::vector<uint8_t> planned, other;
		rival.differs = !read_row(row, planned) || !origin->read(spelling, other) || other != planned;
		plan_.rows[row].rivals.push_back(std::move(rival));
	}

	// A planned file's references (none read for a file the graph does not read). Followed again
	// (a menu, after a stylesheet came): its file edges once more (each planned already), its
	// symbol edges not kept again.
	void follow(PlanNode &node) {
		const std::string file = plan_.rows[node.row].name;
		const AssetKind kind = plan_.rows[node.row].kind;
		const bool again = followed_.count(node.row) > 0;
		followed_.insert(node.row);
		// What it names is not looked for: a file of a kind that names files the graph does not
		// read (a dialog bank, a mission text).
		if (!again && references_unread(kind)) note(ReferenceKind::None, kind, file);
		if (!graph_reads_kind(kind) || !extract(node)) return;
		if (!again) index_symbols(node);
		if (!again && kind == AssetKind::Menu) menus_.push_back({node.row, node.origin});
		for (const GraphEdge &edge : node.content.edges) {
			if (plan_.truncated) return;
			// A stylesheet value the game does not read (another sheet's, or a definition after
			// the place it stops reading the file) loads no file.
			if (kind == AssetKind::MenuStyle && edge.field == "value" &&
			    (!sheet_.has(edge.record) || key(sheet_.get(edge.record)) != key(edge.value)))
				continue;
			follow_edge(node, file, edge, again);
		}
	}

	// One reference of a node's file, in the plan's order (import_plan.h).
	void follow_edge(const PlanNode &node, const std::string &file, const GraphEdge &edge, bool again) {
		const ReferenceKindRow &kind_row = reference_row(edge.kind);
		const AssetKind wanted = kind_row.file;
		if (wanted == AssetKind::Unknown) {
			if (again) return;
			// A symbol or a style variable: followed to the file that defines it once every file is
			// (Phase::Symbols); a record of the file's own (S13 D8) resolves there; an unchecked kind
			// (a sound) names no file the plan can look for.
			if (kind_row.names_symbol() && kind_row.resolution != ReferenceResolution::Record) {
				symbols_.push_back({file, edge});
				return;
			}
			if (kind_row.resolution == ReferenceResolution::Record) return;
			note(edge.kind, AssetKind::Unknown, file);
			return;
		}
		// A file the reader reads only beside another (GraphEdge::needs: a mission's dialog sounds, beside
		// its .dbf): followed only when the project or the plan has that one (the mission's edges come in
		// the table's order, the .dbf's first).
		if (!edge.needs.empty() && !scan_.find(edge.needs) && !provided_.count(key(edge.needs))) return;
		const std::string name = resolve_name(edge.value);
		if (is_style_reference(name)) return; // no stylesheet defines it: its variable's reference is listed
		// The name, then the one its loader takes next where it has one (a mission's text table, then
		// medmssn.bin; its dialog sounds' .lwf, then .pwf): the first the project, the plan or a place
		// serves.
		std::vector<std::string> names{name};
		if (!edge.fallback.empty()) names.push_back(edge.fallback);
		const ImportOrigin *own = node.origin;
		const ImportOrigin *install = install_ != own ? install_ : nullptr;
		ImportNeed need{file, edge.record, edge.field, edge.kind, name, edge.loader_arg};
		std::string mine, theirs;
		for (const std::string &candidate : names) {
			need.name = candidate;
			if (graph_.resolve(edge.kind, candidate, edge.scope, nullptr, edge.loader_arg) == ReferenceStatus::Present) return;
			mine = look(own, need);
			theirs = look(install, need);
			// A file the plan takes serves it: the places that have one too are that file's rivals.
			if (const Provided *taken = provided_for(need)) {
				const size_t row = taken->row;
				add_rival(row, own, mine);
				add_rival(row, install, theirs);
				return;
			}
			if (!mine.empty() || !theirs.empty()) break;
		}
		if (mine.empty() && theirs.empty()) {
			// A file the game runs without (GraphEdge::optional: a mission's script, its dialog bank)
			// is no row when no place has it; any other is not found by the name it is written as.
			if (edge.optional) return;
			need.name = name;
			not_found(need, wanted);
			return;
		}
		if (files_ >= cap_) {
			plan_.truncated = true;
			return;
		}
		// The origin first; the game install's file, when it has one too, is its rival.
		const ImportOrigin *from = mine.empty() ? install : own;
		const std::string spelling = mine.empty() ? theirs : mine;
		if (provided_.count(key(spelling))) return;
		ImportPlanRow row;
		row.state = ImportPlanRow::State::Found;
		row.selected = true;
		row.source = from->source(spelling);
		row.name = spelling;
		row.kind = from->file_kind(spelling);
		row.size = from->size(spelling);
		row.found_in = from->words();
		row.needed_by = need;
		place(row);
		row.selected = row.problem.empty(); // one the project cannot take is listed, not taken
		const size_t index = plan_.rows.size();
		plan_.rows.push_back(std::move(row));
		provided_[key(spelling)] = {index, plan_.rows[index].kind};
		++files_;
		if (!mine.empty() && !theirs.empty()) add_rival(index, install, theirs);
		// Its bytes are read when the walk reads its references (extract), never for a file the
		// graph does not read.
		planned(index, from);
		queue(index, from);
	}

	// A lookup no place meets: one row per name, holding each distinct lookup of it.
	void not_found(const ImportNeed &need, AssetKind wanted) {
		const std::string presentation = std::string(asset_kind_token(wanted)) + '\n' + key(need.name);
		auto found = missing_.find(presentation);
		if (found == missing_.end()) {
			ImportPlanRow row;
			row.state = ImportPlanRow::State::NotFound;
			row.name = need.name;
			row.kind = wanted;
			row.needed_by = need;
			plan_.rows.push_back(std::move(row));
			found = missing_.emplace(presentation, Missing{plan_.rows.size() - 1, {}}).first;
		}
		for (const ImportNeed &known : found->second.needs)
			if (known.reference == need.reference && known.name == need.name && known.loader_arg == need.loader_arg)
				return;
		found->second.needs.push_back(need);
	}

	// Once the walk is done: a lookup a file planned since meets is met; a row whose every
	// lookup is met goes, another names the first lookup still unmet.
	void settle() {
		std::vector<bool> met(plan_.rows.size(), false);
		for (const auto &entry : missing_) {
			const Missing &missing = entry.second;
			const ImportNeed *unmet = nullptr;
			for (const ImportNeed &need : missing.needs)
				if (!provided_for(need)) {
					unmet = &need;
					break;
				}
			if (unmet)
				plan_.rows[missing.row].needed_by = *unmet;
			else
				met[missing.row] = true;
		}
		std::vector<ImportPlanRow> rows;
		rows.reserve(plan_.rows.size());
		for (size_t i = 0; i < plan_.rows.size(); ++i)
			if (!met[i]) rows.push_back(std::move(plan_.rows[i]));
		plan_.rows = std::move(rows);
	}

	ImportPlan plan_;
	const std::vector<ImportChoice> sources_;
	const bool with_dependencies_;
	const std::string retail_directory_;
	const ProjectPaths paths_;        // the planner's own copies: a caller's temporaries may go
	const ProjectDocument document_;
	const AssetScan &scan_;
	const AssetGraph &graph_;
	const size_t cap_;
	Phase phase_ = Phase::Install;
	size_t next_source_ = 0; // the next chosen source to take
	uint64_t cost_ = 0;      // the bytes this step read
	size_t known_ = 0;       // the most files known of at the end of a step
	std::map<std::pair<ImportOrigin::Kind, std::string>, Opened> origins_;
	const ImportOrigin *install_ = nullptr;
	std::deque<PlanNode> queue_;
	// The closure's own books (import_plan.h): the missions planned and their places, one brought
	// in turn; whether the game's manifest was brought (once, for the first); the symbol references
	// kept for the Symbols phase, followed in turn; what the planned files define, by key, each
	// definition's scope; what the places' files define, read once per place and file; the place
	// each planned file came from; the menus followed, followed again after a stylesheet came; the
	// rows followed (a file followed again keeps no second note).
	std::vector<std::pair<size_t, const ImportOrigin *>> missions_;
	size_t next_mission_ = 0;
	bool manifest_done_ = false;
	std::vector<SymbolUse> symbols_;
	size_t next_symbol_ = 0;
	std::map<std::string, std::vector<std::string>> planned_symbols_;
	std::map<std::string, std::unique_ptr<Definitions>> read_definitions_;
	std::map<std::string, std::vector<std::string>> files_by_kind_; // a place's files of a kind, once
	std::map<std::string, const ImportOrigin *> origins_by_file_;
	std::vector<std::pair<size_t, const ImportOrigin *>> menus_;
	size_t again_queued_ = 0; // the menus in queue_ queued again, followed once already
	std::set<size_t> followed_;
	bool sheets_dirty_ = false;
	std::map<std::string, Provided> provided_;                             // the files the plan takes
	std::map<size_t, std::shared_ptr<const std::vector<uint8_t>>> made_; // a converter output's bytes, by row
	std::map<std::string, Missing> missing_;                               // the names looked for in vain
	mns::StyleSheet sheet_;                                                // the variables a %NAME% expands through
	size_t files_ = 0;                                                     // the files the plan takes
};

ImportPlanner::ImportPlanner(std::vector<ImportChoice> sources, bool with_dependencies, const ProjectPaths &paths,
                             const ProjectDocument &document, const AssetScan &scan, const AssetGraph &graph,
                             std::string retail_directory, size_t file_cap,
                             std::shared_ptr<const ImportOrigin> install_mounted)
    : walk_(std::make_unique<Walk>(std::move(sources), with_dependencies, paths, document, scan, graph,
                                   std::move(retail_directory), file_cap, std::move(install_mounted))) {}

ImportPlanner::~ImportPlanner() = default;

bool ImportPlanner::step(uint64_t bytes) { return walk_->step(bytes); }
bool ImportPlanner::done() const { return walk_->done(); }
size_t ImportPlanner::files_known() const { return walk_->files_known(); }
size_t ImportPlanner::files_done() const { return walk_->files_done(); }
ImportPlan ImportPlanner::take() { return walk_->take(); }

ImportPlan plan_import(const std::vector<ImportChoice> &sources, bool with_dependencies, const ProjectPaths &paths,
                       const ProjectDocument &document, const AssetScan &scan, const AssetGraph &graph,
                       const std::string &retail_directory, size_t file_cap,
                       std::shared_ptr<const ImportOrigin> install_mounted) {
	ImportPlanner planner(sources, with_dependencies, paths, document, scan, graph, retail_directory, file_cap,
	                      std::move(install_mounted));
	while (!planner.step(UINT64_MAX)) {
	}
	return planner.take();
}

size_t ImportPlan::file_count() const {
	size_t files = 0;
	for (const ImportPlanRow &row : rows) files += row.state != ImportPlanRow::State::NotFound ? 1 : 0;
	return files;
}

uint64_t ImportPlan::total_bytes() const {
	uint64_t bytes = 0;
	for (const ImportPlanRow &row : rows) bytes += row.state != ImportPlanRow::State::NotFound ? row.size : 0;
	return bytes;
}

std::vector<ImportPlanKind> ImportPlan::by_kind() const {
	std::map<AssetKind, ImportPlanKind> kinds;
	for (const ImportPlanRow &row : rows) {
		if (row.state == ImportPlanRow::State::NotFound) continue;
		ImportPlanKind &entry = kinds[row.kind];
		entry.kind = row.kind;
		++entry.files;
		entry.bytes += row.size;
	}
	std::vector<ImportPlanKind> out;
	for (const auto &entry : kinds) out.push_back(entry.second);
	std::sort(out.begin(), out.end(), [](const ImportPlanKind &a, const ImportPlanKind &b) {
		if (a.bytes != b.bytes) return a.bytes > b.bytes;
		if (a.files != b.files) return a.files > b.files;
		return std::string(asset_kind_token(a.kind)) < asset_kind_token(b.kind);
	});
	return out;
}

bool same_import(const ImportPlan &a, const ImportPlan &b) {
	if (a.truncated != b.truncated || a.rows.size() != b.rows.size()) return false;
	for (size_t i = 0; i < a.rows.size(); ++i) {
		const ImportPlanRow &x = a.rows[i];
		const ImportPlanRow &y = b.rows[i];
		if (x.state != y.state || x.name != y.name || x.kind != y.kind || x.source != y.source ||
		    x.destination != y.destination || x.made_from != y.made_from || x.selected != y.selected || x.held != y.held ||
		    x.problem != y.problem)
			return false;
	}
	return true;
}

} // namespace opennova::editor
