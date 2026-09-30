#include <editor/session/record_batch.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

// What reading one edit knows: its place ("edits[2]"), the document names are found in, the labels
// given so far.
struct Reader {
	const Document *names = nullptr;
	std::map<std::string, Made> labels;
	std::string place;
	std::string error;

	bool refuse(const std::string &why) {
		error = place + ": " + why;
		return false;
	}
	std::string where() const { return names ? names->path() : std::string("the document"); }
};

bool whole(const JsonValue &json, uint64_t &out) {
	if (!json.is_number() || json.number < 0.0 || json.number != std::floor(json.number) ||
			json.number > 9007199254740992.0)
		return false;
	out = static_cast<uint64_t>(json.number);
	return true;
}

bool members_known(const JsonValue &object, std::initializer_list<const char *> known,
		const std::string &place, std::string &error) {
	for (const io::JsonMember &member : object.object) {
		bool found = false;
		for (const char *key : known)
			found = found || member.key == key;
		if (!found) {
			error = "Unknown " + place + " member \"" + member.key + "\".";
			return false;
		}
	}
	return true;
}

// A record named by an identity or a label: its address now (a label's: batch_made of the edit
// that makes it, in the batch's row). False with the reason when it names nothing.
bool record_of(const JsonValue &json, const char *what, Reader &reader, NodeAddress &out) {
	if (json.is_string()) {
		const auto found = reader.labels.find(json.string);
		if (found == reader.labels.end())
			return reader.refuse(std::string("\"") + what + "\" names \"" + json.string +
					"\", which no earlier add or duplicate of the batch gave with \"as\".");
		out = { found->second.row, found->second.kind, batch_made(found->second.edit) };
		return true;
	}
	uint64_t id = 0;
	if (!whole(json, id) || id == 0)
		return reader.refuse(std::string("\"") + what + "\" must be a record identity or a label.");
	if (!reader.names)
		return reader.refuse("record " + std::to_string(id) +
				" is named in the document the request acts on, and none is open.");
	out = reader.names->address_of(NodeId(id));
	if (!out.row)
		return reader.refuse("no record " + std::to_string(id) + " in " + reader.where() + ".");
	return true;
}

// An owner's identity as an edit's `parent` names it: a record by its identity, a record the
// batch makes by batch_made.
NodeId owner_identity(const NodeAddress &owner) {
	return owner.child ? owner.child : owner.row;
}

// A record's identity as the batch form names it: a nested record's, else its row's.
NodeId identity_of(const NodeAddress &address) {
	return address.child ? address.child : address.row;
}

const char *const kOps = "set, clear, write, add, duplicate, remove, move, set_file_value or replace_list";

// {op: replace_list, id, list, records}: the records of `list` that `id` holds removed, then each
// of `records` added at the end with its fields set in the order written.
bool read_list(const JsonValue &json, const NodeAddress &holder, Reader &reader, RecordBatch &out) {
	const JsonValue *list = json.get("list");
	const JsonValue *records = json.get("records");
	if (!list || !list->is_string())
		return reader.refuse("a replace_list names its \"list\", a collection's kind token.");
	if (!records || !records->is_array())
		return reader.refuse("a replace_list names its \"records\", a list of {field: value}.");
	if (is_batch_made(holder.child))
		return reader.refuse("a replace_list names a record the document has, not a label.");
	const Document &document = *reader.names;
	const Document::Collection *collection = nullptr;
	const std::vector<Document::Collection> collections = document.collections_of(holder);
	for (const Document::Collection &candidate : collections)
		if (list->string == document.kind_token(candidate.spec.kind)) collection = &candidate;
	if (!collection)
		return reader.refuse(
				document.record_name(holder) + " holds no \"" + list->string + "\" list.");
	for (const NodeId id : collection->ids) {
		Edit remove;
		remove.operation = EditOperation::Remove;
		remove.address = { holder.row, collection->spec.kind, id };
		out.edits.push_back(remove);
	}
	for (const JsonValue &record : records->array) {
		if (!record.is_object()) return reader.refuse("every record is an object of {field: value}.");
		Edit add;
		add.operation = EditOperation::Add;
		add.address = { holder.row, collection->spec.kind, 0 };
		add.parent = owner_identity(holder);
		const size_t made = out.edits.size();
		out.edits.push_back(add);
		out.made_labels.emplace_back();
		for (const io::JsonMember &member : record.object) {
			Edit set;
			set.address = { holder.row, collection->spec.kind, batch_made(made) };
			set.field = member.key;
			if (!value_from_json(member.value, set.value))
				return reader.refuse(
						"\"" + member.key + "\" must be a number, a string or a bool.");
			out.edits.push_back(std::move(set));
		}
	}
	return true;
}

bool read_edit(const JsonValue &json, Reader &reader, RecordBatch &out) {
	if (!json.is_object()) {
		reader.error = "\"" + reader.place + "\" must be an object.";
		return false;
	}
	if (!members_known(json,
				{ "op", "id", "parent", "kind", "field", "value", "position", "as", "coalesce",
						"gesture", "list", "records" },
				reader.place, reader.error))
		return false;
	const JsonValue *op_json = json.get("op");
	if (!op_json || !op_json->is_string())
		return reader.refuse(std::string("\"op\" names what the edit does: ") + kOps + ".");
	const std::string op = op_json->string;
	const bool replaces_list = op == "replace_list";
	Edit edit;
	if (!replaces_list &&
			(!edit_operation_from_token(op, edit.operation) ||
					edit.operation == EditOperation::Paste))
		return reader.refuse("unknown edit op \"" + op + "\" (" + kOps + ").");
	const bool adds = !replaces_list && edit.operation == EditOperation::Add;
	const bool file_wide = !replaces_list && edit.operation == EditOperation::SetFileValue;
	const bool makes = adds || (!replaces_list && edit.operation == EditOperation::Duplicate);
	const bool sets = !replaces_list && edit.operation == EditOperation::Set;
	const bool fielded = sets || (!replaces_list && (edit.operation == EditOperation::Clear ||
													 edit.operation == EditOperation::Write));
	const JsonValue *id = json.get("id");
	if ((adds || file_wide) && id)
		return reader.refuse(adds ? "an add names its owner with \"parent\", not \"id\"."
								  : "a set_file_value names no record.");
	if (!adds && !file_wide && !id)
		return reader.refuse("edit \"" + op + "\" names its record with \"id\".");
	if (id && !record_of(*id, "id", reader, edit.address)) return false;
	if (!replaces_list && (json.get("list") || json.get("records")))
		return reader.refuse("only a replace_list takes a \"list\" and its \"records\".");
	if (const JsonValue *gesture = json.get("gesture"); gesture && !whole(*gesture, edit.gesture))
		return reader.refuse("\"gesture\" must be a whole number.");
	if (replaces_list) {
		for (const char *member : { "parent", "kind", "field", "value", "position", "as", "coalesce" })
			if (json.get(member))
				return reader.refuse(std::string("a replace_list takes no \"") + member + "\".");
		const size_t first = out.edits.size();
		if (!read_list(json, edit.address, reader, out)) return false;
		for (size_t i = first; i < out.edits.size(); ++i)
			out.edits[i].gesture = edit.gesture;
		return true;
	}
	if (const JsonValue *field = json.get("field")) {
		if (!field->is_string() || field->string.empty())
			return reader.refuse("\"field\" must be a field's id.");
		if (!fielded && !adds && !file_wide)
			return reader.refuse("only set, clear, write, add and set_file_value take a \"field\".");
		edit.field = field->string;
	} else if (fielded || file_wide) {
		return reader.refuse("edit \"" + op + "\" names its \"field\".");
	}
	if (const JsonValue *value = json.get("value")) {
		if (!sets && !file_wide && !(adds && !edit.field.empty()))
			return reader.refuse(
					"only set, set_file_value and an add naming its \"field\" take a \"value\".");
		if (!value_from_json(*value, edit.value))
			return reader.refuse("\"value\" must be a number, a string or a bool.");
	} else if (sets || file_wide || (adds && !edit.field.empty())) {
		return reader.refuse("edit \"" + op + "\" names its \"value\".");
	}
	if (const JsonValue *coalesce = json.get("coalesce")) {
		if (!sets || !coalesce->is_bool())
			return reader.refuse("only a set takes \"coalesce\", true or false.");
		edit.coalesce = coalesce->boolean;
	}
	if (const JsonValue *kind = json.get("kind")) {
		if (!adds || !kind->is_string())
			return reader.refuse("only an add takes a \"kind\" (a record kind's token).");
		if (!reader.names)
			return reader.refuse("record kind \"" + kind->string +
					"\" is named in the document the request acts on, and none is open.");
		const NodeKind token = reader.names->kind_from_name(kind->string);
		if (token < 0)
			return reader.refuse(
					"unknown record kind \"" + kind->string + "\" in " + reader.where() + ".");
		edit.address.kind = token;
	} else if (adds) {
		return reader.refuse("an add names its record \"kind\".");
	}
	if (const JsonValue *parent = json.get("parent")) {
		if (!adds && edit.operation != EditOperation::Move)
			return reader.refuse("only add and move take a \"parent\".");
		NodeAddress owner;
		if (!record_of(*parent, "parent", reader, owner)) return false;
		// A row named as the owner of an add: the record goes straight into that row.
		edit.parent = adds && !owner.child ? 0 : owner_identity(owner);
		if (adds) edit.address.row = owner.row;
	}
	if (const JsonValue *position = json.get("position")) {
		uint64_t at = 0;
		if (!adds && !file_wide && edit.operation != EditOperation::Duplicate &&
				edit.operation != EditOperation::Move)
			return reader.refuse("only add, duplicate, move and set_file_value take a \"position\".");
		if (!whole(*position, at)) return reader.refuse("\"position\" must be a whole number.");
		edit.position = size_t(at);
	} else if (edit.operation == EditOperation::Move) {
		return reader.refuse("a move names its \"position\".");
	} else if (edit.operation == EditOperation::Duplicate && !is_batch_made(edit.address.child)) {
		// Right after the record, as the document stands before the batch.
		Document::Placement at;
		if (reader.names->placement(edit.address, at)) edit.position = at.index + 1;
		for (size_t i = 0; !edit.address.child && i < reader.names->rows().size(); ++i)
			if (reader.names->rows()[i]->id == edit.address.row) edit.position = i + 1;
	}
	std::string label;
	if (const JsonValue *as = json.get("as")) {
		if (!makes || !as->is_string() || as->string.empty())
			return reader.refuse("only an add or a duplicate takes \"as\", a label.");
		if (reader.labels.count(as->string))
			return reader.refuse("the label \"" + as->string + "\" is given twice.");
		label = as->string;
		reader.labels[label] = Made{ out.edits.size(), edit.address.row, edit.address.kind };
	}
	if (makes) out.made_labels.push_back(label);
	out.edits.push_back(std::move(edit));
	return true;
}

// {id, field}: a field whose saved value comes back (revert_to_saved).
bool read_field(const JsonValue &json, Reader &reader, RecordBatch &out) {
	if (!json.is_object()) {
		reader.error = "\"" + reader.place + "\" must be an object.";
		return false;
	}
	if (!members_known(json, { "id", "field" }, reader.place, reader.error)) return false;
	const JsonValue *id = json.get("id");
	const JsonValue *field = json.get("field");
	if (!id) return reader.refuse("it names its record with \"id\".");
	if (!field || !field->is_string() || field->string.empty())
		return reader.refuse("it names its \"field\", a field's id.");
	Edit edit;
	if (!record_of(*id, "id", reader, edit.address)) return false;
	if (is_batch_made(edit.address.child))
		return reader.refuse("\"id\" names a record the document has, not a label.");
	edit.field = field->string;
	out.edits.push_back(std::move(edit));
	return true;
}

} // namespace

bool record_batch_from_json(const io::JsonValue &edits, const Document *names, RecordBatchForm form,
		RecordBatch &out, std::string &error) {
	if (!edits.is_array() || edits.array.empty()) {
		error = "\"edits\" is a list of one edit or more.";
		return false;
	}
	RecordBatch batch;
	Reader reader;
	reader.names = names;
	for (size_t i = 0; i < edits.array.size(); ++i) {
		reader.place = "edits[" + std::to_string(i) + "]";
		const bool read = form == RecordBatchForm::Fields ? read_field(edits.array[i], reader, batch)
														  : read_edit(edits.array[i], reader, batch);
		if (!read) {
			error = reader.error;
			return false;
		}
	}
	out = std::move(batch);
	return true;
}

io::JsonValue record_batch_to_json(
		const std::vector<Edit> &edits, const Document *names, RecordBatchForm form) {
	JsonValue out = JsonValue::make_array();
	// The edits whose records later edits name: each gives its record a label.
	std::set<size_t> named;
	for (const Edit &edit : edits)
		for (const NodeId id : { edit.address.child, edit.parent })
			if (is_batch_made(id)) named.insert(size_t(id - kBatchMadeBase));
	const auto label = [](size_t index) { return "edit" + std::to_string(index); };
	const auto name = [&label](NodeId id) {
		return is_batch_made(id) ? io::json_string(label(size_t(id - kBatchMadeBase)))
								 : io::json_number(double(id));
	};
	for (size_t i = 0; i < edits.size(); ++i) {
		const Edit &edit = edits[i];
		JsonValue entry = JsonValue::make_object();
		if (form == RecordBatchForm::Fields) {
			entry.set("id", name(identity_of(edit.address)));
			entry.set("field", io::json_string(edit.field));
			out.push(std::move(entry));
			continue;
		}
		entry.set("op", io::json_string(edit_operation_token(edit.operation)));
		switch (edit.operation) {
			case EditOperation::Add:
			case EditOperation::Paste:
				entry.set("kind",
						io::json_string(names ? names->kind_token(edit.address.kind) : ""));
				// Into a record, or straight into a row (its identity names it), else a new row.
				if (edit.parent)
					entry.set("parent", name(edit.parent));
				else if (edit.address.row)
					entry.set("parent", name(edit.address.row));
				break;
			case EditOperation::SetFileValue:
				break;
			default:
				entry.set("id", name(identity_of(edit.address)));
				if (edit.operation == EditOperation::Move && edit.parent)
					entry.set("parent", name(edit.parent));
				break;
		}
		const bool fielded = edit.operation == EditOperation::Set ||
				edit.operation == EditOperation::Clear || edit.operation == EditOperation::Write ||
				edit.operation == EditOperation::SetFileValue;
		if (fielded || (edit.operation == EditOperation::Add && !edit.field.empty()))
			entry.set("field", io::json_string(edit.field));
		if (edit.operation == EditOperation::Set || edit.operation == EditOperation::SetFileValue ||
				(edit.operation == EditOperation::Add && !edit.field.empty()))
			entry.set("value", value_to_json(edit.value));
		const bool placed = edit.operation == EditOperation::Add ||
				edit.operation == EditOperation::Duplicate ||
				edit.operation == EditOperation::Move ||
				edit.operation == EditOperation::SetFileValue;
		if (placed && edit.position != SIZE_MAX)
			entry.set("position", io::json_number(double(edit.position)));
		if (edit.operation == EditOperation::Set && edit.coalesce)
			entry.set("coalesce", JsonValue::make_bool(true));
		if (edit.gesture) entry.set("gesture", io::json_number(double(edit.gesture)));
		if (named.count(i)) entry.set("as", io::json_string(label(i)));
		out.push(std::move(entry));
	}
	return out;
}

} // namespace opennova::editor
