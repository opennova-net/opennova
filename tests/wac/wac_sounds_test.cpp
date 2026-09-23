#include <cstdio>
#include <memory>
#include <runtime/audio/oneshot_play.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::world;
using namespace opennova::wac;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

struct Fixture {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    audio::SoundSetIndex sounds;
    Fixture() {
        for (int pool = 0; pool < 4; ++pool) world.registry.configure_pool(pool, 16);
        lwf::File bank;
        for (const char *name : {"TONE", "12"}) {
            lwf::Multi set; set.name = name; bank.multis.push_back(set);
        }
        sounds.add_bank(0, bank);
    }
    Program compile(const std::string &source) {
        CompileEnv env; env.registry = &world.registry; env.sounds = &sounds;
        return compile_source(source, env);
    }
    void script(const std::string &source) {
        const auto program = compile(source); CHECK(program.ok());
        WacVm vm; vm.load(program); vm.execute(world);
    }
    EntityHandle entity(int pool, int ssn, int number = 7, bool has_def = true) {
        Entity entity;
        entity.net_id = uint16_t(ssn); entity.item_id = kParticleEffectMarkerTypeId;
        entity.has_item_def = has_def; entity.wp_number = number;
        entity.health = 0; entity.alive = false; // no health gate on these commands
        entity.position = {10, 20, 30};
        return world.registry.spawn(pool, entity);
    }
};

static void test_sound_handles_raw_operands_and_retry() {
    Fixture f;
    // A leading minus folds into the previous accumulator in retail; start
    // from zero to author the negative raw operand this sound case exercises.
    f.script("v1=SS_TONE\nv2=163841\nv3=0-257\n"
             "sound(v1,v2,v3) store(v4)\nsound(12,2.5,128) store(v5)\n"
             "v6=0\nsound(v6,1,0) store(v7)\n");
    CHECK(f.world.script.vars.get_mission(1) == 1);
    CHECK(f.world.script.vars.get_mission(4) == 0);
    CHECK(f.world.script.vars.get_mission(5) == 0 && f.world.script.vars.get_mission(7) == 0);
    CHECK(f.world.out.script_sounds.size() == 2);
    if (f.world.out.script_sounds.size() == 2) {
        const auto &direct = f.world.out.script_sounds[0];
        CHECK(direct.name == "tone" && direct.distance_q16 == 163841 && direct.bearing == -257);
        const auto &literal = f.world.out.script_sounds[1];
        CHECK(literal.name == "12" && literal.distance_q16 == 163840 && literal.bearing == 128);
    }
    const auto baseline = f.world.snapshot();
    f.world.out.script_sounds.clear();
    // A bare word reaches the sound banks upper-cased; a quoted one keeps its
    // quote and names no set. [orig: Script_Compile @0x4F3418..0x4F341D,
    // @0x4F3338; WacScript_ResolveParameter @0x4F2FDA]
    CHECK(!f.compile("sound(\"TONE\",1,0)\n").ok());
    f.script("sound(ToNe,3F,64)\n");
    CHECK(f.world.out.script_sounds.size() == 1);
    CHECK(f.world.out.script_sounds[0].distance_q16 == 3 * 21501);
    f.world.restore(baseline);
    CHECK(f.world.out.script_sounds.size() == 2);
    CHECK(f.world.out.script_sounds[0].distance_q16 == 163841);
    // Replacing the program must retain the catalog's handle order for aliases.
    f.world.out.script_sounds.clear();
    f.script("sound(v1,1,0)\n");
    CHECK(f.world.out.script_sounds.size() == 1 && f.world.out.script_sounds[0].name == "tone");
    CHECK(!f.compile("sound(MISSING,1,0)\n").ok());
    CHECK(!f.compile("sound(1,1,0)\n").ok()); // numeric sound literals are names
    CHECK(!compile_source("v1=SS_TONE\n", {}).ok());
    CHECK(f.world.diagnostics.empty());
}

static void test_target_and_ssn_admission_and_exact_position() {
    Fixture f;
    f.entity(2, 1); // target lookup only walks pool 3
    f.entity(3, 2, 7, false); // target lookup requires ItemDef
    const auto first = f.entity(3, 3);
    f.entity(3, 4);
    f.world.ai.attach(first);
    auto *body = f.world.ai.for_handle(first);
    body->pos[0] = 16777217; body->pos[1] = -234567; body->pos[2] = 456789;
    const auto source = f.entity(0, 5, 0, false); // SS2SSN does not require ItemDef
    f.script("sound2tgt(SS_TONE,7) store(v1)\n"
             "sound2tgt(TONE,9) store(v2)\nv3=0\nsound2tgt(v3,7) store(v4)\n"
             "SS2SSN(TONE,5) store(v5)\nSS2SSN(v3,5) store(v6)\n");
    CHECK(f.world.script.vars.get_mission(1) == 0);
    CHECK(f.world.script.vars.get_mission(2) == 1 && f.world.script.vars.get_mission(4) == 1);
    CHECK(f.world.script.vars.get_mission(5) == 1 && f.world.script.vars.get_mission(6) == 1);
    CHECK(f.world.out.slot_sounds.size() == 2);
    if (f.world.out.slot_sounds.size() == 2) {
        const auto &target = f.world.out.slot_sounds[0];
        CHECK(target.source_handle == first.packed);
        CHECK(target.pos[0] == 16777217 && target.pos[1] == -234567 && target.pos[2] == 456789);
        CHECK(std::string(target.set_name) == "tone");
        CHECK(f.world.out.slot_sounds[1].source_handle == source.packed);
        CHECK(f.world.out.slot_sounds[1].pos[0] == 10 * 65536);
    }
    const auto baseline = f.world.snapshot();
    f.world.out.slot_sounds.clear();
    f.world.restore(baseline);
    CHECK(f.world.out.slot_sounds.size() == 2);
    f.world.out.slot_sounds.clear();
    f.world.registry.get(source)->item_id = 0;
    f.script("SS2SSN(TONE,5) store(v7)\nSS2SSN(TONE,99) store(v8)\n");
    CHECK(f.world.script.vars.get_mission(7) == 0 && f.world.script.vars.get_mission(8) == 0);
    CHECK(f.world.out.slot_sounds.empty() && f.world.diagnostics.empty());
}

int main() {
    test_sound_handles_raw_operands_and_retry();
    test_target_and_ssn_admission_and_exact_position();
    std::printf("wac_sounds: %d failures\n", failures);
    return failures ? 1 : 0;
}
