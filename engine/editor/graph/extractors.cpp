// The extractors: what one file references and defines, from its bytes through the
// engine's own parser for its kind (extract_from_bytes). The record types (the def
// catalogs, the string tables, the menus, the stylesheets, the models, the clips and the
// animation tables) walk their schema: a field's reference, and the symbol a field
// defines (a weapon's name, a string's key, a menu's screen or window by the NAME its
// ACTIONs find it by); a text type reads the names its text makes, each at its span (a
// script's operands, S13 D9); the native kinds (an environment, the avatar table, a particle
// file, a mission) read their parsed structs.
#include <editor/graph/asset_graph.h>

#include <filesystem>
#include <sstream>
#include <variant>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/document_types.h>
#include <editor/graph/graph_names.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/avatars/avatars.h>
#include <formats/env/env.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/particle/parser.h>

namespace opennova::editor {

namespace {

using graph_names::is_style_reference;
using graph_names::symbol_name;

GraphEdge edge_of(const std::string &source, const std::string &record, const std::string &field, ReferenceKind kind,
                  const std::string &value, const std::string &scope = std::string(), bool rewritable = false) {
	GraphEdge edge;
	edge.source = source;
	edge.record = record;
	edge.field = field;
	edge.kind = kind;
	edge.value = value;
	edge.scope = scope;
	edge.rewritable = rewritable;
	return edge;
}

GraphSymbol symbol_of(ReferenceKind kind, const std::string &display, const std::string &file,
                      const std::string &record = std::string(), const std::string &scope = std::string()) {
	GraphSymbol symbol;
	symbol.kind = kind;
	symbol.display = display;
	symbol.name = symbol_name(kind, display);
	symbol.file = file;
	symbol.record = record;
	symbol.scope = scope;
	return symbol;
}

// The name a field's value gives: a text, or a number other than 0 (an item's id); "" for
// none.
std::string value_name(const Value &value) {
	if (const auto *text = std::get_if<std::string>(&value)) return *text;
	if (const auto *number = std::get_if<int64_t>(&value)) return *number ? std::to_string(*number) : std::string();
	return std::string();
}

// A record's references and the symbols it defines, each field as it applies to that record
// (Document::field_on: a menu STRING's value is a string id when its TYPE says so, an
// APPEARANCE's value a texture or a colour by its TYPE, an ACTION's target a screen or a
// window by its verb, a part's NAME defines nothing); a field the file leaves out (an
// optional one, one in a block left out, an empty one), or one the game does not read there
// (a FILE on an ACTION that is not SCREEN, a DATASOURCE on a window that is not a marquee),
// references and defines nothing. A symbol is found where its field's scope says, and the
// type says which definitions no lookup finds and what value one carries
// (Document::refine_symbol). Each field is asked of its record as a FieldUse, which copies
// nothing of the schema (an animation table key's 252 choices stay in the type's table).
void extract_record(const Document &document, const NodeAddress &address, Extracted &out) {
	if (!document.present(address, std::string())) return;
	std::string record, locator;
	const auto place = [&] {
		if (!locator.empty()) return;
		record = document.record_path(address);
		locator = document.locator(address);
	};
	for (const FieldSchema &schema : document.fields(address.kind)) {
		const FieldUse field = document.field_on(address, schema);
		if (field.reference == ReferenceKind::None && field.defines == ReferenceKind::None &&
		    field.variable_through == ReferenceKind::None)
			continue;
		if (field.applies == Applicability::Ignored || !document.present(address, schema.id)) continue;
		Value value;
		if (!document.get(address, schema.id, value)) continue;
		const std::string defined = field.defines == ReferenceKind::None ? std::string() : value_name(value);
		if (!defined.empty()) {
			place();
			GraphSymbol symbol = symbol_of(field.defines, defined, document.path(), record, field.scope);
			symbol.locator = locator;
			symbol.address = address;
			symbol.field = schema.id;
			SymbolFacts facts;
			document.refine_symbol(address, facts);
			symbol.value = std::move(facts.value);
			symbol.inert = facts.inert;
			symbol.inert_reason = std::move(facts.inert_reason);
			symbol.line = facts.line;
			out.symbols.push_back(std::move(symbol));
		}
		ReferenceKind kind;
		std::string name, scope;
		if (!reference_target(field, value, kind, name, scope)) continue;
		place();
		GraphEdge edge = edge_of(document.path(), record, schema.id, kind, name, scope, !field.read_only);
		edge.locator = locator;
		edge.address = address;
		edge.loader_arg = field.loader_arg;
		// A text that is one %NAME% stands for the variable's value: a use of the variable alone
		// (FieldUse::variable_through), which Rename rewrites with it.
		if (field.reference == ReferenceKind::None) edge.through = field.variable_through;
		out.edges.push_back(std::move(edge));
		// A menu's font or texture through a style variable is two references: the
		// variable, and the file it names once resolved.
		if (field.reference != ReferenceKind::None && field.reference != ReferenceKind::StyleVar &&
		    is_style_reference(name)) {
			GraphEdge var = edge_of(document.path(), record, schema.id, ReferenceKind::StyleVar, name, std::string(),
			                        !field.read_only);
			var.locator = locator;
			var.address = address;
			var.through = field.reference; // what the variable's value must name here
			out.edges.push_back(std::move(var));
		}
	}
}

bool extract_environment(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	std::istringstream input(std::string(bytes.begin(), bytes.end()));
	env::Config config;
	std::string message;
	if (!env::load_env(input, config, message)) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, message, name);
		return false;
	}
	auto edge = [&](const char *field, ReferenceKind kind, const std::string &value) {
		if (!value.empty()) out.edges.push_back(edge_of(name, std::string(), field, kind, value));
	};
	edge("sky_map1", ReferenceKind::Texture, config.sky_map1);
	edge("sky_map2", ReferenceKind::Texture, config.sky_map2);
	edge("sun_3di", ReferenceKind::Model, config.sun_3di);
	edge("moon_3di", ReferenceKind::Model, config.moon_3di);
	edge("glare_3di", ReferenceKind::Model, config.glare_3di);
	edge("star_3di", ReferenceKind::Model, config.star_3di);
	return true;
}

bool extract_avatars(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	avatars::AvatarsFile file{};
	if (avatars::avatars_parse_memory(bytes.data(), bytes.size(), &file) != 0) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, "The avatar table could not be read.", name);
		return false;
	}
	for (size_t i = 0; i < file.parts_count; ++i) {
		const avatars::AvatarPart &part = file.parts[i];
		const std::string record(part.name);
		auto edge = [&](const char *field, ReferenceKind kind, const char *value) {
			if (value && *value) out.edges.push_back(edge_of(name, record, field, kind, value));
		};
		edge("graphic", ReferenceKind::Model, part.graphic);
		edge("graphic_j", ReferenceKind::Model, part.graphic_j);
		edge("graphic_s", ReferenceKind::Model, part.graphic_s);
		edge("name", ReferenceKind::TextId, part.display_name);
	}
	avatars::avatars_free(&file);
	return true;
}

bool extract_particles(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	particle::ParticleFile file;
	particle::ParseError parse_error;
	if (!particle::load_particles_from_buffer(reinterpret_cast<const char *>(bytes.data()), bytes.size(), file, parse_error)) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, "The particle file could not be read.", name);
		return false;
	}
	for (const particle::EffectDef &effect : file.effects)
		if (!effect.id.empty()) out.symbols.push_back(symbol_of(ReferenceKind::Particle, effect.id, name, effect.id));
	for (const particle::ParticleDef &definition : file.particles) {
		for (size_t g = 0; g < definition.graphics.size(); ++g) {
			const particle::GraphicLayer &layer = definition.graphics[g];
			if (!layer.present || layer.texture.empty()) continue;
			out.edges.push_back(edge_of(name, definition.id, "graphic" + std::to_string(g + 1), ReferenceKind::Texture,
			                            layer.texture));
		}
	}
	return true;
}

bool extract_mission(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	bms::File file;
	std::string message;
	if (!bms::parse(bytes.data(), bytes.size(), file, message)) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, message, name);
		return false;
	}
	if (!file.get_terrain().empty())
		out.edges.push_back(edge_of(name, std::string(), "terrain", ReferenceKind::Terrain, file.get_terrain()));
	if (!file.get_environment().empty())
		out.edges.push_back(edge_of(name, std::string(), "environment", ReferenceKind::Environment, file.get_environment()));
	struct Pool { const char *label; const std::vector<bms::Entity> *entities; };
	const Pool pools[] = {{"item", &file.items}, {"building", &file.buildings}, {"marker", &file.markers}, {"organic", &file.organics}};
	for (const Pool &pool : pools) {
		for (size_t i = 0; i < pool.entities->size(); ++i) {
			const int id = mission::entity_item_id((*pool.entities)[i]);
			if (id <= 0) continue;
			out.edges.push_back(edge_of(name, std::string(pool.label) + "[" + std::to_string(i) + "]", "item_id",
			                            ReferenceKind::Item, std::to_string(id)));
		}
	}
	return true;
}

// The kinds the graph reads through the engine's own parser, not a document type.
using NativeExtractor = bool (*)(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out,
                                 Diagnostic &error);
struct NativeKind {
	AssetKind kind;
	NativeExtractor extract;
};
constexpr NativeKind kNativeKinds[] = {
	{AssetKind::Environment, extract_environment},
	{AssetKind::AvatarDefs, extract_avatars},
	{AssetKind::Particles, extract_particles},
	{AssetKind::Mission, extract_mission},
};

NativeExtractor native_extractor(AssetKind kind) {
	for (const NativeKind &native : kNativeKinds)
		if (native.kind == kind) return native.extract;
	return nullptr;
}

} // namespace

ReferenceKind value_reference(const FieldUse &field, const Value &value) {
	if (field.reference != ReferenceKind::None) return field.reference;
	return field.variable_through != ReferenceKind::None && is_style_reference(value_name(value)) ? ReferenceKind::StyleVar
	                                                                                                : ReferenceKind::None;
}

bool reference_target(const FieldUse &field, const Value &value, ReferenceKind &kind, std::string &name,
                      std::string &scope) {
	kind = value_reference(field, value);
	scope.clear();
	name.clear();
	if (kind == ReferenceKind::None) return false;
	name = value_name(value);
	// A text's whole %NAME% names the variable, which has no scope (the field's is what it defines).
	if (field.reference == ReferenceKind::None) return true;
	if (name.empty()) return false;
	const std::string normalized = graph_names::key(name);
	if (normalized == "NONE" || normalized == "NULL") return false;
	if (kind == ReferenceKind::StyleVar && !is_style_reference(name)) return false; // a literal color, not a reference
	scope = field.scope; // where the document type says the record's reference resolves
	return true;
}

void extract_from_text(const TextDocument &document, Extracted &out) {
	const DocumentType *type = document_type_for(document.kind());
	if (!type || !type->references) return;
	std::vector<TextReference> references;
	type->references(document, references);
	for (TextReference &reference : references) {
		// A name its row reads as none (an empty one, NONE, NULL) is no reference, as a field's.
		const std::string normalized = graph_names::key(reference.value);
		if (reference.value.empty() || normalized == "NONE" || normalized == "NULL") continue;
		GraphEdge edge = edge_of(document.path(), std::string(), std::string(), reference.kind,
				reference.value, reference.scope, true);
		edge.locator = TextDocument::locator(reference.span.line, reference.span.column);
		edge.span = reference.span;
		edge.fallback = std::move(reference.fallback);
		out.edges.push_back(std::move(edge));
	}
}

void extract_from_document(const Document &document, Extracted &out) {
	for (const auto &row : document.rows()) {
		if (!row) continue;
		extract_record(document, {row->id, row->kind, 0}, out);
		document.walk_records(*row, [&](const NodeAddress &nested, const Document::Placement &) {
			extract_record(document, nested, out);
			return true;
		});
	}
}

bool graph_reads_kind(AssetKind kind) {
	// A record type's documents, whose records the extraction reads, a text type's whose text names
	// references (S13 D9), or a native extractor; any other type's documents give the graph nothing
	// (S13 D6).
	const DocumentType *type = document_type_for(kind);
	if (type) {
		const DocumentContent content = document_content(*type);
		if (content == DocumentContent::Records) return true;
		if (content == DocumentContent::Text && type->references) return true;
	}
	return native_extractor(kind) != nullptr;
}

bool graph_reads_file(AssetKind kind, const std::string &name) {
	// A .mis is a mission too, the mission editors' text form (docs/mission/mis-format-re.md),
	// which no BMS parse reads.
	if (kind == AssetKind::Mission && !strutil::ends_with_icase(name, ".bms"))
		return false;
	return graph_reads_kind(kind);
}

bool extract_from_bytes(const std::string &name, AssetKind kind, const std::vector<uint8_t> &bytes,
                        const std::string &game, Extracted &out, Diagnostic &error) {
	if (!graph_reads_file(kind, name))
		return true;
	// A record type's document, its records extracted; a text type's, its text's references; a type
	// of another kind falls through to a native extractor, or gives nothing.
	if (const DocumentType *type = document_type_for(kind)) {
		const DocumentContent content = document_content(*type);
		if (content == DocumentContent::Records || content == DocumentContent::Text) {
			const std::unique_ptr<DocumentBase> document = type->make();
			if (!document->load_bytes(bytes, name, kind, game, error)) return false;
			if (const Document *records = records_of(*document))
				extract_from_document(*records, out);
			else
				extract_from_text(*text_of(*document), out);
			return true;
		}
	}
	const NativeExtractor extract = native_extractor(kind);
	if (!extract) return true;
	// Decoded as the game's loader decodes a stored file (a document decodes its own).
	std::vector<uint8_t> decoded = bytes;
	vfs_decode_payload(decoded, gameprofile::gameprofile_scr_policy_for_code(game.c_str()));
	return extract(name, decoded, out, error);
}

bool extract_from_asset(const ProjectPaths &paths, const ProjectDocument &project, const AssetEntry &asset,
                        Extracted &out, Diagnostic &error) {
	std::vector<uint8_t> bytes;
	std::string message;
	if (!read_file_bytes((std::filesystem::path(paths.root) / asset.relative_path).generic_string(), bytes, message)) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, message, asset.relative_path);
		return false;
	}
	return extract_from_bytes(asset.relative_path, asset.kind, bytes, project.target_game, out, error);
}

} // namespace opennova::editor
