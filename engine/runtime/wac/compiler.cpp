#include <runtime/wac/compiler.h>
#include <base/io/crt_ftol.h>
#include <runtime/anim/adm_clip_index.h>
#include <runtime/particle/effect_catalog_names.h>
#include <runtime/audio/oneshot_play.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <formats/wac/bytecode.h>
#include <formats/wac/command.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/entity_registry.h>
#include <runtime/world/infantry.h>
#include <runtime/world/facial_animation.h>
#include <runtime/world/ammo_table.h>

#include <base/io/strutil.h>

namespace opennova::wac {
namespace {

bool ieq(std::string_view a, std::string_view b) { return opennova::strutil::iequals(a, b); }

constexpr uint32_t hash4(char a, char b, char c, char d) {
	return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
			(uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

// A token's hash is its first four bytes padded with ';', big-endian.
constexpr uint32_t kHashUnset = hash4(';', ';', ';', ';');
constexpr uint32_t kHashLParen = hash4('(', ';', ';', ';');
constexpr uint32_t kHashRParen = hash4(')', ';', ';', ';');
constexpr uint32_t kHashAssign = hash4('=', ';', ';', ';');
constexpr uint32_t kHashBang = hash4('!', ';', ';', ';');
constexpr uint32_t kHashNot = hash4('N', 'O', 'T', ';');
constexpr uint32_t kHashIf = hash4('I', 'F', ';', ';');
constexpr uint32_t kHashThen = hash4('T', 'H', 'E', 'N');
constexpr uint32_t kHashElse = hash4('E', 'L', 'S', 'E');
constexpr uint32_t kHashElseIf = hash4('E', 'L', 'S', 'I');
constexpr uint32_t kHashEnd = hash4('E', 'N', 'D', ';');
constexpr uint32_t kHashEnter = hash4('E', 'N', 'T', 'E');
constexpr uint32_t kHashLeave = hash4('L', 'E', 'A', 'V');
constexpr uint32_t kHashDoSeq = hash4('D', 'O', 'S', 'E');
constexpr uint32_t kHashDoRnd = hash4('D', 'O', 'R', 'N');
constexpr uint32_t kHashNext = hash4('N', 'E', 'X', 'T');
constexpr uint32_t kHashGloop = hash4('G', 'L', 'O', 'O');
constexpr uint32_t kHashPloop = hash4('P', 'L', 'O', 'O');
constexpr uint32_t kHashCheat = hash4('C', 'H', 'E', 'A');
constexpr uint32_t kHashVar = hash4('V', 'A', 'R', ';');
constexpr uint32_t kHashRun = hash4('R', 'U', 'N', ';');
constexpr uint32_t kHashOpenBracket = hash4('[', ';', ';', ';');
constexpr uint32_t kHashCloseBracket = hash4(']', ';', ';', ';');

// END, and every word starting ENDD, ENDI, ENDL or ENDP (ENDDO, ENDIF,
// ENDLOOP...). [orig: Script_Compile @0x4F4065 (END;), the switch
// @0x4F461E..0x4F4634 (cases 0/5/8/12 -> @0x4F463B)]
bool is_end_hash(uint32_t h) {
	return h == kHashEnd || h == hash4('E', 'N', 'D', 'D') || h == hash4('E', 'N', 'D', 'I') ||
			h == hash4('E', 'N', 'D', 'L') || h == hash4('E', 'N', 'D', 'P');
}

// [orig: Script_GetOperatorPrecedence @0x4EE540] 18 {hash, level} rows.
uint8_t operator_precedence(uint32_t h) {
	struct Row { uint32_t hash; uint8_t level; };
	static constexpr Row kRows[] = {
		{hash4('A', 'N', 'D', ';'), 1}, {hash4('&', '&', ';', ';'), 1},
		{hash4('O', 'R', ';', ';'), 1}, {hash4('|', '|', ';', ';'), 1},
		{hash4('=', '=', ';', ';'), 2}, {hash4('!', '=', ';', ';'), 2},
		{hash4('~', '=', ';', ';'), 2}, {hash4('<', '>', ';', ';'), 2},
		{hash4('<', ';', ';', ';'), 2}, {hash4('>', ';', ';', ';'), 2},
		{hash4('<', '=', ';', ';'), 2}, {hash4('>', '=', ';', ';'), 2},
		{hash4('+', ';', ';', ';'), 3}, {hash4('-', ';', ';', ';'), 3},
		{hash4('*', ';', ';', ';'), 4}, {hash4('/', ';', ';', ';'), 4},
		{hash4('%', ';', ';', ';'), 4}, {hash4('^', ';', ';', ';'), 5},
	};
	for (const Row &row : kRows)
		if (row.hash == h) return row.level;
	return 0;
}

// The binary operators: the fold opcode each sets pending, and the error a
// second pending operator or a pending NOT reports.
// [orig: Script_Compile @0x4F3B02..0x4F4013, @0x4F538C..0x4F53C2]
struct BinaryOperator { uint32_t hash; uint8_t fold; const char *unexpected; };
constexpr BinaryOperator kBinaryOperators[] = {
	{hash4('!', '=', ';', ';'), 0x18, "Unexpected !="},
	{hash4('%', ';', ';', ';'), 0x15, "Unexpected %"},
	{hash4('*', ';', ';', ';'), 0x13, "Unexpected *"},
	{hash4('&', '&', ';', ';'), 0x0F, "Unexpected &&"},
	{hash4('+', ';', ';', ';'), 0x11, "Unexpected +"},
	{hash4('/', ';', ';', ';'), 0x14, "Unexpected /"},
	{hash4('-', ';', ';', ';'), 0x12, "Unexpected -"},
	{hash4('<', ';', ';', ';'), 0x19, "Unexpected <"},
	{hash4('<', '=', ';', ';'), 0x1B, "Unexpected <="},
	{hash4('<', '>', ';', ';'), 0x18, "Unexpected <>"},
	{hash4('>', ';', ';', ';'), 0x1A, "Unexpected >"},
	{hash4('=', '=', ';', ';'), 0x17, "Unexpected =="},
	{hash4('>', '=', ';', ';'), 0x1C, "Unexpected >="},
	{hash4('A', 'N', 'D', ';'), 0x0F, "Unexpected AND"},
	{hash4('^', ';', ';', ';'), 0x16, "Unexpected ^"},
	{hash4('O', 'R', ';', ';'), 0x10, "Unexpected OR"},
	{hash4('|', '|', ';', ';'), 0x10, "Unexpected ||"},
	{hash4('~', '=', ';', ';'), 0x18, "Unexpected ~="},
};

// The 24 named-value rows @0x82EEF0 (count 0x18 @0x82F130); Player, Item and
// auto share one dword.
struct NamedValue { const char *name; Builtin id; };
constexpr NamedValue kNamedValues[] = {
	{"result", Builtin::Result}, {"ticks", Builtin::Ticks}, {"GameOver", Builtin::GameOver},
	{"WinVar", Builtin::WinVar}, {"LoseVar", Builtin::LoseVar}, {"SquadSSN", Builtin::SquadSSN},
	{"SquadWho", Builtin::SquadWho}, {"night", Builtin::Night}, {"seatbelt", Builtin::Seatbelt},
	{"wind", Builtin::Wind}, {"breathtime", Builtin::Breathtime}, {"fallmps", Builtin::Fallmps},
	{"accuracyspread", Builtin::AccuracySpread}, {"autogain", Builtin::Autogain},
	{"health", Builtin::Health}, {"mana", Builtin::Mana}, {"bluekills", Builtin::Bluekills},
	{"greenkills", Builtin::Greenkills}, {"humans", Builtin::Humans}, {"RND", Builtin::RandomResult},
	{"Player", Builtin::AutoItem}, {"Item", Builtin::AutoItem}, {"auto", Builtin::AutoItem},
	{"CurTOD", Builtin::CurTOD},
};

// The CRT atol the resolver calls: strtol base 10, saturating at 32 bits
// (io::retail_atol). [orig: _atol @0x76AB0A -> strtol @0x76B2D9]
int32_t crt_atol(const char *s) {
	return io::retail_atol(s);
}

// The CRT atof (io::retail_atof): blanks, a sign, digits with an optional
// fraction, then an exponent the CRT also accepts after D or d. No
// hexadecimal, infinity or NaN forms, whatever the process locale.
// [orig: _atof @0x76B6A1 -> __strgtold12_l @0x779FBE, the exponent markers
// @0x77A0D8..0x77A0EE]
double crt_atof(const char *s) {
	return io::retail_atof(s);
}

constexpr size_t kTokenMax = 64;             // [orig: `cmp edi, 40h` @0x4F335A / @0x4F3457]
constexpr int kValuePoolSlots = 0x200;       // [orig: `cmp ecx, 200h` @0x4F313F]
constexpr size_t kStringPoolClamp = 0x100F;  // [orig: `cmp eax, 100Fh` @0x4F2DC5]
constexpr size_t kStringPoolLimit = 0x1000;  // [orig: `cmp eax, 1000h` @0x4F2DF5]
constexpr size_t kOutputBytes = 0x47A0;      // [orig: WacScript_InitAndLoad @0x4F93FC]
constexpr int kEventSpace = 0x400;           // [orig: `cmp eax, 400h` @0x4F4A06]
constexpr uint32_t kDoSpace = 0x400;         // [orig: `cmp ecx, 400h` @0x4F417E]
constexpr size_t kParenFrames = 16;          // [orig: `cmp esi, 10h` @0x4F39DE / @0x4F543E]
constexpr int kDeclarationSpace = 0x100;     // [orig: `cmp dword_C60E14, 100h` @0x4F3812]

// Script_Compile's paren frames, token buffer and operator set are
// neighbouring stack arrays: a frame index past 15 lands in the next array
// over, as it does in retail. One byte block keeps that layout.
// [orig: Script_Compile frame: var_1BC (+0x4894, the 16 frame precedences),
// String1 (+0x48A4, 72 bytes), var_164 (+0x48EC, the 16 frame operators),
// var_154 (+0x48FC, the 16 automatic flags), Str (+0x490C, the operator set
// copied @0x4F320F), dest (+0x4920, zeroed @0x4F3282), buffer (+0x494C)]
constexpr size_t kPrecAt = 0x00;
constexpr size_t kTokenAt = 0x10;
constexpr size_t kOpnegAt = 0x58;
constexpr size_t kAutoAt = 0x68;
constexpr size_t kOpSetAt = 0x78;
constexpr size_t kLocalsSize = 0xB8;
constexpr char kOperatorSet[] = "{}()[]+-*/|&^%<>=!~"; // [orig: @0x7CE2E8, 20 bytes]

// An IF/DO/LOOP nesting level. [orig: Script_Compile's 0x120-byte block
// records @var_49BC: type +0, start line +4, DO id +8, loop jump-back word
// +0xC, jump count +0x10, the jump words +0x14, the pending branch word
// +0x114, the DO section count +0x118, the DO parameter word +0x11C]
struct Block {
	int type = 0; // 1 condition, 2 THEN, 3 ELSE/NEXT, 4 DOSEQ, 5 DORND, 9 LOOP, 0xB ENTER, 0xC LEAVE
	int line = 0;
	uint32_t loop_id = 0;
	int64_t loop_back = -1;
	std::vector<size_t> jumps;
	int64_t pending_branch = -1;
	uint32_t sections = 0;
	int64_t do_param = -1;
};

// A VAR (type 1) or CHEAT (type 0x19) name. [orig: byte_C68218, 24-byte rows]
struct Declaration {
	std::string name;
	uint8_t type = 1;
};

// A resolved operand word and, for a catalog-bound pool value, its symbol.
struct Operand {
	uint32_t ref = 0;
	std::string symbol;
};

class ScriptCompiler {
public:
	explicit ScriptCompiler(const CompileEnv &env) : env_(env) {
		prog_.music_globals = env.music_globals;
		prog_.source_names = env.source_names;
		if (prog_.source_names.empty()) prog_.source_names.emplace_back();
	}

	// [orig: Script_Compile @0x4F31F0] One file appended to the shared buffer.
	void compile_file(uint32_t source, std::string_view text) {
		File f;
		f.source = source;
		f.text.assign(text.begin(), text.end());
		f.end = text.size();
		f.text.push_back('\0');
		std::memcpy(&f.locals[kOpSetAt], kOperatorSet, sizeof(kOperatorSet));
		run(f);
	}

	Program finish() {
		prog_.code.push_back(kProgramTerminator); // [orig: WacScript_InitAndLoad @0x4F95A9]
		prog_.event_count = event_count_;
		prog_.event_depths.resize(size_t(event_count_));
		prog_.loop_count = do_counter_;
		prog_.operands.resize(size_t(pool_high_));
		// Retain the mounted table's existing handles too: a replacement
		// script may consume an FX handle already held in a mission variable.
		if (env_.effects) prog_.effect_names = env_.effects->interned_names();
		if (env_.sounds) prog_.sound_names = env_.sounds->names();
		return std::move(prog_);
	}

private:
	struct File {
		uint32_t source = 0;
		std::string text;
		size_t end = 0;
		size_t cursor = 0;             // var_4A18
		int line = 1;                  // lineNum: CR alone counts
		uint8_t last = 0;              // bl, the last byte the tokenizer read
		size_t token_length = 0;       // paramLen
		uint32_t token_hash = 0;       // edi
		size_t lookahead_cursor = 0;   // var_4A08
		int lookahead_line = 1;        // var_49EC
		uint32_t operator_hash = kHashUnset;
		uint8_t lookahead_prec = 0;    // var_4A2D
		uint8_t pending_op = 0;        // var_4A3E
		uint8_t pending_prec = 0;      // var_4A3D
		uint8_t negate = 0;            // var_4A36: 0x80
		uint8_t push = 0;              // var_4A35: 0x40
		std::array<uint8_t, kLocalsSize> locals{};
		size_t paren_depth = 0;        // esi
		int params_pending = 0;        // var_4A1C
		std::array<int, 4> expected{}; // expectedType[], the last parameter first
		int action = 0;                // actionIndex
		bool in_condition = false;     // var_4A20
		int decl_mode = 0;             // var_4A0C: 1 VAR, 2 CHEAT, 3 IF name
		bool run_pending = false;      // var_49FC
		int64_t gloop_patch = -1;      // var_49F8
		int loop_nesting = 0;          // var_4A10
		std::optional<uint32_t> assign_target; // var_4A04
		bool emitted_since_assign = false;     // var_49F4
		std::vector<Block> blocks = std::vector<Block>(1); // depth 0 is never opened
		size_t depth = 0;              // var_4A28
	};

	const CompileEnv &env_;
	Program prog_;
	std::vector<std::string> event_names_;   // [orig: byte_C61E18, 24-byte rows]
	std::vector<bool> event_named_;          // the row's fired-dword pointer is set
	int event_count_ = 0;                    // [orig: dword_C60E04]
	std::vector<Declaration> declarations_;  // [orig: byte_C68218, count dword_C60E14]
	uint32_t do_counter_ = 0;                // [orig: dword_C60DFC]
	int run_depth_ = 0;                      // [orig: dword_C6EB20]
	int pool_count_ = 1;                     // [orig: dword_C69A1C; slot 0 holds 0]
	int pool_high_ = 1;
	size_t string_length_ = 0;               // [orig: dword_C69A18]
	int text_miss_ = -1;                     // the shared "" a missing key answers

	// ---- output ----

	size_t emit(uint32_t word) {
		prog_.code.push_back(word);
		return prog_.code.size() - 1;
	}
	uint32_t here() const { return static_cast<uint32_t>(prog_.code.size()); }
	void or_word(int64_t at, uint32_t value) {
		if (at >= 0 && size_t(at) < prog_.code.size()) prog_.code[size_t(at)] |= value;
	}
	void emit_operand(const Operand &operand) {
		if (!operand.symbol.empty()) prog_.operand_symbols.push_back({here(), operand.symbol});
		emit(operand.ref);
	}

	// Retail keeps the first error only ("%s (%d) %s" into byte_C6EB30,
	// Script_SetCompileError @0x4EE7C0); every one is kept here, in order.
	void error(const File &f, int line, std::string message, bool catalog_miss = false) {
		prog_.diagnostics.push_back(Diagnostic{line, 0, std::move(message), catalog_miss, f.source});
	}

	// [orig: WacScript_FormatActionParameters @0x4EFC20] "  name (type, type)".
	static std::string action_signature(int index) {
		const CommandDef &def = wac_commands()[index];
		std::string text = "  ";
		text += def.name;
		text += " (";
		for (int i = 0; i < 4; ++i) {
			if (def.params[i] == ParamType::Null) continue;
			if (i) text += ", ";
			text += param_type_name(def.params[i]);
		}
		text += ")";
		return text;
	}

	// ---- the token buffer and the paren frames ----

	static uint8_t at(const File &f, size_t i) {
		return i < f.text.size() ? static_cast<uint8_t>(f.text[i]) : 0;
	}
	static const char *token(const File &f) {
		return reinterpret_cast<const char *>(&f.locals[kTokenAt]);
	}
	static std::string_view token_view(const File &f) {
		return std::string_view(token(f), std::strlen(token(f)));
	}
	// strrchr over the operator-set copy. [orig: @0x4F3370 / @0x4F3448 / @0x4F3513]
	static bool in_operator_set(const File &f, uint8_t c) {
		for (size_t i = kOpSetAt; i < f.locals.size(); ++i) {
			if (f.locals[i] == c) return true;
			if (f.locals[i] == 0) return false;
		}
		return false;
	}
	static uint8_t &frame_prec(File &f, size_t i) { return f.locals[slot(kPrecAt + i)]; }
	static uint8_t &frame_opneg(File &f, size_t i) { return f.locals[slot(kOpnegAt + i)]; }
	static uint8_t &frame_auto(File &f, size_t i) { return f.locals[slot(kAutoAt + i)]; }
	static size_t slot(size_t i) { return i < kLocalsSize ? i : kLocalsSize - 1; }

	// The two-character operators <=, >=, <>, !=, ~=, ==, ||, &&.
	// [orig: Script_Compile @0x4F3397..0x4F3401; the lookahead @0x4F3532..0x4F357A]
	static bool operator_pair(uint8_t first, uint8_t second) {
		if ((first == '<' || first == '>') && second == '=') return true;
		if (first == '<') return second == '>';
		if (first == '!' || first == '~') return second == '=';
		if (first == '=' || first == '|' || first == '&') return second == first;
		return false;
	}

	// [orig: Script_Compile @0x4F3327..0x4F34C9]
	void tokenize(File &f, uint8_t first) {
		uint8_t *t = &f.locals[kTokenAt];
		size_t length = 0;
		uint8_t bl = first;
		if (bl == '"') {
			// The buffer keeps the opening quote and no case change; the
			// string ends at the closing quote (consumed), a control byte
			// (kept for the main loop) or 64 bytes (the next byte is
			// consumed and dropped). [orig: @0x4F3330..0x4F335F]
			while (f.cursor < f.end) {
				t[length] = bl;
				bl = at(f, f.cursor);
				++length;
				if (bl == '"') { ++f.cursor; break; }
				if (bl < ' ') break;
				++f.cursor;
				if (length >= kTokenMax) break;
			}
		} else if (in_operator_set(f, bl) && !(bl == '-' && (f.pending_op != 0 || f.params_pending != 0))) {
			// A '-' with an operator or a parameter pending starts a word.
			// [orig: @0x4F3380..0x4F3395]
			t[0] = bl;
			length = 1;
			const uint8_t next = at(f, f.cursor);
			if (operator_pair(bl, next)) {
				t[1] = next;
				length = 2;
				++f.cursor;
			}
		} else {
			// Every byte above 0x60 folds down by 0x20; a word ends at a
			// blank, ';', ',' or an operator byte, or after 64 bytes (the
			// next byte is consumed and dropped). [orig: @0x4F3412..0x4F345A]
			while (f.cursor <= f.end) {
				if (bl > 0x60) bl = static_cast<uint8_t>(bl - 0x20);
				t[length] = bl;
				bl = at(f, f.cursor);
				++length;
				if (bl <= ' ' || bl == ';' || bl == ',' || in_operator_set(f, bl)) break;
				++f.cursor;
				if (length >= kTokenMax) break;
			}
		}
		f.last = bl;
		f.token_length = length;
		// ';' padding, the first four bytes hashed as signed chars, then the
		// terminator; ELSEIF alone is renamed so ELSE keeps its own hash.
		// [orig: @0x4F3464..0x4F34C9]
		t[length] = ';';
		t[length + 1] = ';';
		t[length + 2] = ';';
		uint32_t h = static_cast<uint32_t>(int32_t(int8_t(t[0])));
		for (size_t i = 1; i < 4; ++i) h = (h << 8) + static_cast<uint32_t>(int32_t(int8_t(t[i])));
		t[length] = 0;
		f.token_hash = ieq(token_view(f), "ELSEIF") ? kHashElseIf : h;
	}

	// The operator after the token, for the auto-paren, drain and '=' tests.
	// [orig: Script_Compile @0x4F34CE..0x4F3596]
	void lookahead(File &f) {
		int line = f.line;
		size_t i = f.cursor;
		uint8_t bl = f.last;
		f.operator_hash = kHashUnset;
		while (i < f.end) {
			bl = at(f, i++);
			if (bl == '\r') { ++line; continue; }
			if (bl <= ' ' || bl == ',') continue;
			break;
		}
		f.last = bl;
		f.lookahead_cursor = i;
		f.lookahead_line = line;
		if (!in_operator_set(f, bl)) return;
		if (bl == '-' && (f.pending_op != 0 || f.params_pending != 0)) return;
		uint8_t second = at(f, f.lookahead_cursor);
		if (operator_pair(bl, second)) ++f.lookahead_cursor;
		else second = ';';
		f.operator_hash = ((uint32_t(bl) << 8) + second) << 16;
		f.operator_hash += 0x3B3B;
	}

	// ---- the per-file loop ----

	void run(File &f) {
		for (;;) {
			// [orig: Script_Compile @0x4F32E0..0x4F3321]
			if (f.cursor >= f.end) {
				finish_file(f);
				return;
			}
			const uint8_t c = at(f, f.cursor++);
			f.last = c;
			if (c == '\r') {
				++f.line;
				continue;
			}
			if (c <= ' ' || c == ',') continue;
			if (c == ';' || (c == '/' && at(f, f.cursor) == '/')) {
				// A comment runs to the next CR, which the loop then counts.
				// [orig: @0x4F54BA..0x4F54D9]
				while (f.cursor < f.end && at(f, f.cursor) != '\r') ++f.cursor;
				continue;
			}
			tokenize(f, c);
			lookahead(f);
			if (!take_by_mode(f)) continue;
			if (!process(f)) return;
		}
	}

	// A pending RUN, the GLOOP operand and the declaration modes take the
	// token before anything else; false when one consumed it.
	// [orig: Script_Compile @0x4F359A..0x4F3964]
	bool take_by_mode(File &f) {
		if (f.run_pending) {
			if (f.depth != 0) {
				error(f, f.line, "Can't run files inside blocks");
			} else if (run_depth_ > 1) {
				error(f, f.line, "A run file can't run more files");
			} else {
				++run_depth_;
				const bool loaded = run_file(run_path(f));
				--run_depth_;
				if (!loaded) error(f, f.line, "Unable to run file");
			}
			f.run_pending = false;
			return false;
		}
		if (f.gloop_patch >= 0) {
			// The resolved address's dword ORs into the GROUP word; the
			// group slot type always resolves. [orig: @0x4F365D..0x4F369D]
			int out_type = 0;
			const std::optional<Operand> group = resolve(f, int(ParamType::Group), out_type);
			or_word(f.gloop_patch, group ? compile_time_dword(group->ref) : 0u);
			f.gloop_patch = -1;
			return false;
		}
		if (f.decl_mode == 1 || f.decl_mode == 2) {
			// A refused name leaves the mode set for the next token.
			// [orig: @0x4F3812..0x4F3964]
			if (int(declarations_.size()) >= kDeclarationSpace) {
				error(f, f.line, "Over Variable Buffersize");
				return false;
			}
			int out_type = 0;
			if (resolve(f, int(ParamType::Null), out_type)) {
				error(f, f.line, "Variable Name already used");
				return false;
			}
			declarations_.push_back({std::string(token_view(f).substr(0, 18)),
					static_cast<uint8_t>(f.decl_mode == 2 ? 0x19 : 1)});
			f.decl_mode = 0;
			return false;
		}
		if (f.decl_mode == 3) {
			// [orig: @0x4F36C0..0x4F380D]
			if (event_count_ == 0) {
				error(f, f.line, "[ifname] without IF");
				return false;
			}
			if (!event_names_[size_t(event_count_ - 1)].empty()) {
				error(f, f.line, "If already named");
				return false;
			}
			int out_type = 0;
			if (resolve(f, int(ParamType::Null), out_type)) {
				error(f, f.line, "IF Name already used");
				return false;
			}
			event_names_[size_t(event_count_ - 1)] = std::string(token_view(f).substr(0, 18));
			event_named_[size_t(event_count_ - 1)] = true;
			f.decl_mode = 0;
			return false;
		}
		return true;
	}

	// [orig: Path_ReplaceOrAppendExtension @0x53C780] Everything from the
	// FIRST '.' becomes ".wac"; without a dot ".wac" is appended.
	static std::string run_path(const File &f) {
		std::string path(token_view(f));
		const size_t dot = path.find('.');
		if (dot != std::string::npos) path.resize(dot);
		return path + ".wac";
	}

	// [orig: Script_LoadAndCompileFile @0x4EE660] A nested compile with its
	// own locals, appending to the same buffer; false only when no file loads.
	bool run_file(const std::string &path) {
		std::string text;
		if (!env_.load_source || !env_.load_source(path, text)) return false;
		const uint32_t source = static_cast<uint32_t>(prog_.source_names.size());
		prog_.source_names.push_back(path);
		compile_file(source, text);
		return true;
	}

	// The keyword arms first unwind the paren stack. A frame whose precedence
	// is below the lookahead's drops without its POP and takes the keyword
	// with it: retail returns to the tokenizer. True then.
	// [orig: Script_Compile's drain loops, e.g. @0x4F4200..0x4F426C; the ENTER
	//  arm's check @0x4F4818..0x4F4823]
	bool drain_abandons(File &f) {
		while (f.paren_depth != 0) {
			--f.paren_depth;
			if (frame_auto(f, f.paren_depth) == 0) error(f, f.line, "Open Paren");
			if (f.lookahead_prec > frame_prec(f, f.paren_depth)) return true;
			emit(0x07000000u + frame_opneg(f, f.paren_depth));
			f.push = f.pending_op = f.pending_prec = f.negate = 0;
		}
		return false;
	}

	void emit_store(const File &f) {
		emit(0x08000000u);
		emit(*f.assign_target);
	}

	// [orig: Script_Compile @0x4F3969..0x4F5439] One token past the modes;
	// false only on the output overflow, which ends the file.
	bool process(File &f) {
		for (;;) {
			// [orig: @0x4F3990 (loc_4F3990)]
			if (prog_.code.size() * 4 > kOutputBytes) {
				error(f, f.line, "Over Compile Buffersize"); // [orig: @0x4F55E1]
				return false;
			}
			f.lookahead_prec = operator_precedence(f.operator_hash);
			// A pending operator facing a higher-precedence lookahead opens an
			// automatic paren and marks the next call to push the accumulator.
			// [orig: @0x4F39B2..0x4F3A4F]
			if (f.params_pending == 0 && f.push == 0 && f.pending_op != 0 &&
					f.lookahead_prec != 0 && f.lookahead_prec > f.pending_prec) {
				if (f.paren_depth >= kParenFrames) error(f, f.line, "Auto Paren nesting too deep");
				frame_opneg(f, f.paren_depth) = static_cast<uint8_t>(f.negate + f.pending_op);
				frame_prec(f, f.paren_depth) = f.pending_prec;
				frame_auto(f, f.paren_depth) = 1;
				++f.paren_depth;
				f.pending_op = f.pending_prec = f.negate = 0;
				f.push = 0x40;
			}
			const uint32_t h = f.token_hash;
			if (h == kHashLParen) {
				open_paren(f);
				return true;
			}
			if (h == kHashRParen) {
				close_paren(f);
				return true;
			}
			if (f.params_pending != 0) {
				// A refused token takes the scratch dword and feeds the next
				// slot. [orig: @0x4F3A71..0x4F3AFD, the scratch @0x4F3AE2, the
				// jump back to loc_4F3990 @0x4F3AED]
				--f.params_pending;
				int out_type = 0;
				const std::optional<Operand> param =
						resolve(f, f.expected[size_t(f.params_pending)], out_type);
				if (!param) {
					error(f, f.line, action_signature(f.action));
					emit(encode_operand(OperandKind::Builtin, static_cast<uint32_t>(Builtin::Scratch)));
					continue;
				}
				emit_operand(*param);
				return true;
			}
			if (h == kHashBang || h == kHashNot) {
				// [orig: @0x4F3B7F..0x4F3BA0, @0x4F3F1A]
				if (f.negate != 0) error(f, f.line, "Unexpected NOT");
				else f.negate = 0x80;
				return true;
			}
			for (const BinaryOperator &op : kBinaryOperators) {
				if (op.hash != h) continue;
				if (f.pending_op != 0 || f.negate != 0) {
					error(f, f.line, op.unexpected);
				} else {
					f.pending_op = op.fold;
					f.pending_prec = operator_precedence(h);
				}
				return true;
			}
			// Any other token first stores a finished assignment.
			// [orig: @0x4F4019..0x4F404B]
			if (f.assign_target && f.emitted_since_assign && f.pending_op == 0) {
				emit_store(f);
				f.assign_target.reset();
			}
			if (!keyword(f, h)) value_or_call(f);
			return true;
		}
	}

	// [orig: Script_Compile @0x4F543E..0x4F54B5]
	void open_paren(File &f) {
		if (f.paren_depth >= kParenFrames) {
			error(f, f.line, "Paren nesting too deep");
			return;
		}
		if (f.params_pending != 0) f.pending_op = f.pending_prec = f.negate = 0;
		const uint8_t opneg = static_cast<uint8_t>(f.negate + f.pending_op);
		frame_opneg(f, f.paren_depth) = opneg;
		frame_prec(f, f.paren_depth) = f.pending_prec;
		frame_auto(f, f.paren_depth) = 0;
		++f.paren_depth;
		if (opneg == 0) return;
		f.pending_op = f.pending_prec = f.negate = 0;
		f.push = 0x40;
	}

	// [orig: Script_Compile @0x4F53C7..0x4F5439] A frame with an operator
	// either pops it into the accumulator or, facing a stronger lookahead,
	// stays as an automatic frame.
	void close_paren(File &f) {
		if (f.paren_depth == 0) {
			error(f, f.line, "Unexpected )");
			return;
		}
		const uint8_t opneg = frame_opneg(f, f.paren_depth - 1);
		--f.paren_depth;
		if (opneg == 0) return;
		if (f.lookahead_prec > frame_prec(f, f.paren_depth)) {
			frame_auto(f, f.paren_depth) = 1;
			++f.paren_depth;
			return;
		}
		emit(0x07000000u + opneg);
		f.push = f.pending_op = f.pending_prec = f.negate = 0;
	}

	Block &block(File &f) {
		if (f.blocks.size() <= f.depth) f.blocks.resize(f.depth + 1);
		return f.blocks[f.depth];
	}

	// IF and ELSEIF open an event: its depth byte, the EVENT word and the
	// condition state. [orig: Script_Compile @0x4F49EA..0x4F4A57 (IF),
	// @0x4F4481..0x4F44EE (ELSEIF)]
	void open_event(File &f, size_t depth) {
		if (event_count_ < kEventSpace) {
			event_names_.resize(size_t(event_count_) + 1);
			event_named_.resize(size_t(event_count_) + 1);
			prog_.event_depths.resize(size_t(event_count_) + 1);
			prog_.event_depths[size_t(event_count_)] = static_cast<uint8_t>(depth);
		}
		emit(0x01000000u + static_cast<uint32_t>(event_count_));
		f.in_condition = true;
		f.pending_op = f.pending_prec = f.negate = f.push = 0;
		if (event_count_ >= kEventSpace) {
			error(f, f.line, "Out of IF space");
			return;
		}
		++event_count_;
		if (event_count_ == kEventSpace) error(f, f.line, "Out of IF space");
	}

	// THEN / ENTER / LEAVE close the condition with their branch word.
	// [orig: Script_Compile @0x4F4FC0..0x4F5032 (THEN), @0x4F4861..0x4F48D3
	// (ENTER), @0x4F4C00..0x4F4C72 (LEAVE)]
	void close_condition(File &f, int type, uint32_t word, const char *without_if, const char *after_not) {
		if (!f.in_condition) {
			error(f, f.line, without_if);
			return;
		}
		if (f.negate != 0) {
			error(f, f.line, after_not);
			f.negate = 0;
		}
		Block &b = block(f);
		b.pending_branch = static_cast<int64_t>(emit(word));
		b.type = type;
		f.in_condition = false;
		b.loop_back = -1;
	}

	// DOSEQ and DORND both emit opcode 4; only the block type differs.
	// [orig: Script_Compile @0x4F4134..0x4F41E4 (DORND, type 5),
	// @0x4F428E..0x4F433D (DOSEQ, type 4)]
	void open_do(File &f, int type, const char *unexpected) {
		if (f.in_condition) {
			error(f, f.line, unexpected);
			return;
		}
		++f.depth;
		Block &b = block(f);
		b.pending_branch = static_cast<int64_t>(emit(0x04000000u));
		b.do_param = static_cast<int64_t>(emit(do_counter_ << 16));
		b.sections = 1;
		b.jumps.clear();
		b.line = f.line;
		b.type = type;
		b.loop_id = do_counter_;
		b.loop_back = -1;
		if (do_counter_ >= kDoSpace) {
			error(f, f.line, "Out of DO space");
			return;
		}
		++do_counter_;
		if (do_counter_ == kDoSpace) error(f, f.line, "Out of DO space");
	}

	// [orig: Script_Compile @0x4F4ACF..0x4F4B80 (GLOOP), @0x4F4D02..0x4F4DAF (PLOOP)]
	void open_loop(File &f, bool gloop) {
		if (f.in_condition) {
			error(f, f.line, gloop ? "Unexpected GLOOP" : "Unexpected PLOOP");
			return;
		}
		if (f.loop_nesting != 0) error(f, f.line, "No LOOP Nesting!");
		const size_t group = emit(gloop ? 0x0A000000u : 0x0A000001u);
		if (gloop) f.gloop_patch = static_cast<int64_t>(group);
		++f.depth;
		Block &b = block(f);
		b.pending_branch = static_cast<int64_t>(emit(0x09000000u));
		b.loop_back = b.pending_branch;
		++f.loop_nesting;
		b.do_param = -1;
		b.sections = 0;
		b.loop_id = 0;
		b.jumps.clear();
		b.line = f.line;
		b.type = 9;
	}

	// The END family closes ONE block: the loop jump-back, the DO section
	// count, then the pending branch and every jump point past the end.
	// [orig: Script_Compile @0x4F46AF..0x4F47E9]
	void close_block(File &f) {
		if (f.in_condition) {
			error(f, f.line, "END inside IF");
			return;
		}
		if (f.depth == 0 || (block(f).pending_branch < 0 && block(f).jumps.empty())) {
			error(f, f.line, "Unexpected END");
			return;
		}
		Block &b = block(f);
		if (b.loop_back >= 0) {
			emit(0x03000000u + static_cast<uint32_t>(b.loop_back));
			b.loop_back = -1;
		}
		if (b.do_param >= 0) {
			or_word(b.do_param, b.sections);
			b.do_param = -1;
			b.sections = 0;
		}
		if (b.pending_branch >= 0) {
			or_word(b.pending_branch, here());
			b.pending_branch = -1;
		}
		for (size_t i = b.jumps.size(); i-- > 0;) or_word(int64_t(b.jumps[i]), here());
		b.jumps.clear();
		if (b.type == 9) --f.loop_nesting;
		--f.depth;
		b.type = 0;
	}

	// ELSE and ELSEIF need a THEN/ENTER/LEAVE block: its branch lands past
	// the JUMP they emit for the END to patch. [orig: Script_Compile
	// @0x4F43CF..0x4F447B (ELSEIF), @0x4F4566..0x4F4619 (ELSE)]
	bool open_alternative(File &f, bool elseif) {
		if (f.in_condition) {
			error(f, f.line, elseif ? "Unexpected ELSEIF" : "Unexpected ELSE");
			return false;
		}
		Block &b = block(f);
		if (b.type != 2 && b.type != 0xB && b.type != 0xC) {
			error(f, f.line, elseif ? "ELSEIF without THEN/ENTER/LEAVE" : "ELSE without THEN/ENTER/LEAVE");
			return false;
		}
		or_word(b.pending_branch, here() + 1);
		b.type = elseif ? 1 : 3;
		b.pending_branch = -1;
		b.jumps.push_back(emit(0x03000000u));
		return true;
	}

	// NEXT starts the following DO section. [orig: Script_Compile @0x4F4E2F..0x4F4EFF]
	void next_section(File &f) {
		if (f.in_condition) {
			error(f, f.line, "Unexpected NEXT");
			return;
		}
		Block &b = block(f);
		if (b.sections == 0) {
			error(f, f.line, "NEXT without DO");
			return;
		}
		or_word(b.pending_branch, here() + 1);
		b.jumps.push_back(emit(0x03000000u));
		b.type = 3;
		b.pending_branch = static_cast<int64_t>(emit(0x06000000u));
		emit(b.loop_id);
		++b.sections;
	}

	// [orig: Script_Compile @0x4F4053..0x4F50E7] True when the token is a keyword.
	bool keyword(File &f, uint32_t h) {
		if (h == kHashCheat) {
			f.decl_mode = 2;
			return true;
		}
		if (h == kHashVar) {
			f.decl_mode = 1;
			return true;
		}
		if (h == kHashRun) {
			f.run_pending = true;
			return true;
		}
		if (h == kHashCloseBracket) return true;
		const bool drains = h == kHashIf || h == kHashThen || h == kHashElse || h == kHashElseIf ||
				is_end_hash(h) || h == kHashEnter || h == kHashLeave || h == kHashDoSeq ||
				h == kHashDoRnd || h == kHashNext || h == kHashGloop || h == kHashPloop ||
				h == kHashOpenBracket;
		if (!drains) return false;
		if (drain_abandons(f)) return true;
		if (h == kHashIf) {
			// The new block's type waits for THEN. [orig: @0x4F498F..0x4F4A57]
			if (f.in_condition) {
				error(f, f.line, "Unexpected IF");
				return true;
			}
			++f.depth;
			Block &b = block(f);
			b.do_param = -1;
			b.sections = 0;
			b.jumps.clear();
			b.pending_branch = -1;
			b.loop_id = 0;
			b.line = f.line;
			open_event(f, f.depth);
		} else if (h == kHashThen) {
			close_condition(f, 2, 0x02000000u, "THEN without IF", "THEN after NOT");
		} else if (h == kHashEnter) {
			close_condition(f, 0xB, 0x0B000000u, "ENTER without IF", "ENTER after NOT");
		} else if (h == kHashLeave) {
			close_condition(f, 0xC, 0x0C000000u, "LEAVE without IF", "LEAVE after NOT");
		} else if (h == kHashElse) {
			open_alternative(f, false);
		} else if (h == kHashElseIf) {
			if (open_alternative(f, true)) open_event(f, f.depth);
		} else if (is_end_hash(h)) {
			close_block(f);
		} else if (h == kHashDoSeq) {
			open_do(f, 4, "Unexpected DOSEQ");
		} else if (h == kHashDoRnd) {
			open_do(f, 5, "Unexpected DORND");
		} else if (h == kHashNext) {
			next_section(f);
		} else if (h == kHashGloop) {
			open_loop(f, true);
		} else if (h == kHashPloop) {
			open_loop(f, false);
		} else {
			// [orig: @0x4F50AF..0x4F50D6]
			if (!f.in_condition) error(f, f.line, "[ifname] without IF");
			else f.decl_mode = 3;
		}
		return true;
	}

	// [orig: Script_Compile @0x4F50EB..0x4F5387] A value becomes `load value`;
	// an lvalue before '=' starts an assignment; any other token must name a
	// command, whose parameters the following tokens fill.
	void value_or_call(File &f) {
		int out_type = 0;
		const std::optional<Operand> value = resolve(f, int(ParamType::Value), out_type);
		const bool assign_next = f.operator_hash == kHashAssign;
		if (value && !assign_next) {
			std::memcpy(&f.locals[kTokenAt], "load", 5); // [orig: @0x4F5124..0x4F5136]
		}
		if (out_type != 0 && value && assign_next) {
			// [orig: @0x4F513D..0x4F524D]
			if (f.paren_depth != 0) {
				if (drain_abandons(f)) return;
			} else if (f.pending_op != 0) {
				error(f, f.line, "Unexpected =");
			}
			if (f.assign_target) {
				if (f.emitted_since_assign) emit_store(f);
				else error(f, f.line, "Variable not set");
			}
			f.cursor = f.lookahead_cursor;
			f.line = f.lookahead_line;
			f.assign_target = value->ref;
			f.emitted_since_assign = false;
			return;
		}
		// [orig: @0x4F5252..0x4F52B4] The command walk, stricmp over the table.
		const int index = wac_command_index(token_view(f));
		if (index < 0) {
			error(f, f.line, "Unknown '" + std::string(token_view(f)) + "'");
			return;
		}
		const CommandDef &def = wac_commands()[index];
		f.params_pending = 0;
		for (int param = 3; param >= 0; --param) {
			if (def.params[param] == ParamType::Null) continue;
			f.expected[size_t(f.params_pending++)] = int(def.params[param]);
		}
		f.action = index;
		prog_.instruction_sources.push_back({here(), f.source, f.line});
		emit((static_cast<uint32_t>(f.negate + f.push + f.pending_op) << 24) + static_cast<uint32_t>(index));
		if (value && !assign_next) {
			emit_operand(*value);
			--f.params_pending;
		}
		f.pending_op = f.pending_prec = f.negate = f.push = 0;
		f.emitted_since_assign = true;
	}

	// [orig: Script_Compile @0x4F54DE..0x4F5734] Missing parameters take the
	// scratch dword; the paren stack drains (an abandoned frame is skipped
	// and the drain goes on); a pending assignment stores; open blocks report
	// "Missing END" and point their branches and jumps at the end, with no
	// loop jump-back and no DO section count.
	void finish_file(File &f) {
		while (f.params_pending != 0) {
			error(f, f.line, action_signature(f.action));
			emit(encode_operand(OperandKind::Builtin, static_cast<uint32_t>(Builtin::Scratch)));
			--f.params_pending;
		}
		while (f.paren_depth != 0) {
			--f.paren_depth;
			if (frame_auto(f, f.paren_depth) == 0) error(f, f.line, "Open Paren");
			if (f.lookahead_prec > frame_prec(f, f.paren_depth)) continue;
			emit(0x07000000u + frame_opneg(f, f.paren_depth));
			f.push = f.pending_op = f.pending_prec = f.negate = 0;
		}
		if (f.assign_target) {
			if (f.emitted_since_assign) emit_store(f);
			else error(f, f.line, "Variable not set");
		}
		if (f.depth != 0) error(f, block(f).line, "Missing END");
		for (; f.depth > 0; --f.depth) {
			Block &b = block(f);
			if (b.pending_branch >= 0) {
				or_word(b.pending_branch, here());
				b.pending_branch = -1;
			}
			for (size_t i = b.jumps.size(); i-- > 0;) or_word(int64_t(b.jumps[i]), here());
			b.jumps.clear();
		}
	}

	// ---- WacScript_ResolveParameter ----

	// [orig: WacScript_ResolveParameter @0x4F3115..0x4F31C4] Pool a value:
	// the probe goes into the next slot, equal values share the first slot
	// holding them, the 512th distinct value reports and reuses slot 511,
	// and a kinded literal in a slot of another type reports "Wrong Parameter".
	uint32_t pool(File &f, int32_t value, int kind, int expected) {
		if (int(prog_.operands.size()) <= pool_count_) prog_.operands.resize(size_t(pool_count_) + 1, 0);
		prog_.operands[size_t(pool_count_)] = value;
		int slot = 0;
		while (prog_.operands[size_t(slot)] != value) ++slot;
		if (slot == pool_count_) ++pool_count_;
		if (pool_count_ >= kValuePoolSlots) {
			error(f, f.line, "Out of num space");
			--pool_count_;
		}
		if (slot + 1 > pool_high_) pool_high_ = slot + 1;
		if (kind != 0 && expected != int(ParamType::Value) && expected != kind)
			error(f, f.line, "Wrong Parameter");
		return static_cast<uint32_t>(slot);
	}

	Operand pooled(File &f, int32_t value, int kind, int expected, std::string symbol = {}) {
		return Operand{encode_operand(OperandKind::Pool, pool(f, value, kind, expected)), std::move(symbol)};
	}

	// [orig: WacScript_ResolveParameter @0x4F2DA3..0x4F2E1C] The Text and
	// Filename slots copy the token, past a leading quote, into the string
	// pool; writes clamp at byte 0x100F, and a pool reaching 0x1000 bytes
	// reports. Copies are never shared.
	Operand pool_string(File &f) {
		const char *s = token(f);
		if (*s == '"') ++s;
		std::string &strings = prog_.string_pool;
		const size_t start = string_length_;
		auto put = [&](char c) {
			if (strings.size() <= string_length_) strings.resize(string_length_ + 1, '\0');
			strings[string_length_] = c;
			if (string_length_ < kStringPoolClamp) ++string_length_;
		};
		for (; *s != '\0'; ++s) put(*s);
		put('\0');
		if (string_length_ >= kStringPoolLimit) error(f, f.line, "Out of string space");
		return Operand{encode_operand(OperandKind::Text, static_cast<uint32_t>(start)), {}};
	}

	// A TextToken key's text: a found key keeps its own entry, and every
	// missing key shares the one "" string the lookup answers with.
	// [orig: MissionText_GetStringByKeyOrGameText @0x51ECD0, the "" @0x51ED2A]
	int32_t text_token(const std::string &key, std::string &symbol) {
		std::optional<std::string> text;
		if (env_.text_token) text = env_.text_token(key);
		if (!text) {
			symbol = "TT:";
			if (text_miss_ < 0) {
				text_miss_ = static_cast<int32_t>(prog_.text_tokens.size());
				prog_.text_tokens.push_back({std::string(), std::string()});
			}
			return text_miss_;
		}
		symbol = "TT:" + key;
		for (size_t i = 0; i < prog_.text_tokens.size(); ++i)
			if (int32_t(i) != text_miss_ && prog_.text_tokens[i].key == key) return int32_t(i);
		prog_.text_tokens.push_back({key, *text});
		return static_cast<int32_t>(prog_.text_tokens.size() - 1);
	}

	// The resolver; nullopt is retail's NULL. `out_type` is 1 for the
	// declared, named-value and M/V/G legs (the assignable ones).
	// [orig: WacScript_ResolveParameter @0x4F2920]
	std::optional<Operand> resolve(File &f, int expected, int &out_type) {
		out_type = 0;
		const char *s = token(f);
		const std::string_view name = token_view(f);
		// The last token byte; before an empty token it is the byte ahead of
		// the buffer. [orig: @0x4F2953]
		const char last = static_cast<char>(f.locals[kTokenAt + f.token_length - 1]);
		// Table 0, the declared names. [orig: @0x4F2960..0x4F2993 -> @0x4F2A39]
		for (size_t i = 0; i < declarations_.size(); ++i) {
			if (!ieq(declarations_[i].name, name)) continue;
			out_type = 1;
			return Operand{encode_operand(OperandKind::MissionVar, static_cast<uint32_t>(256 + i)), {}};
		}
		// Table 1, the IF names: NULL for a Variable slot, the event index
		// for an IfName slot, else the fired dword. [orig: @0x4F2995..0x4F29C2
		// -> @0x4F2A4E..0x4F2A8A]
		for (int i = 0; i < event_count_; ++i) {
			if (!ieq(event_names_[size_t(i)], name)) continue;
			if (expected == int(ParamType::Variable)) return std::nullopt;
			if (expected == int(ParamType::IfName)) return pooled(f, i, int(ParamType::IfName), expected);
			if (!event_named_[size_t(i)]) return std::nullopt;
			return Operand{encode_operand(OperandKind::EventFired, static_cast<uint32_t>(i)), {}};
		}
		// Table 2, the named values. [orig: @0x4F29C4..0x4F29F1 -> @0x4F2A8F]
		for (const NamedValue &row : kNamedValues) {
			if (!ieq(row.name, name)) continue;
			out_type = 1;
			return Operand{encode_operand(OperandKind::Builtin, static_cast<uint32_t>(row.id)), {}};
		}
		const bool digit_next = s[0] != '\0' && s[1] >= '0' && s[1] <= '9';
		if (s[0] == 'M' && digit_next) {
			// A music-context dword, else the scratch dword. [orig: @0x4F29F3..0x4F2A34]
			out_type = 1;
			const int32_t index = crt_atol(s + 1);
			if (!env_.music_globals)
				return Operand{encode_operand(OperandKind::Builtin, static_cast<uint32_t>(Builtin::Scratch)), {}};
			return Operand{encode_operand(OperandKind::MusicVar, static_cast<uint32_t>(index)), {}};
		}
		if (s[0] == 'V' && digit_next) {
			// [orig: @0x4F2AA4..0x4F2B06]
			int32_t index = crt_atol(s + 1);
			if (index >= 256) {
				error(f, f.line, "V# too big");
				index = 255;
			}
			out_type = 1;
			return Operand{encode_operand(OperandKind::MissionVar, static_cast<uint32_t>(index)), {}};
		}
		if (s[0] == 'G' && digit_next) {
			// [orig: @0x4F2B0B..0x4F2B6D]
			int32_t index = crt_atol(s + 1);
			if (index >= 256) {
				error(f, f.line, "G# too big");
				index = 255;
			}
			out_type = 1;
			return Operand{encode_operand(OperandKind::GlobalVar, static_cast<uint32_t>(index)), {}};
		}
		if (expected == int(ParamType::Variable)) return std::nullopt; // [orig: @0x4F2B7B]

		// The prefix legs, each also taken by its slot type; the name is what
		// follows the prefix. [orig: @0x4F2B84..0x4F2CE9]
		auto prefix = [&](const char *p) {
			const size_t n = std::strlen(p);
			return std::strncmp(s, p, n) == 0 ? n : size_t(0);
		};
		if (const size_t p = prefix("G_"); p || expected == int(ParamType::Group)) {
			// [orig: @0x4F30A0..0x4F3104] An unknown group stores 0.
			const std::string_view group_name = name.substr(p);
			int group = env_.registry ? env_.registry->script_group_index(group_name)
					: world::EntityRegistry::default_script_group_index(group_name);
			if (group < 0) {
				error(f, f.line, "Unknown Group");
				group = 0;
			}
			return pooled(f, group, int(ParamType::Group), expected);
		}
		if (const size_t p = prefix("FX_"); p || expected == int(ParamType::Fx)) {
			// [orig: @0x4F305F..0x4F30FC -> CEffectWorld_InternEffectHandle @0x5F7310]
			const std::string effect(name.substr(p));
			const particle::EffectHandle handle = env_.effects ? env_.effects->intern(effect)
					: particle::EffectHandle{};
			if (!handle) {
				error(f, f.line, "Unknown FX", true);
				return pooled(f, 0, int(ParamType::Fx), expected);
			}
			return pooled(f, int32_t(handle.value), int(ParamType::Fx), expected, "FX:" + effect);
		}
		if (const size_t p = prefix("FACE_"); p || expected == int(ParamType::Face)) {
			// [orig: @0x4F3015..0x4F305A -> AnimState_FindByName @0x5800B0]
			int index = world::facial_expression_index(std::string(name.substr(p)));
			if (index < 0) {
				error(f, f.line, "Unknown FACE");
				index = 0;
			}
			return pooled(f, index, int(ParamType::Face), expected);
		}
		if (const size_t p = prefix("SS_"); p || expected == int(ParamType::SoundSet)) {
			// [orig: @0x4F2FDA..0x4F3010 -> SoundBank_FindSetByNameAnyBank @0x5274F0]
			const std::string set(name.substr(p));
			int32_t handle = 0;
			if (env_.sounds) {
				const std::vector<std::string> &names = env_.sounds->names();
				for (size_t i = 0; i < names.size(); ++i) {
					if (ieq(set, names[i])) {
						handle = int32_t(i + 1);
						break;
					}
				}
			}
			if (handle == 0) {
				error(f, f.line, "Unknown SOUNDSET", true);
				return pooled(f, 0, int(ParamType::SoundSet), expected);
			}
			return pooled(f, handle, int(ParamType::SoundSet), expected, "SS:" + set);
		}
		if (const size_t p = prefix("TT_"); p || expected == int(ParamType::TextToken)) {
			// The lookup answers "" for a missing key, so the leg's "Unknown
			// TextTool Token" never fires. [orig: @0x4F2F96..0x4F2FD5 ->
			// MissionText_GetStringByKeyOrGameText @0x51ECD0]
			std::string symbol;
			const int32_t index = text_token(std::string(name.substr(p)), symbol);
			return pooled(f, index, int(ParamType::TextToken), expected, symbol);
		}
		if (const size_t p = prefix("ANIM_"); p || expected == int(ParamType::Anim)) {
			// Every token in an Anim slot, a number too, is looked up as
			// "anim_" + name through the slot lookup (anim::adm_slot_index:
			// the same compare past the key's first five bytes).
			// [orig: @0x4F2EF2..0x4F2F91 -> AnimMap_FindSlotByName @0x40CFA0]
			int index = anim::adm_slot_index("anim_" + std::string(name.substr(p)));
			if (index < 0) {
				error(f, f.line, "Unknown ANIM");
				index = 0;
			}
			return pooled(f, index, int(ParamType::Anim), expected);
		}
		if (const size_t p = prefix("SSN_"); p || expected == int(ParamType::Ssn)) {
			// atol (0 for a name or a quoted token) then the 16-bit net-id
			// lookup; the leg never answers NULL. The port binds the net id
			// when the VM first meets its world, and the embedder's registry
			// answers the "Unknown SSN" question the pooled handle answered.
			// The 10000 player alias is EntityCommands::resolve_ssn's
			// authoring seam, not a retail lookup. [orig: @0x4F2E97..0x4F2EED
			// -> EntityPool_FindByNetId @0x4F0A20, the 16-bit key @0x4F0A3A]
			const int32_t net = crt_atol(s + p) & 0xFFFF;
			if (env_.registry != nullptr && uint16_t(net) != world::EntityCommands::kLocalPlayerSsn &&
					!env_.registry->find_by_net_id(uint16_t(net)).valid())
				error(f, f.line, "Unknown SSN");
			return Operand{encode_operand(OperandKind::EntitySsn, pool(f, net, int(ParamType::Ssn), expected)), {}};
		}
		if (const size_t p = prefix("AMMO_"); p || expected == int(ParamType::Ammo)) {
			// The name, then "ammo_" + name; row 0 is a miss too.
			// [orig: @0x4F2E21..0x4F2E92 -> AmmoDef_LookupByName @0x409870]
			std::string ammo(name.substr(p));
			int index = env_.ammo ? env_.ammo->index_of(ammo.c_str()) : -1;
			if (index <= 0 && env_.ammo) {
				ammo = "ammo_" + ammo;
				index = env_.ammo->index_of(ammo.c_str());
			}
			if (index <= 0) {
				error(f, f.line, "Unknown AMMO", true);
				return pooled(f, 0, int(ParamType::Ammo), expected);
			}
			return pooled(f, index, int(ParamType::Ammo), expected, "AMMO:" + ammo);
		}
		if (expected == int(ParamType::Text) || expected == int(ParamType::Filename)) return pool_string(f);

		// The number leg, a digit, '-' or '.' first. An F suffix scales by
		// 21501, an M suffix or a Distance slot by 65536; else a ':' makes
		// minutes plus hours times 60, and an Hour slot scales by 60.
		// [orig: @0x4F2CEF..0x4F2D9E]
		if (!((s[0] >= '0' && s[0] <= '9') || s[0] == '-' || s[0] == '.')) return std::nullopt;
		double value = crt_atof(s);
		const size_t colon = std::strcspn(s, ":"); // StrCSpnIA
		int kind = 0;
		if (last == 'F') {
			value *= 21501.0;
			kind = int(ParamType::Distance);
		} else if (last == 'M' || expected == int(ParamType::Distance)) {
			value *= 65536.0;
			kind = int(ParamType::Distance);
		} else if (colon != f.token_length) {
			const double minutes = crt_atof(s + colon + 1);
			const double hours = value * 60.0;
			value = minutes + hours;
			kind = int(ParamType::Hour);
		} else if (expected == int(ParamType::Hour)) {
			value *= 60.0;
		}
		// [orig: WacScript_ResolveParameter @0x4F2920 (the _ftol2_sse call @0x4F2D8C)]
		return pooled(f, io::retail_ftol_sse2(value), kind, expected);
	}

	// The dword a resolved address holds during the compile: a pool slot's
	// value, else what the embedder's variable banks, events and engine
	// words hold before the load resets them. [orig: `mov ecx, [eax]`
	// @0x4F368A]
	uint32_t compile_time_dword(uint32_t ref) const {
		if (operand_kind(ref) == OperandKind::Pool)
			return operand_index(ref) < prog_.operands.size()
					? static_cast<uint32_t>(prog_.operands[operand_index(ref)]) : 0u;
		return env_.load_dword ? env_.load_dword(ref) : 0u;
	}
};

} // namespace

Program compile_source(std::string_view source, const CompileEnv &env) {
	ScriptCompiler compiler(env);
	compiler.compile_file(0, source);
	return compiler.finish();
}

Program compile_program(const std::vector<std::string> &sources, const CompileEnv &env) {
	CompileEnv source_env = env;
	source_env.source_names.resize(sources.size());
	ScriptCompiler compiler(source_env);
	for (size_t i = 0; i < sources.size(); ++i) compiler.compile_file(static_cast<uint32_t>(i), sources[i]);
	return compiler.finish();
}

} // namespace opennova::wac
