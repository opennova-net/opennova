#include <editor/session/record_batch.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <base/io/cp1252.h>
#include <editor/model/text_document.h>
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
	// False on a first read before the document opens: identities are read, not looked for.
	bool resolve = true;
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

using F = RecordBatchForm;
constexpr uint8_t kEdits = batch_form_bit(F::Edits), kFields = batch_form_bit(F::Fields),
		kSpans = batch_form_bit(F::Spans);

constexpr BatchOp kOps[] = {
	{ "set", F::Edits,
			"A field of the record id set to value; with coalesce it folds into the set before it on its "
			"field (typing)." },
	{ "clear", F::Edits, "An optional field of the record id left out of the file (its value kept)." },
	{ "write", F::Edits, "An optional field of the record id the file leaves out written again." },
	{ "add", F::Edits,
			"A record of kind added into parent (a record; a row's identity: straight into that row; none: "
			"a new row) at position, the end by default; with field, its value set in the same step; as "
			"labels it for later edits." },
	{ "duplicate", F::Edits,
			"The record id copied, right after it or at position; as labels the copy." },
	{ "remove", F::Edits, "The record id removed." },
	{ "move", F::Edits, "The record id moved into parent at position." },
	{ "set_file_value", F::Edits, "A file-wide field set to value (position: where in a file-wide list)." },
	{ "replace_list", F::Edits,
			"The records of list the record id holds replaced by records, each {field: value}, added at "
			"the end in order." },
	{ "apply", F::Spans,
			"A text document's span replaced (payload text.span): the length characters from line and "
			"column replaced by text, against the text as the edits before it left it." },
};

constexpr BatchMember kMembers[] = {
	{ "op", BatchJson::String, kEdits | kSpans, -1, "", "What the edit does: an op of its form." },
	{ "id", BatchJson::Id, kEdits | kFields, -1, "",
			"The record: its identity, or the label an earlier add or duplicate of the batch gave." },
	{ "parent", BatchJson::Id, kEdits, -1, "",
			"An add's owner (a row's identity: straight into that row; none: a new row), a move's "
			"destination." },
	{ "kind", BatchJson::String, kEdits, -1, "", "An add's record kind token (window, action, sound, items.item, ...)." },
	{ "field", BatchJson::String, kEdits | kFields, -1, "",
			"The field a set, clear, write, set_file_value or add names; revert_to_saved's field." },
	{ "value", BatchJson::Value, kEdits, -1, "",
			"A set's, a set_file_value's and an add's (with its field) value: a number, a string or a bool." },
	{ "position", BatchJson::Integer, kEdits, 0, "",
			"An index in the owner's collection (add, duplicate, move, set_file_value)." },
	{ "as", BatchJson::String, kEdits, -1, "", "The label an add or a duplicate gives what it makes." },
	{ "coalesce", BatchJson::Boolean, kEdits | kSpans, -1, "",
			"A set, or a span, that folds into the one before (typing)." },
	{ "gesture", BatchJson::Integer, kEdits | kSpans, 0, "",
			"Edits that fold into one undo step until end_edit (a drag)." },
	{ "list", BatchJson::String, kEdits, -1, "", "A replace_list's collection kind token." },
	{ "records", BatchJson::Records, kEdits, -1, "", "A replace_list's records, each {field: value}." },
	{ "payload", BatchJson::String, kSpans, -1, kTextSpanToken,
			"An apply's change: text.span, a span of a text document's text replaced." },
	{ "line", BatchJson::Integer, kSpans, 1, "", "An apply's span: its line, from 1." },
	{ "column", BatchJson::Integer, kSpans, 1, "",
			"An apply's span: its column, from 1 (a character a byte of the game's code page)." },
	{ "length", BatchJson::Integer, kSpans, 0, "",
			"An apply's span: how many characters it replaces (a line end counts its own), 0 by default." },
	{ "text", BatchJson::String, kSpans, -1, "",
			"An apply's span: what takes its place, UTF-8 stored in the game's code page (Windows-1252), "
			"\"\" by default." },
};

// A row's op by its token, of any form; null for none.
const BatchOp *op_row(const std::string &token) {
	for (const BatchOp &row : kOps)
		if (token == row.token) return &row;
	return nullptr;
}

// A form's ops as a sentence lists them: "set, clear, ... or replace_list".
std::string ops_of(RecordBatchForm form) {
	std::vector<const char *> tokens;
	for (const BatchOp &row : kOps)
		if (row.form == form) tokens.push_back(row.token);
	std::string out;
	for (size_t i = 0; i < tokens.size(); ++i)
		out += std::string(i == 0 ? "" : i + 1 == tokens.size() ? " or " : ", ") + tokens[i];
	return out;
}

// Every member of an edit one its form reads (the table's), else refused as unknown.
bool members_known(const JsonValue &object, RecordBatchForm form, const std::string &place, std::string &error) {
	for (const io::JsonMember &member : object.object) {
		bool found = false;
		for (const BatchMember &row : kMembers)
			found = found || (member.key == row.name && (row.forms & batch_form_bit(form)));
		if (!found) {
			error = "Unknown " + place + " member \"" + member.key + "\".";
			return false;
		}
	}
	return true;
}

// A record named by an identity or a label: its address now (a label's: batch_made of the edit
// that makes it, a row or a record, in the row the edit named). False with the reason when it
// names nothing.
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
	// Read before the document opens: an identity as it is, looked for once it is open.
	if (!reader.resolve) {
		out = { NodeId(id), 0, 0 };
		return true;
	}
	// No document, or a blank of the type (whose kinds alone name something): no record to find.
	if (!reader.names || reader.names->path().empty())
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
	if (!reader.resolve) {
		// Before the document opens: the records' shapes alone; the list is the open document's.
		for (const JsonValue &record : records->array) {
			if (!record.is_object())
				return reader.refuse("every record is an object of {field: value}.");
			for (const io::JsonMember &member : record.object) {
				Value value;
				if (!value_from_json(member.value, value))
					return reader.refuse(
							"\"" + member.key + "\" must be a number, a string or a bool.");
			}
		}
		return true;
	}
	const Document &document = *reader.names;
	const Document::Collection *collection = nullptr;
	const std::vector<Document::Collection> collections = document.collections_of(holder);
	for (const Document::Collection &candidate : collections)
		if (list->string == document.kind_token(candidate.spec.kind))
			collection = &candidate;
	if (!collection)
		return reader.refuse(
				document.record_name(holder) + " holds no \"" + list->string + "\" list.");
	// A list of several kinds is replaced record by record (each Add names its own kind).
	if (collection->spec.kinds)
		return reader.refuse("the \"" + list->string + "\" list of " + document.record_name(holder) +
		                     " holds several kinds: add and remove its records one by one.");
	for (const NodeId id : collection->ids) {
		Edit remove;
		remove.operation = EditOperation::Remove;
		remove.address = { holder.row, collection->spec.kind, id };
		out.edits.push_back(remove);
	}
	for (const JsonValue &record : records->array) {
		if (!record.is_object())
			return reader.refuse("every record is an object of {field: value}.");
		Edit add;
		add.operation = EditOperation::Add;
		add.address = { holder.row, collection->spec.kind, 0 };
		add.parent = owner_identity(holder);
		const size_t made = out.edits.size();
		out.edits.push_back(add);
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
	// Operations and members the core knows that a batch does not send over records: an apply's
	// change, and any payload, is made in C++ by its document type (Edit::payload, S13 D6), which
	// JSON cannot carry (a text document's span is the Spans form's); a paste pastes the clipboard
	// (the paste request).
	const std::string ops = ops_of(RecordBatchForm::Edits);
	const JsonValue *op_json = json.get("op");
	const std::string op = op_json && op_json->is_string() ? op_json->string : std::string();
	const BatchOp *row = op_row(op);
	if (row && row->form == RecordBatchForm::Spans)
		return reader.refuse("an apply edit carries a change its document type makes in C++: a "
							 "batch cannot send one.");
	if (json.get("payload"))
		return reader.refuse("\"payload\" names a change a document type makes in C++; the "
							 "editor's JSON cannot carry one.");
	if (!members_known(json, RecordBatchForm::Edits, reader.place, reader.error))
		return false;
	if (!op_json || !op_json->is_string())
		return reader.refuse("\"op\" names what the edit does: " + ops + ".");
	if (op == edit_operation_token(EditOperation::Paste))
		return reader.refuse("a batch takes no \"paste\" edit (" + ops +
				"): the paste request pastes the clipboard.");
	if (!row || row->form != RecordBatchForm::Edits)
		return reader.refuse("unknown edit op \"" + op + "\" (" + ops + ").");
	const bool replaces_list = op == "replace_list";
	Edit edit;
	if (!replaces_list && !edit_operation_from_token(op, edit.operation))
		return reader.refuse("unknown edit op \"" + op + "\" (" + ops + ").");
	const bool adds = !replaces_list && edit.operation == EditOperation::Add;
	const bool file_wide = !replaces_list && edit.operation == EditOperation::SetFileValue;
	const bool makes = adds || (!replaces_list && edit.operation == EditOperation::Duplicate);
	const bool sets = !replaces_list && edit.operation == EditOperation::Set;
	const bool fielded = sets ||
			(!replaces_list &&
					(edit.operation == EditOperation::Clear ||
							edit.operation == EditOperation::Write));
	const JsonValue *id = json.get("id");
	if ((adds || file_wide) && id)
		return reader.refuse(adds ? "an add names its owner with \"parent\", not \"id\"."
								  : "a set_file_value names no record.");
	if (!adds && !file_wide && !id)
		return reader.refuse("edit \"" + op + "\" names its record with \"id\".");
	if (id && !record_of(*id, "id", reader, edit.address))
		return false;
	if (!replaces_list && (json.get("list") || json.get("records")))
		return reader.refuse("only a replace_list takes a \"list\" and its \"records\".");
	if (const JsonValue *gesture = json.get("gesture"); gesture && !whole(*gesture, edit.gesture))
		return reader.refuse("\"gesture\" must be a whole number.");
	if (replaces_list) {
		for (const char *member :
				{ "parent", "kind", "field", "value", "position", "as", "coalesce" })
			if (json.get(member))
				return reader.refuse(std::string("a replace_list takes no \"") + member + "\".");
		const size_t first = out.edits.size();
		if (!read_list(json, edit.address, reader, out))
			return false;
		for (size_t i = first; i < out.edits.size(); ++i)
			out.edits[i].gesture = edit.gesture;
		return true;
	}
	if (const JsonValue *field = json.get("field")) {
		if (!field->is_string() || field->string.empty())
			return reader.refuse("\"field\" must be a field's id.");
		if (!fielded && !adds && !file_wide)
			return reader.refuse(
					"only set, clear, write, add and set_file_value take a \"field\".");
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
		if (!record_of(*parent, "parent", reader, owner))
			return false;
		// A row named as the owner of an add: the record goes straight into that row.
		edit.parent = adds && !owner.child ? 0 : owner_identity(owner);
		if (adds)
			edit.address.row = owner.row;
	}
	if (const JsonValue *position = json.get("position")) {
		uint64_t at = 0;
		if (!adds && !file_wide && edit.operation != EditOperation::Duplicate &&
				edit.operation != EditOperation::Move)
			return reader.refuse(
					"only add, duplicate, move and set_file_value take a \"position\".");
		if (!whole(*position, at))
			return reader.refuse("\"position\" must be a whole number.");
		edit.position = size_t(at);
	} else if (edit.operation == EditOperation::Move) {
		return reader.refuse("a move names its \"position\".");
	} // a duplicate naming none: right after its record as the edits before it left it (the core's)
	if (const JsonValue *as = json.get("as")) {
		if (!makes || !as->is_string() || as->string.empty())
			return reader.refuse("only an add or a duplicate takes \"as\", a label.");
		if (reader.labels.count(as->string))
			return reader.refuse("the label \"" + as->string + "\" is given twice.");
		// What the edit makes, in its row: a row's copy is a row of its own (named as a row), as a
		// new row is; a record's copy or a record added into a row is in that row.
		const bool row_copy = edit.operation == EditOperation::Duplicate && !edit.address.child;
		reader.labels[as->string] =
				Made{ out.edits.size(), row_copy ? 0 : edit.address.row, edit.address.kind };
	}
	out.edits.push_back(std::move(edit));
	return true;
}

// {op: apply, payload: "text.span", line, column, length, text, coalesce, gesture}: a text
// document's span replaced (S13 D9).
bool read_span(const JsonValue &json, Reader &reader, RecordBatch &out) {
	if (!json.is_object()) {
		reader.error = "\"" + reader.place + "\" must be an object.";
		return false;
	}
	const JsonValue *op = json.get("op");
	const BatchOp *row = op && op->is_string() ? op_row(op->string) : nullptr;
	if (!row || row->form != RecordBatchForm::Spans)
		return reader.refuse("a text document takes \"" + ops_of(RecordBatchForm::Spans) +
				"\" edits alone: a span of its text replaced (payload \"text.span\").");
	if (!members_known(json, RecordBatchForm::Spans, reader.place, reader.error))
		return false;
	const JsonValue *payload = json.get("payload");
	if (!payload || !payload->is_string() || payload->string != kTextSpanToken)
		return reader.refuse(std::string("a text document's apply names its payload \"") +
				kTextSpanToken + "\".");
	uint64_t line = 0, column = 0, length = 0;
	const JsonValue *line_json = json.get("line");
	const JsonValue *column_json = json.get("column");
	if (!line_json || !whole(*line_json, line) || line == 0)
		return reader.refuse("\"line\" is the span's line, 1 or more.");
	if (!column_json || !whole(*column_json, column) || column == 0)
		return reader.refuse("\"column\" is the span's column, 1 or more.");
	if (const JsonValue *length_json = json.get("length"); length_json && !whole(*length_json, length))
		return reader.refuse("\"length\" is how many characters the span replaces, 0 or more.");
	std::string stored;
	if (const JsonValue *text = json.get("text")) {
		std::u32string unstorable;
		if (!text->is_string())
			return reader.refuse("\"text\" is what takes the span's place, a string.");
		if (!utf8_to_cp1252(text->string, stored, &unstorable))
			return reader.refuse("\"text\" holds a character the game's text encoding (Windows-1252) has "
								 "no byte for.");
	}
	bool coalesce = false;
	if (const JsonValue *value = json.get("coalesce")) {
		if (!value->is_bool()) return reader.refuse("\"coalesce\" is true or false.");
		coalesce = value->boolean;
	}
	uint64_t gesture = 0;
	if (const JsonValue *value = json.get("gesture"); value && !whole(*value, gesture))
		return reader.refuse("\"gesture\" must be a whole number.");
	TextSpan span;
	span.line = size_t(line);
	span.column = size_t(column);
	span.length = size_t(length);
	out.edits.push_back(TextDocument::replace(span, std::move(stored), coalesce, gesture));
	return true;
}

// {id, field}: a field whose saved value comes back (revert_to_saved).
bool read_field(const JsonValue &json, Reader &reader, RecordBatch &out) {
	if (!json.is_object()) {
		reader.error = "\"" + reader.place + "\" must be an object.";
		return false;
	}
	if (!members_known(json, RecordBatchForm::Fields, reader.place, reader.error))
		return false;
	const JsonValue *id = json.get("id");
	const JsonValue *field = json.get("field");
	if (!id)
		return reader.refuse("it names its record with \"id\".");
	if (!field || !field->is_string() || field->string.empty())
		return reader.refuse("it names its \"field\", a field's id.");
	Edit edit;
	if (!record_of(*id, "id", reader, edit.address))
		return false;
	if (is_batch_made(edit.address.child))
		return reader.refuse("\"id\" names a record the document has, not a label.");
	edit.field = field->string;
	out.edits.push_back(std::move(edit));
	return true;
}

} // namespace

BatchRows<BatchOp> batch_ops() {
	return { kOps, std::size(kOps) };
}

BatchRows<BatchMember> batch_members() {
	return { kMembers, std::size(kMembers) };
}

const char *batch_form_token(RecordBatchForm form) {
	switch (form) {
	case RecordBatchForm::Edits: return "edits";
	case RecordBatchForm::Fields: return "fields";
	case RecordBatchForm::Spans: return "spans";
	case RecordBatchForm::kCount: break;
	}
	return "";
}

const char *batch_form_doc(RecordBatchForm form) {
	switch (form) {
	case RecordBatchForm::Edits:
		return "edit_record over a record document: changes of its records, rows and file-wide values, "
		       "a record by its identity or by the label (as) an earlier add or duplicate of the batch "
		       "gave, an add's kind by its token.";
	case RecordBatchForm::Fields:
		return "revert_to_saved: the fields whose saved value comes back, each {id, field}.";
	case RecordBatchForm::Spans:
		return "edit_record over a text document (a script, a music script, credits, a shader, a "
		       "configuration, a text: one open at the path, or one its path's file opens as): its "
		       "spans replaced, each against the text as the ones before left it (editor_query document "
		       "pages its lines).";
	case RecordBatchForm::kCount: break;
	}
	return "";
}

const char *batch_json_token(BatchJson json) {
	switch (json) {
	case BatchJson::String: return "string";
	case BatchJson::Integer: return "integer";
	case BatchJson::Id: return "id";
	case BatchJson::Boolean: return "boolean";
	case BatchJson::Value: return "value";
	case BatchJson::Records: return "records";
	}
	return "string";
}

bool record_batch_from_json(const io::JsonValue &edits, const Document *names, RecordBatchForm form,
		RecordBatch &out, std::string &error, bool resolve) {
	if (!edits.is_array() || edits.array.empty()) {
		error = "\"edits\" is a list of one edit or more.";
		return false;
	}
	RecordBatch batch;
	Reader reader;
	reader.names = names;
	reader.resolve = resolve;
	for (size_t i = 0; i < edits.array.size(); ++i) {
		reader.place = "edits[" + std::to_string(i) + "]";
		const bool read = form == RecordBatchForm::Fields ? read_field(edits.array[i], reader, batch)
				: form == RecordBatchForm::Spans          ? read_span(edits.array[i], reader, batch)
														  : read_edit(edits.array[i], reader, batch);
		if (!read) {
			error = reader.error;
			return false;
		}
	}
	batch.labels.assign(batch.edits.size(), std::string());
	for (const auto &[label, made] : reader.labels)
		batch.labels[made.edit] = label;
	out = std::move(batch);
	return true;
}

io::JsonValue record_batch_to_json(
		const std::vector<Edit> &edits, const Document *names, RecordBatchForm form) {
	JsonValue out = JsonValue::make_array();
	// The edits whose records later edits name: each gives its record a label.
	std::set<size_t> named;
	for (const Edit &edit : edits)
		for (const NodeId id : { edit.address.row, edit.address.child, edit.parent })
			if (is_batch_made(id))
				named.insert(size_t(id - kBatchMadeBase));
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
				entry.set(
						"kind", io::json_string(names ? names->kind_token(edit.address.kind) : ""));
				// Into a record, or straight into a row (its identity names it), else a new row.
				if (edit.parent)
					entry.set("parent", name(edit.parent));
				else if (edit.address.row)
					entry.set("parent", name(edit.address.row));
				break;
			case EditOperation::SetFileValue:
				break;
			case EditOperation::Apply:
				// A text document's span replaced (S13 D9), as the Spans form reads it.
				if (const auto *span = dynamic_cast<const TextSpanEdit *>(edit.payload.get())) {
					entry.set("payload", io::json_string(span->token()));
					entry.set("line", io::json_number(double(span->span.line)));
					entry.set("column", io::json_number(double(span->span.column)));
					entry.set("length", io::json_number(double(span->span.length)));
					entry.set("text", io::json_string(cp1252_to_utf8(span->text)));
					if (edit.coalesce) entry.set("coalesce", JsonValue::make_bool(true));
					if (edit.gesture) entry.set("gesture", io::json_number(double(edit.gesture)));
					out.push(std::move(entry));
					continue;
				}
				// Any other change is made in C++ (Edit::payload, S13 D6): the record it applies to
				// and the payload's token, which the reader refuses.
				if (const NodeId id = identity_of(edit.address))
					entry.set("id", name(id));
				if (edit.payload)
					entry.set("payload", io::json_string(edit.payload->token()));
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
		if (edit.gesture)
			entry.set("gesture", io::json_number(double(edit.gesture)));
		if (named.count(i))
			entry.set("as", io::json_string(label(i)));
		out.push(std::move(entry));
	}
	return out;
}

} // namespace opennova::editor
