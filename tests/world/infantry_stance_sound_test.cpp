#include <runtime/world/infantry_sound.h>
#include <runtime/world/world.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>
#include <cstdio>
#include <cstring>
#include <memory>
using namespace opennova;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n",__LINE__,#c);return 1;} } while(0)
int main() {
    auto world = std::make_unique<world::World>();
    uint8_t previous = 0;
    const int32_t pos[3] = {1,2,3};
    const auto emit = [&](uint8_t bits,uint32_t flags=0,bool parent=false) {
        world::emit_stance_change_sound(*world,7,pos,previous,bits,flags,parent);
    };
    emit(0); CHECK(world->out.slot_sounds.empty());
    emit(2); CHECK(std::strcmp(world->out.slot_sounds.back().set_name,"TO_CROUCH")==0);
    emit(2); CHECK(world->out.slot_sounds.size()==1);
    emit(1); CHECK(std::strcmp(world->out.slot_sounds.back().set_name,"TO_PRONE")==0);
    emit(1,0x2000); CHECK(std::strcmp(world->out.slot_sounds.back().set_name,"TO_STAND")==0);
    emit(3); CHECK(previous==3 && world->out.slot_sounds.size()==3);
    emit(1,0,true); CHECK(previous==0 && world->out.slot_sounds.size()==4);
    CHECK(world->out.slot_sounds.back().source_handle==7 && world->out.slot_sounds.back().pos[2]==3);

    auto kernel = std::make_unique<mission::MissionKernel>();
    inmatch::JoinerRole role;
    role.bind(*kernel);
    role.create_runtime("Stance",inmatch::JoinRole::Player,"","");
    role.poll_preload();
    auto &row = role.runtime->state().upsert(2);
    row.cls = EntityClass::Player;
    row.net_stance_bits = 2;
    role.run_tick(inmatch::TickInput{});
    CHECK(kernel->world.out.slot_sounds.size()==1);
    CHECK(std::strcmp(kernel->world.out.slot_sounds[0].set_name,"TO_CROUCH")==0);
    kernel->world.out.slot_sounds.clear();
    role.run_tick(inmatch::TickInput{});
    CHECK(kernel->world.out.slot_sounds.empty());
    role.runtime->state().find(2)->net_stance_bits=1;
    role.run_tick(inmatch::TickInput{});
    CHECK(kernel->world.out.slot_sounds.size()==1);
    CHECK(std::strcmp(kernel->world.out.slot_sounds[0].set_name,"TO_PRONE")==0);
    return 0;
}
