#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <formats/mus/mus.h>

#include <string>
#include <vector>

using namespace opennova::mus;

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

/* D1 / D2: compiler skeleton + minimal grammar.

   The Phase A revisions to the spec dropped runtime binds: `bind sound_N`
   declarations are an aesthetic emitted by the decompiler so the source text
   reads naturally. The `play sound_N` opcode operand is a 1-byte SBF index,
   not a bind-table key. The compiler accepts (and ignores the encoded
   payload of) bind declarations purely so the round-trip text shape works. */
static int test_compile_minimal_script(void) {
    const char *src =
        "script test\n"
        "section Begin\n"
        "{\n"
        "  play sound_0\n"
        "  done\n"
        "}\n";
    MusScript out = {};
    int err_line = 0, err_col = 0;
    const char *err_msg = NULL;
    int rc = mus_compile(src, &out, &err_line, &err_col, &err_msg);
    CHECK(rc == 0, err_msg ? err_msg : "compile");
    CHECK(strncmp(out.name, "test", 4) == 0, "script name");
    CHECK(out.section_count == 1, "one section");
    CHECK(out.code_size > 0, "non-empty code");
    /* Section table offsets are bytecode-relative (matches mus_parse.cpp). */
    CHECK(out.sections != NULL, "sections allocated");
    mus_script_free(&out);
    return 1;
}

/* mus_encode_file: a compiled MusScript serialises to a valid SCR0 blob
   that mus_open_memory accepts and re-parses with the original name and
   bytecode preserved. The encoder is exercised end-to-end here; the
   roundtrip test (`mus_roundtrip_test.cpp`) covers the in-memory path. */
static int test_encode_file_minimal(void) {
    const char *src =
        "script test\n"
        "section Begin\n"
        "{\n"
        "  play sound_0\n"
        "  done\n"
        "}\n";
    MusScript script = {};
    int err_line = 0, err_col = 0;
    const char *err = NULL;
    int rc = mus_compile(src, &script, &err_line, &err_col, &err);
    CHECK(rc == 0, err ? err : "compile");

    const MusScript *scripts[1] = { &script };
    uint8_t *buf = NULL;
    size_t   bufsize = 0;
    rc = mus_encode_file(scripts, 1, &buf, &bufsize);
    CHECK(rc == 0, "encode");
    CHECK(buf != NULL, "buf written");
    CHECK(bufsize >= 44 + 4 + 72, "buf big enough for header + chunk");

    /* Re-parse the encoded buffer */
    MusFile mf;
    rc = mus_open_memory(&mf, buf, bufsize);
    CHECK(rc == 0, "re-parse");
    CHECK(mf.header.magic == MUS_MAGIC_SCR0, "magic");
    CHECK(mf.header.chunk_count == 1, "1 chunk");
    CHECK(mf.scripts != NULL, "scripts present");
    CHECK(strncmp(mf.scripts[0].name, "test", 4) == 0, "name preserved");
    CHECK(mf.scripts[0].section_count == 1, "section_count preserved");
    CHECK(mf.scripts[0].code_size == script.code_size, "code_size preserved");
    CHECK(mf.scripts[0].code != NULL && script.code != NULL, "code present");
    CHECK(memcmp(mf.scripts[0].code, script.code, script.code_size) == 0,
          "bytecode preserved");
    /* Intrinsic names must round-trip too. */
    CHECK(mf.intrinsic_count == 11, "the method table: MUSIC.LAN's eleven");
    CHECK(strncmp(mf.intrinsic_names[0], "GEcho", 5) == 0, "intrinsic[0] GEcho");
    CHECK(strncmp(mf.intrinsic_names[10], "TStop", 5) == 0, "intrinsic[10] TStop");
    mus_close(&mf);
    mus_free(buf);
    mus_script_free(&script);
    return 1;
}

/* Operand-bounds guards (editor write-path safety). The compiler emits 1-byte
   operands for play targets, switch (tablexec) section/sound indices, and the
   tablexec count + skip_size. An out-of-range value used to silently wrap to a
   different-but-valid byte, so a visual edit could compile yet encode the wrong
   program. These now error, so the editor's compile gate rolls the edit back. */
static int compiles(const char *src) {
    MusScript out = {};
    int el = 0, ec = 0;
    const char *em = NULL;
    int rc = mus_compile(src, &out, &el, &ec, &em);
    mus_script_free(&out);   /* idempotent; safe on the partial/failed result */
    return rc == 0;
}

/* The compiled bytecode of `src`, empty when it does not compile. */
static std::string bytecode(const char *src) {
    MusScript out = {};
    int el = 0, ec = 0;
    const char *em = NULL;
    std::string code;
    if (mus_compile(src, &out, &el, &ec, &em) == 0 && out.code)
        code.assign(reinterpret_cast<const char *>(out.code), out.code_size);
    mus_script_free(&out);
    return code;
}

/* The engine's two play opcodes differ only in the index's width: 0x3E reads a byte, 0x3D a word
   [orig: AudioVM_Op_Play @ 0x672CB0, AudioVM_Op_PlayWait @ 0x672C90]. Retail's compiler writes the
   byte form up to 255 and the word form past it (jox01's MJox01.bin: 823 byte plays, 61 word plays,
   all of 256..316), so a sound past 255 compiles to the word form, never wraps; past a word is no
   index. A play table holding one past a byte takes the word form in every entry (the table runs its
   entry through the opcode table [orig: AudioVM_Op_TableExec @ 0x672BFB]). And the decompiled text of
   a wide play compiles back to the same bytes. */
static int test_play_track_widths(void) {
    const std::string narrow = bytecode("script t\nsection Begin\n{\n  play sound_255\n  done\n}\n");
    CHECK(narrow.find(std::string("\x3E\xFF", 2)) != std::string::npos, "sound_255: the byte form");
    const std::string wide = bytecode("script t\nsection Begin\n{\n  play sound_256\n  done\n}\n");
    CHECK(wide.find(std::string("\x3D\x00\x01", 3)) != std::string::npos, "sound_256: the word form, little-endian");
    CHECK(wide.find('\x3E') == std::string::npos, "sound_256 never wraps to a byte play");
    const std::string jox = bytecode("script t\nsection Begin\n{\n  play sound_316\n  done\n}\n");
    CHECK(jox.find(std::string("\x3D\x3C\x01", 3)) != std::string::npos, "sound_316: the word form");
    CHECK(!compiles("script t\nsection Begin\n{\n  play sound_65536\n  done\n}\n"),
          "play sound_65536 must be rejected, not wrapped");
    /* tablexec 0x35: count 2, inner 0x3D, entry size 3, total 5 + 2*3 = 11, then the entries. */
    const std::string table = bytecode("script t\nsection Begin\n{\n  on (Var00) play sound_1 sound_300\n  done\n}\n");
    CHECK(table.find(std::string("\x35\x02\x3D\x03\x0B\x3D\x01\x00\x3D\x2C\x01", 11)) != std::string::npos,
          "a play table with a sound past 255: word-form entries");
    const std::string small = bytecode("script t\nsection Begin\n{\n  on (Var00) play sound_1 sound_2\n  done\n}\n");
    CHECK(small.find(std::string("\x35\x02\x3E\x02\x09\x3E\x01\x3E\x02", 9)) != std::string::npos,
          "a play table within a byte keeps the byte form");
    /* Compiled, decompiled and compiled again: the same bytecode. */
    MusScript script = {};
    int el = 0, ec = 0;
    const char *em = NULL;
    CHECK(mus_compile("script t\nsection Begin\n{\n  play sound_255\n  play sound_316\n  on (Var00) play sound_2 sound_400\n}\n",
                      &script, &el, &ec, &em) == 0, em ? em : "compile");
    const int needed = mus_decompile(&script, NULL, 0);
    std::string text(size_t(needed > 0 ? needed : 0) + 1, '\0');
    CHECK(needed > 0 && mus_decompile(&script, &text[0], text.size()) == needed, "decompile");
    text.resize(size_t(needed));
    const std::string first(reinterpret_cast<const char *>(script.code), script.code_size);
    mus_script_free(&script);
    const std::string again = bytecode(text.c_str());
    if (again != first) {
        fprintf(stderr, "%s\n", text.c_str());
        for (const std::string *code : {&first, &again}) {
            for (unsigned char c : *code) fprintf(stderr, "%02X ", c);
            fprintf(stderr, "\n");
        }
    }
    CHECK(again == first, "the decompiled text compiles to the same bytecode");
    /* The bound: no number in a sound name wraps (it stops accumulating past a word). */
    const std::string top = bytecode("script t\nsection Begin\n{\n  play sound_65535\n  done\n}\n");
    CHECK(top.find(std::string("\x3D\xFF\xFF", 3)) != std::string::npos, "sound_65535: the widest play");
    CHECK(!compiles("script t\nsection Begin\n{\n  play sound_4294967296\n  done\n}\n"),
          "sound_4294967296 must be refused, not wrapped to sound_0");
    CHECK(!compiles("script t\nsection Begin\n{\n  play sound_99999999999999999999\n  done\n}\n"),
          "a sound number past an int is refused");
    CHECK(!compiles("script t\nsection Begin\n{\n  on (Var00) play sound_1 sound_4294967552\n  done\n}\n"),
          "a play table's sound past a word is refused");
    return 1;
}

/* `src` compiles, decompiles and compiles again to the same bytecode, and its second
   decompile is its first. */
static bool round_trips(const char *src, std::string *text_out = nullptr) {
    MusScript script = {};
    int el = 0, ec = 0;
    const char *em = NULL;
    if (mus_compile(src, &script, &el, &ec, &em) != 0) {
        fprintf(stderr, "  compile: %s (line %d)\n", em ? em : "?", el);
        return false;
    }
    const int needed = mus_decompile(&script, NULL, 0);
    std::string text(size_t(needed > 0 ? needed : 0) + 1, '\0');
    const bool decompiled = needed > 0 && mus_decompile(&script, &text[0], text.size()) == needed;
    text.resize(size_t(needed > 0 ? needed : 0));
    const std::string first(reinterpret_cast<const char *>(script.code), script.code_size);
    mus_script_free(&script);
    if (text_out) *text_out = text;
    if (!decompiled) return false;
    const std::string again = bytecode(text.c_str());
    if (again != first) fprintf(stderr, "  the text compiles to other bytes:\n%s\n", text.c_str());
    return again == first;
}

/* Review M3: a `goto` or a `call` inside an if-body, to a section before or after it, names
   its section in the text (a body prints no section headers, but names them as the top
   level does) and compiles back; a body whose last statement is a `goto` to a later section
   stays an if, not an if/else (the else-goto is only the one the compiler writes past an
   else, which names no section). */
static int test_branches_in_bodies(void) {
    const char *src =
        "script t\n"
        "section A\n{\n"
        "  if ((Var01 == 1))\n  {\n    goto C\n  }\n"
        "  play sound_1\n"
        "}\n"
        "section B\n{\n"
        "  if ((Var01 == 2))\n  {\n    call A\n  }\n"
        "  if ((Var01 == 3))\n  {\n    goto A\n  }\n  else\n  {\n    call C\n  }\n"
        "  if ((Var01 == 4))\n  {\n    play sound_2\n  }\n  else\n  {\n  }\n"
        "}\n"
        "section C\n{\n"
        "  play sound_3\n"
        "}\n";
    std::string text;
    CHECK(round_trips(src, &text), "goto and call inside if-bodies round-trip");
    CHECK(text.find("goto C") != std::string::npos && text.find("call A") != std::string::npos &&
          text.find("goto A") != std::string::npos && text.find("call C") != std::string::npos,
          "every body branch names its section");
    CHECK(text.find('@') == std::string::npos, "no unresolved @XXXX target");
    return 1;
}

/* Review M2/L8: deep nesting is bounded on both sides: 64 nested ifs compile and decompile,
   65 are refused by the compiler. */
static int test_nesting_bound(void) {
    const auto nested = [](int depth) {
        std::string src = "script t\nsection Begin\n{\n";
        for (int i = 0; i < depth; ++i) src += "if ((Var01 == 1))\n{\n";
        src += "play sound_1\n";
        for (int i = 0; i < depth; ++i) src += "}\n";
        src += "}\n";
        return src;
    };
    CHECK(round_trips(nested(64).c_str()), "64 nested ifs round-trip");
    CHECK(!compiles(nested(65).c_str()), "65 nested ifs are refused");
    std::string deep = "script t\nsection Begin\n{\nVar01 = ";
    for (int i = 0; i < 300; ++i) deep += "~ ";
    deep += "1\n}\n";
    CHECK(!compiles(deep.c_str()), "an expression nested past the bound is refused");
    return 1;
}

/* A one-byte global or local operand past 255 is refused, never wrapped [orig:
   AudioVM_Op_PushGlobal @ 0x6727B0, AudioVM_Op_PushLocal @ 0x6727D0 read one byte]. */
static int test_variable_operand_bounds(void) {
    CHECK(compiles("script t\nsection Begin\n{\n  Var63 = 1\n}\n"), "Var63 (offset 252) compiles");
    CHECK(!compiles("script t\nsection Begin\n{\n  Var64 = 1\n}\n"), "Var64 (offset 256) is refused");
    CHECK(!compiles("script t\nsection Begin\n{\n  g_300++\n}\n"), "g_300 is refused");
    CHECK(!compiles("script t\nsection Begin\n{\n  l_256 = 1\n}\n"), "l_256 is refused");
    CHECK(!compiles("script t\nsection Begin\n{\n  Var99999999999999999999 = 1\n}\n"), "a huge Var number is refused");
    CHECK(compiles("script t\nsection Begin\n{\n  l_255 = (l_0 + 1)\n}\n"), "l_255 compiles");
    return 1;
}

/* A program as raw bytecode, one section at 0, decompiled: the result's sign and text. */
static int decompile_bytes(const std::vector<uint8_t> &code, std::string &text) {
    MusSection section = {};
    strcpy(section.name, "Begin");
    MusScript script = {};
    strcpy(script.name, "t");
    script.code = const_cast<uint8_t *>(code.data());
    script.code_size = (uint32_t)code.size();
    script.sections = &section;
    script.section_count = 1;
    script.globals_size = 64;
    const int needed = mus_decompile(&script, NULL, 0);
    text.clear();
    if (needed <= 0) return needed;
    text.assign((size_t)needed + 1, '\0');
    const int written = mus_decompile(&script, &text[0], text.size());
    text.resize((size_t)needed);
    return written;
}

/* Review M2/F1: the decompiler never loops and fails on malformed bytecode. A brfalse to its
   own offset or before it (at the top level or inside a body) prints as its comment; a branch
   past the end or into an instruction, an instruction cut short, and ifs nested past the bound
   do not decompile (the editor reports the script unreadable). */
static int test_decompiler_robustness(void) {
    std::string text;
    /* push 1; brfalse 0; done */
    CHECK(decompile_bytes({0x01, 0x01, 0x31, 0x00, 0x00, 0x00, 0x00, 0x3F}, text) > 0 &&
          text.find("// if !(1) goto Begin") != std::string::npos, "a backward brfalse is its comment");
    /* push 1; brfalse 15 { push 1; brfalse 0; done }; done: the backward one inside a body */
    CHECK(decompile_bytes({0x01, 0x01, 0x31, 0x0F, 0x00, 0x00, 0x00, 0x01, 0x01, 0x31, 0x00, 0x00, 0x00, 0x00,
                           0x3F, 0x3F}, text) > 0 &&
          text.find("// if !(1) goto Begin") != std::string::npos, "a backward brfalse inside a body");
    CHECK(decompile_bytes({0x01, 0x01, 0x31, 0x02, 0x00, 0x00, 0x00, 0x3F}, text) > 0, "a brfalse to itself");
    CHECK(decompile_bytes({0x01, 0x01, 0x31, 0xFF, 0x00, 0x00, 0x00, 0x3F}, text) < 0, "a branch past the end");
    CHECK(decompile_bytes({0x01, 0x01, 0x31, 0x01, 0x00, 0x00, 0x00, 0x3F}, text) < 0, "a branch into an instruction");
    CHECK(decompile_bytes({0x30, 0x01, 0x00, 0x00, 0x00, 0x3F}, text) < 0, "a goto into an instruction");
    CHECK(decompile_bytes({0x01, 0x01, 0x31, 0x05, 0x00}, text) < 0, "an instruction cut short");
    CHECK(decompile_bytes({0x01, 0x00, 0x35, 0x02, 0x3B, 0x02, 0x09, 0x3B, 0x00}, text) < 0, "a table cut short");
    /* ifs nested `depth` deep, every brfalse to the final done */
    const auto nest = [](int depth) {
        std::vector<uint8_t> code;
        const uint32_t end = (uint32_t)depth * 7 + 2;
        for (int i = 0; i < depth; ++i) {
            const uint8_t bytes[] = {0x01, 0x01, 0x31, (uint8_t)(end & 0xFF), (uint8_t)(end >> 8), 0x00, 0x00};
            code.insert(code.end(), bytes, bytes + 7);
        }
        code.push_back(0x3E);
        code.push_back(0x01);
        code.push_back(0x3F);
        return code;
    };
    CHECK(decompile_bytes(nest(64), text) > 0, "64 nested ifs decompile");
    CHECK(decompile_bytes(nest(65), text) < 0, "65 nested ifs do not");
    return 1;
}

/* Review L10/F7: a table's entries read by their own opcode, as the VM dispatches them, the
   header's +1 byte only the action word: a byte play and a word play in one stride-3 table,
   and a null entry (opcode 0: the VM skips the table [orig: AudioVM_Op_TableExec
   @ 0x672BE5..0x672BFA]), which compiles back to zeros. */
static int test_table_entries_by_opcode(void) {
    std::string text;
    /* push 0; tablexec count 2, header 0x3E, stride 3, skip 11: [3E 05 00] [3D 2C 01]; done */
    CHECK(decompile_bytes({0x01, 0x00, 0x35, 0x02, 0x3E, 0x03, 0x0B, 0x3E, 0x05, 0x00, 0x3D, 0x2C, 0x01, 0x3F}, text) > 0,
          "a mixed table decompiles");
    CHECK(text.find("on (0) play sound_5 sound_300") != std::string::npos, "each entry by its own opcode");
    /* push 0; tablexec count 2, setstate, stride 2, skip 9: [3B 00] [00 00]; done */
    CHECK(decompile_bytes({0x01, 0x00, 0x35, 0x02, 0x3B, 0x02, 0x09, 0x3B, 0x00, 0x00, 0x00, 0x3F}, text) > 0 &&
          text.find("on (0) enter Begin null") != std::string::npos, "a null entry prints as null");
    const std::string null_table = bytecode("script t\nsection Begin\n{\n  on (Var00) enter Begin null\n}\n");
    CHECK(null_table.find(std::string("\x35\x02\x3B\x02\x09\x3B\x00\x00\x00", 9)) != std::string::npos,
          "null compiles to a zero entry");
    CHECK(round_trips("script t\nsection Begin\n{\n  on (Var00) enter Begin null\n}\n"), "a null entry round-trips");
    return 1;
}

static int test_reject_switch_over_64_targets(void) {
    char src[4096];
    int n = snprintf(src, sizeof(src), "script t\nsection Begin\n{\n  on (Var00) enter");
    for (int i = 0; i < 65; ++i)
        n += snprintf(src + n, sizeof(src) - (size_t)n, " Begin");
    snprintf(src + n, sizeof(src) - (size_t)n, "\n  done\n}\n");
    CHECK(!compiles(src), "65-target on(...) table must be rejected, not truncated");
    return 1;
}

static int test_reject_goto_table_too_large(void) {
    /* goto entries are 5 bytes each, so 51 targets make total_size = 5 + 51*5 =
       260 > 255 -- the skip_size byte would wrap. Count (51) is under 64, so this
       specifically exercises the total_size guard, not the target-count guard. */
    char src[4096];
    int n = snprintf(src, sizeof(src), "script t\nsection Begin\n{\n  on (Var00) goto");
    for (int i = 0; i < 51; ++i)
        n += snprintf(src + n, sizeof(src) - (size_t)n, " Begin");
    snprintf(src + n, sizeof(src) - (size_t)n, "\n  done\n}\n");
    CHECK(!compiles(src), "oversized goto table must be rejected, not wrapped");
    return 1;
}

/* The exact template the editor mints for "New music program from scratch":
   a script with a single empty section. An empty body `{ }` compiles to one
   implicit `done`, and that section is the entry (index 0). This locks the
   from-scratch path so a future grammar change can't silently break New. The
   GDScript side seeds this same text (music_editor_document.new_script). */
static int test_compile_minimal_single_section(void) {
    const char *src =
        "script gamescript\n"
        "section Begin\n"
        "{\n"
        "}\n";
    MusScript out = {};
    int err_line = 0, err_col = 0;
    const char *err_msg = NULL;
    int rc = mus_compile(src, &out, &err_line, &err_col, &err_msg);
    CHECK(rc == 0, err_msg ? err_msg : "empty single-section template must compile");
    CHECK(out.section_count == 1, "one section");
    CHECK(out.code_size > 0, "empty body emits a single done byte");
    CHECK(out.entry_section_index == 0, "the only section is the entry");

    /* It must also encode to a file the loader re-opens (the New write path is
       compile -> encode -> set_compiled_file_bytes -> save). */
    const MusScript *scripts[1] = { &out };
    uint8_t *buf = NULL;
    size_t bufsize = 0;
    rc = mus_encode_file(scripts, 1, &buf, &bufsize);
    CHECK(rc == 0 && buf != NULL, "encode succeeds");
    MusFile mf;
    rc = mus_open_memory(&mf, buf, bufsize);
    CHECK(rc == 0, "re-parse");
    CHECK(mf.header.chunk_count == 1, "1 chunk");
    CHECK(mf.scripts[0].section_count == 1, "1 section preserved");
    CHECK(mf.scripts[0].entry_section_index == 0, "entry index preserved");
    mus_close(&mf);
    mus_free(buf);
    mus_script_free(&out);
    return 1;
}

/* What the text refuses, each where MDEdit's debug table or the game would not read it as written: a function's name
   or a parameter's `Function::name` past the table's 31 characters (cut, it would match no parameter), a function or
   a parameter twice, a function and a section of one name, an `enter` to a section no block defines (it would sit at
   the code's leading nop), a function of no parameter and a `frame 0` (D-MUS-16). An error's line is the text's
   own, whatever `#line` says. */
static int test_function_rules(void) {
    int line = 0;
    const auto refused = [&line](const std::string &body) {
        MusScript out = {};
        int err_col = 0;
        const char *err = NULL;
        const std::string src = "script test\n" + body;
        const int rc = mus_compile(src.c_str(), &out, &line, &err_col, &err);
        mus_script_free(&out);
        return rc != 0;
    };
    const std::string begin = "section Begin\n{\n  done\n}\n";
    CHECK(!refused(begin + "handler H(a)\n{\n  a = 5\n}\n"), "a function of a parameter compiles");
    CHECK(refused(begin + "handler " + std::string(32, 'F') + "(a)\n{\n}\n"), "a function's name past 31 characters");
    CHECK(!refused(begin + "handler " + std::string(28, 'F') + "(a)\n{\n}\n"), "Function::a in 31 characters");
    CHECK(refused(begin + "handler AVeryLongFunctionName(parameterName)\n{\n}\n"), "a Function::name past 31");
    CHECK(refused(begin + "handler H(a)\n{\n}\nhandler H(b)\n{\n}\n"), "a function twice");
    CHECK(refused(begin + "handler H(a, a)\n{\n}\n"), "a parameter twice");
    CHECK(refused(begin + "handler Begin(a)\n{\n}\n"), "a function named like a section");
    CHECK(refused("handler H(a)\n{\n}\nsection H\n{\n  done\n}\n"), "a section named like a function");
    CHECK(refused("section Begin\n{\n  enter Nowhere\n}\n"), "an enter to a section defined nowhere");
    CHECK(refused("section Begin\n{\n  on (Var01) enter Begin Nowhere\n}\n"), "a table's enter to one");
    CHECK(refused("declsection Nowhere\nsection Begin\n{\n  done\n}\n"), "a declsection of a section defined nowhere");
    CHECK(refused(begin + "handler H()\n{\n}\n"), "a function of no parameter");
    CHECK(refused("section Begin\n{\n  frame 0\n  done\n}\n"), "a frame setup of none");
    CHECK(!refused("section Begin\n{\n  frame 2\n  done\n}\n"), "a frame setup of two");
    CHECK(refused("section Begin\n{\n#line 500\n  play nowhere\n}\n") && line == 5, "an error at its own line");
    // A section's name past 31 characters, refused wherever it is named, never cut to one another name could share.
    const std::string longest(31, 'S'), past(32, 'S');
    CHECK(!refused("section " + longest + "\n{\n  goto " + longest + "\n}\n"), "a section's name of 31 characters");
    CHECK(refused("section " + past + "\n{\n  done\n}\n"), "a section's name past 31 characters");
    CHECK(refused("section Begin\n{\n  enter " + past + "\n}\n"), "an enter to a name past 31");
    CHECK(refused("section Begin\n{\n  goto " + past + "\n}\n"), "a goto to a name past 31");
    CHECK(refused("section Begin\n{\n  call " + past + "\n}\n"), "a call to a name past 31");
    CHECK(refused("section Begin\n{\n  on (Var01) enter Begin " + past + "\n}\n"), "a table's enter to a name past 31");
    CHECK(refused("declsection " + past + "\nsection Begin\n{\n  done\n}\n"), "a declsection of a name past 31");
    return 1;
}

/* The decompiler holds as many functions and parameters as the script has: a handler of 17 parameters and nine
   handlers decompile and compile back to the same bytes; a parameter its debug table names nothing is argN. */
static int test_function_limits(void) {
    std::string params;
    for (int i = 0; i < 17; ++i) params += (i ? ", p" : "p") + std::to_string(i);
    std::string src = "script test\nsection Begin\n{\n  play sound_0\n}\nhandler Wide(" + params + ")\n{\n  p16 = 1\n}\n";
    for (int i = 0; i < 9; ++i) src += "handler H" + std::to_string(i) + "(a)\n{\n  a = " + std::to_string(i) + "\n}\n";
    const auto bytes_of = [](const char *text, std::vector<uint8_t> &out, MusScript *keep) {
        MusScript script = {};
        int line = 0, col = 0;
        const char *err = NULL;
        if (mus_compile(text, &script, &line, &col, &err) != 0) {
            fprintf(stderr, "  compile: %s (line %d)\n", err ? err : "?", line);
            return false;
        }
        const MusScript *one[] = {&script};
        uint8_t *buf = NULL;
        size_t size = 0;
        const bool ok = mus_encode_file(one, 1, &buf, &size) == 0;
        if (ok) out.assign(buf, buf + size);
        mus_free(buf);
        if (keep) *keep = script;
        else mus_script_free(&script);
        return ok;
    };
    std::vector<uint8_t> first, again;
    MusScript script = {};
    CHECK(bytes_of(src.c_str(), first, &script), "the wide script compiles");
    CHECK(script.function_count == 10 && script.local_count == 26, "ten functions, 26 parameters");
    const int needed = mus_decompile(&script, NULL, 0);
    std::string text((size_t)(needed > 0 ? needed : 1), '\0');
    CHECK(needed > 0 && mus_decompile(&script, &text[0], text.size()) == needed, "decompile");
    text.resize(strlen(text.c_str()));
    CHECK(bytes_of(text.c_str(), again, NULL) && again == first, "its text compiles back to its bytes");
    /* A parameter of no debug name is argN. */
    script.locals[0].name[strlen("Wide::")] = 0;
    const int renamed = mus_decompile(&script, NULL, 0);
    std::string unnamed((size_t)(renamed > 0 ? renamed : 1), '\0');
    CHECK(renamed > 0 && mus_decompile(&script, &unnamed[0], unnamed.size()) == renamed &&
              unnamed.find("handler Wide(arg1, p1,") != std::string::npos,
          "an empty parameter name is argN");
    mus_script_free(&script);
    return 1;
}

/* Where the text names each play's sound (mus_compile_plays): a statement's `sound_N`, a bound name, a quoted
   bound name (its place without the quotes) and an `on (...) play` table's entries (its `null` none), each at
   its place in the text with the stream index it compiles to, in the text's order; none for a text that does
   not compile. The bank a script plays from is its name made .SBF. */
static int test_play_places(void) {
    const std::string src =
        "script test\n"
        "bind sound_3 \"Win Sting\"\n"
        "bind sound_4 \"Lose\"\n"
        "section Begin\n"
        "{\n"
        "  play sound_2\n"
        "  play Lose\n"
        "  play \"Win Sting\"\n"
        "  on (Var01) play sound_7 null sound_300\n"
        "  done\n"
        "}\n";
    MusScript out = {};
    std::vector<MusPlayUse> plays;
    int err_line = 0, err_col = 0;
    const char *err_msg = NULL;
    CHECK(mus_compile_plays(src.c_str(), &out, &plays, &err_line, &err_col, &err_msg) == 0,
          err_msg ? err_msg : "compile");
    mus_script_free(&out);
    CHECK(plays.size() == 5, "five plays named");
    const char *names[5] = {"sound_2", "Lose", "Win Sting", "sound_7", "sound_300"};
    const uint32_t indices[5] = {2, 4, 3, 7, 300};
    for (int i = 0; i < 5; ++i) {
        CHECK(src.substr(plays[i].offset, plays[i].length) == names[i], "each play at its name's place");
        CHECK(plays[i].index == indices[i], "each play's stream index");
    }
    CHECK(plays[0].offset < plays[1].offset && plays[1].offset < plays[2].offset, "in the text's order");
    const std::string bad = "script test\nsection Begin\n{\n  play sound_1\n  play nowhere\n}\n";
    CHECK(mus_compile_plays(bad.c_str(), &out, &plays, &err_line, &err_col, &err_msg) != 0 && plays.empty(),
          "a text that does not compile names none");
    CHECK(mus_bank_name("music\\GJox01.bin") == "GJOX01.SBF" && mus_bank_name("gamemus.bin") == "GAMEMUS.SBF" &&
              mus_bank_name("MENUMUS") == "MENUMUS.SBF",
          "the bank is the script's name made .SBF");
    return 1;
}

/* The method table `names` (malloc'd, the script's from then on) in place of the script's own. */
static void set_methods(MusScript &script, const std::vector<std::string> &names) {
    free(script.intrinsic_names);
    script.intrinsic_names =
        names.empty() ? NULL : (char (*)[MUS_METHOD_NAME_SIZE])calloc(names.size(), MUS_METHOD_NAME_SIZE);
    for (size_t i = 0; i < names.size(); ++i)
        memcpy(script.intrinsic_names[i], names[i].data(),
               names[i].size() < (size_t)MUS_METHOD_NAME_SIZE ? names[i].size() : (size_t)MUS_METHOD_NAME_SIZE);
    script.intrinsic_count = (uint32_t)names.size();
}

/* `scripts` written and read back: 0 and the parsed file, or the encoder's refusal. */
static int encode_and_open(const MusScript *const *scripts, uint32_t count, MusFile &out, std::vector<uint8_t> &bytes) {
    uint8_t *buf = NULL;
    size_t size = 0;
    const int rc = mus_encode_file(scripts, count, &buf, &size);
    bytes.assign(buf, buf + (buf ? size : 0));
    mus_free(buf);
    if (rc != 0) return rc;
    return mus_open_memory(&out, bytes.data(), bytes.size()) == 0 ? 0 : 1;
}

/* The writer writes the model's own fields: the method table the script holds, in its order, whatever its names
   and count (the loader takes any [orig: AudioVM_LoadScriptFile @ 0x672DBD..0x672E01]), the globals' size and the
   `enter` frame base as they are, 0 included (the game allocates and adds them as the chunk holds them [orig:
   ScriptInstance_Init @ 0x672F78; AudioVM_Op_Enter @ 0x672C39]); each reads back as written and writes again to
   the same bytes. It refuses a name not ended within its slot (its length byte past 127, which the loader reads
   signed [orig: @ 0x672DEB]) and two programs of different tables (the file holds one [orig: ScriptInstance_Init
   @ 0x672FAF]). */
static int test_encode_model_fields(void) {
    const char *src = "script test\nsection Begin\n{\n  play sound_0\n  done\n}\n";
    MusScript script = {};
    int err_line = 0, err_col = 0;
    const char *err_msg = NULL;
    CHECK(mus_compile(src, &script, &err_line, &err_col, &err_msg) == 0, "compile");
    const std::string longest(125, 'L');
    set_methods(script, {"FSet", "Mine", "GEcho", longest, ""});
    script.globals_size = 0;
    script.locals_frame_offset = 0;
    const MusScript *one[1] = {&script};
    MusFile mf = {};
    std::vector<uint8_t> bytes;
    CHECK(encode_and_open(one, 1, mf, bytes) == 0, "the model writes and reads back");
    CHECK(mf.intrinsic_count == 5 && mf.scripts[0].intrinsic_count == 5, "five names, the file's and the script's");
    CHECK(strcmp(mf.intrinsic_names[0], "FSet") == 0 && strcmp(mf.intrinsic_names[1], "Mine") == 0 &&
          strcmp(mf.intrinsic_names[2], "GEcho") == 0 && mf.intrinsic_names[3] == longest &&
          mf.intrinsic_names[4][0] == 0, "in the model's order, each as it is");
    CHECK(mf.scripts[0].globals_size == 0, "the globals' size as the model holds it, 0");
    CHECK(mf.scripts[0].locals_frame_offset == 0, "the frame base as the model holds it, 0");
    {
        const MusScript *again[1] = {&mf.scripts[0]};
        MusFile mf2 = {};
        std::vector<uint8_t> bytes2;
        const bool same = encode_and_open(again, 1, mf2, bytes2) == 0 && bytes2 == bytes;
        mus_close(&mf2);
        CHECK(same, "and writes again to the same bytes");
    }
    mus_close(&mf);

    /* No table: no name, none read. */
    set_methods(script, {});
    mf = {};
    CHECK(encode_and_open(one, 1, mf, bytes) == 0 && mf.intrinsic_count == 0 && mf.intrinsic_names == NULL &&
          mf.scripts[0].intrinsic_count == 0, "a script of no method name writes none");
    mus_close(&mf);

    /* A slot without its NUL: 126 characters. */
    set_methods(script, {std::string(126, 'L')});
    CHECK(encode_and_open(one, 1, mf, bytes) == -5, "a name past 125 characters is refused");

    /* Two programs: one table, or nothing. */
    MusScript other = {};
    CHECK(mus_compile(src, &other, &err_line, &err_col, &err_msg) == 0, "compile the second");
    set_methods(script, {"GEcho", "GSV"});
    set_methods(other, {"GEcho", "GSDV"});
    const MusScript *two[2] = {&script, &other};
    CHECK(encode_and_open(two, 2, mf, bytes) == -4, "two programs of different tables are refused");
    set_methods(other, {"GEcho", "GSV"});
    mf = {};
    CHECK(encode_and_open(two, 2, mf, bytes) == 0 && mf.intrinsic_count == 2 && mf.header.chunk_count == 2,
          "two of the same table share the file's");
    mus_close(&mf);
    mus_script_free(&other);
    mus_script_free(&script);
    return 1;
}

/* The method table as the loader reads it [orig: AudioVM_LoadScriptFile @ 0x672DBD..0x672E01]: each name up to its
   NUL, the next from its length byte on (a length byte that runs past the NUL skips the bytes after it), and a
   resolve table that would reach past the file refused (the loader writes each handler into the file's buffer
   [orig: @ 0x672DE9]). */
static int test_parse_method_table(void) {
    const char *src = "script test\nsection Begin\n{\n  done\n}\n";
    MusScript script = {};
    int err_line = 0, err_col = 0;
    const char *err_msg = NULL;
    CHECK(mus_compile(src, &script, &err_line, &err_col, &err_msg) == 0, "compile");
    set_methods(script, {"GEcho", "GSV"});
    const MusScript *one[1] = {&script};
    MusFile mf = {};
    std::vector<uint8_t> bytes;
    CHECK(encode_and_open(one, 1, mf, bytes) == 0, "write a two-name table");
    const MusFileHeader header = mf.header;
    mus_close(&mf);
    mus_script_free(&script);
    /* The first name's length byte 7 -> 9: the second name read two bytes on ("V"), as the game reads it. */
    const size_t blob = header.name_strings_blob_offset;
    CHECK(bytes[blob] == 7 && memcmp(&bytes[blob + 1], "GEcho", 6) == 0, "GEcho first, its length byte 7");
    std::vector<uint8_t> stepped = bytes;
    stepped[blob] = 9;
    mf = {};
    const bool read = mus_open_memory(&mf, stepped.data(), stepped.size()) == 0 && mf.intrinsic_count == 2 &&
                      strcmp(mf.intrinsic_names[0], "GEcho") == 0 && strcmp(mf.intrinsic_names[1], "V") == 0;
    mus_close(&mf);
    CHECK(read, "the next name where the length byte steps to");
    /* A count whose resolve table reaches past the file. */
    std::vector<uint8_t> counted = bytes;
    const uint32_t many = (uint32_t)(bytes.size() / 4);
    memcpy(&counted[16], &many, 4);
    mf = {};
    CHECK(mus_open_memory(&mf, counted.data(), counted.size()) == -21, "a resolve table past the file is refused");
    return 1;
}

/* A section the file names none keeps no name in the model (the game reads no label) and is written so; its label,
   "Section_N", is for display alone (mus_section_label: the decompiler's text, the VM's section names). A script
   holding strings `pushstr` reads [orig: AudioVM_Op_PushStr @ 0x672836] is not written (-6): the model holds their
   count alone. */
static int test_unnamed_sections_and_strings(void) {
    const char *src = "script test\nsection Begin\n{\n  done\n}\nsection Next\n{\n  done\n}\n";
    MusScript script = {};
    int err_line = 0, err_col = 0;
    const char *err_msg = NULL;
    CHECK(mus_compile(src, &script, &err_line, &err_col, &err_msg) == 0, "compile");
    CHECK(script.section_count == 2, "two sections");
    script.sections[1].name[0] = 0;
    const MusScript *one[1] = {&script};
    MusFile mf = {};
    std::vector<uint8_t> bytes;
    CHECK(encode_and_open(one, 1, mf, bytes) == 0, "write a script with an unnamed section");
    const MusScript &back = mf.scripts[0];
    const bool named = strcmp(back.sections[0].name, "Begin") == 0 && back.sections[1].name[0] == 0 &&
                       mus_section_label(&back, 0) == "Begin" && mus_section_label(&back, 1) == "Section_1" &&
                       mus_find_section(&back, "Section_1") == &back.sections[1];
    std::string text;
    const int needed = mus_decompile(&back, NULL, 0);
    if (needed > 0) {
        text.assign((size_t)needed + 1, '\0');
        mus_decompile(&back, &text[0], text.size());
    }
    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &back);
    const bool jumped = mus_vm_jump_to_section(vm, "Section_1") == 0 &&
                        strcmp(mus_vm_current_section(vm), "Section_1") == 0;
    mus_vm_destroy(vm);
    std::vector<uint8_t> again;
    {
        const MusScript *read[1] = {&back};
        MusFile mf2 = {};
        const bool same = encode_and_open(read, 1, mf2, again) == 0 && again == bytes;
        mus_close(&mf2);
        CHECK(same, "the unnamed section writes back unnamed");
    }
    mus_close(&mf);
    CHECK(named, "the model keeps the name empty; the label is Section_1");
    CHECK(text.find("section Section_1") != std::string::npos, "the text names it by its label");
    CHECK(jumped, "the VM names and finds it by its label");

    /* The chunk's string count (+0x34) made 1: read, and the writer refuses it. */
    std::vector<uint8_t> strings = bytes;
    uint32_t chunk = 0;
    memcpy(&chunk, &strings[0x2C], 4);   /* the chunk table's one entry, past the 44-byte header */
    const uint32_t count = 1;
    memcpy(&strings[chunk + 0x34], &count, 4);
    mf = {};
    CHECK(mus_open_memory(&mf, strings.data(), strings.size()) == 0 && mf.scripts[0].string_count == 1,
          "a chunk holding a string reads its count");
    const MusScript *held[1] = {&mf.scripts[0]};
    uint8_t *buf = NULL;
    size_t size = 0;
    const int rc = mus_encode_file(held, 1, &buf, &size);
    mus_free(buf);
    mus_close(&mf);
    CHECK(rc == -6, "and is not written: the model holds no string");
    mus_script_free(&script);
    return 1;
}

/* A `done` statement and a section's closing brace both compile to done (0x3F): the decompiler prints
   the brace for the done the section's code ends on and the statement for any other, so the minimal
   script (play, done, brace) decompiles to text that compiles back to its bytes. It printed every done
   as a brace, and `play / } / }` does not compile. */
static int test_done_statement_round_trips(void) {
    std::string text;
    CHECK(round_trips("script test\nsection Begin\n{\n  play sound_0\n  done\n}\n", &text),
          "the minimal script round-trips");
    CHECK(text.find("done\n}") != std::string::npos, "its done a statement before the brace");
    CHECK(round_trips("script t\nsection A\n{\n  done\n  play sound_1\n}\nsection B\n{\n  play sound_2\n  done\n}\n"),
          "a done ahead of a statement, then the next section's");
    CHECK(round_trips("script t\nsection A\n{\n  if ((Var01 == 1))\n  {\n    done\n  }\n  play sound_1\n}\n"),
          "a done in an if body");
    CHECK(round_trips("script t\nsection A\n{\n  play sound_1\n}\n"), "a section closed by its brace alone");
    CHECK(round_trips("script t\nsection A\n{\n  play sound_0\n}\nplay sound_1\n"),
          "a top-level statement after a section's brace");
    CHECK(round_trips("script t\nsection A\n{\n  play sound_0\n}\ndone\nsection B\n{\n  play sound_2\n}\n"),
          "a top-level done after a section's brace");
    CHECK(round_trips("script t\nsection A\n{\n  play sound_0\n}\nif ((Var01 == 1))\n{\n  done\n}\n"),
          "a top-level if whose body holds a done, after a section's brace");
    return 1;
}

/* An expression the compiler takes whose text runs past the decompiler's 255 characters a slot, or
   that holds more than 32 pending operands: the decompile fails rather than cut the text (a cut
   text compiled to other bytes, or not at all). Shorter ones still round-trip. */
static int test_long_expression_fails_to_decompile(void) {
    const auto left = [](int n) {
        std::string expr = "(Var02 + Var03)";
        for (int i = 1; i < n; ++i) expr = "(" + expr + " + Var03)";
        return "script t\nsection Begin\n{\n  Var01 = " + expr + "\n}\n";
    };
    const auto right = [](int n) {
        std::string expr = "1";
        for (int i = 0; i < n; ++i) expr = "(1 + " + expr + ")";
        return "script t\nsection Begin\n{\n  Var01 = " + expr + "\n}\n";
    };
    CHECK(round_trips(left(3).c_str()), "a short left-nested sum round-trips");
    CHECK(round_trips(right(31).c_str()), "31 right-nested sums round-trip");
    for (const std::string &src : {left(26), left(40), right(33), right(36)}) {
        MusScript script = {};
        int el = 0, ec = 0;
        const char *em = NULL;
        CHECK(mus_compile(src.c_str(), &script, &el, &ec, &em) == 0, "the compiler takes it");
        CHECK(mus_decompile(&script, NULL, 0) < 0, "the decompiler gives no cut text");
        mus_script_free(&script);
    }
    return 1;
}

/* A literal is read unsigned, the push's 32 bits: one past 4294967295 is refused (it compiled as
   another number: 4294967297 as 1), and one of 2^31 or more decompiles as itself (it printed
   negative, which compiles to a neg over the magnitude, other bytes). */
static int test_literal_range(void) {
    CHECK(round_trips("script t\nsection Begin\n{\n  Var01 = 7\n}\n"), "7 round-trips");
    std::string text;
    CHECK(round_trips("script t\nsection Begin\n{\n  Var01 = 3000000000\n}\n", &text), "3000000000 round-trips");
    CHECK(text.find("3000000000") != std::string::npos, "printed as itself");
    CHECK(round_trips("script t\nsection Begin\n{\n  Var01 = 4294967295\n}\n"), "4294967295 round-trips");
    CHECK(round_trips("script t\nsection Begin\n{\n  Var01 = -5\n}\n"), "a negative literal (neg over 5) round-trips");
    CHECK(!compiles("script t\nsection Begin\n{\n  Var01 = 4294967296\n}\n"), "4294967296 is refused");
    CHECK(!compiles("script t\nsection Begin\n{\n  Var01 = 4294967297\n}\n"), "4294967297 is refused");
    CHECK(!compiles("script t\nsection Begin\n{\n  Var01 = 99999999999\n}\n"), "99999999999 is refused");
    CHECK(!compiles("script t\nsection Begin\n{\n  frame 3000000000\n}\n"), "a frame count past 255 read unsigned");
    return 1;
}

int main(void) {
    RUN_TEST(test_done_statement_round_trips);
    RUN_TEST(test_long_expression_fails_to_decompile);
    RUN_TEST(test_literal_range);
    RUN_TEST(test_unnamed_sections_and_strings);
    RUN_TEST(test_encode_model_fields);
    RUN_TEST(test_parse_method_table);
    RUN_TEST(test_function_rules);
    RUN_TEST(test_function_limits);
    RUN_TEST(test_play_places);
    RUN_TEST(test_compile_minimal_script);
    RUN_TEST(test_compile_minimal_single_section);
    RUN_TEST(test_encode_file_minimal);
    RUN_TEST(test_play_track_widths);
    RUN_TEST(test_branches_in_bodies);
    RUN_TEST(test_nesting_bound);
    RUN_TEST(test_variable_operand_bounds);
    RUN_TEST(test_decompiler_robustness);
    RUN_TEST(test_table_entries_by_opcode);
    RUN_TEST(test_reject_switch_over_64_targets);
    RUN_TEST(test_reject_goto_table_too_large);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
