#pragma once
// The skinned fixture's clips as the Blender add-on writes them (an .o3a): SKIN.adm, its
// reset and walk rows over the three bones of fixtures/threedi/o3d/skinned.o3d, the walk's
// trigger events on frames 1 and 3 (the last row repeats the one before, as retail's
// exporter writes it). The model preview's tests and the Preview window's import them with
// the model.

namespace editor_test {

inline constexpr const char *kSkinClips = R"(o3a 1
adm SKIN.adm
row anim_reset "reset"
row anim_walk_forward "walk"
clip reset
fps 30
flags 0x1
frames 1
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x0 0.9 1.7
clip walk
fps 30
flags 0x1
frames 4
bone -1 0 0 0 0.5 "BN01 Pelvis"
 k 0 0 0 1
 k 0 0 0.3826834 0.9238795
 k 0 0 0.7071068 0.7071068
 k 0 0 0.3826834 0.9238795
 k 0 0 0 1
bone 0 0 0 1 0.5 "BN02 Spine"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
bone 0 0 0 -1 0.5 "BN03 Leg"
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
 k 0 0 0 1
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x1 0.9 1.7
event 0 0 0 0x0 0.9 1.7
event 0 0 0 0x2 0.9 1.7
event 0 0 0 0x2 0.9 1.7
)";

} // namespace editor_test
