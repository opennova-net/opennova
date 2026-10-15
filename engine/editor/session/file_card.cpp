#include <editor/session/file_card.h>

#include <vector>

#include <editor/assets/asset_kind_words.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/name_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/model/field_text.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project_build/archive_routing.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/wav_pcm.h>
#include <formats/lwf/wav_source.h>

namespace opennova::editor {

using io::JsonValue;

namespace {

FileCard::Sound decode_sound(const std::string &file, const AssetEntry &entry) {
	FileCard::Sound sound;
	sound.size = entry.size_bytes;
	sound.modified = entry.modified_ticks;
	if (entry.size_bytes > kWaveCardBytes) {
		sound.error = "It is " + std::to_string(entry.size_bytes >> 20) + " MB: the editor reads a wave of " +
		              std::to_string(kWaveCardBytes >> 20) + " MB at most.";
		return sound;
	}
	std::vector<uint8_t> bytes;
	if (!io::read_file_bytes(file, bytes, sound.error)) return sound;
	// What the game's loader makes of it, and what it holds (the sound lane: formats/lwf/wav_source.h).
	const lwf::WaveFacts facts = lwf::wave_facts(bytes, kWaveCardBins);
	sound.plays = facts.retail.plays;
	sound.refusal = facts.retail.why;
	if (facts.read) {
		sound.format = lwf::wave_format_words(facts.format);
		sound.peak = facts.peak;
		sound.rms = facts.rms;
		sound.envelope = facts.envelope;
	}
	lwf::WavPcm pcm;
	if (!lwf::wav_decode_pcm16(bytes.data(), bytes.size(), pcm, sound.error)) return sound;
	sound.decoded = true;
	sound.rate = pcm.sample_rate;
	sound.channels = pcm.channels;
	const double frames = pcm.channels ? double(pcm.pcm16.size()) / (2.0 * pcm.channels) : 0.0;
	sound.seconds = pcm.sample_rate ? frames / double(pcm.sample_rate) : 0.0;
	return sound;
}

// The place a Go to names a record of the file by (usage_target, symbol_target): its locator, else its
// path as the graph names it where the file keeps no places.
const std::string &place_of(const std::string &locator, const std::string &record) {
	return locator.empty() ? record : locator;
}

// Whether a record of the file is the one the card's Go to names: its place, and the field where the Go to
// names one (a record named alone marks each of its lines; the file alone, none).
bool marked(const FileCard &card, const std::string &place, const std::string &field) {
	if (card.at_locator.empty() && card.at_field.empty()) return false;
	return place == card.at_locator && (card.at_field.empty() || field.empty() || field == card.at_field);
}

// A use as the card lists it: the naming file and the record in its words, its record and field, where a
// click goes.
FileCard::User user_of(const AssetScan &scan, const GraphEdge &edge) {
	const AssetEntry *source = scan.at_path(edge.source);
	FileCard::User user;
	const std::string place = edge_place_words(edge, source ? source->kind : AssetKind::Unknown);
	user.text = (source ? source->logical_name : edge.source) + (place.empty() ? std::string() : ": " + place);
	user.file = edge.source;
	user.record = edge.record;
	user.field = edge_field_words(scan, edge);
	user.target = usage_target(scan, edge);
	return user;
}

// What the editor does with a file of its kind.
std::string editor_words(const AssetEntry &entry, bool packed) {
	if (entry.kind == AssetKind::ImportSource)
		return "The editor imports it: its outputs are the project's files, opened as their kinds are.";
	if (is_editable_kind(entry.kind)) return "The editor opens it as a document.";
	return packed ? "The editor has no editor for this kind yet: it keeps the file as it is, and a build packs it as it is."
	              : "The editor has no editor for this kind yet: it keeps the file as it is.";
}

} // namespace

const char *reference_status_words(ReferenceStatus status) {
	switch (status) {
	case ReferenceStatus::Present: return "found in the project";
	case ReferenceStatus::Missing: return "missing from the project";
	case ReferenceStatus::Unverified: return "not checked";
	case ReferenceStatus::NotAReference: break;
	}
	return "";
}

std::string edge_field_words(const AssetScan &scan, const GraphEdge &edge) {
	const AssetEntry *source = scan.at_path(edge.source);
	const DocumentType *type = source ? document_type_for(source->kind) : nullptr;
	if (!type || !type->fields) return edge.field;
	for (const FieldSchema &field : type->fields(edge.address.kind))
		if (field.id == edge.field) return field_title(field);
	return edge.field;
}

FileUsers file_users(const SessionView &view, const std::string &path) {
	FileUsers users;
	if (!view.project.open || !view.project.scan) return users;
	const AssetScan &scan = *view.project.scan;
	const AssetEntry *entry = scan.named(path);
	if (!entry) return users;
	users.found = true;
	users.path = entry->relative_path;
	users.name = entry->logical_name;
	users.reading = !view.activity.validation.read || view.activity.validation.files_unread;
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return users;
	const auto line_of = [&scan](const GraphEdge &edge) {
		const AssetEntry *source = scan.at_path(edge.source);
		FileUsers::Line line;
		line.file = edge.source;
		line.words = edge_place_words(edge, source ? source->kind : AssetKind::Unknown);
		// A name the file makes with no record or field to say (a text's whole name): the value it names.
		if (line.words.empty()) line.words = edge.value;
		line.field = edge.field;
		line.target = usage_target(scan, edge);
		return line;
	};
	const auto name_of = [&scan](const std::string &file) {
		const AssetEntry *source = scan.at_path(file);
		return source ? source->logical_name : file;
	};
	// Each group in the order its file's first line comes (the graph's).
	const auto group_of = [](auto &groups, const std::string &file, const std::string &name) -> auto & {
		for (auto &group : groups)
			if (group.file == file) return group;
		groups.emplace_back();
		groups.back().file = file;
		groups.back().name = name;
		return groups.back();
	};
	for (const FileUse &use : file_uses(*graph, users.path)) {
		FileUsers::Use made;
		made.line = line_of(*use.edge);
		for (const GraphEdge *edge : use.further) {
			group_of(made.further, edge->source, name_of(edge->source)).lines.push_back(line_of(*edge));
			++made.further_count;
		}
		users.further_count += made.further_count;
		++users.count;
		group_of(users.groups, use.edge->source, name_of(use.edge->source)).uses.push_back(std::move(made));
	}
	return users;
}

size_t file_use_count(const SessionView &view, const std::string &path) {
	if (!view.project.open || !view.project.scan || !view.findings.graph) return 0;
	const AssetEntry *entry = view.project.scan->named(path);
	return entry ? view.findings.graph->usages_of(entry->relative_path).size() : 0;
}

JsonValue file_users_json(const FileUsers &users) {
	JsonValue out = JsonValue::make_object();
	out.set("found", JsonValue::make_bool(users.found));
	if (!users.found) return out;
	out.set("path", JsonValue::make_string(users.path));
	out.set("name", JsonValue::make_string(users.name));
	out.set("count", JsonValue::make_number(double(users.count)));
	out.set("further_count", JsonValue::make_number(double(users.further_count)));
	if (users.reading) out.set("reading", JsonValue::make_bool(true));
	const auto line_json = [](const FileUsers::Line &line) {
		JsonValue item = JsonValue::make_object();
		item.set("words", JsonValue::make_string(line.words));
		item.set("field", JsonValue::make_string(line.field));
		item.set("file", JsonValue::make_string(line.target.file));
		if (!line.target.locator.empty()) item.set("locator", JsonValue::make_string(line.target.locator));
		item.set("editable", JsonValue::make_bool(line.target.editable));
		return item;
	};
	JsonValue files = JsonValue::make_array();
	for (const FileUsers::UseGroup &group : users.groups) {
		JsonValue file = JsonValue::make_object();
		file.set("file", JsonValue::make_string(group.file));
		file.set("name", JsonValue::make_string(group.name));
		JsonValue list = JsonValue::make_array();
		for (const FileUsers::Use &use : group.uses) {
			JsonValue item = line_json(use.line);
			JsonValue further = JsonValue::make_array();
			for (const FileUsers::Group &next : use.further) {
				JsonValue hop = JsonValue::make_object();
				hop.set("file", JsonValue::make_string(next.file));
				hop.set("name", JsonValue::make_string(next.name));
				JsonValue lines = JsonValue::make_array();
				for (const FileUsers::Line &line : next.lines) lines.push(line_json(line));
				hop.set("uses", std::move(lines));
				further.push(std::move(hop));
			}
			item.set("further", std::move(further));
			list.push(std::move(item));
		}
		file.set("uses", std::move(list));
		files.push(std::move(file));
	}
	out.set("files", std::move(files));
	return out;
}

FileCard shown_file_card(const SessionView &view, const std::string &path, const FileCard::Sound *known) {
	const bool shown = !path.empty() && path == view.documents.page;
	return shown ? file_card(view, path, known, view.documents.page_locator, view.documents.page_field)
	             : file_card(view, path, known);
}

FileCard file_card(const SessionView &view, const std::string &path, const FileCard::Sound *known, const std::string &locator,
                   const std::string &field) {
	FileCard card;
	card.at_locator = locator;
	card.at_field = field;
	if (!view.project.open || !view.project.scan) return card;
	const AssetScan &scan = *view.project.scan;
	const AssetEntry *entry = scan.named(path);
	if (!entry) return card;
	card.found = true;
	card.path = entry->relative_path;
	card.name = entry->logical_name;
	card.kind = entry->kind;
	card.kind_label = asset_kind_label(entry->kind);
	const AssetKindWords &words = asset_kind_words(entry->kind);
	card.what = words.what;
	card.read_by = words.read_by;
	card.cite = words.cite;
	card.size = entry->size_bytes;
	// Where a build puts it, by the plan's own rule (build_place_words: its word on the file first, then the
	// archive, the expansion's archive or folder, or nowhere), and what the editor does with it.
	const BuildPlaceWords place =
	        build_place_words(*entry, view.project.document ? view.project.document->expansion.name : std::string());
	card.build = place.words;
	card.editor = editor_words(*entry, place.packed);
	card.imported_from = entry->imported_from;
	card.opens = is_editable_kind(entry->kind);
	card.wave = entry->kind == AssetKind::Wave;
	if (card.wave)
		card.sound = known && known->modified != 0 && known->size == entry->size_bytes && known->modified == entry->modified_ticks
		                     ? *known
		                     : decode_sound(join_path(view.project.root, entry->relative_path), *entry);
	// Until the graph has read the project's references, or while it has not read the files as the scan
	// lists them now (an import's): not during an edit's validation, which the graph keeps up with.
	card.reading = !view.activity.validation.read || view.activity.validation.files_unread;
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.asset != card.path) continue;
		if (d.severity == DiagnosticSeverity::Error) ++card.errors;
		if (d.severity == DiagnosticSeverity::Warning) ++card.warnings;
	}
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return card;
	// What it defines (DI-17), each name with who names it, in the words the project's names give it.
	const GraphNameSource names(*graph);
	for (const FileDefinition &definition : file_definitions(*graph, card.path)) {
		const GraphSymbol &symbol = *definition.symbol;
		FileCard::Definition defined;
		defined.text = std::string(reference_row(symbol.kind).label) + " " + symbol.display;
		const std::string said = definition_words(symbol, &names);
		if (!said.empty() && said != symbol.display) defined.text += ": " + said;
		defined.read = definition.read;
		if (!definition.read) defined.unread = symbol.inert_reason.empty() ? "no lookup of the game finds it" : symbol.inert_reason;
		defined.at = marked(card, place_of(symbol.locator, symbol.record), symbol.field);
		for (const GraphEdge *edge : definition.users) defined.used_by.push_back(user_of(scan, *edge));
		card.defines.push_back(std::move(defined));
	}
	// What it names: each reference (a name the game's reader would not follow, a file read only beside
	// another the project lacks, is none), where in the file it is named (DI-17: the line a Go to marks reads
	// which record it is), whether the project has it.
	for (const GraphEdge *edge : graph->references_of(card.path)) {
		FileCard::Named named;
		named.status = graph->resolve(*edge, &named.file);
		if (named.status == ReferenceStatus::NotAReference) continue;
		named.field = edge_field_words(scan, *edge);
		named.record = edge->record;
		named.value = edge->value;
		const std::string &where = edge->record.empty() ? edge->field : edge->record;
		named.text = (where.empty() ? std::string() : where + ": ") + reference_row(edge->kind).label + " " + edge->value;
		named.at = marked(card, place_of(edge->locator, edge->record), edge->field);
		if (const AssetEntry *target = named.file.empty() ? nullptr : scan.at_path(named.file)) {
			named.wave = target->kind == AssetKind::Wave;
			named.target = file_target(scan, named.file);
		}
		if (named.missing()) {
			// A name nothing resolves: a symbol's Go to lands where it belongs; a file's in Problems, its fixes.
			named.text += " (not in the project)";
			missing_target(ReferenceSubject{edge->kind, edge->value, graph->lookup_scope(*edge), edge->loader_arg}, view,
			               named.target);
		}
		card.names.push_back(std::move(named));
	}
	for (const GraphEdge *edge : graph->usages_of(card.path)) card.used_by.push_back(user_of(scan, *edge));
	return card;
}

JsonValue file_card_json(const FileCard &card) {
	JsonValue out = JsonValue::make_object();
	out.set("found", JsonValue::make_bool(card.found));
	if (!card.found) return out;
	out.set("path", JsonValue::make_string(card.path));
	out.set("name", JsonValue::make_string(card.name));
	out.set("kind", JsonValue::make_string(asset_kind_token(card.kind)));
	out.set("kind_label", JsonValue::make_string(card.kind_label));
	out.set("size", JsonValue::make_number(double(card.size)));
	out.set("what", JsonValue::make_string(card.what));
	out.set("read_by", JsonValue::make_string(card.read_by));
	out.set("cite", JsonValue::make_string(card.cite));
	out.set("build", JsonValue::make_string(card.build));
	out.set("editor", JsonValue::make_string(card.editor));
	out.set("imported_from", JsonValue::make_string(card.imported_from));
	out.set("opens", JsonValue::make_bool(card.opens));
	out.set("errors", JsonValue::make_number(double(card.errors)));
	out.set("warnings", JsonValue::make_number(double(card.warnings)));
	if (card.wave) {
		out.set("wave", JsonValue::make_bool(true));
		JsonValue sound = JsonValue::make_object();
		sound.set("decoded", JsonValue::make_bool(card.sound.decoded));
		if (!card.sound.decoded) sound.set("error", JsonValue::make_string(card.sound.error));
		sound.set("rate", JsonValue::make_number(card.sound.rate));
		sound.set("channels", JsonValue::make_number(card.sound.channels));
		sound.set("seconds", JsonValue::make_number(card.sound.seconds));
		sound.set("plays", JsonValue::make_bool(card.sound.plays));
		if (!card.sound.plays) sound.set("refusal", JsonValue::make_string(card.sound.refusal));
		sound.set("format", JsonValue::make_string(card.sound.format));
		sound.set("peak", JsonValue::make_number(card.sound.peak));
		sound.set("rms", JsonValue::make_number(card.sound.rms));
		JsonValue envelope = JsonValue::make_array();
		for (const float bin : card.sound.envelope) envelope.push(JsonValue::make_number(bin));
		sound.set("envelope", std::move(envelope));
		out.set("sound", std::move(sound));
	}
	// Where the Go to that showed it landed (DI-17).
	if (!card.at_locator.empty()) out.set("at_locator", JsonValue::make_string(card.at_locator));
	if (!card.at_field.empty()) out.set("at_field", JsonValue::make_string(card.at_field));
	// A line's words, its mark, and where a click goes (open_document's path, locator and field).
	const auto line_json = [](const FileCard::Line &line) {
		JsonValue item = JsonValue::make_object();
		item.set("text", JsonValue::make_string(line.text));
		if (line.at) item.set("at", JsonValue::make_bool(true));
		if (!line.target.file.empty()) item.set("file", JsonValue::make_string(line.target.file));
		if (!line.target.locator.empty()) item.set("locator", JsonValue::make_string(line.target.locator));
		if (!line.target.field.empty()) item.set("field", JsonValue::make_string(line.target.field));
		return item;
	};
	const auto users_json = [&line_json](const std::vector<FileCard::User> &users) {
		JsonValue list = JsonValue::make_array();
		for (const FileCard::User &user : users) list.push(line_json(user));
		return list;
	};
	JsonValue defines = JsonValue::make_array();
	for (const FileCard::Definition &defined : card.defines) {
		JsonValue item = JsonValue::make_object();
		item.set("text", JsonValue::make_string(defined.text));
		item.set("read", JsonValue::make_bool(defined.read));
		if (!defined.unread.empty()) item.set("unread", JsonValue::make_string(defined.unread));
		if (defined.at) item.set("at", JsonValue::make_bool(true));
		item.set("used_by", users_json(defined.used_by));
		defines.push(std::move(item));
	}
	out.set("defines", std::move(defines));
	JsonValue names = JsonValue::make_array();
	for (const FileCard::Named &named : card.names) {
		JsonValue item = line_json(named);
		item.set("value", JsonValue::make_string(named.value));
		item.set("status", JsonValue::make_string(reference_status_words(named.status)));
		if (named.wave) item.set("wave", JsonValue::make_bool(true));
		if (named.missing()) item.set("missing", JsonValue::make_bool(true));
		names.push(std::move(item));
	}
	out.set("names", std::move(names));
	out.set("used_by", users_json(card.used_by));
	if (card.reading) out.set("reading", JsonValue::make_bool(true));
	return out;
}

} // namespace opennova::editor
