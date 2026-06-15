// Parse test for opennova libs/avatars over the real retail Avatars.def.
// Fixture: fixtures/avatars/Avatars.def, copied verbatim from a retail
// Joint Operations + JOX extract (Desktop/REVX02/AVATARS.DEF). Pins the parser
// against the witnessed grammar (docs/playerinfo/avatars-re.md): part pool,
// nationality -> division -> combo tree, trailing flags, quoted values.
#include <cstring>
#include <string>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include "avatars/avatars.h"

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

} // namespace

int main() {
    const std::string root = test_paths_repo_root(__FILE__);
    const std::string path = root + "/fixtures/avatars/Avatars.def";

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
