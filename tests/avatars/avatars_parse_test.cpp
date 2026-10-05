// Parse test for opennova engine/formats/avatars: the minted fixtures/avatars/synth_avatars.def
// (tests/fixtures/minimal_avatars_gen.cpp) unconditionally, the shipped Avatars.def from the
// reference fixture set behind OPENNOVA_JO_ASSETS.
// The retail leg pins the shipped table (a retail JO + JOX extract,
// Desktop/REVX02/AVATARS.DEF) against the witnessed grammar; the synthetic leg pins the parser
// over the same shapes (docs/playerinfo/avatars-re.md): part pool,
// nationality -> division -> combo tree, trailing flags, quoted values.
#include <cstdio>
#include <cstring>
#include <string>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include <formats/avatars/avatars.h>

using namespace opennova::avatars;

namespace {

const AvatarPart *find_part(const AvatarsFile &f, const char *name) {
    for (size_t i = 0; i < f.parts_count; ++i)
        if (std::strcmp(f.parts[i].name, name) == 0) return &f.parts[i];
    return nullptr;
}

const AvatarNationality *find_nat(const AvatarsFile &f, const char *raw_id) {
    for (size_t i = 0; i < f.nationalities_count; ++i)
        if (std::strcmp(f.nationalities[i].raw_id, raw_id) == 0) return &f.nationalities[i];
    return nullptr;
}

bool has_diag(const AvatarsFile &f, const char *code) {
    for (size_t i = 0; i < f.diagnostics_count; ++i)
        if (std::strcmp(f.diagnostics[i].code, code) == 0) return true;
    return false;
}

} // namespace

// The minted table (tests/fixtures/minimal_avatars_gen.cpp): 12 heads + 8
// bodies + 6 arms = 26 parts; 8 nationalities (0..3 good, 4..7 evil); 39 combos.
int check_synth(const std::string &path) {
    AvatarsFile f;
    TEST_EXPECT(avatars_parse(path.c_str(), &f) == 0);
    TEST_EXPECT(f.parts_count == 26);
    TEST_EXPECT(f.nationalities_count == 8);
    size_t heads = 0, bodies = 0, arms = 0, combos = 0;
    for (size_t i = 0; i < f.parts_count; ++i) {
        switch (f.parts[i].kind) {
        case AVATAR_PART_HEAD: ++heads; break;
        case AVATAR_PART_BODY: ++bodies; break;
        case AVATAR_PART_ARMS: ++arms; break;
        default: break;
        }
    }
    TEST_EXPECT(heads == 12);
    TEST_EXPECT(bodies == 8);
    TEST_EXPECT(arms == 6);
    for (size_t i = 0; i < f.nationalities_count; ++i)
        for (size_t j = 0; j < f.nationalities[i].divisions_count; ++j)
            combos += f.nationalities[i].divisions[j].combos_count;
    TEST_EXPECT(combos == 39);

    // First head: define head SYN_HEAD_BOONIE { name AV_SYNTH_BOONIE; graphic synth_boonie.3di;
    //   camo 0 0 0; voice 1; sex m }.
    const AvatarPart *boonie = find_part(f, "SYN_HEAD_BOONIE");
    TEST_EXPECT(boonie != nullptr);
    TEST_EXPECT(boonie->kind == AVATAR_PART_HEAD);
    TEST_EXPECT(std::strcmp(boonie->display_name, "AV_SYNTH_BOONIE") == 0);
    TEST_EXPECT(std::strcmp(boonie->graphic, "synth_boonie.3di") == 0);
    TEST_EXPECT(boonie->camo[0] == 0 && boonie->camo[1] == 0 && boonie->camo[2] == 0);
    TEST_EXPECT(boonie->voice == 1);
    TEST_EXPECT(boonie->sex == AVATAR_SEX_MALE);

    // camo variant indices are preserved (camo 3 0 0).
    const AvatarPart *camo1 = find_part(f, "SYN_HEAD_BOONIE_CAMO_1");
    TEST_EXPECT(camo1 != nullptr);
    TEST_EXPECT(camo1->camo[0] == 3 && camo1->camo[1] == 0 && camo1->camo[2] == 0);

    // Quoted multi-word value: name "Synth Bare Arms" (quote-aware tokenization).
    const AvatarPart *bare = find_part(f, "SYN_ARMS_BARE");
    TEST_EXPECT(bare != nullptr);
    TEST_EXPECT(bare->kind == AVATAR_PART_ARMS);
    TEST_EXPECT(std::strcmp(bare->display_name, "Synth Bare Arms") == 0);

    // Nationality N00, alignment good, 4 divisions (D00..D03).
    const AvatarNationality *alpha = find_nat(f, "N00");
    TEST_EXPECT(alpha != nullptr);
    TEST_EXPECT(alpha->id == 0); // lenient id parse skips the leading 'N'
    TEST_EXPECT(std::strcmp(alpha->name_key, "AV_NAT_SYNTH_ALPHA") == 0);
    TEST_EXPECT(alpha->has_alignment && alpha->alignment == AVATAR_ALIGN_GOOD);
    TEST_EXPECT(alpha->divisions_count == 4);

    // Division D00, trailing "skipdemo" flag, 4 combos; first combo 001 wears the boonie set.
    const AvatarDivision *d0 = &alpha->divisions[0];
    TEST_EXPECT(std::strcmp(d0->raw_id, "D00") == 0);
    TEST_EXPECT(d0->id == 0);
    TEST_EXPECT(std::strcmp(d0->name_key, "AV_DIV_SYNTH_0_0") == 0);
    TEST_EXPECT(std::strcmp(d0->flags, "skipdemo") == 0);
    TEST_EXPECT(d0->combos_count == 4);
    const AvatarCombo *c0 = &d0->combos[0];
    TEST_EXPECT(std::strcmp(c0->raw_id, "001") == 0);
    TEST_EXPECT(c0->id == 1);
    TEST_EXPECT(std::strcmp(c0->head_name, "SYN_HEAD_BOONIE") == 0);
    TEST_EXPECT(std::strcmp(c0->body_name, "SYN_BODY_0") == 0);
    TEST_EXPECT(std::strcmp(c0->arms_name, "SYN_ARMS_1") == 0);

    // Nationality-level trailing flag (N01 skipdemo); a division with no flag (N00 D02); an
    // evil nationality (N07).
    const AvatarNationality *bravo = find_nat(f, "N01");
    TEST_EXPECT(bravo != nullptr);
    TEST_EXPECT(std::strcmp(bravo->flags, "skipdemo") == 0);
    TEST_EXPECT(std::strcmp(alpha->divisions[2].name_key, "AV_DIV_SYNTH_0_2") == 0);
    TEST_EXPECT(alpha->divisions[2].flags[0] == '\0');
    const AvatarNationality *hotel = find_nat(f, "N07");
    TEST_EXPECT(hotel != nullptr && hotel->alignment == AVATAR_ALIGN_EVIL);
    TEST_EXPECT(!has_diag(f, "AVATAR_DUPLICATE_PART"));

    avatars_free(&f);
    return 0;
}

// The shipped table: 51 heads + 36 bodies + 22 arms = 109 parts; 8 nationalities; 69 combos.
int check_retail(const std::string &path) {
    AvatarsFile f;
    TEST_EXPECT(avatars_parse(path.c_str(), &f) == 0);

    // Counts: 51 heads + 36 bodies + 22 arms = 109 parts; 8 nationalities; 69 combos.
    TEST_EXPECT(f.parts_count == 109);
    TEST_EXPECT(f.nationalities_count == 8);
    size_t heads = 0, bodies = 0, arms = 0, combos = 0;
    for (size_t i = 0; i < f.parts_count; ++i) {
        switch (f.parts[i].kind) {
        case AVATAR_PART_HEAD: ++heads; break;
        case AVATAR_PART_BODY: ++bodies; break;
        case AVATAR_PART_ARMS: ++arms; break;
        default: break;
        }
    }
    TEST_EXPECT(heads == 51);
    TEST_EXPECT(bodies == 36);
    TEST_EXPECT(arms == 22);
    for (size_t i = 0; i < f.nationalities_count; ++i)
        for (size_t j = 0; j < f.nationalities[i].divisions_count; ++j)
            combos += f.nationalities[i].divisions[j].combos_count;
    TEST_EXPECT(combos == 69);

    // First head: define head JO_HEAD_SEAL { name AV_BOONIEHAT; graphic Boonie.3di;
    //   camo 0 0 0; voice 1; sex m }.
    const AvatarPart *seal = find_part(f, "JO_HEAD_SEAL");
    TEST_EXPECT(seal != nullptr);
    TEST_EXPECT(seal->kind == AVATAR_PART_HEAD);
    TEST_EXPECT(std::strcmp(seal->display_name, "AV_BOONIEHAT") == 0);
    TEST_EXPECT(std::strcmp(seal->graphic, "Boonie.3di") == 0);
    TEST_EXPECT(seal->camo[0] == 0 && seal->camo[1] == 0 && seal->camo[2] == 0);
    TEST_EXPECT(seal->voice == 1);
    TEST_EXPECT(seal->sex == AVATAR_SEX_MALE);

    // camo variant indices are preserved (camo 3 0 0).
    const AvatarPart *camo1 = find_part(f, "JO_HEAD_SEAL_CAMO_1");
    TEST_EXPECT(camo1 != nullptr);
    TEST_EXPECT(camo1->camo[0] == 3 && camo1->camo[1] == 0 && camo1->camo[2] == 0);

    // Quoted multi-word value: name "Bare Arms" (quote-aware tokenization).
    const AvatarPart *bare = find_part(f, "JO_ARMS_BARE");
    TEST_EXPECT(bare != nullptr);
    TEST_EXPECT(bare->kind == AVATAR_PART_ARMS);
    TEST_EXPECT(std::strcmp(bare->display_name, "Bare Arms") == 0);

    // Nationality N00 = United States, alignment good, 9 divisions (D00..D08).
    const AvatarNationality *us = find_nat(f, "N00");
    TEST_EXPECT(us != nullptr);
    TEST_EXPECT(us->id == 0); // lenient id parse skips the leading 'N'
    TEST_EXPECT(std::strcmp(us->name_key, "AV_NAT_UNITEDSTATES") == 0);
    TEST_EXPECT(us->has_alignment && us->alignment == AVATAR_ALIGN_GOOD);
    TEST_EXPECT(us->divisions_count == 9);

    // Division D00 = SEAL, trailing "skipdemo" flag, 4 combos; first combo 001.
    const AvatarDivision *d0 = &us->divisions[0];
    TEST_EXPECT(std::strcmp(d0->raw_id, "D00") == 0);
    TEST_EXPECT(d0->id == 0);
    TEST_EXPECT(std::strcmp(d0->name_key, "AV_DIV_SEAL") == 0);
    TEST_EXPECT(std::strcmp(d0->flags, "skipdemo") == 0);
    TEST_EXPECT(d0->combos_count == 4);
    const AvatarCombo *c0 = &d0->combos[0];
    TEST_EXPECT(std::strcmp(c0->raw_id, "001") == 0);
    TEST_EXPECT(c0->id == 1);
    TEST_EXPECT(std::strcmp(c0->head_name, "JO_HEAD_SEAL") == 0);
    TEST_EXPECT(std::strcmp(c0->body_name, "JO_BODY_SEAL_1") == 0);
    TEST_EXPECT(std::strcmp(c0->arms_name, "JO_ARMS_US_1") == 0);

    // Nationality-level trailing flag (Great Britain skipdemo); a division with no flag.
    const AvatarNationality *gb = find_nat(f, "N01");
    TEST_EXPECT(gb != nullptr);
    TEST_EXPECT(std::strcmp(gb->flags, "skipdemo") == 0);
    TEST_EXPECT(std::strcmp(us->divisions[2].name_key, "AV_DIV_FORCERECON") == 0);
    TEST_EXPECT(us->divisions[2].flags[0] == '\0');

    avatars_free(&f);
    return 0;
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    const std::string root = test_paths_repo_root(__FILE__);
    if (check_synth(root + "/fixtures/avatars/synth_avatars.def") != 0) return 1;

    {
        const char src[] =
            "define head HEAD_A\r\n{\r\n\tgraphic old_head.3di\r\n}\r\n"
            "define body BODY_A\r\n{\r\n\tgraphic body.3di\r\n}\r\n"
            "define head HEAD_A\r\n{\r\n\tgraphic new_head.3di\r\n}\r\n"
            "nationality N00 AV_NAT\r\n{\r\n\talignment good\r\n\tdivision D00 AV_DIV\r\n\t{\r\n"
            "\t\tcombo 001 HEAD_A BODY_A\r\n"
            "\t}\r\n}\r\n"
            "define head HEAD_A\r\n{\r\n\tgraphic too_late.3di\r\n}\r\n";
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(src, sizeof(src) - 1, &model) == 0);
        TEST_EXPECT(model.nationalities_count == 1);
        TEST_EXPECT(model.nationalities[0].divisions_count == 1);
        TEST_EXPECT(model.nationalities[0].divisions[0].combos_count == 1);
        const AvatarCombo &combo = model.nationalities[0].divisions[0].combos[0];
        TEST_EXPECT(std::strcmp(combo.head_name, "HEAD_A") == 0);
        TEST_EXPECT(std::strcmp(combo.head.graphic, "new_head.3di") == 0);
        TEST_EXPECT(std::strcmp(combo.body.graphic, "body.3di") == 0);
        TEST_EXPECT(std::strcmp(combo.arms.name, "") == 0);
        TEST_EXPECT(combo.has_arms == 0);
        avatars_free(&model);
    }

    {
        const char src[] =
            "define body BODY_A\r\n{\r\n\tgraphic body.3di\r\n}\r\n"
            "nationality N00 AV_NAT\r\n{\r\n\talignment good\r\n\tdivision D00 AV_DIV\r\n\t{\r\n"
            "\t\tcombo 001 MISSING_HEAD BODY_A\r\n"
            "\t\tcombo 002 LATER_HEAD BODY_A\r\n"
            "\t}\r\n}\r\n"
            "define head LATER_HEAD\r\n{\r\n\tgraphic later.3di\r\n}\r\n";
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(src, sizeof(src) - 1, &model) == 0);
        TEST_EXPECT(model.nationalities_count == 1);
        TEST_EXPECT(model.nationalities[0].divisions_count == 1);
        TEST_EXPECT(model.nationalities[0].divisions[0].combos_count == 0);
        TEST_EXPECT(has_diag(model, "combo_missing_head"));
        avatars_free(&model);
    }

    {
        const char src[] =
            "define head HEAD_A\r\n{\r\n\tgraphic head.3di\r\n}\r\n"
            "define body BODY_A\r\n{\r\n\tgraphic body.3di\r\n}\r\n"
            "nationality N00 AV_NAT\r\n{\r\n\talignment good\r\n\tdivision D00 AV_DIV\r\n\t{\r\n"
            "\t\tcombo 001 HEAD_A BODY_A MISSING_ARMS\r\n"
            "\t}\r\n}\r\n";
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(src, sizeof(src) - 1, &model) == 0);
        TEST_EXPECT(model.nationalities[0].divisions[0].combos_count == 1);
        const AvatarCombo &combo = model.nationalities[0].divisions[0].combos[0];
        TEST_EXPECT(combo.has_arms == 0);
        TEST_EXPECT(combo.arms_name[0] == '\0');
        TEST_EXPECT(has_diag(model, "combo_missing_arms"));
        avatars_free(&model);
    }

    {
        const char src[] =
            "nationality N00 FIRST\r\n{\r\n\tdivision D00 FIRST_DIV\r\n\t{\r\n\t}\r\n"
            "\tdivision D00 DUP_DIV\r\n\t{\r\n\t}\r\n}\r\n"
            "nationality N00 DUP_NAT\r\n{\r\n}\r\n";
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(src, sizeof(src) - 1, &model) == 0);
        TEST_EXPECT(model.nationalities_count == 1);
        TEST_EXPECT(std::strcmp(model.nationalities[0].name_key, "FIRST") == 0);
        TEST_EXPECT(model.nationalities[0].divisions_count == 1);
        TEST_EXPECT(std::strcmp(model.nationalities[0].divisions[0].name_key, "FIRST_DIV") == 0);
        TEST_EXPECT(has_diag(model, "duplicate_nationality"));
        TEST_EXPECT(has_diag(model, "duplicate_division"));
        avatars_free(&model);
    }

    // The lenient id skip compares a signed byte: a first byte of 0x80 or above
    // is no character above '9', so atol reads it and stops at once (id 0)
    // where 'N' is skipped. [orig: CAvatarDefs_ParseConfigLine, `cmp byte ptr
    // [eax], 39h; jle` @0x57A628 / @0x57A74E]
    {
        const char src[] =
            "nationality \xA7" "5 HIGH\r\n{\r\n\tdivision \xA7" "3 HIGH_DIV\r\n\t{\r\n\t}\r\n}\r\n"
            "nationality N6 LOW\r\n{\r\n}\r\n";
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(src, sizeof(src) - 1, &model) == 0);
        TEST_EXPECT(model.nationalities_count == 2);
        TEST_EXPECT(model.nationalities[0].id == 0);
        TEST_EXPECT(model.nationalities[0].divisions_count == 1);
        TEST_EXPECT(model.nationalities[0].divisions[0].id == 0);
        TEST_EXPECT(model.nationalities[1].id == 6);
        avatars_free(&model);
    }

    {
        const char src[] =
            "define head HEAD_A\r\n{\r\n\tcamo 256 -1 511\r\n\tvoice 260\r\n}\r\n";
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(src, sizeof(src) - 1, &model) == 0);
        TEST_EXPECT(model.parts_count == 1);
        TEST_EXPECT(model.parts[0].camo[0] == 0);
        TEST_EXPECT(model.parts[0].camo[1] == 255);
        TEST_EXPECT(model.parts[0].camo[2] == 255);
        TEST_EXPECT(model.parts[0].voice == 4);
        avatars_free(&model);
    }

    {
        std::string src;
        for (int i = 0; i < 512; ++i) {
            char line[128];
            std::snprintf(line, sizeof(line), "define head H%03d\r\n{\r\n}\r\n", i);
            src += line;
        }
        src += "nationality N00 AV_NAT\r\n{\r\n}\r\n";
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(src.data(), src.size(), &model) != 0);
    }

    {
        std::string src =
            "define head HEAD_A\r\n{\r\n}\r\n"
            "define body BODY_A\r\n{\r\n}\r\n"
            "nationality N00 AV_NAT\r\n{\r\n\tdivision D00 AV_DIV\r\n\t{\r\n";
        for (int i = 0; i < 129; ++i) {
            char line[96];
            std::snprintf(line, sizeof(line), "\t\tcombo %03d HEAD_A BODY_A\r\n", i);
            src += line;
        }
        src += "\t}\r\n}\r\n";
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(src.data(), src.size(), &model) != 0);
    }

    // The retail walk and tokenizer [orig: CAvatarDefs_Init @ 0x57b180 ->
    // File_ParseASCIIFile @ 0x53d810 -> CAvatarDefs_ParseConfigLine @ 0x57a3f0]:
    // a comma separates and `;` ends the line; a part key reads its value
    // whether or not the line carries one; a combo id is a plain atol
    // (@ 0x57a90d); a `define` of no known kind leaves the pending state, so the
    // next `{` writes the previous part again; `}` is any token beginning with
    // the brace; `alignment` takes good or evil and nothing else.
    {
        const char src[] =
            "define head HEAD_A\r\n{\r\n\tname FIRST\r\n\tcamo 3,4,5 ; three values\r\n}\r\n"
            "define legs LEGS_X\r\n{\r\n\tname SECOND\r\n\tgraphic legs.3di\r\n}x\r\n"
            "define body BODY_A\r\n{\r\n\tname BODY_NAME\r\n\tname\r\n}\r\n"
            "nationality N03 AV_NAT\r\n{\r\n\talignment evil\r\n\talignment neutral\r\n"
            "\tdivision D00 AV_DIV\r\n\t{\r\n\t\tcombo c05 HEAD_A BODY_A\r\n\t}\r\n}\r\n";
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(src, sizeof(src) - 1, &model) == 0);
        TEST_EXPECT(model.parts_count == 2);
        const AvatarPart *head = find_part(model, "HEAD_A");
        const AvatarPart *body = find_part(model, "BODY_A");
        TEST_EXPECT(head != nullptr && body != nullptr);
        if (head != nullptr) {
            TEST_EXPECT(head->camo[0] == 3 && head->camo[1] == 4 && head->camo[2] == 5);
            TEST_EXPECT(std::strcmp(head->display_name, "SECOND") == 0);
            TEST_EXPECT(std::strcmp(head->graphic, "legs.3di") == 0);
        }
        if (body != nullptr) TEST_EXPECT(body->display_name[0] == '\0');
        TEST_EXPECT(model.nationalities_count == 1);
        if (model.nationalities_count == 1) {
            const AvatarNationality &nat = model.nationalities[0];
            TEST_EXPECT(nat.id == 3 && nat.alignment == AVATAR_ALIGN_EVIL);
            TEST_EXPECT(nat.divisions_count == 1 && nat.divisions[0].combos_count == 1);
            if (nat.divisions_count == 1 && nat.divisions[0].combos_count == 1)
                TEST_EXPECT(nat.divisions[0].combos[0].id == 0);
        }
        avatars_free(&model);
    }

    // The 512-part guard runs ahead of every line but a brace [orig: @ 0x57a456]:
    // a 512th part whose block carries a key ends the walk, one with an empty
    // block at the end of the file does not.
    {
        std::string full, empty;
        for (int i = 0; i < 512; ++i) {
            char line[128];
            std::snprintf(line, sizeof(line), "define head H%03d\r\n{\r\n", i);
            full += line;
            empty += line;
            if (i == 511) full += "\tvoice 1\r\n";
            full += "}\r\n";
            empty += "}\r\n";
        }
        AvatarsFile model;
        TEST_EXPECT(avatars_parse_memory(full.data(), full.size(), &model) != 0);
        TEST_EXPECT(avatars_parse_memory(empty.data(), empty.size(), &model) == 0);
        TEST_EXPECT(model.parts_count == 512);
        avatars_free(&model);
    }

    // The retail leg: the shipped Avatars.def from the reference fixture set.
    const std::string retail = retail::reference_fixture("avatars/Avatars.def");
    if (retail.empty())
        return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/avatars/Avatars.def (the shipped avatar table)");
    if (check_retail(retail) != 0) return 1;
    std::printf("retail leg: Avatars.def parsed with the shipped pins\n");
    return 0;
}
