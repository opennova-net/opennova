add_library(opennova_renderer STATIC
    src/light_runtime.cpp
    src/material_eval.cpp
    src/material_classify.cpp
    src/object_shader_template.cpp
)

target_include_directories(opennova_renderer
    PUBLIC  include
)

# threedi_3di3.h + threedi_panm.h come from the threedi lib's include path.
# We don't link opennova_threedi here for the same reason as libs/oed:
# preserving --whole-archive de-dup at the shared lib level.
# oed/material_descriptor.h provides the exact OED shader descriptor table used
# by material_classify.cpp.
target_include_directories(opennova_renderer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../threedi/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../oed/include
)

target_compile_features(opennova_renderer PRIVATE cxx_std_17)
