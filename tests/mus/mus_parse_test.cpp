/* Parse pins for engine/formats/mus over the minted programs (tests/fixtures/synth_mus_gen.cpp, every
   one in MDEdit's layout): synth_gamemus.bin, the shipped gamemus.bin's shape (eight sections, its
   MessageHandler function); and synth_forms.bin, the shipped menu programs' form (menumus.bin and
   jox01's MJox01.bin: eleven sections, no handler function, a global of the script's own past
   Var00..Var15 in its debug table, the globals 0x44 bytes). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <formats/mus/mus.h>

#include <string>


using namespace opennova::mus;

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

#define SYNTH_GAMEMUS MUS_FIXTURE_DIR "/synth_gamemus.bin"
#define SYNTH_FORMS MUS_FIXTURE_DIR "/synth_forms.bin"

static uint8_t *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

/* The canonical eleven intrinsic names, in MDEdit order (position == method index). */
static int check_intrinsics(const MusFile *mf) {
    CHECK(mf->intrinsic_count == 11, "11 intrinsic names parsed");
    CHECK(strncmp(mf->intrinsic_names[0],  "GEcho",    5)  == 0, "intrinsic[0] GEcho");
    CHECK(strncmp(mf->intrinsic_names[1],  "GGRnd",    5)  == 0, "intrinsic[1] GGRnd");
    CHECK(strncmp(mf->intrinsic_names[2],  "GSV",      3)  == 0, "intrinsic[2] GSV");
    CHECK(strncmp(mf->intrinsic_names[3],  "GSDV",     4)  == 0, "intrinsic[3] GSDV");
    CHECK(strncmp(mf->intrinsic_names[4],  "GFB",      3)  == 0, "intrinsic[4] GFB");
    CHECK(strncmp(mf->intrinsic_names[5],  "FSet",     4)  == 0, "intrinsic[5] FSet");
    CHECK(strncmp(mf->intrinsic_names[6],  "FClear",   6)  == 0, "intrinsic[6] FClear");
    CHECK(strncmp(mf->intrinsic_names[7],  "FIsSet",   6)  == 0, "intrinsic[7] FIsSet");
    CHECK(strncmp(mf->intrinsic_names[8],  "FIsClear", 8)  == 0, "intrinsic[8] FIsClear");
    CHECK(strncmp(mf->intrinsic_names[9],  "TStart",   6)  == 0, "intrinsic[9] TStart");
    CHECK(strncmp(mf->intrinsic_names[10], "TStop",    5)  == 0, "intrinsic[10] TStop");
    return 1;
}

/* The minted gamescript: eight named sections (Begin first, Multiplayerstart
   last), entry 0, the authored source path, section offsets inside the
   bytecode. */
static int test_open_synth_gamemus(void) {
    size_t n;
    uint8_t *buf = slurp(SYNTH_GAMEMUS, &n);
    CHECK(buf, "fixture present");
    MusFile mf;
    int rc = mus_open_memory(&mf, buf, n);
    CHECK(rc == 0, "open succeeded");
    CHECK(mf.header.magic == MUS_MAGIC_SCR0, "magic");
    CHECK(mf.header.chunk_count == 1, "single chunk");
    CHECK(mf.header.name_count == 11, "11 intrinsic names");
    CHECK(mf.scripts != NULL, "scripts allocated");
    const MusScript *s = &mf.scripts[0];
    CHECK(strncmp(s->name, "gamescript", 10) == 0, "first script gamescript");
    CHECK(s->globals_size == 0x40, "globals_size 0x40 (the MDEdit Var00..Var15 area)");
    CHECK(s->section_count == 8, "8 sections");
    CHECK(s->entry_section_index == 0, "entry section 0");
    CHECK(s->code_size > 0 && s->code != NULL, "bytecode present");
    for (uint32_t i = 0; i < s->section_count; ++i)
        CHECK(s->sections[i].code_offset < s->code_size, "section offset inside the bytecode");
    CHECK(s->sections[0].code_offset == 1 && s->code[0] == 0x00,
          "the entry section starts after MDEdit's leading nop, as the shipped scripts' do");
    /* The editor debug export table carries the authored labels. */
    CHECK(strcmp(s->sections[0].name, "Begin") == 0, "sec[0] name Begin");
    CHECK(strcmp(s->sections[7].name, "Multiplayerstart") == 0, "sec[7] name Multiplayerstart");
    CHECK(strstr(s->source_path, "synth_gamemus.mus") != NULL, "source_path embeds synth_gamemus.mus");
    CHECK(check_intrinsics(&mf), "intrinsics");
    mus_close(&mf);
    free(buf);
    return 1;
}

/* The forms program, as MDEdit lays a shipped menu program out: the menuscript chunk, the globals 0x44
   bytes (Var00..Var15 and `counter` at 64 in the debug table), no locals, eleven sections from Begin,
   entry 0 after the leading nop, no handler function (the +0x40 entry the code's start, as menumus.bin's
   0x94 is its bytecode's), the frame base 0x20, a line per statement, the authored source path. */
static int test_open_synth_forms(void) {
    MusFile mf;
    CHECK(mus_open(&mf, SYNTH_FORMS) == 0, "open succeeded");
    CHECK(mf.header.magic == MUS_MAGIC_SCR0 && mf.header.chunk_count == 1 && mf.header.name_count == 11,
          "one chunk, the eleven method names");
    const MusScript *s = &mf.scripts[0];
    CHECK(strncmp(s->name, "menuscript", 10) == 0, "first script menuscript");
    CHECK(s->globals_size == 0x44 && s->locals_size == 0, "globals 0x44, no locals");
    CHECK(s->section_count == 11 && s->entry_section_index == 0, "11 sections, entry 0");
    CHECK(s->code_size == 320 && s->code[0] == 0x00 && s->sections[0].code_offset == 1,
          "the entry section after MDEdit's leading nop");
    CHECK(strcmp(s->sections[0].name, "Begin") == 0 && strcmp(s->sections[10].name, "Server") == 0,
          "the debug table's labels");
    CHECK(s->has_message_handler && s->message_handler_offset == 0 && s->function_count == 0 && s->local_count == 0,
          "no handler function: the +0x40 entry the code's start");
    CHECK(s->locals_frame_offset == 0x20, "the frame base MDEdit writes");
    CHECK(s->variable_count == 17 && strcmp(s->variables[16].name, "counter") == 0 &&
          s->variables[16].byte_offset == 64, "the script's own global past Var15");
    CHECK(s->line_count == 78, "a line per statement");
    CHECK(strstr(s->source_path, "synth_forms.mus") != NULL, "source_path embeds synth_forms.mus");
    CHECK(check_intrinsics(&mf), "intrinsics");
    mus_close(&mf);
    return 1;
}

static int test_close_idempotent(void) {
    MusFile mf;
    memset(&mf, 0, sizeof(mf));
    mus_close(&mf);  /* should not crash on zero-init */
    mus_close(&mf);  /* idempotent */
    return 1;
}

static int test_open_file(void) {
    MusFile mf;
    int rc = mus_open(&mf, SYNTH_GAMEMUS);
    CHECK(rc == 0, "open from path");
    CHECK(mf.header.chunk_count == 1, "single chunk");
    CHECK(strncmp(mf.scripts[0].name, "gamescript", 10) == 0, "gamescript");
    mus_close(&mf);
    return 1;
}

/* mus_reads_the_same: two reads of one file hold the same in what the game reads; a change to a
   field it reads (a code byte, a section's entry, a method name, the MessageHandler's entry, the
   globals' size) tells them apart; a NULL file is never the same. */
static int test_reads_the_same(void) {
    MusFile a, b;
    CHECK(mus_open(&a, SYNTH_GAMEMUS) == 0 && mus_open(&b, SYNTH_GAMEMUS) == 0, "open twice");
    CHECK(mus_reads_the_same(&a, &b), "two reads of one file");
    CHECK(!mus_reads_the_same(&a, NULL) && !mus_reads_the_same(NULL, &b), "a NULL file");
    MusScript &s = b.scripts[0];
    CHECK(s.code_size > 0 && s.section_count > 0, "the script has code and sections");
    s.code[0] ^= 0xFF;
    CHECK(!mus_reads_the_same(&a, &b), "a code byte");
    s.code[0] ^= 0xFF;
    s.sections[0].code_offset += 1;
    CHECK(!mus_reads_the_same(&a, &b), "a section's entry");
    s.sections[0].code_offset -= 1;
    s.globals_size += 4;
    CHECK(!mus_reads_the_same(&a, &b), "the globals' size");
    s.globals_size -= 4;
    s.has_message_handler = !s.has_message_handler;
    CHECK(!mus_reads_the_same(&a, &b), "the MessageHandler entry");
    s.has_message_handler = !s.has_message_handler;
    if (b.intrinsic_count > 0) {
        b.intrinsic_names[0][0] ^= 0x20;
        CHECK(!mus_reads_the_same(&a, &b), "a method name");
        b.intrinsic_names[0][0] ^= 0x20;
    }
    CHECK(mus_reads_the_same(&a, &b), "the same again once restored");
    mus_close(&a);
    mus_close(&b);
    return 1;
}

static int test_open_missing_file(void) {
    MusFile mf;
    memset(&mf, 0xCD, sizeof(mf));
    int rc = mus_open(&mf, "fixtures/mus/__definitely_not_present__.bin");
    CHECK(rc != 0, "fail on missing file");
    /* close after failed open must be safe */
    mus_close(&mf);
    return 1;
}

/* A section name from the debug export table round-trips through find; the
   returned pointer is the table entry itself. */
static int find_section_over(const char *path, uint32_t win_index, uint32_t win_offset) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    CHECK(mf.scripts[0].section_count == 8, "8 sections");
    const MusSection *s3 = mus_find_section(&mf.scripts[0], "Win000");
    CHECK(s3 != NULL, "Win000 found");
    CHECK(s3 == &mf.scripts[0].sections[win_index], "ptr match");
    CHECK(s3->code_offset == win_offset, "Win000 code_offset (bytecode-relative)");
    const MusSection *miss = mus_find_section(&mf.scripts[0], "NOPE");
    CHECK(miss == NULL, "miss returns NULL");
    /* NULL guards */
    CHECK(mus_find_section(NULL, "x") == NULL, "NULL script returns NULL");
    CHECK(mus_find_section(&mf.scripts[0], NULL) == NULL, "NULL name returns NULL");
    mus_close(&mf);
    return 1;
}

static int test_find_section_synth(void) {
    /* The compiler interns sections in first-mention order: Begin, Testmission
       (forward-referenced from Begin), then the MessageHandler's table's Missionnull,
       Missionwin and Missionlose, then Win000, ... */
    MusFile mf;
    CHECK(mus_open(&mf, SYNTH_GAMEMUS) == 0, "open");
    uint32_t win_index = 0, win_offset = 0;
    for (uint32_t i = 0; i < mf.scripts[0].section_count; ++i)
        if (strcmp(mf.scripts[0].sections[i].name, "Win000") == 0) {
            win_index = i;
            win_offset = mf.scripts[0].sections[i].code_offset;
        }
    mus_close(&mf);
    CHECK(win_index == 5, "Win000 is the sixth interned section");
    return find_section_over(SYNTH_GAMEMUS, win_index, win_offset);
}

static int test_find_section_forms(void) {
    /* Route, the fifth section the forms program declares, entered at its code's 0x73. */
    MusFile mf;
    CHECK(mus_open(&mf, SYNTH_FORMS) == 0, "open");
    const MusSection *route = mus_find_section(&mf.scripts[0], "Route");
    CHECK(route != NULL && route == &mf.scripts[0].sections[4] && route->code_offset == 0x73, "Route found");
    CHECK(mus_find_section(&mf.scripts[0], "Win000") == NULL, "a section another program names, missed");
    mus_close(&mf);
    return 1;
}

int main(void) {
    RUN_TEST(test_open_synth_gamemus);
    RUN_TEST(test_open_synth_forms);
    RUN_TEST(test_close_idempotent);
    RUN_TEST(test_open_file);
    RUN_TEST(test_reads_the_same);
    RUN_TEST(test_open_missing_file);
    RUN_TEST(test_find_section_synth);
    RUN_TEST(test_find_section_forms);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
