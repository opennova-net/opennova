# Sim-consumed asset resolution (ADR 0028): the simulation's own portable
# model source. Builds world collision/occlusion models and bound radii from
# parsed .3di data, with a parse-once cache over the mounted resource index —
# the engine-side replacement for the adapter's render-object duck calls.
# Model B only (C++ static link); never part of the C ABI.
add_library(opennova_simassets STATIC
    src/adm_clip_index.cpp
    src/adm_root_motion.cpp
    src/adm_skeletal_clips.cpp
    src/item_traits.cpp
    src/model_builders.cpp
    src/sim_collision_pose.cpp
    src/sim_model_cache.cpp
)

target_include_directories(opennova_simassets
    PUBLIC  include
)

# model_builders.h names Threedi* and world::Collision/Occlusion types in its
# public signatures, item_traits.h names DefItemsFile, and the skeletal pose
# surface (adm_skeletal_clips.h, pose_inputs.h) names anim::Clip/PoseBone ->
# real PUBLIC dependencies. resource_index (and its vfs stack) is the cache's
# byte source; mission_format supplies only kItemIdOffset (.cpp-only).
target_link_libraries(opennova_simassets
    PUBLIC opennova_threedi opennova_world opennova_resource_index opennova_def
           opennova_anim
    PRIVATE opennova_bad opennova_io opennova_mission_format
)

target_compile_features(opennova_simassets PRIVATE cxx_std_17)
set_property(TARGET opennova_simassets PROPERTY POSITION_INDEPENDENT_CODE ON)
