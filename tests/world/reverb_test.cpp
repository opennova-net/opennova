#include <runtime/world/reverb.h>
#include <cstdio>
using namespace opennova::world;
int main() {
    ReverbState s; s.mission = 2;
    const int32_t inside[3] = {5,5,5}, edge[3] = {0,5,5}, outside[3] = {11,5,5};
    s.update(inside,0); if(s.selected!=2)return 1;
    s.update(inside,7); if(s.selected!=7)return 2;
    s.regions.push_back({{0,0,0},{10,10,10},3});
    s.update(inside,7); if(s.selected!=3)return 3;
    s.regions.push_back({{0,0,0},{10,10,10},0});
    s.update(inside,7); if(s.selected!=0)return 4;
    s.update(edge,7); if(s.selected!=7)return 5;
    s.update(outside,0); if(s.selected!=2)return 6;
    std::puts("reverb selector: mission/building/overlap/zero/edge/exit passed");
}
