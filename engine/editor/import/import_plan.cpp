#include <editor/import/import_plan.h>

#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/graph/graph_names.h>
#include <editor/import/converter.h>
#include <editor/import/importer.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/mns/mns.h>
#include <runtime/menu/menu_style.h>

namespace fs = std::filesystem;

namespace opennova::editor {

using graph_names::is_style_reference;
using graph_names::key;
using graph_names::style_variable;

bool references_unread(AssetKind kind, const std::string &file) {
	const AssetKindRow &row = asset_kind_row(kind);
	return row.names_files && (row.names_unfollowed || !graph_reads_file(kind, file));
}

bool ImportOrigin::open(Kind kind, const std::string &path, const ProjectDocument &document, std::string &error) {
	kind_ = kind;
	path_ = path;
	names_.clear();
	kinds_.clear();
	vfs_.clear();
	if (kind == Kind::Folder) {
		std::error_code ec;
		fs::directory_iterator it(path, ec);
		if (ec) {
			error = "The folder " + path + " could not be listed: " + ec.message() + ".";
			return false;
		}
		for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
			std::error_code status;
			try {
				if (it->is_regular_file(status)) {
					const std::string name = it->path().filename().string();
					names_.emplace(normalized_logical_name(name), name);
				}
			} catch (const std::exception &) {
				// A name the narrow encoding cannot carry is no file an import reads.
			}
		}
		return true;
	}
	if (kind == Kind::Archive) {
		vfs_.set_scr_policy(gameprofile::gameprofile_scr_policy_for_code(document.target_game.c_str()));
		if (!vfs_.set_primary_archive(path)) {
			error = "Could not open archive: " + path;
			return false;
		}
	} else if (!mount_retail(vfs_, path, document)) {
		error = "No game archives found under " + path + ".";
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
	if (kind_ != Kind::Folder) return read_served(vfs_, name, out);
	std::string error;
	return read_file_bytes((fs::path(path_) / name).generic_string(), out, error);
}

uint64_t ImportOrigin::size(const std::string &name) const {
	const std::string spelling = find(name);
	if (spelling.empty()) return 0;
	if (kind_ != Kind::Folder) {
		uint64_t stored = 0;
		return vfs_.file_size(spelling, stored) ? stored : 0;
	}
	std::error_code ec;
	const auto on_disk = fs::file_size(fs::path(path_) / spelling, ec);
	return ec ? 0 : static_cast<uint64_t>(on_disk);
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
		out.path = (fs::path(path_) / name).generic_string();
		out.native = true;
	} else {
		out.path = path_;
		out.entry = name;
		out.install = kind_ == Kind::GameInstall;
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
	if (!a.entry.empty()) return fs::path(a.path) == fs::path(b.path) && key(a.entry) == key(b.entry);
	const fs::path x(a.path), y(b.path);
	return x.parent_path() == y.parent_path() && key(x.filename().string()) == key(y.filename().string());
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
				phase_ = with_dependencies_ && !plan_.truncated ? Phase::Stylesheets : Phase::Done;
				break;
			case Phase::Stylesheets:
				read_stylesheets();
				phase_ = Phase::Follow;
				break;
			case Phase::Follow:
				if (!queue_.empty() && !plan_.truncated) {
					PlanNode node = std::move(queue_.front());
					queue_.pop_front();
					follow(node);
					break;
				}
				queue_.clear();
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
	size_t files_done() const { return files_ - std::min(files_, queue_.size()); }
	ImportPlan take() { return std::move(plan_); }

private:
	enum class Phase : uint8_t { Install, Sources, Stylesheets, Follow, Done };

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
			if (from->find(source.entry).empty() || (converter && !from->read(source.entry, bytes))) {
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
			std::string folder = fs::path(source.path).parent_path().generic_string();
			if (folder.empty()) {
				std::error_code ec;
				folder = fs::current_path(ec).generic_string();
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
			                                                 : from->file_kind(source.entry);
			row.size = loaded ? output.bytes.size() : from->size(source.entry);
			row.made_from = converter ? name : std::string();
			row.found_in = found_in;
			const bool first = !provided_.count(key(output.name));
			if (first)
				provided_[key(output.name)] = {plan_.rows.size(), row.kind};
			else
				row.problem = (converter ? "More than one selected file makes " : "More than one selected file has the name ") +
				              output.name + ".";
			place(row);
			plan_.rows.push_back(std::move(row));
			++files_;
			const size_t index = plan_.rows.size() - 1;
			if (converter) made_[index] = std::make_shared<const std::vector<uint8_t>>(output.bytes);
			if (walk && first) queue(index, from, std::move(output.bytes), loaded);
		}
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
		queue_.push_back(std::move(node));
	}

	// Where import_assets writes the file, and why the project could not take it.
	void place(ImportPlanRow &row) const {
		row.destination = import_destination(scan_, row.name, row.kind);
		if (!row.problem.empty()) return;
		FileNameProblem problem = FileNameProblem::None;
		std::string message;
		if (row.kind == AssetKind::Unknown || row.kind == AssetKind::Archive)
			row.problem = "Unsupported asset type: " + row.name;
		else if (!check_project_file_name(paths_.root, fs::path(row.destination).parent_path().generic_string(), row.name,
		                                  row.kind, problem, message))
			row.problem = message;
	}

	// A stylesheet the shell reads by name, as the project holds it once the import is in: the
	// selection's copy, else the project's file of the name.
	bool stylesheet_bytes(const std::string &name, std::vector<uint8_t> &bytes) {
		for (const PlanNode &node : queue_) {
			const ImportPlanRow &row = plan_.rows[node.row];
			if (row.state == ImportPlanRow::State::Selected && row.kind == AssetKind::MenuStyle && key(row.name) == key(name)) {
				if (!node.loaded) return read_row(node.row, bytes);
				bytes = node.bytes;
				return true;
			}
		}
		const AssetEntry *asset = scan_.find(name);
		if (!asset || asset->kind != AssetKind::MenuStyle) return false;
		std::string error;
		return read_file_bytes((fs::path(paths_.root) / asset->relative_path).generic_string(), bytes, error);
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
			read = from && from->read(source.entry, out);
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

	// A planned file's references (none read for a file the graph does not read).
	void follow(PlanNode &node) {
		const std::string file = plan_.rows[node.row].name;
		const AssetKind kind = plan_.rows[node.row].kind;
		// What it names is not looked for: a file of a kind that names files the graph does not
		// read (a terrain; a mission's .mis, of a kind it reads).
		if (references_unread(kind, file)) note(ReferenceKind::None, kind, file);
		if (!graph_reads_file(kind, file) || !extract(node)) return;
		for (const GraphEdge &edge : node.content.edges) {
			if (plan_.truncated) return;
			// A stylesheet value the game does not read (another sheet's, or a definition after
			// the place it stops reading the file) loads no file.
			if (kind == AssetKind::MenuStyle && edge.field == "value" &&
			    (!sheet_.has(edge.record) || key(sheet_.get(edge.record)) != key(edge.value)))
				continue;
			follow_edge(node, file, edge);
		}
	}

	// One reference of a node's file, in the plan's order (import_plan.h).
	void follow_edge(const PlanNode &node, const std::string &file, const GraphEdge &edge) {
		const AssetKind wanted = reference_row(edge.kind).file;
		if (wanted == AssetKind::Unknown) {
			note(edge.kind, AssetKind::Unknown, file); // a symbol, a sound: no file to look for
			return;
		}
		const std::string name = resolve_name(edge.value);
		if (is_style_reference(name)) return; // no stylesheet defines it: its variable's reference is listed
		if (graph_.resolve(edge.kind, name, edge.scope, nullptr, edge.loader_arg) == ReferenceStatus::Present) return;
		const ImportNeed need{file, edge.record, edge.field, edge.kind, name, edge.loader_arg};
		const ImportOrigin *own = node.origin;
		const ImportOrigin *install = install_ != own ? install_ : nullptr;
		const std::string mine = look(own, need);
		const std::string theirs = look(install, need);
		// A file the plan takes serves it: the places that have one too are that file's rivals.
		if (const Provided *taken = provided_for(need)) {
			const size_t row = taken->row;
			add_rival(row, own, mine);
			add_rival(row, install, theirs);
			return;
		}
		if (mine.empty() && theirs.empty()) {
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
	const ProjectPaths &paths_;
	const ProjectDocument &document_;
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

bool same_import(const ImportPlan &a, const ImportPlan &b) {
	if (a.truncated != b.truncated || a.rows.size() != b.rows.size()) return false;
	for (size_t i = 0; i < a.rows.size(); ++i) {
		const ImportPlanRow &x = a.rows[i];
		const ImportPlanRow &y = b.rows[i];
		if (x.state != y.state || x.name != y.name || x.kind != y.kind || x.source != y.source ||
		    x.destination != y.destination || x.made_from != y.made_from || x.selected != y.selected || x.problem != y.problem)
			return false;
	}
	return true;
}

} // namespace opennova::editor
