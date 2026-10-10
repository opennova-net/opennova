#include <editor/session/particle_keys_query.h>

#include <string>

#include <editor/assets/asset_kind.h>
#include <editor/documents/particle_keys.h>
#include <editor/model/document_base.h>
#include <editor/model/text_document.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

const char *block_token(particle::BlockKind kind) {
	switch (kind) {
	case particle::BlockKind::Effect: return "effect";
	case particle::BlockKind::Table: return "table";
	case particle::BlockKind::Handles: return "handles";
	default: return "particle";
	}
}

} // namespace

JsonValue answer_particle_keys(const QueryContext &context, const QueryArgs &args, std::string &error) {
	const std::string path = args.text("path");
	const DocumentBase *open = context.core.documents().document_for(path);
	const TextDocument *text = open ? text_of(*open) : nullptr;
	if (!open || !text || open->kind() != AssetKind::Particles) {
		error = open ? open->path() + " is no particle file."
		             : (path.empty() ? std::string("no document is open.") : "no open document " + path + ".");
		return JsonValue::make_null();
	}
	JsonValue out = JsonValue::make_object();
	out.set("path", io::json_string(open->path()));
	bool read = false;
	const std::vector<ParticleKeyBlock> blocks = particle_key_blocks(*text, &read);
	// Whether the reader takes the text (one of comments alone reads, its blocks none).
	out.set("read", JsonValue::make_bool(read));
	JsonValue list = JsonValue::make_array();
	for (const ParticleKeyBlock &block : blocks) {
		JsonValue entry = JsonValue::make_object();
		entry.set("title", io::json_string(particle_block_title(block)));
		entry.set("kind", io::json_string(block_token(block.kind)));
		entry.set("index", io::json_number(double(block.index)));
		entry.set("id", io::json_string(block.id));
		entry.set("first_line", io::json_number(double(block.first_line)));
		entry.set("last_line", io::json_number(double(block.last_line)));
		JsonValue keys = JsonValue::make_array();
		for (const ParticleKeyField &field : block.fields) {
			JsonValue key = JsonValue::make_object();
			key.set("key", io::json_string(field.key));
			key.set("present", JsonValue::make_bool(field.present));
			key.set("value", io::json_string(field.value));
			if (field.present) {
				key.set("line", io::json_number(double(field.span.line)));
				key.set("column", io::json_number(double(field.span.column)));
				key.set("length", io::json_number(double(field.span.length)));
			}
			key.set("read", JsonValue::make_bool(field.row && field.row->read));
			key.set("words", io::json_string(field.row ? particle_key_words(*field.row)
			                                           : std::string("a key the game's reader keeps as unknown")));
			keys.push(std::move(key));
		}
		entry.set("keys", std::move(keys));
		list.push(std::move(entry));
	}
	out.set("blocks", std::move(list));
	return out;
}

} // namespace opennova::editor
