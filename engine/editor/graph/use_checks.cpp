#include <editor/graph/use_checks.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <base/gameprofile/gameprofile.h>
#include <base/io/file_io.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_validation.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/particle_token_slots.h>
#include <editor/documents/particle_type.h>
#include <editor/documents/strings_document.h>
#include <editor/documents/terrain_document.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/texture_checks.h>
#include <editor/model/diagnostic.h>
#include <editor/model/text_document.h>
#include <editor/project/project_files.h>
#include <formats/def/def.h>
#include <formats/env/env.h>
#include <formats/mission/authoring.h>
#include <formats/mns/mns.h>
#include <formats/mnu/mnu_layout.h>
#include <formats/trn/trn_io.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/menu/menu_style.h>

namespace opennova::editor {

namespace {

// A finding on the record a symbol's definition is in, on `field` (and on the definition's line
// where the finding names one: a stylesheet's).
Diagnostic on_definition(const GraphSymbol &symbol, DiagnosticSeverity severity, const FindingCodeRow &code,
		const std::string &message, const char *field, size_t line) {
	Diagnostic d = make_finding(code, severity, message, symbol.file, field);
	d.line = line;
	d.record = symbol.record;
	d.record_key = symbol.record_key;
	d.record_title = symbol.title;
	d.row_id = symbol.address.row;
	d.child_id = symbol.address.child;
	d.record_kind = symbol.address.kind;
	return d;
}

// What the menus make of the variables of the stylesheets the game reads (ADR 0046 S9i), on the
// definition the game reads of each name in its file (the last one: the graph keeps a
// stylesheet's variables in its line order) and its value as the game reads it: a name brand.mns
// defines too; and on the one the game reads of all the shell's stylesheets (the graph's binding),
// through the menus' uses of it, a name no menu uses, a value used as a colour that is not one,
// and a value used as more than one of a colour, a font and an image.
void check_style_uses(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &,
		std::vector<Diagnostic> &out) {
	const std::vector<const GraphSymbol *> symbols = graph.symbols_of_kind(ReferenceKind::StyleVar);
	for (size_t begin = 0, end = 0; begin < symbols.size(); begin = end) {
		const std::string &path = symbols[begin]->file;
		for (end = begin; end < symbols.size() && symbols[end]->file == path; ++end) {
		}
		if (!menu::is_shell_stylesheet(basename_of(path)) || !files.records_checked(path))
			continue;
		// The last definition of each name in the file: the one the game reads of it.
		std::unordered_map<std::string, const GraphSymbol *> last;
		for (size_t i = begin; i < end; ++i)
			last[symbols[i]->name] = symbols[i];
		for (size_t i = begin; i < end; ++i) {
			const GraphSymbol &symbol = *symbols[i];
			// An empty value: a name the game ignores. A name written as a %NAME% itself is no
			// definition the file reads of that name (MnsDocument::winning_row looks the name up
			// by its variable_name).
			if (last[symbol.name] != &symbol || symbol.value.empty() ||
					mns::is_variable_reference(symbol.display))
				continue;
			const std::string &name = symbol.display;
			const std::string &value = symbol.value;
			const auto add = [&](DiagnosticSeverity severity, StyleFinding code,
									 const std::string &message) {
				out.push_back(on_definition(symbol, severity, finding_code(code), message, "value", symbol.line));
			};
			const GraphSymbol *binding = graph.style_binding(name);
			const bool is_binding = binding && binding->file == path;
			if (binding && !is_binding && menu::is_shell_stylesheet(basename_of(binding->file)) &&
					strutil::iequals(basename_of(path), menu::kShellStylesheets[0].name))
				add(DiagnosticSeverity::Info, StyleFinding::OverriddenByBrand,
						basename_of(binding->file) + " defines " + name +
								" too: the game reads its value, '" + binding->value + "'.");
			if (!is_binding)
				continue;
			// The uses of the variable, by what its value must be there (a string id, a screen's
			// or a window's NAME and any other text are none of a colour, a font and an image).
			bool color = false, font = false, image = false;
			const std::vector<const GraphEdge *> uses =
					graph.referrers_of(ReferenceKind::StyleVar, name);
			for (const GraphEdge *edge : uses) {
				const StyleVariableUse use = style_variable_use(edge->through);
				color = color || use == StyleVariableUse::Colour;
				font = font || use == StyleVariableUse::Font;
				image = image || use == StyleVariableUse::Image;
			}
			// Every field value of a menu that is one whole %NAME% is an edge: a colour, a font, an
			// image, a string id, a screen's or a window's name, and any other text the game reads
			// (a NAME, a shown text, an ACTION's target: FieldUse::variable_through), so none
			// means no menu uses it. A %NAME% inside a longer text is no edge: the frame compiler
			// reads a whole value alone.
			if (uses.empty())
				add(DiagnosticSeverity::Info, StyleFinding::Unused,
						"No menu of the project names %" + name + "%.");
			if (color && !mnu::color_reads_whole(value))
				add(DiagnosticSeverity::Warning, StyleFinding::NotAColor,
						name + " is used as a colour, but '" + value +
								"' is not one (AARRGGBB hex digits): the game reads only its "
								"leading hex digits.");
			if (int(color) + int(font) + int(image) > 1)
				add(DiagnosticSeverity::Warning, StyleFinding::MixedUse,
						name + " is used as more than one of a colour, a font and an image.");
		}
	}
}

// The pool an item's TYPE puts a record in, as the game's editor places it (formats/mission/
// authoring.h: a vehicle, an object and a powerup among the items, a decoration, foliage and a
// building among the buildings, a person among the organics, a marker among the markers).
NodeKind pool_of_type(int type) {
	switch (mission::authoring::entity_kind_for_item_type(type)) {
	case mission::EntityKind::Building: return node_kind(MissionKind::Building);
	case mission::EntityKind::Marker: return node_kind(MissionKind::Marker);
	case mission::EntityKind::Organic: return node_kind(MissionKind::Organic);
	default: return node_kind(MissionKind::Item);
	}
}

const char *pool_words(NodeKind kind) {
	switch (static_cast<MissionKind>(kind)) {
	case MissionKind::Building: return "buildings";
	case MissionKind::Marker: return "markers";
	case MissionKind::Organic: return "organics";
	default: return "items";
	}
}

// What a mission's records make of their items' TYPE (ADR 0046 S14): an entity in another pool than
// the one the game's editor places its item's TYPE in (one to one over the shipped missions,
// D-MIS-1; what the game makes of such a record is not known), read through the graph: each
// entity's item edge and the item symbol it reaches, whose value is the item's TYPE
// (DefCatalogDocument::refine_symbol). An item the project lacks is the reference's own finding.
void pool_findings(const AssetGraph &graph, const std::string &path, std::vector<Diagnostic> &out) {
	for (const GraphEdge *edge : graph.references_of(path)) {
		if (edge->kind != ReferenceKind::Item || edge->field != "item" || !is_entity_kind(edge->address.kind)) continue;
		const GraphSymbol *item = graph.symbol_reached(*edge);
		const std::optional<int> type = item ? strutil::parse_int(item->value) : std::nullopt;
		if (!type || *type < 0) continue;
		const NodeKind placed = pool_of_type(*type);
		if (placed == edge->address.kind) continue;
		Diagnostic d = make_finding(finding_code(MissionFinding::Pool), DiagnosticSeverity::Warning,
		                            "Item " + edge->value + " is a " + def::def_item_type_name(*type) +
		                                    ", which the game's editor places among the " + pool_words(placed) +
		                                    "; this record is among the " + pool_words(edge->address.kind) +
		                                    ". What the game makes of it is not known.",
		                            path, edge->field);
		d.record = edge->record;
		d.record_key = edge->record_key;
		d.record_title = edge->record_title;
		d.row_id = edge->address.row;
		d.child_id = edge->address.child;
		d.record_kind = edge->address.kind;
		out.push_back(std::move(d));
	}
}

// Every mission's pool_findings, made once per state of the graph (AssetGraph::generation: while it
// stands, every edge and symbol is where it was), not at every composition of the rows (the editor
// composes them after every edit that validates a file); a composition takes those of each mission
// whose records its validation checked.
void check_mission_pools(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &,
		std::vector<Diagnostic> &out) {
	struct Made {
		std::mutex mutex;
		uint64_t generation = 0; // no graph's (the counter starts at 1)
		std::vector<std::pair<std::string, std::vector<Diagnostic>>> missions; // each mission's path, its findings
	};
	static Made made;
	const std::lock_guard<std::mutex> lock(made.mutex);
	if (made.generation != graph.generation()) {
		made.missions.clear();
		std::string last;
		for (const GraphSymbol *symbol : graph.symbols_of_kind(ReferenceKind::MissionEntity)) {
			const std::string &path = symbol->file;
			if (path == last) continue;
			last = path;
			made.missions.emplace_back(path, std::vector<Diagnostic>());
			pool_findings(graph, path, made.missions.back().second);
		}
		made.generation = graph.generation();
	}
	for (const auto &[path, findings] : made.missions)
		if (files.records_checked(path)) out.insert(out.end(), findings.begin(), findings.end());
}

// The gametext keys a single-player mission's flow reads, by name, each where the game reads it: the
// engine's table (hud::kMissionFlowKeys, each witnessed there); a key the table lacks, or holds empty,
// reads "" and shows nothing there [orig: GameText_GetString, the miss @0x51EC00; the section by its first
// name in any case, TextResource_FindEntryBySectionAndKey @0x75D250]. What the game shows with each, in
// the table's order:
constexpr const char *kFlowKeyShown[] = {
	"the chat line a newly shown objective posts first",
	"the objectives panel's heading",
	"the waypoint label's words between its distance and its name",
	"the name of a waypoint the mission's text names none for",
	"the win screen's objectives line",
	"the win screen's enemies line",
	"the win screen's team line",
	"the win screen's friendly fire line",
	"the lose screen's title",
	"the end screens' key help",
};
static_assert(std::size(kFlowKeyShown) == std::size(hud::kMissionFlowKeys), "one wording a flow key");

// What a single-player mission reads of the project's gametext.bin (the boot's string table [orig:
// Game_InitSubsystems @0x4a6fed]): where the project has a mission, each flow key the table's first
// section of its name lacks, or holds empty, a warning on the table, read through the graph (its string
// ids' symbols, each with its text).
void check_gametext_flow(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &input,
		std::vector<Diagnostic> &out) {
	const AssetEntry *table = input.scan.find(hud::kGameTextTable);
	if (!table || !is_strings_kind(table->kind) || !files.records_checked(table->relative_path)) return;
	const bool missions = std::any_of(input.scan.entries.begin(), input.scan.entries.end(),
			[](const AssetEntry &entry) { return entry.kind == AssetKind::Mission; });
	if (!missions) return;
	const std::string scope = strutil::to_upper(hud::kGameTextTable) + "/";
	for (size_t i = 0; i < std::size(hud::kMissionFlowKeys); ++i) {
		const hud::GameTextKey &row = hud::kMissionFlowKeys[i];
		const GraphSymbol *symbol = graph.resolve_symbol(ReferenceKind::TextId, row.key, scope + row.section);
		if (symbol && symbol->file == table->relative_path && !symbol->value.empty()) continue;
		out.push_back(make_finding(finding_code(StringsFinding::FlowKeyMissing), DiagnosticSeverity::Warning,
				std::string("A single-player mission reads ") + row.section + "/" + row.key + ", " + kFlowKeyShown[i] + ": " +
						(symbol ? "it holds no text" : "the table has no such string") +
						", so the game shows nothing there.",
				table->relative_path));
	}
}

// What the list ends with, in the words of its fix: a list stops at a ';' attached to its last token alone.
std::string slot_fault_words(const ParticleSlotFault &fault) {
	const std::string slot = std::to_string(fault.slot);
	const std::string where = fault.outside
	        ? "past the 30 slots of the game's token table"
	        : "into token slot " + slot + ", which no line parsed before it filled (the longest so far held " +
	                  std::to_string(fault.filled) + " tokens)";
	std::string what;
	if (strutil::iequals(fault.key, "flags") || strutil::iequals(fault.key, "move"))
		what = "The " + fault.key + " list here ends with no ';' attached to its last name (as the game's own writer "
		       "ends it, \" ;\"), so the game's parser takes one token past it";
	else if (strutil::iequals(fault.key, "pdefs"))
		what = "The pdefs list here has no ';' attached to its last name, so the game's parser takes tokens on past it";
	else
		what = "This row of fewer than 8 values has no ';' attached to its last, so the game's parser takes tokens on "
		       "past it";
	return what + ", reading " + where + ": the game faults as the mission loads its effects. Attach the ';' to the last "
	       "token (\"" + fault.key + " = ... LAST;\"), or let a line of at least " + std::to_string(fault.slot + 1) +
	       " tokens come first (in this file, or in a particle file whose name sorts before it).";
}

// The particle files as the game parses them at a mission's start (the .ptl and gore-set files the build packs, in the
// archive walk's order [orig: CEffectSystem_Init @ 0x5F6070]; opennova::effect_file_order) through its line tokenizer's process-wide token
// table: each list that reads a slot no line before it filled faults the game (documents/particle_token_slots.h), an
// error at the line. An open document stands in with its text. An expansion's base game parses its own particle files
// among them, which the project does not list: none there.
void check_particle_token_slots(const AssetGraph &, const ValidationCache &, const ValidationInput &input,
		std::vector<Diagnostic> &out) {
	if (!input.project.expansion.standalone()) return;
	std::vector<std::string> names;
	std::set<std::string> seen;
	for (const AssetEntry &entry : input.scan.entries)
		if (entry.kind == AssetKind::Particles && seen.insert(strutil::to_lower(entry.logical_name)).second)
			names.push_back(entry.logical_name);
	if (names.empty()) return;
	const std::vector<std::string> order =
	        effect_file_order(names, gore_particle_extension(input.scan.find(kGoreContentMarker) != nullptr));
	const int policy = gameprofile::gameprofile_scr_policy_for_code(input.project.target_game.c_str());
	std::vector<ParticleLoadFile> files;
	for (const std::string &name : order) {
		const AssetEntry *entry = input.scan.find(name);
		if (!entry) continue;
		ParticleLoadFile file;
		file.path = entry->relative_path;
		const std::shared_ptr<const DocumentBase> open = input.open_document(*entry);
		if (const TextDocument *text = open ? text_of(*open) : nullptr) {
			file.bytes.assign(text->text().begin(), text->text().end());
		} else {
			std::string error;
			if (!io::read_file_bytes(join_path(input.paths.root, entry->relative_path), file.bytes, error) ||
			    !vfs_decode_payload(file.bytes, policy))
				continue;
		}
		files.push_back(std::move(file));
	}
	for (const ParticleSlotFault &fault : particle_token_slot_faults(files)) {
		Diagnostic d = make_finding(finding_code(ParticleFinding::UnfilledTokenSlot), DiagnosticSeverity::Error,
		                            slot_fault_words(fault), files[fault.file].path, fault.key);
		d.line = size_t(fault.line);
		d.column = 1;
		out.push_back(std::move(d));
	}
}

// A file's text as this validation reads it, the file a build packs: an open document with changes, its save
// (a build saves first); else the file as it sits, its bytes (the paths' source by its logical name, else the
// project's folder), an SCR- or BFC-wrapped one decoded as the game reads it; false where none reads. Its stamp
// follows what it is read from: an open document with changes, its identity, load and revision; else the file's
// size and last write as the scan lists it (which a save of the document moves).
bool validation_text(const ValidationInput &input, const AssetEntry &asset, std::string &out) {
	const std::shared_ptr<const DocumentBase> open = input.open_document(asset);
	if (open && open->dirty()) {
		const SerializeResult written = open->serialize();
		if (written.ok()) {
			out = written.text;
			return true;
		}
	}
	std::vector<uint8_t> bytes;
	std::string error;
	const int policy = gameprofile::gameprofile_scr_policy_for_code(input.project.target_game.c_str());
	if (!(input.paths.files ? input.paths.files->read(asset.logical_name, bytes)
	                        : io::read_file_bytes(join_path(input.paths.root, asset.relative_path), bytes, error)) ||
	    !vfs_decode_payload(bytes, policy))
		return false;
	out.assign(bytes.begin(), bytes.end());
	return true;
}
std::string validation_stamp(const ValidationInput &input, const AssetEntry &asset) {
	const std::shared_ptr<const DocumentBase> open = input.open_document(asset);
	if (open && open->dirty())
		return asset.relative_path + "|o" + std::to_string(open->identity()) + "." +
		       std::to_string(open->load_generation()) + "." + std::to_string(open->revision());
	return asset.relative_path + "|f" + std::to_string(asset.size_bytes) + "." + std::to_string(asset.modified_ticks);
}

// The keyword whose value a leg of the terrain's gate reads (formats/trn trn_refusal).
const char *refusal_key(TrnRefusal leg) {
	switch (leg) {
	case TrnRefusal::NoColormap: return "polytrn_colormap";
	case TrnRefusal::NoDetailmap: return "polytrn_detailmap";
	case TrnRefusal::NoPolydata: return "polytrn_polydata";
	case TrnRefusal::SectorRows: return "polytrn_sectors";
	case TrnRefusal::SectorCount: return "polytrn_sectorcount";
	case TrnRefusal::None: break;
	}
	return "";
}

// A mission's terrain as the game loads it: the .trn, then overcast.def, then the mission's .env through the
// terrain's parser, and only then the gate [orig: Terrain_LoadEnvironmentConfig @ 0x610940:
// Environment_LoadTimeOfDayConfig @ 0x6109c0 with Terrain_ParseConfigCallback, the gate @ 0x610a2a..0x610a6b;
// formats/trn load_mission_trn, D-TERRAIN-18]. What the gate refuses it by, and the .trn's refusal read alone.
struct TerrainAdmission {
	std::string terrain; // the .trn's project path
	bool read = false;   // its .trn read: judged
	bool admitted = true;
	std::string why, alone; // the load's refusal and the .trn's read alone, "" where the gate takes it
	Diagnostic finding;     // the mission's row, where the gate refuses its load
};

// The terrain each mission loads, judged as the game judges it (TerrainAdmission): a mission whose load the gate
// refuses gets terrain.refused, on the later file's line that set the value the gate refuses (overcast.def's or
// the .env's) or else on its terrain reference, unless the .trn's own row says the same (the .trn's then, with its
// fixes); a .trn every mission reading it loads is taken, its own row taken back (the .trn's own check,
// documents/terrain_document validate_terrain_file, judges it alone, which stands for a .trn no mission reads).
// Each mission's judgement is made again only once one of its three files' stamps moves.
void check_terrain_admission(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &input,
		std::vector<Diagnostic> &out) {
	static std::mutex mutex;
	static std::map<std::string, std::pair<std::string, TerrainAdmission>> made; // by mission: its stamps, its judgement
	const std::lock_guard<std::mutex> lock(mutex);
	const AssetEntry *overcast = input.scan.find(env::kOvercastFile);
	std::map<std::string, std::vector<const TerrainAdmission *>> by_terrain;
	std::set<std::string> unjudged; // a .trn a mission reads that was not judged (its records unchecked, unread)
	std::set<std::string> missions;
	for (const AssetEntry &mission : input.scan.entries) {
		if (mission.kind != AssetKind::Mission) continue;
		const GraphEdge *terrain_edge = nullptr;
		std::string terrain_file, environment_file;
		for (const GraphEdge *edge : graph.references_of(mission.relative_path)) {
			std::string file;
			if (edge->kind == ReferenceKind::Terrain && !terrain_edge) {
				terrain_edge = edge;
				if (graph.resolve(*edge, &file) == ReferenceStatus::Present) terrain_file = file;
			} else if (edge->kind == ReferenceKind::Environment && environment_file.empty() &&
			           graph.resolve(*edge, &file) == ReferenceStatus::Present) {
				environment_file = file;
			}
		}
		const AssetEntry *terrain = terrain_file.empty() ? nullptr : input.scan.at_path(terrain_file);
		if (!terrain) continue;
		if (!files.records_checked(mission.relative_path)) {
			unjudged.insert(terrain->relative_path);
			continue;
		}
		const AssetEntry *environment = environment_file.empty() ? nullptr : input.scan.at_path(environment_file);
		std::string stamp = input.paths.root + "|" + validation_stamp(input, *terrain) + "|" +
		                    (overcast ? validation_stamp(input, *overcast) : std::string()) + "|" +
		                    (environment ? validation_stamp(input, *environment) : std::string()) + "|" +
		                    terrain_edge->record_key + "|" + terrain_edge->field;
		missions.insert(mission.relative_path);
		auto &[kept, judged] = made[mission.relative_path];
		if (kept != stamp) {
			kept = std::move(stamp);
			judged = TerrainAdmission();
			judged.terrain = terrain->relative_path;
			std::string trn, overcast_text, environment_text;
			if ((judged.read = validation_text(input, *terrain, trn))) {
				TrnLaterTexts later;
				if (overcast && validation_text(input, *overcast, overcast_text)) later.overcast = &overcast_text;
				if (environment && validation_text(input, *environment, environment_text))
					later.environment = &environment_text;
				TrnConfig config;
				std::vector<TrnLaterLine> taken;
				TrnRefusal leg = TrnRefusal::None;
				judged.admitted = load_mission_trn(trn, later, config, judged.why, &taken, &leg);
				if (!judged.admitted) {
					load_mission_trn(trn, TrnLaterTexts(), config, judged.alone);
					// The later line that set the value the gate refuses, the last of them: the finding's place.
					const TrnLaterLine *at = nullptr;
					for (const TrnLaterLine &line : taken)
						if (line.key == refusal_key(leg)) at = &line;
					const std::string prefix = "TRN rejected: ";
					const std::string why =
					        judged.why.rfind(prefix, 0) == 0 ? judged.why.substr(prefix.size()) : judged.why;
					const std::string loads =
					        mission.relative_path + " loads " + terrain->relative_path +
					        (later.overcast ? std::string(", then ") + env::kOvercastFile : std::string()) +
					        (later.environment ? ", then " + environment->relative_path : std::string()) +
					        " through the terrain's parser";
					const std::string refused =
					        "the game refuses the terrain (" + why + "). The mission stops loading and returns to the menus.";
					if (at) {
						const AssetEntry &file = at->file == TrnLaterLine::File::Environment ? *environment : *overcast;
						judged.finding = make_finding(finding_code(TerrainFinding::Refused), DiagnosticSeverity::Error,
						                              loads + ", this line last of them: " + refused, file.relative_path,
						                              "key");
						judged.finding.line = size_t(at->line);
					} else {
						judged.finding = make_finding(finding_code(TerrainFinding::Refused), DiagnosticSeverity::Error,
						                              loads + ": " + refused, terrain_edge->source, terrain_edge->field);
						judged.finding.record = terrain_edge->record;
						judged.finding.record_key = terrain_edge->record_key;
						judged.finding.record_title = terrain_edge->record_title;
						judged.finding.row_id = terrain_edge->address.row;
						judged.finding.child_id = terrain_edge->address.child;
						judged.finding.record_kind = terrain_edge->address.kind;
					}
				}
			}
		}
		if (judged.read) by_terrain[judged.terrain].push_back(&judged);
		else unjudged.insert(judged.terrain);
	}
	for (auto it = made.begin(); it != made.end();) it = missions.count(it->first) ? std::next(it) : made.erase(it);
	const auto own_row = [](const Diagnostic &d, const std::string &terrain) {
		return d.asset == terrain && d.code() == finding_code(TerrainFinding::Refused).token;
	};
	for (const auto &entry : by_terrain) {
		const std::string &terrain = entry.first;
		const std::vector<const TerrainAdmission *> &judged = entry.second;
		const bool own = std::any_of(out.begin(), out.end(), [&](const Diagnostic &d) { return own_row(d, terrain); });
		const bool taken = std::all_of(judged.begin(), judged.end(), [](const TerrainAdmission *j) { return j->admitted; });
		if (own && taken && !unjudged.count(terrain))
			out.erase(std::remove_if(out.begin(), out.end(), [&](const Diagnostic &d) { return own_row(d, terrain); }),
			          out.end());
		for (const TerrainAdmission *j : judged)
			if (!j->admitted && !(own && j->why == j->alone)) out.push_back(j->finding);
	}
}

// The cross-file checks, one row per asset kind, in AssetKind's order.
constexpr UseCheckRow kUseChecks[] = {
	{ AssetKind::Texture, check_texture_uses },
	{ AssetKind::Strings, check_gametext_flow },
	{ AssetKind::Mission, check_mission_pools },
	{ AssetKind::Terrain, check_terrain_admission },
	{ AssetKind::MenuStyle, check_style_uses },
	{ AssetKind::Particles, check_particle_token_slots },
};

constexpr bool rows_in_kind_order() {
	for (size_t i = 0; i < sizeof(kUseChecks) / sizeof(kUseChecks[0]); ++i) {
		if (kUseChecks[i].kind == AssetKind::Unknown || kUseChecks[i].kind >= AssetKind::kCount ||
				!kUseChecks[i].check)
			return false;
		if (i > 0 && !(kUseChecks[i - 1].kind < kUseChecks[i].kind))
			return false;
	}
	return true;
}
static_assert(rows_in_kind_order(),
		"one use check per asset kind, in AssetKind's order, each with its check");

} // namespace

const UseCheckRow *use_check(AssetKind kind) {
	for (const UseCheckRow &row : kUseChecks)
		if (row.kind == kind)
			return &row;
	return nullptr;
}

void run_use_checks(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &input,
		std::vector<Diagnostic> &out) {
	for (const UseCheckRow &row : kUseChecks)
		row.check(graph, files, input, out);
}

} // namespace opennova::editor
