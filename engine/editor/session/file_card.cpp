#include <editor/session/file_card.h>

#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/field_text.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project_build/archive_routing.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/wav_pcm.h>

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
	if (!read_file_bytes(file, bytes, sound.error)) return sound;
	lwf::WavPcm pcm;
	if (!lwf::wav_decode_pcm16(bytes.data(), bytes.size(), pcm, sound.error)) return sound;
	sound.decoded = true;
	sound.rate = pcm.sample_rate;
	sound.channels = pcm.channels;
	const double frames = pcm.channels ? double(pcm.pcm16.size()) / (2.0 * pcm.channels) : 0.0;
	sound.seconds = pcm.sample_rate ? frames / double(pcm.sample_rate) : 0.0;
	return sound;
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

FileCard file_card(const SessionView &view, const std::string &path, const FileCard::Sound *known) {
	FileCard card;
	if (!view.project.open || !view.project.scan) return card;
	const AssetScan &scan = *view.project.scan;
	const AssetEntry *entry = scan.named(path);
	if (!entry) return card;
	card.found = true;
	card.path = entry->relative_path;
	card.name = entry->logical_name;
	card.kind = entry->kind;
	card.kind_label = asset_kind_label(entry->kind);
	card.about = asset_kind_row(entry->kind).about;
	card.size = entry->size_bytes;
	// Where a build puts it, by the plan's own rule (build_place_words: its word on the file first, then the
	// archive, the expansion's archive or folder, or nowhere), as a file's page says it.
	card.build = build_place_words(*entry, view.project.document->expansion.name).words;
	card.imported_from = entry->imported_from;
	card.opens = is_editable_kind(entry->kind);
	card.wave = entry->kind == AssetKind::Wave;
	if (card.wave)
		card.sound = known && known->modified != 0 && known->size == entry->size_bytes && known->modified == entry->modified_ticks
		                     ? *known
		                     : decode_sound(join_path(view.project.root, entry->relative_path), *entry);
	card.reading = view.activity.validation.running || !view.activity.validation.read;
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return card;
	for (const GraphEdge *edge : graph->references_of(entry->relative_path)) {
		FileCard::Named named;
		named.field = edge_field_words(scan, *edge);
		named.record = edge->record;
		named.value = edge->value;
		if (!edge->target.empty()) named.status = graph->resolve(*edge, &named.file);
		if (const AssetEntry *target = named.file.empty() ? nullptr : scan.at_path(named.file)) {
			named.wave = target->kind == AssetKind::Wave;
			named.target = file_target(scan, named.file);
		}
		card.names.push_back(std::move(named));
	}
	for (const GraphEdge *edge : graph->usages_of(entry->relative_path)) {
		FileCard::User user;
		user.file = edge->source;
		user.record = edge->record;
		user.field = edge_field_words(scan, *edge);
		user.target = usage_target(scan, *edge);
		card.named_by.push_back(std::move(user));
	}
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
	out.set("about", JsonValue::make_string(card.about));
	out.set("size", JsonValue::make_number(double(card.size)));
	out.set("build", JsonValue::make_string(card.build));
	out.set("imported_from", JsonValue::make_string(card.imported_from));
	out.set("opens", JsonValue::make_bool(card.opens));
	if (card.wave) {
		JsonValue sound = JsonValue::make_object();
		sound.set("decoded", JsonValue::make_bool(card.sound.decoded));
		if (!card.sound.decoded) sound.set("error", JsonValue::make_string(card.sound.error));
		sound.set("rate", JsonValue::make_number(card.sound.rate));
		sound.set("channels", JsonValue::make_number(card.sound.channels));
		sound.set("seconds", JsonValue::make_number(card.sound.seconds));
		out.set("sound", std::move(sound));
	}
	JsonValue names = JsonValue::make_array();
	for (const FileCard::Named &named : card.names) {
		JsonValue item = JsonValue::make_object();
		item.set("field", JsonValue::make_string(named.field));
		item.set("record", JsonValue::make_string(named.record));
		item.set("value", JsonValue::make_string(named.value));
		item.set("status", JsonValue::make_string(reference_status_words(named.status)));
		item.set("file", JsonValue::make_string(named.file));
		item.set("wave", JsonValue::make_bool(named.wave));
		names.push(std::move(item));
	}
	out.set("names", std::move(names));
	JsonValue users = JsonValue::make_array();
	for (const FileCard::User &user : card.named_by) {
		JsonValue item = JsonValue::make_object();
		item.set("file", JsonValue::make_string(user.file));
		item.set("record", JsonValue::make_string(user.record));
		item.set("field", JsonValue::make_string(user.field));
		users.push(std::move(item));
	}
	out.set("named_by", std::move(users));
	if (card.reading) out.set("reading", JsonValue::make_bool(true));
	return out;
}

} // namespace opennova::editor
