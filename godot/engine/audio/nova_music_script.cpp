// MUS interactive-music script wrapper. The underlying parser is libs/mus
// (mus_open_memory). Witnessed: Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20.

#include "nova_music_script.h"

#include "nova_sbf_bank.h"

#include "mus/ast.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdlib>
#include <cstring>
#include <vector>

using namespace godot;

NovaMusicScript::NovaMusicScript() {
	std::memset(&_mf, 0, sizeof(_mf));
}

NovaMusicScript::~NovaMusicScript() {
	if (_opened) {
		mus_close(&_mf);
		_opened = false;
	}
}

void NovaMusicScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_script_count"), &NovaMusicScript::get_script_count);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaMusicScript::get_source_path);
	ClassDB::bind_method(D_METHOD("get_default_script_name"), &NovaMusicScript::get_default_script_name);
	ClassDB::bind_method(D_METHOD("get_scripts"), &NovaMusicScript::get_scripts);
	ClassDB::bind_method(D_METHOD("has_script", "name"), &NovaMusicScript::has_script);
	ClassDB::bind_method(D_METHOD("get_script_names"), &NovaMusicScript::get_script_names);
	ClassDB::bind_method(D_METHOD("get_section_names", "script_name"), &NovaMusicScript::get_section_names);
	ClassDB::bind_method(D_METHOD("get_locals_frame_offset", "script_name"), &NovaMusicScript::get_locals_frame_offset);
	ClassDB::bind_method(D_METHOD("get_intrinsic_names"), &NovaMusicScript::get_intrinsic_names);
	ClassDB::bind_method(D_METHOD("get_decompiled_text", "script_name"), &NovaMusicScript::get_decompiled_text);
	ClassDB::bind_method(D_METHOD("get_decompiled_text_with_bank", "script_name", "bank"), &NovaMusicScript::get_decompiled_text_with_bank);
	ClassDB::bind_method(D_METHOD("get_section_model", "script_name"), &NovaMusicScript::get_section_model);
	ClassDB::bind_method(D_METHOD("get_program_ast", "script_name"), &NovaMusicScript::get_program_ast);
	ClassDB::bind_method(D_METHOD("get_annotated_decompile", "script_name"), &NovaMusicScript::get_annotated_decompile);
	ClassDB::bind_method(D_METHOD("compile_text", "text"), &NovaMusicScript::compile_text);
	ClassDB::bind_method(D_METHOD("set_compiled_bytecode", "bytecode"), &NovaMusicScript::set_compiled_bytecode);
	ClassDB::bind_method(D_METHOD("set_compiled_file_bytes", "file_bytes"), &NovaMusicScript::set_compiled_file_bytes);
	ClassDB::bind_method(D_METHOD("load_from_decrypted_bytes", "bytes", "source"),
			&NovaMusicScript::load_from_decrypted_bytes);
	ClassDB::bind_method(D_METHOD("get_raw_file_bytes"), &NovaMusicScript::get_raw_file_bytes);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_path",
								   PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT),
			"", "get_source_path");
}

void NovaMusicScript::load_from_decrypted_bytes(const PackedByteArray &bytes, const String &p_source) {
	if (_opened) {
		mus_close(&_mf);
		_opened = false;
	}
	source_path = p_source;
	_file_bytes = PackedByteArray();

	if (bytes.size() <= 0) {
		UtilityFunctions::push_warning("NovaMusicScript: empty bytes for ", p_source);
		return;
	}
	if (mus_open_memory(&_mf, bytes.ptr(), (size_t)bytes.size()) != 0) {
		UtilityFunctions::push_warning("NovaMusicScript: mus_open_memory failed for ", p_source);
		return;
	}
	_opened = true;
	_file_bytes = bytes;
}

int NovaMusicScript::get_script_count() const {
	return _opened ? (int)_mf.header.chunk_count : 0;
}

String NovaMusicScript::get_default_script_name() const {
	if (!_opened || _mf.header.chunk_count == 0 || _mf.scripts == nullptr) {
		return String();
	}
	// MusScript.name is null-padded to MUS_NAME_SIZE.
	char buf[MUS_NAME_SIZE + 1] = { 0 };
	std::memcpy(buf, _mf.scripts[0].name, MUS_NAME_SIZE);
	return String(buf);
}

Array NovaMusicScript::get_scripts() const {
	Array out;
	if (!_opened) {
		return out;
	}
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		const MusScript &s = _mf.scripts[i];
		char name_buf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(name_buf, s.name, MUS_NAME_SIZE);

		Array section_names;
		for (uint32_t j = 0; j < s.section_count; ++j) {
			char sec_buf[MUS_SECTION_NAME_SIZE + 1] = { 0 };
			std::memcpy(sec_buf, s.sections[j].name, MUS_SECTION_NAME_SIZE);
			section_names.append(String(sec_buf));
		}

		Dictionary d;
		d["name"] = String(name_buf);
		d["code_size"] = (int64_t)s.code_size;
		d["section_count"] = (int64_t)s.section_count;
		d["sections"] = section_names;
		out.append(d);
	}
	return out;
}

bool NovaMusicScript::has_script(const StringName &p_name) const {
	if (!_opened) {
		return false;
	}
	String want = String(p_name);
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		char buf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(buf, _mf.scripts[i].name, MUS_NAME_SIZE);
		if (want == String(buf)) {
			return true;
		}
	}
	return false;
}

PackedStringArray NovaMusicScript::get_script_names() const {
	PackedStringArray out;
	if (!_opened) {
		return out;
	}
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		char buf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(buf, _mf.scripts[i].name, MUS_NAME_SIZE);
		out.push_back(String(buf));
	}
	return out;
}

PackedStringArray NovaMusicScript::get_section_names(const StringName &p_script_name) const {
	PackedStringArray out;
	if (!_opened) {
		return out;
	}
	String want = String(p_script_name);
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		char nbuf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(nbuf, _mf.scripts[i].name, MUS_NAME_SIZE);
		if (want != String(nbuf)) {
			continue;
		}
		const MusScript &s = _mf.scripts[i];
		for (uint32_t j = 0; j < s.section_count; ++j) {
			char sec_buf[MUS_SECTION_NAME_SIZE + 1] = { 0 };
			std::memcpy(sec_buf, s.sections[j].name, MUS_SECTION_NAME_SIZE);
			out.push_back(String(sec_buf));
		}
		break;
	}
	return out;
}

// Byte offset where the `enter` (0x38) frame op banks the caller's arguments
// in the locals area: l_<base + 4k> is the state's (k+1)-th input. Stock files
// use 0x20 [orig: AudioVM_Op_Enter @0x672C20 reads instance[+0x3C]]; the editor
// uses this to render those slots as "Input N".
int NovaMusicScript::get_locals_frame_offset(const StringName &p_script_name) const {
	const MusScript *s = raw_script(String(p_script_name));
	if (s == nullptr || s->locals_frame_offset == 0) {
		return 0x20;
	}
	return (int)s->locals_frame_offset;
}

PackedStringArray NovaMusicScript::get_intrinsic_names() const {
	PackedStringArray out;
	if (!_opened) {
		return out;
	}
	for (uint32_t i = 0; i < _mf.intrinsic_count && i < (uint32_t)MUS_INTRINSIC_NAMES; ++i) {
		char buf[MUS_INTRINSIC_NAME_SIZE + 1] = { 0 };
		std::memcpy(buf, _mf.intrinsic_names[i], MUS_INTRINSIC_NAME_SIZE);
		out.push_back(String(buf));
	}
	return out;
}

const MusScript *NovaMusicScript::raw_script(const String &p_name) const {
	if (!_opened) {
		return nullptr;
	}
	for (uint32_t i = 0; i < _mf.header.chunk_count; ++i) {
		char buf[MUS_NAME_SIZE + 1] = { 0 };
		std::memcpy(buf, _mf.scripts[i].name, MUS_NAME_SIZE);
		if (p_name == String(buf)) {
			return &_mf.scripts[i];
		}
	}
	return nullptr;
}

Dictionary NovaMusicScript::compile_text(const String &p_text) {
	Dictionary out;
	out["rc"] = -1;
	out["bytecode"] = PackedByteArray();
	out["file_bytes"] = PackedByteArray();
	out["err_line"] = 0;
	out["err_col"] = 0;
	out["err_msg"] = String();

	MusScript script = {};
	int err_line = 0;
	int err_col = 0;
	const char *err_msg = nullptr;
	CharString utf8 = p_text.utf8();
	int rc = mus_compile(utf8.get_data(), &script, &err_line, &err_col, &err_msg);
	if (rc != 0) {
		out["rc"] = rc;
		out["err_line"] = err_line;
		out["err_col"] = err_col;
		out["err_msg"] = String(err_msg ? err_msg : "compile error");
		return out;
	}

	// Keep the legacy raw-code field for narrow tests, but also encode the full
	// SCR0/MU01 file so editor runs can replace script name, section table,
	// debug names, locals frame offset, and bytecode together.
	PackedByteArray bytecode;
	if (script.code != nullptr && script.code_size > 0) {
		bytecode.resize((int)script.code_size);
		std::memcpy(bytecode.ptrw(), script.code, script.code_size);
	}

	PackedByteArray file_bytes;
	const MusScript *scripts[1] = { &script };
	uint8_t *encoded = nullptr;
	size_t encoded_size = 0;
	rc = mus_encode_file(scripts, 1, &encoded, &encoded_size);
	if (rc != 0 || encoded == nullptr) {
		mus_script_free(&script);
		out["rc"] = rc != 0 ? rc : -1;
		out["err_msg"] = String("encode failed");
		return out;
	}
	file_bytes.resize((int)encoded_size);
	std::memcpy(file_bytes.ptrw(), encoded, encoded_size);
	mus_free(encoded);

	mus_script_free(&script);

	out["rc"] = 0;
	out["bytecode"] = bytecode;
	out["file_bytes"] = file_bytes;
	return out;
}

void NovaMusicScript::set_compiled_bytecode(const PackedByteArray &p_bytecode) {
	// Replaces the default script's chunk bytecode in place, then rewrites
	// _file_bytes so the saver's raw-passthrough path picks up the new bytes
	// without needing a separate re-encode hook.
	if (!_opened || _mf.scripts == nullptr || _mf.header.chunk_count == 0) {
		return;
	}
	MusScript &s = _mf.scripts[0];
	if (s.code != nullptr) {
		std::free(s.code);
		s.code = nullptr;
		s.code_size = 0;
	}
	int n = p_bytecode.size();
	if (n > 0) {
		s.code = (uint8_t *)std::malloc((size_t)n);
		if (s.code == nullptr) {
			return;
		}
		std::memcpy(s.code, p_bytecode.ptr(), (size_t)n);
		s.code_size = (uint32_t)n;
	}

	// Re-emit the SCR0/MU01 wrapper from the live MusFile so the saver's raw
	// passthrough writes the rewritten bytes. We collect chunk_count script
	// pointers (typically 1 for jo_gamemus / bhd_menumus), feed mus_encode_file
	// with the canonical 11 intrinsic-method names baked in by the encoder.
	uint32_t cc = _mf.header.chunk_count;
	std::vector<const MusScript *> ptrs(cc, nullptr);
	for (uint32_t i = 0; i < cc; ++i) {
		ptrs[i] = &_mf.scripts[i];
	}
	uint8_t *out_buf = nullptr;
	size_t out_size = 0;
	if (mus_encode_file(ptrs.data(), cc, &out_buf, &out_size) != 0 || out_buf == nullptr) {
		UtilityFunctions::push_warning("NovaMusicScript: mus_encode_file failed in set_compiled_bytecode");
		return;
	}
	PackedByteArray fresh;
	fresh.resize((int)out_size);
	std::memcpy(fresh.ptrw(), out_buf, out_size);
	mus_free(out_buf);
	_file_bytes = fresh;
}

void NovaMusicScript::set_compiled_file_bytes(const PackedByteArray &p_file_bytes) {
	if (p_file_bytes.size() <= 0) {
		return;
	}
	String keep_source = source_path;
	load_from_decrypted_bytes(p_file_bytes, keep_source);
}

String NovaMusicScript::get_decompiled_text(const StringName &p_script_name) {
	if (!_opened) {
		return String();
	}
	const MusScript *s = raw_script(String(p_script_name));
	if (s == nullptr) {
		return String();
	}
	// Two-pass decompile: query required size, then write into a sized buffer.
	int needed = mus_decompile(s, nullptr, 0);
	if (needed < 0) {
		UtilityFunctions::push_warning("NovaMusicScript: mus_decompile size query failed");
		return String();
	}
	std::vector<char> buf((size_t)needed + 1, 0);
	int written = mus_decompile(s, buf.data(), buf.size());
	if (written < 0) {
		UtilityFunctions::push_warning("NovaMusicScript: mus_decompile write failed");
		return String();
	}
	return String::utf8(buf.data(), written);
}

String NovaMusicScript::get_decompiled_text_with_bank(const StringName &p_script_name,
		const Ref<NovaSbfBank> &p_bank) {
	if (!_opened) {
		return String();
	}
	const MusScript *s = raw_script(String(p_script_name));
	if (s == nullptr) {
		return String();
	}
	// When the bank is missing, route through the names-less path so callers
	// don't need a separate code branch.
	if (p_bank.is_null()) {
		return get_decompiled_text(p_script_name);
	}
	// Walk the bank's entries into a parallel CharString + const char* array
	// so libs/mus can read the names without owning the storage. The
	// CharStrings keep the utf8 bytes alive for the duration of this call.
	int entry_count = p_bank->get_entry_count();
	std::vector<CharString> name_storage;
	name_storage.reserve((size_t)entry_count);
	std::vector<const char *> name_ptrs;
	name_ptrs.reserve((size_t)entry_count);
	for (int i = 0; i < entry_count; ++i) {
		String n = p_bank->get_entry_name(i);
		name_storage.push_back(n.utf8());
		name_ptrs.push_back(name_storage.back().get_data());
	}

	int needed = mus_decompile_with_names(s, name_ptrs.data(),
			(uint32_t)name_ptrs.size(), nullptr, 0);
	if (needed < 0) {
		UtilityFunctions::push_warning(
				"NovaMusicScript: mus_decompile_with_names size query failed");
		return String();
	}
	std::vector<char> buf((size_t)needed + 1, 0);
	int written = mus_decompile_with_names(s, name_ptrs.data(),
			(uint32_t)name_ptrs.size(), buf.data(), buf.size());
	if (written < 0) {
		UtilityFunctions::push_warning(
				"NovaMusicScript: mus_decompile_with_names write failed");
		return String();
	}
	return String::utf8(buf.data(), written);
}

Array NovaMusicScript::get_section_model(const StringName &p_script_name) const {
	Array out;
	if (!_opened) {
		return out;
	}
	const MusScript *s = raw_script(String(p_script_name));
	if (s == nullptr) {
		return out;
	}
	MusModel model;
	if (mus_build_section_model(s, &model) != 0) {
		return out;
	}
	for (uint32_t i = 0; i < model.section_count; ++i) {
		const MusSectionInfo &si = model.sections[i];
		Dictionary d;
		char sec_buf[MUS_SECTION_NAME_SIZE + 1] = { 0 };
		if (si.section_index < s->section_count) {
			std::memcpy(sec_buf, s->sections[si.section_index].name, MUS_SECTION_NAME_SIZE);
		}
		d["name"] = String(sec_buf);
		d["index"] = (int64_t)si.section_index;
		d["is_entry"] = (bool)si.is_entry;
		d["is_idle_loop"] = (bool)si.is_idle_loop;

		Array edges;
		for (uint32_t e = 0; e < si.edge_count; ++e) {
			Dictionary ed;
			uint32_t to = si.edges[e].to_section_index;
			ed["to"] = (int64_t)to;
			char to_buf[MUS_SECTION_NAME_SIZE + 1] = { 0 };
			if (to < s->section_count) {
				std::memcpy(to_buf, s->sections[to].name, MUS_SECTION_NAME_SIZE);
			}
			ed["to_name"] = String(to_buf);
			ed["kind"] = (int64_t)si.edges[e].kind;
			edges.append(ed);
		}
		d["edges"] = edges;

		Array plays;
		for (uint32_t p = 0; p < si.play_count; ++p) {
			Dictionary pd;
			pd["track"] = (int64_t)si.plays[p].track_index;
			pd["wait"] = (bool)si.plays[p].wait;
			plays.append(pd);
		}
		d["plays"] = plays;

		out.append(d);
	}
	mus_model_free(&model);
	return out;
}

// ---- Structured statement tree (Phase 1, visual-first) ----

static const char *ast_kind_name(int kind) {
	switch (kind) {
		case MUS_AST_PLAY: return "play";
		case MUS_AST_TRANSITION: return "transition";
		case MUS_AST_GOTO: return "goto";
		case MUS_AST_CALL: return "call";
		case MUS_AST_RETURN: return "return";
		case MUS_AST_YIELD: return "yield";
		case MUS_AST_NOP: return "nop";
		case MUS_AST_DONE: return "done";
		case MUS_AST_ASSIGN: return "assign";
		case MUS_AST_INCDEC: return "incdec";
		case MUS_AST_EXPR: return "expr";
		case MUS_AST_IF: return "if";
		case MUS_AST_SWITCH: return "switch";
		case MUS_AST_BRANCH_COMMENT: return "branch_comment";
		case MUS_AST_FRAME_ENTER: return "frame_enter";
		default: return "unknown";
	}
}

static const char *ast_switch_action_name(int inner_op) {
	if (inner_op == 0x3D || inner_op == 0x3E) return "play";
	if (inner_op == 0x30) return "goto";
	return "enter"; // 0x3B and default
}

static Array ast_stmts_to_array(const MusAstProgram *prog, const MusAstStmt *stmts, uint32_t count);

// Structured expression tree -> the editor's MusExpr node-dict shape
// (modtools/music/mus_expr.gd): kinds share the same integer values, so
// MusExpr.serialize(dict) reproduces the canonical flat text byte-for-byte.
static Dictionary ast_expr_to_dict(const MusAstExpr *e) {
	Dictionary d;
	if (e == nullptr) {
		return d;
	}
	d["kind"] = (int64_t)e->kind;
	switch (e->kind) {
		case MUS_EXPR_LITERAL:
			d["value"] = (int64_t)e->value;
			break;
		case MUS_EXPR_VARREF:
			d["form"] = String(e->var_form);
			d["index"] = (int64_t)e->var_index;
			d["name"] = String(e->name);
			break;
		case MUS_EXPR_ME:
			break;
		case MUS_EXPR_BINOP:
			d["op"] = String(e->op);
			d["left"] = ast_expr_to_dict(e->left);
			d["right"] = ast_expr_to_dict(e->right);
			break;
		case MUS_EXPR_UNOP:
			d["op"] = String(e->op);
			d["operand"] = ast_expr_to_dict(e->left);
			break;
		case MUS_EXPR_CALL:
			d["intrinsic"] = String(e->name);
			if (e->left != nullptr) {
				d["arg"] = ast_expr_to_dict(e->left);
			} else {
				d["arg"] = Variant(); // null: no argument
			}
			break;
		case MUS_EXPR_RAW:
			d["text"] = String(e->name);
			break;
		default:
			break;
	}
	return d;
}

// Resolve a section index to its name via the program's section table, or "".
static String ast_section_name(const MusAstProgram *prog, int idx) {
	if (prog != nullptr && idx >= 0 && (uint32_t)idx < prog->section_count) {
		return String(prog->sections[idx].name);
	}
	return String();
}

static Dictionary ast_stmt_to_dict(const MusAstProgram *prog, const MusAstStmt &s) {
	Dictionary d;
	d["kind"] = String(ast_kind_name(s.kind));
	d["code_offset"] = (int64_t)s.code_offset;
	d["byte_size"] = (int64_t)s.byte_size;
	d["text"] = String(s.text ? s.text : "");
	switch (s.kind) {
		case MUS_AST_PLAY:
			d["track"] = (int64_t)s.track_index;
			d["wait"] = (bool)s.wait;
			break;
		case MUS_AST_TRANSITION:
		case MUS_AST_GOTO:
		case MUS_AST_CALL:
			d["target_section"] = (int64_t)s.target_section;
			d["target_name"] = ast_section_name(prog, s.target_section);
			break;
		case MUS_AST_ASSIGN:
			d["var_name"] = String(s.var_name ? s.var_name : "");
			d["var_offset"] = (int64_t)s.var_offset;
			d["is_local"] = (bool)s.is_local;
			d["rhs"] = String(s.rhs_text ? s.rhs_text : "");
			d["has_call"] = (bool)s.has_call;
			d["call_name"] = String(s.call_name ? s.call_name : "");
			if (s.rhs_tree != nullptr) {
				d["rhs_tree"] = ast_expr_to_dict(s.rhs_tree);
			}
			break;
		case MUS_AST_INCDEC:
			d["var_name"] = String(s.var_name ? s.var_name : "");
			d["var_offset"] = (int64_t)s.var_offset;
			d["is_local"] = (bool)s.is_local;
			d["is_inc"] = (bool)s.is_inc;
			break;
		case MUS_AST_EXPR:
			d["expr"] = String(s.expr_text ? s.expr_text : "");
			d["has_call"] = (bool)s.has_call;
			d["call_name"] = String(s.call_name ? s.call_name : "");
			if (s.expr_tree != nullptr) {
				d["expr_tree"] = ast_expr_to_dict(s.expr_tree);
			}
			break;
		case MUS_AST_BRANCH_COMMENT:
			d["expr"] = String(s.expr_text ? s.expr_text : "");
			d["target_section"] = (int64_t)s.target_section;
			if (s.expr_tree != nullptr) {
				d["expr_tree"] = ast_expr_to_dict(s.expr_tree);
			}
			break;
		case MUS_AST_FRAME_ENTER:
			// Frame setup (0x38): read-only annotation. Carry the locals dword
			// count; deliberately NO target_section/target_name (it is not a
			// transition, so the editor must not offer to navigate/edit it).
			d["locals_count"] = (int64_t)s.var_offset;
			break;
		case MUS_AST_IF:
			d["expr"] = String(s.expr_text ? s.expr_text : "");
			if (s.expr_tree != nullptr) {
				d["expr_tree"] = ast_expr_to_dict(s.expr_tree);
			}
			d["then"] = ast_stmts_to_array(prog, s.then_body, s.then_count);
			// else_body non-NULL marks an if/else (the else block exists even when empty).
			d["else_present"] = (bool)(s.else_body != nullptr);
			d["else"] = ast_stmts_to_array(prog, s.else_body, s.else_count);
			break;
		case MUS_AST_SWITCH: {
			d["expr"] = String(s.expr_text ? s.expr_text : "");
			if (s.expr_tree != nullptr) {
				d["expr_tree"] = ast_expr_to_dict(s.expr_tree);
			}
			d["action"] = String(ast_switch_action_name(s.switch_action));
			Array targets;
			for (uint32_t t = 0; t < s.target_count; ++t) {
				Dictionary td;
				td["name"] = String(s.targets[t].name);
				td["section"] = (int64_t)s.targets[t].section_index;
				td["track"] = (int64_t)s.targets[t].track_index;
				targets.append(td);
			}
			d["targets"] = targets;
			break;
		}
		default:
			break;
	}
	return d;
}

static Array ast_stmts_to_array(const MusAstProgram *prog, const MusAstStmt *stmts, uint32_t count) {
	Array out;
	for (uint32_t i = 0; i < count; ++i) {
		out.append(ast_stmt_to_dict(prog, stmts[i]));
	}
	return out;
}

Array NovaMusicScript::get_program_ast(const StringName &p_script_name) const {
	Array out;
	if (!_opened) {
		return out;
	}
	const MusScript *s = raw_script(String(p_script_name));
	if (s == nullptr) {
		return out;
	}
	MusAstProgram *prog = mus_parse_to_ast(s);
	if (prog == nullptr) {
		return out;
	}
	for (uint32_t i = 0; i < prog->section_count; ++i) {
		const MusAstSection &sec = prog->sections[i];
		Dictionary d;
		d["name"] = String(sec.name);
		d["index"] = (int64_t)sec.section_index;
		d["is_entry"] = (bool)sec.is_entry;
		d["code_offset"] = (int64_t)sec.code_offset;
		d["statements"] = ast_stmts_to_array(prog, sec.statements, sec.statement_count);
		out.append(d);
	}
	mus_program_free(prog);
	return out;
}

Dictionary NovaMusicScript::get_annotated_decompile(const StringName &p_script_name) const {
	Dictionary out;
	out["text"] = String();
	out["rows"] = Array();
	if (!_opened) {
		return out;
	}
	const MusScript *s = raw_script(String(p_script_name));
	if (s == nullptr) {
		return out;
	}
	MusAstProgram *prog = mus_parse_to_ast(s);
	if (prog == nullptr) {
		return out;
	}
	char *text = nullptr;
	MusStmtLineSpan *spans = nullptr;
	uint32_t span_count = 0;
	// Names-less spans: the editor's write path operates on the names-less
	// decompile (the proven round-trip), so the line spans must index that text.
	int rc = mus_ast_emit_text_spans(prog, nullptr, 0, &text, &spans, &span_count);
	if (rc == 0) {
		if (text != nullptr) {
			out["text"] = String::utf8(text);
		}
		Array rows;
		for (uint32_t i = 0; i < span_count; ++i) {
			Dictionary r;
			r["section_index"] = (int64_t)spans[i].section_index;
			r["ordinal"] = (int64_t)spans[i].ordinal;
			r["code_offset"] = (int64_t)spans[i].code_offset;
			r["kind"] = (int64_t)spans[i].kind;
			r["line_start"] = (int64_t)spans[i].line_start;
			r["line_end"] = (int64_t)spans[i].line_end;
			rows.append(r);
		}
		out["rows"] = rows;
	}
	mus_free(text);
	mus_free(spans);
	mus_program_free(prog);
	return out;
}
