#include "inspector_layout.h"

#include <base/io/strutil.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/field_text.h>
#include <editor/session/session_view.h>
#include <editor/ui/editor_requests.h>

#include <map>
#include <memory>
#include <set>

namespace opennova::editor {

std::string field_title(const Document &document, NodeKind kind, const std::string &id) {
	for (const FieldSchema &field : document.fields(kind))
		if (field.id == id) return field_title(field);
	return id;
}

std::string edge_field_title(const SessionView &view, const GraphEdge &edge) {
	const AssetEntry *source = view.scan.at_path(edge.source);
	const DocumentType *type = source ? document_type_for(source->kind) : nullptr;
	if (!type) return edge.field;
	// A type's schema never depends on a file's content: a blank document of it, made once
	// per kind, answers for every file of the kind.
	static std::map<AssetKind, std::unique_ptr<Document>> blanks;
	std::unique_ptr<Document> &blank = blanks[source->kind];
	if (!blank) blank = type->make();
	return field_title(*blank, edge.address.kind, edge.field);
}

namespace {

std::string first_step(const std::string &id) { return id.substr(0, id.find('.')); }

bool matches(const std::string &text, const std::string &filter) {
	return filter.empty() || window_requests::matches(text, filter.c_str());
}

} // namespace

std::vector<InspectorSection> plan_inspector(const Document &document, const NodeAddress &record,
                                             const NodeAddress &collections_owner, const std::string &filter) {
	const std::vector<Document::Collection> collections = document.collections_of(collections_owner);
	const bool own = collections_owner == record;
	// The groups: every first step of a dotted id, and of a collection's token when the
	// record's own collections are shown (a part's toggle has no dotted field of its own).
	const std::vector<FieldSchema> &fields = document.fields(record.kind);
	std::set<std::string> groups;
	for (const FieldSchema &field : fields)
		if (field.id.find('.') != std::string::npos) groups.insert(first_step(field.id));
	if (own)
		for (const Document::Collection &collection : collections) groups.insert(first_step(collection.spec.kind_name));

	std::vector<InspectorSection> out(1); // the general fields first
	std::map<std::string, size_t> at;     // a group's section
	auto group = [&](const std::string &key) -> InspectorSection & {
		const auto found = at.find(key);
		if (found != at.end()) return out[found->second];
		at.emplace(key, out.size());
		out.emplace_back();
		out.back().key = key;
		return out.back();
	};
	for (const FieldSchema &schema : fields) {
		Value value;
		if (!document.get(record, schema.id, value)) continue;
		const FieldUse field = document.field_on(record, schema);
		const bool is_written = written(document, record, schema);
		const bool shown = field.applies != Applicability::Ignored || is_written;
		const bool dotted = schema.id.find('.') != std::string::npos;
		const bool toggle = !dotted && groups.count(schema.id) && is_yes_no(schema);
		// A field of one step that names no group joins the fields sharing its heading.
		const std::string key = dotted || toggle ? first_step(schema.id) : groups.count(schema.id) ? std::string() : schema.section;
		InspectorSection &section = key.empty() ? out.front() : group(key);
		if (section.title.empty() && !schema.section.empty()) section.title = schema.section;
		if (toggle) {
			// The block's own field reads as always written; its value says whether the block is.
			section.has_toggle = shown;
			section.toggle = field;
			section.written = section.written || is_written;
			continue;
		}
		if (!shown || !(matches(schema.id, filter) || matches(schema.label, filter) || matches(schema.token, filter))) continue;
		section.fields.push_back(field);
		section.written = section.written || is_written;
	}
	for (const Document::Collection &collection : collections) {
		const Document::CollectionSpec &spec = collection.spec;
		if (spec.applies == Applicability::Ignored && collection.ids.empty()) continue;
		if (!matches(spec.label, filter) && !matches(spec.kind_name, filter)) continue;
		const auto claimed = own ? at.find(first_step(spec.kind_name)) : at.end();
		InspectorSection *section = nullptr;
		if (claimed != at.end()) {
			section = &out[claimed->second];
		} else {
			out.emplace_back();
			section = &out.back();
			section->key = spec.kind_name;
			section->title = spec.label;
		}
		section->collections.push_back(collection);
		section->written = section->written || !collection.ids.empty();
	}
	// A group with nothing left to show goes; so does a toggle whose block has no field the
	// filter kept, while a filter is typed.
	std::vector<InspectorSection> kept;
	for (InspectorSection &section : out) {
		if (!filter.empty() && section.fields.empty() && !matches(section.title, filter)) section.has_toggle = false;
		if (section.title.empty() && !section.key.empty()) section.title = section.key;
		if (section.fields.empty() && !section.has_toggle && section.collections.empty()) continue;
		kept.push_back(std::move(section));
	}
	return kept;
}

std::vector<InspectorSection> plan_shared_inspector(const Document &document, const std::vector<NodeAddress> &records,
                                                    const std::string &filter) {
	if (records.empty()) return {};
	const NodeAddress &primary = records.front();
	std::string name_field;
	Document::Placement at;
	if (document.placement(primary, at)) name_field = at.spec.name_field;
	const std::vector<FieldSchema> &fields = document.fields(primary.kind);
	std::set<std::string> groups;
	for (const FieldSchema &field : fields)
		if (field.id.find('.') != std::string::npos) groups.insert(first_step(field.id));

	std::vector<InspectorSection> out(1);
	std::map<std::string, size_t> placed;
	for (const FieldSchema &schema : fields) {
		if (schema.read_only || schema.id == name_field) continue;
		bool shared = true;
		for (const NodeAddress &record : records) {
			Value value;
			if (record.kind != primary.kind || !document.get(record, schema.id, value)) {
				shared = false;
				break;
			}
			const FieldUse on = document.field_on(record, schema);
			shared = on.applies != Applicability::Ignored || written(document, record, schema);
			if (!shared) break;
		}
		if (!shared) continue;
		const FieldUse field = document.field_on(primary, schema);
		if (!(matches(schema.id, filter) || matches(schema.label, filter) || matches(schema.token, filter))) continue;
		const bool dotted = schema.id.find('.') != std::string::npos;
		const bool toggle = !dotted && groups.count(schema.id) && is_yes_no(schema);
		const std::string key = dotted || toggle ? first_step(schema.id) : groups.count(schema.id) ? std::string() : schema.section;
		auto found = placed.find(key);
		if (!key.empty() && found == placed.end()) {
			found = placed.emplace(key, out.size()).first;
			out.emplace_back();
			out.back().key = key;
		}
		InspectorSection &section = key.empty() ? out.front() : out[found->second];
		if (section.title.empty() && !schema.section.empty()) section.title = schema.section;
		// The block's own switch leads its group.
		if (toggle) section.fields.insert(section.fields.begin(), field);
		else section.fields.push_back(field);
		section.written = section.written || written(document, primary, schema);
	}
	std::vector<InspectorSection> kept;
	for (InspectorSection &section : out) {
		if (section.fields.empty()) continue;
		if (section.title.empty() && !section.key.empty()) section.title = section.key;
		kept.push_back(std::move(section));
	}
	return kept;
}

bool field_mixed(const Document &document, const std::vector<NodeAddress> &records, const std::string &field) {
	if (records.size() < 2) return false;
	Value first;
	const bool has_first = document.get(records.front(), field, first);
	const bool first_present = document.present(records.front(), field);
	for (size_t i = 1; i < records.size(); ++i) {
		Value value;
		const bool has_value = document.get(records[i], field, value);
		if (has_value != has_first || (has_value && !(value == first)) || document.present(records[i], field) != first_present)
			return true;
	}
	return false;
}

} // namespace opennova::editor
