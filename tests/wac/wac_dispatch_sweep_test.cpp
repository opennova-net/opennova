// Every registry entry dispatches explicitly: no registered command name
// reaches the VM's unsupported-command diagnostic (RuntimeGapKind::WacCommand).
// One statement per command is synthesized from the registry's own parameter
// types [orig: the 165-row table @0x82D290], compiled against a fixture that
// resolves every asset class, and executed once on a fixture world.
#include <cstdio>
#include <memory>
#include <string>

#include <formats/wac/command.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/particle/effect_catalog_names.h>
#include <runtime/particle/script_effects.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/vm.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::world;
using namespace opennova::wac;
namespace p = opennova::particle;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

struct Motion : IRootMotionSource {
    bool has_clip(int, int state) const override { return state > 0 && state < 252; }
    int32_t clip_length_ticks(int, int, int) const override { return -1; }
    bool advance(int, int state, int32_t &phase, RootMotionFrame &out) override {
        if (!has_clip(0, state)) return false;
        ++phase;
        out = {};
        return true;
    }
};

static p::EffectSceneConfig catalog() {
    p::EffectSceneConfig config;
    p::EffectCatalogDocument document;
    document.source = "script.ptl";
    p::ParticleDef particle;
    particle.id = "brief";
    particle.emit_dur = 0.016f;
    document.file.particles.push_back(particle);
    document.file.effects = {{"FLASH", {"brief"}}};
    config.documents.push_back(std::move(document));
    return config;
}

// The fixture token for each parameter type, in the literal forms the other
// wac tests use; every asset class resolves against the fixture below.
static std::string token_for(ParamType type) {
    switch (type) {
        case ParamType::Group: return "ai";
        case ParamType::Variable: return "v1";
        case ParamType::IfName: return "root";
        case ParamType::Text:
        case ParamType::Filename:
        case ParamType::TextToken: return "\"x\"";
        case ParamType::Face: return "HAPPY";
        case ParamType::Fx: return "FLASH";
        case ParamType::Ammo: return "SCRIPT";
        case ParamType::SoundSet: return "TONE";
        case ParamType::Anim: return "ANIM_EMOTE_1";
        default: return "1"; // numbers, distances, headings, SSN 1, area/target/wplist 1
    }
}

struct Fixture {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    Motion motion;
    // The compiler's name catalog (MissionKernel::script_effect_catalog): the
    // sweep binds FX literals at compile time and never presents a scene.
    p::EffectCatalogNames names;
    audio::SoundSetIndex sounds;
    Fixture() {
        for (int pool = 0; pool < 4; ++pool) world.registry.configure_pool(pool, 16);
        for (const auto &document : catalog().documents) names.add_document(document.file);
        lwf::File bank;
        lwf::Multi set; set.name = "TONE"; bank.multis.push_back(set);
        sounds.add_bank(0, bank);
        world.tables.ammo.entries.resize(2);
        auto &null = world.tables.ammo.entries[0];
        null.valid = true; null.name = "AT_NULL";
        auto &ammo = world.tables.ammo.entries[1];
        ammo.valid = true; ammo.name = "SCRIPT";
        ammo.velocity = 620; ammo.max_age_ticks = 62;
        ammo.weight_in_grains = 875; ammo.max_damage = 25;
        Entity e;
        e.net_id = 1; e.item_id = 11; e.has_item_def = true;
        e.kind = EntityKind::Organic; e.item_type = 3; e.health = 100;
        const EntityHandle actor = world.registry.spawn(0, e);
        world.cached.local_player = actor;
        world.ai.attach(actor);
        AiEntity &body = *world.ai.for_handle(actor);
        body.net_id = 1; body.health = 100;
        body.inf.active = true; body.inf.adm_id = 1;
        world.ai.root_motion = &motion;
        world.ai.is_authority = true;
        world.cached.humans = 1;
    }
    Program compile(const std::string &source) {
        CompileEnv env;
        env.registry = &world.registry;
        env.effects = &names;
        env.sounds = &sounds;
        env.ammo = &world.tables.ammo;
        return compile_source(source, env);
    }
};

static void test_every_registry_entry_dispatches_explicitly() {
    Fixture f;
    std::string source = "if [root] never then set(v0,1) endif\n";
    for (int i = 0; i < wac_command_count(); ++i) {
        const CommandDef &def = wac_commands()[i];
        source += "if ";
        source += def.name;
        source += "(";
        for (int a = 0; a < def.argc; ++a) {
            if (a) source += ",";
            source += token_for(def.params[a]);
        }
        source += ") then set(v1,1) endif\n";
    }
    const Program program = f.compile(source);
    CHECK(program.ok());
    CHECK(program.diagnostics.empty());
    for (const Diagnostic &d : program.diagnostics)
        std::printf("diagnostic line %d: %s\n", d.line, d.message.c_str());
    WacVm vm;
    vm.load(program);
    vm.execute(f.world);
    for (const auto &gap : f.world.diagnostics.gaps()) {
        CHECK(gap.origin.kind != RuntimeGapKind::WacCommand);
        if (gap.origin.kind == RuntimeGapKind::WacCommand) {
            const int cmd = gap.origin.code;
            std::printf("fall-through: command %d (%s)\n", cmd,
                    cmd >= 0 && cmd < wac_command_count() ? wac_commands()[cmd].name : "?");
        }
    }
}

int main() {
    CHECK(wac_command_count() == 165);
    test_every_registry_entry_dispatches_explicitly();
    std::printf("wac dispatch sweep: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
