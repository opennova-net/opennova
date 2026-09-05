#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/client_world_materializer.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <cstdio>
#include <memory>

using namespace opennova;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
    replication::ClientReplicaPipeline pipeline;
    std::vector<uint8_t> load(23 + kWorldStateScoreCount * 4 + 4);
    load[22] = 8;
    pipeline.apply(s2c::WORLD_STATE_LOAD, load);
    CHECK(pipeline.state().cease_fire);
    load[22] = 0;
    pipeline.apply(s2c::WORLD_STATE_LOAD, load);
    CHECK(pipeline.state().cease_fire); // absent bit does not clear the latch
    const auto command = [&](const char *text) {
        pipeline.apply(s2c::TEXT_COMMAND,
                std::vector<uint8_t>(text, text + std::char_traits<char>::length(text) + 1));
    };
    command("setceasefire \"0\"");
    CHECK(!pipeline.state().cease_fire);
    command("SETCEASEFIRE -2");
    CHECK(pipeline.state().cease_fire);
    command("SETCEASEFIRE");
    CHECK(pipeline.state().cease_fire);

    auto world = std::make_unique<world::World>();
    world->registry.configure_pool(2, 16);
    StaticEntityRecord record{};
    record.item_type_id = 1896;
    record.section_mask = 4;
    StaticEntityBatch batch{};
    batch.records.push_back(record);
    pipeline.apply(s2c::STATIC_ENTITY_BATCH, encode_static_entity_batch(batch));
    replication::ClientWorldMaterializer materializer;
    materializer.sync(pipeline.state(), *world);
    const world::EntityHandle handle{0x2000};
    auto *field = world->registry.get(handle);
    CHECK(field != nullptr);
    if (field == nullptr) return 1;
    world->minefields.initialize(*world, *field, true, true, true, 0, 0, "", "", {});
    CHECK(field->section_mask == 4);
    field->section_mask |= 1;
    const uint64_t lifetime = field->registry_spawn_id;
    materializer.sync(pipeline.state(), *world);
    CHECK(field->section_mask == 5);
    field->section_mask = 0x3FFF;
    world->minefields.think(*world, *field);
    materializer.sync(pipeline.state(), *world);
    CHECK(world->registry.get(handle) == nullptr);
    pipeline.apply(s2c::STATIC_ENTITY_BATCH, encode_static_entity_batch(batch));
    materializer.sync(pipeline.state(), *world);
    field = world->registry.get(handle);
    CHECK(field != nullptr);
    if (field != nullptr) {
        CHECK(field->registry_spawn_id != lifetime);
        CHECK(!field->minefield.initialized && field->section_mask == 4);
    }
    auto &remote = pipeline.state().upsert(0x12);
    remote.type_id = 5305;
    remote.x = 10; remote.y = 20; remote.z = 30;
    remote.net_stance_bits = 1;
    remote.move_input = 0xA5;
    remote.state_flags_known = true;
    remote.state_flags = 2; // dead remains a contact candidate
    auto &self = pipeline.state().upsert(0x15);
    self.type_id = 5305;
    std::vector<world::MinefieldActor> actors;
    materializer.fill_minefield_actors(pipeline.state(), 0x15, actors);
    CHECK(actors.size() == 1);
    CHECK(actors[0].handle == 0x12 && actors[0].move_order == 0x1A5);
    CHECK(actors[0].flags == 2 && actors[0].position.z == 30);
    return failures ? 1 : 0;
}
