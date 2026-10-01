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

// A file the walk reads the references of: its row, the place it came from, its bytes as
// read (none for a file whose kind needs none read), and what they reference once read.
struct Node {
	size_t row = 0;
	const ImportOrigin *origin = nullptr;
	std::vector<uint8_t> bytes;
	enum class Read { Not, Done, Failed } read = Read::Not;
	Extracted content;
};

class Planner {
public:
	Planner(ImportPlan &plan, const ProjectPaths &paths, const ProjectDocument &document, const AssetScan &scan,
	        const AssetGraph &graph, size_t cap)
	    : plan_(plan), paths_(paths), document_(document), scan_(scan), graph_(graph), cap_(cap) {}

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

	// A selected source, read as import_assets reads it; what a converter makes of it, each
	// output a row (the first file of a name only is walked). The cap stops it before it is
	// read, and takes a converter's outputs whole or not at all.
	void add_source(const ImportChoice &source, bool walk) {
		if (files_ >= cap_) {
			plan_.truncated = true;
			return;
		}
		const std::string name = source.name();
		const auto fail = [&](CoreFinding code, const std::string &message) {
			plan_.diagnostics.push_back(make_finding(code, DiagnosticSeverity::Error, message, name));
		};
		const ImportOrigin *from = nullptr;
		std::string found_in, error;
		std::vector<uint8_t> bytes;
		if (source.install || !source.entry.empty()) {
			from = origin(source.install ? ImportOrigin::Kind::GameInstall : ImportOrigin::Kind::Archive, source.path, error);
			if (!from) {
				fail(source.install ? CoreFinding::ImportInstall : CoreFinding::ImportArchive, error);
				return;
			}
			if (!from->read(source.entry, bytes)) {
				fail(CoreFinding::ImportRead, source.install ? "The game data has no file named " + name + "."
				                                  : "Could not read " + name + " from " + source.path);
				return;
			}
			found_in = from->words();
		} else {
			std::string io_error;
			if (!read_file_bytes(source.path, bytes, io_error)) {
				fail(CoreFinding::ImportRead, io_error);
				return;
			}
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
		const Converter *converter = converter_for(name);
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
			                                                 : classify_asset(output.name, &output.bytes);
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
			if (walk && first) queue(index, from, std::move(output.bytes));
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

	void walk() {
		while (!queue_.empty() && !plan_.truncated) {
			Node node = std::move(queue_.front());
			queue_.pop_front();
			follow(node);
		}
		queue_.clear();
		settle();
	}

private:
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

	void queue(size_t row, const ImportOrigin *from, std::vector<uint8_t> bytes) {
		Node node;
		node.row = row;
		node.origin = from;
		node.bytes = std::move(bytes);
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
	bool stylesheet_bytes(const std::string &name, std::vector<uint8_t> &bytes) const {
		for (const Node &node : queue_) {
			const ImportPlanRow &row = plan_.rows[node.row];
			if (row.state == ImportPlanRow::State::Selected && row.kind == AssetKind::MenuStyle && key(row.name) == key(name)) {
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
		if (!source.install && source.entry.empty()) return read_file_bytes(source.path, out, error);
		const ImportOrigin *from = origin(source.install ? ImportOrigin::Kind::GameInstall : ImportOrigin::Kind::Archive,
		                                  source.path, error);
		return from && from->read(source.entry, out);
	}

	// A node's references and symbols, read once; false (with one warning) when they cannot be.
	bool extract(Node &node) {
		if (node.read != Node::Read::Not) return node.read == Node::Read::Done;
		const ImportPlanRow &row = plan_.rows[node.row];
		Diagnostic error;
		if (!extract_from_bytes(row.name, row.kind, node.bytes, document_.target_game, node.content, error)) {
			node.read = Node::Read::Failed;
			std::string reason = error.message.empty() ? std::string("The file could not be read.") : error.message;
			if (reason.back() != '.') reason += '.';
			plan_.diagnostics.push_back(make_finding(CoreFinding::ImportUnreadable, DiagnosticSeverity::Warning,
			                                         reason + " The files it names are not looked for.", row.name));
			return false;
		}
		node.read = Node::Read::Done;
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
	void follow(Node &node) {
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
	void follow_edge(const Node &node, const std::string &file, const GraphEdge &edge) {
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
		row.found_in = from->words();
		row.needed_by = need;
		place(row);
		row.selected = row.problem.empty(); // one the project cannot take is listed, not taken
		const size_t index = plan_.rows.size();
		plan_.rows.push_back(std::move(row));
		provided_[key(spelling)] = {index, plan_.rows[index].kind};
		++files_;
		if (!mine.empty() && !theirs.empty()) add_rival(index, install, theirs);
		// The bytes are read when the walk reads the file's references.
		std::vector<uint8_t> bytes;
		if (graph_reads_file(plan_.rows[index].kind, spelling)) from->read(spelling, bytes);
		queue(index, from, std::move(bytes));
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

	ImportPlan &plan_;
	const ProjectPaths &paths_;
	const ProjectDocument &document_;
	const AssetScan &scan_;
	const AssetGraph &graph_;
	const size_t cap_;
	std::map<std::pair<ImportOrigin::Kind, std::string>, Opened> origins_;
	const ImportOrigin *install_ = nullptr;
	std::deque<Node> queue_;
	std::map<std::string, Provided> provided_;                             // the files the plan takes
	std::map<size_t, std::shared_ptr<const std::vector<uint8_t>>> made_; // a converter output's bytes, by row
	std::map<std::string, Missing> missing_;                               // the names looked for in vain
	mns::StyleSheet sheet_;                                                // the variables a %NAME% expands through
	size_t files_ = 0;                                                     // the files the plan takes
};

} // namespace

ImportPlan plan_import(const std::vector<ImportChoice> &sources, bool with_dependencies, const ProjectPaths &paths,
                       const ProjectDocument &document, const AssetScan &scan, const AssetGraph &graph,
                       const std::string &retail_directory, size_t file_cap,
                       std::shared_ptr<const ImportOrigin> install_mounted) {
	ImportPlan plan;
	Planner planner(plan, paths, document, scan, graph, file_cap);
	if (install_mounted && !retail_directory.empty())
		planner.adopt(ImportOrigin::Kind::GameInstall, retail_directory, std::move(install_mounted));
	if (with_dependencies && !retail_directory.empty()) {
		std::string error;
		const ImportOrigin *install = planner.origin(ImportOrigin::Kind::GameInstall, retail_directory, error);
		if (!install)
			plan.diagnostics.push_back(make_finding(CoreFinding::ImportInstall, DiagnosticSeverity::Warning,
			                                        error + " The files the import needs are not looked for there."));
		planner.set_install(install);
	}
	for (const ImportChoice &source : sources) {
		if (plan.truncated) break;
		planner.add_source(source, with_dependencies);
	}
	if (!with_dependencies || plan.truncated) return plan;
	planner.read_stylesheets();
	planner.walk();
	return plan;
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
