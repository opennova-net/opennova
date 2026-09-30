#include <editor/session/record_batch.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <editor/session/project_session.h>
#include <editor/session/session_json.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

// A record the batch made, by the label its edit gave it.
struct Made {
	size_t edit = 0; // the index of the edit that makes it
	NodeId row = 0;
	NodeKind kind = 0;
};

bool members_known(const JsonValue &object, std::initializer_list<const char *> known, std::string &error) {
	for (const io::JsonMember &member : object.object) {
		bool found = false;
		for (const char *key : known) found = found || member.key == key;
		if (!found) {
			error = "Unknown edit member \"" + member.key + "\".";
			return false;
		}
	}
	return true;
}

bool whole(const JsonValue &json, uint64_t &out) {
	if (!json.is_number() || json.number < 0.0 || json.number != std::floor(json.number) ||
	    json.number > 9007199254740992.0)
		return false;
	out = static_cast<uint64_t>(json.number);
	return true;
}

// A record named by an identity or a label: its address now (a label's: batch_made of
// the edit that makes it). False with the reason when it names nothing.
bool record_of(const JsonValue &json, const char *what, const Document &document,
               const std::map<std::string, Made> &labels, NodeAddress &out, std::string &error) {
	if (json.is_string()) {
		const auto found = labels.find(json.string);
		if (found == labels.end()) {
			error = std::string("\"") + what + "\" names \"" + json.string +
			        "\", which no earlier add or duplicate of the batch gave with \"as\".";
			return false;
		}
		out = {found->second.row, found->second.kind, batch_made(found->second.edit)};
		return true;
	}
	uint64_t id = 0;
	if (!whole(json, id) || id == 0) {
		error = std::string("\"") + what + "\" must be a record identity or a label.";
		return false;
	}
	out = document.address_of(NodeId(id));
	if (!out.row) {
		error = "No record " + std::to_string(id) + " in " + document.path() + ".";
		return false;
	}
	return true;
}

// An owner's identity as an edit's `parent` names it: a row by its own identity, a record by
// its identity, a record the batch makes by batch_made.
NodeId owner_identity(const NodeAddress &owner) { return owner.child ? owner.child : owner.row; }

bool parse_edit(const JsonValue &json, size_t index, const Document &document, std::map<std::string, Made> &labels,
                RecordBatch &out, std::string &error) {
	if (!json.is_object()) {
		error = "Every edit is an object.";
		return false;
	}
	if (!members_known(json, {"op", "id", "field", "value", "kind", "parent", "position", "as"}, error)) return false;
	const std::string op = json.get_string("op", "");
	Edit edit;
	if (!edit_operation_from_token(op, edit.operation) || edit.operation == EditOperation::Paste ||
	    edit.operation == EditOperation::SetFileValue) {
		error = "Unknown edit op \"" + op + "\" (set, clear, write, add, duplicate, remove or move).";
		return false;
	}
	const bool adds = edit.operation == EditOperation::Add;
	const bool makes = adds || edit.operation == EditOperation::Duplicate;
	const JsonValue *id = json.get("id");
	const JsonValue *parent = json.get("parent");
	if (adds == (id != nullptr)) {
		error = adds ? "An add names its owner with \"parent\", not \"id\"." : "Edit \"" + op + "\" names its record with \"id\".";
		return false;
	}
	if (id && !record_of(*id, "id", document, labels, edit.address, error)) return false;
	if (const JsonValue *field = json.get("field")) {
		if (!field->is_string()) {
			error = "\"field\" must be a string.";
			return false;
		}
		edit.field = field->string;
	}
	const bool fielded = edit.operation == EditOperation::Set || edit.operation == EditOperation::Clear ||
	                     edit.operation == EditOperation::Write;
	if (fielded != !edit.field.empty()) {
		error = fielded ? "Edit \"" + op + "\" names its \"field\"." : "Only set, clear and write take a \"field\".";
		return false;
	}
	if (const JsonValue *value = json.get("value")) {
		if (edit.operation != EditOperation::Set) {
			error = "Only set takes a \"value\".";
			return false;
		}
		if (!value_from_json(*value, edit.value)) {
			error = "\"value\" must be a number, a string or a bool.";
			return false;
		}
	} else if (edit.operation == EditOperation::Set) {
		error = "A set names its \"value\".";
		return false;
	}
	if (const JsonValue *kind = json.get("kind")) {
		if (!adds || !kind->is_string()) {
			error = "Only an add takes a \"kind\" (a record kind's token).";
			return false;
		}
		const NodeKind token = document.kind_from_name(kind->string);
		if (token < 0) {
			error = "Unknown record kind \"" + kind->string + "\" in " + document.path() + ".";
			return false;
		}
		edit.address.kind = token;
	} else if (adds) {
		error = "An add names its record \"kind\".";
		return false;
	}
	if (parent) {
		if (!adds && edit.operation != EditOperation::Move) {
			error = "Only add and move take a \"parent\".";
			return false;
		}
		NodeAddress owner;
		if (!record_of(*parent, "parent", document, labels, owner, error)) return false;
		edit.parent = owner_identity(owner);
		if (adds) edit.address.row = owner.row;
	}
	if (const JsonValue *position = json.get("position")) {
		uint64_t at = 0;
		if (!whole(*position, at)) {
			error = "\"position\" must be a whole number.";
			return false;
		}
		edit.position = size_t(at);
	} else if (edit.operation == EditOperation::Move) {
		error = "A move names its \"position\".";
		return false;
	} else if (edit.operation == EditOperation::Duplicate && !is_batch_made(edit.address.child)) {
		// Right after the record, as the document stands before the batch.
		Document::Placement at;
		if (document.placement(edit.address, at)) edit.position = at.index + 1;
		for (size_t i = 0; !edit.address.child && i < document.rows().size(); ++i)
			if (document.rows()[i]->id == edit.address.row) edit.position = i + 1;
	}
	std::string label;
	if (const JsonValue *as = json.get("as")) {
		if (!makes || !as->is_string() || as->string.empty()) {
			error = "Only an add or a duplicate takes \"as\", a label.";
			return false;
		}
		if (labels.count(as->string)) {
			error = "The label \"" + as->string + "\" is given twice.";
			return false;
		}
		label = as->string;
		labels[label] = Made{index, edit.address.row, edit.address.kind};
	}
	if (makes) out.made_labels.push_back(label);
	out.edits.push_back(std::move(edit));
	return true;
}

} // namespace

bool record_batch_from_json(const io::JsonValue &edits, const Document &document, RecordBatch &out,
                            std::string &error) {
	if (!edits.is_array() || edits.array.empty()) {
		error = "\"edits\" is a list of one edit or more.";
		return false;
	}
	RecordBatch batch;
	std::map<std::string, Made> labels;
	for (size_t i = 0; i < edits.array.size(); ++i) {
		if (!parse_edit(edits.array[i], i, document, labels, batch, error)) {
			error = "Edit " + std::to_string(i + 1) + ": " + error;
			return false;
		}
	}
	out = std::move(batch);
	return true;
}

bool list_batch_from_json(const Document &document, NodeId owner, const std::string &list,
                          const io::JsonValue &records, RecordBatch &out, std::string &error) {
	const NodeAddress holder = document.address_of(owner);
	if (!holder.row) {
		error = "No record " + std::to_string(owner) + " in " + document.path() + ".";
		return false;
	}
	if (!records.is_array()) {
		error = "\"records\" is a list of records, each {field: value, ...}.";
		return false;
	}
	const Document::Collection *collection = nullptr;
	const std::vector<Document::Collection> collections = document.collections_of(holder);
	for (const Document::Collection &candidate : collections)
		if (list == candidate.spec.kind_name) collection = &candidate;
	if (!collection) {
		error = document.record_name(holder) + " holds no \"" + list + "\" list.";
		return false;
	}
	RecordBatch batch;
	for (const NodeId id : collection->ids) {
		Edit remove;
		remove.operation = EditOperation::Remove;
		remove.address = {holder.row, collection->spec.kind, id};
		batch.edits.push_back(remove);
	}
	for (const JsonValue &record : records.array) {
		if (!record.is_object()) {
			error = "Every record is an object of {field: value}.";
			return false;
		}
		Edit add;
		add.operation = EditOperation::Add;
		add.address = {holder.row, collection->spec.kind, 0};
		add.parent = owner_identity(holder);
		const size_t made = batch.edits.size();
		batch.edits.push_back(add);
		batch.made_labels.emplace_back();
		for (const io::JsonMember &member : record.object) {
			Edit set;
			set.address = {holder.row, collection->spec.kind, batch_made(made)};
			set.field = member.key;
			if (!value_from_json(member.value, set.value)) {
				error = "\"" + member.key + "\" must be a number, a string or a bool.";
				return false;
			}
			batch.edits.push_back(std::move(set));
		}
	}
	out = std::move(batch);
	return true;
}

io::JsonValue record_batch_request(ProjectSession &session, const std::string &path, const io::JsonValue &request) {
	JsonValue answer = JsonValue::make_object();
	const auto refuse = [&](const std::string &error) {
		answer.set("ok", JsonValue::make_bool(false));
		answer.set("error", JsonValue::make_string(error));
		return answer;
	};
	if (!request.is_object()) return refuse("The request is an object.");
	Document *document = session.document_for(path);
	if (!document && !path.empty() && session.project_open()) {
		session.handle(make_request(EditorRequestKind::OpenDocument, path));
		document = session.document_for(path);
	}
	if (!document) return refuse(path.empty() ? "No document is open." : "No document " + path + " could be opened.");
	RecordBatch batch;
	std::string error;
	if (const JsonValue *edits = request.get("edits")) {
		if (!record_batch_from_json(*edits, *document, batch, error)) return refuse(error);
	} else {
		const JsonValue *owner = request.get("id");
		const JsonValue *list = request.get("list");
		const JsonValue *records = request.get("records");
		uint64_t id = 0;
		if (!owner || !whole(*owner, id) || !list || !list->is_string() || !records)
			return refuse("Name the edits, or the record (id), its list (a kind token) and the records that replace it.");
		if (!list_batch_from_json(*document, NodeId(id), list->string, *records, batch, error)) return refuse(error);
	}
	answer.set("ok", JsonValue::make_bool(true));
	if (batch.edits.empty()) {
		// An empty list replaced by nothing: nothing to do, done.
		answer.set("outcome", action_outcome_to_json(ActionOutcome()));
	} else {
		EditorRequest edit = make_request(EditorRequestKind::EditRecord, document->path());
		edit.edits = batch.edits;
		session.handle(edit);
		answer.set("outcome", action_outcome_to_json(session.outcome()));
	}
	JsonValue made = JsonValue::make_object(), added = JsonValue::make_array();
	// A Rescan never runs inside an edit, so the document is still the one the batch changed.
	const std::vector<NodeId> &records = document->last_added_records();
	if (!batch.edits.empty() && session.outcome().done() && !batch.made_labels.empty() &&
	    records.size() == batch.made_labels.size()) {
		for (size_t i = 0; i < records.size(); ++i) {
			added.push(JsonValue::make_number(double(records[i])));
			if (!batch.made_labels[i].empty()) made.set(batch.made_labels[i], JsonValue::make_number(double(records[i])));
		}
	}
	answer.set("made", std::move(made));
	answer.set("added", std::move(added));
	answer.set("revision", JsonValue::make_number(double(document->revision())));
	return answer;
}

} // namespace opennova::editor
