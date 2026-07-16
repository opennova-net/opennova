add_library(opennova_renderer STATIC
    src/light_runtime.cpp
    src/material_eval.cpp
    src/material_classify.cpp
    src/object_shader_template.cpp
    src/particle_atlas.cpp
    src/particle_frame.cpp
    src/render_order.cpp
    src/uv_anim.cpp
)

target_include_directories(opennova_renderer
    PUBLIC  include
)

# material_eval.h (public) includes threedi_3di3.h -> real PUBLIC dependency.
target_link_libraries(opennova_renderer PUBLIC opennova_threedi)

# oed/material_descriptor.h provides the exact OED shader descriptor table used
# by material_classify.cpp - a header-only use, so we take the include dir
# rather than linking opennova_oed (which would drag ase+tdp for one header).
target_include_directories(opennova_renderer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../oed/include
)

target_compile_features(opennova_renderer PRIVATE cxx_std_17)
