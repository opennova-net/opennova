# rgbcx (Rich Geldreich's BC1-5 block encoder, public domain or MIT, you choose: the
# license block at the end of rgbcx.h), fetched from GitHub at configure time.
#
# The authoring encoder behind every DXT texture the tools write (the image import and
# `opennova-3di texture`, engine/runtime/renderer/dxt_encode): a tooling choice,
# not a port. The game's own DXT path (the terrain atlas made DXT at load) stays the
# D3DX codec's port, engine/runtime/renderer/texture_dxt, which nothing here replaces.
#
# Pinned to an exact commit of richgel999/bc7enc (rgbcx.h v1.12, the single-header form
# with its implementation, and its rgbcx_table4.h) and verified by hash. To bump it,
# change the commit in the URL and the URL_HASH together. For offline builds, point
# -DFETCHCONTENT_SOURCE_DIR_RGBCX=<dir> at a pre-extracted copy.

include(FetchContent)

# richgel999/bc7enc @ f66c2e4 (2022-07-04): rgbcx.h v1.12. The archive's own CMakeLists
# builds its sample tools, so SOURCE_SUBDIR names a folder it lacks: MakeAvailable then
# populates the sources and adds no project of theirs.
FetchContent_Declare(rgbcx
    URL      https://github.com/richgel999/bc7enc/archive/f66c2e489b07138f2673a2fb3d27c1aa1d565c48.zip
    URL_HASH SHA256=8f43a3d11cf8c7febd5e8d653ac6039f8d9a91ff00d6400fc571a686aa9d68e2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR opennova-header-only
)
FetchContent_MakeAvailable(rgbcx)

# Only the header is taken, its implementation compiled once here (rgbcx.cpp defines
# RGBCX_IMPLEMENTATION).
add_library(opennova_rgbcx STATIC rgbcx.cpp)

target_include_directories(opennova_rgbcx
    PUBLIC
        "${rgbcx_SOURCE_DIR}"
)

target_compile_features(opennova_rgbcx PUBLIC cxx_std_17)

set_target_properties(opennova_rgbcx PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Suppress the noise that comes with vendored sources.
if(MSVC)
    target_compile_options(opennova_rgbcx PRIVATE /W0)
else()
    target_compile_options(opennova_rgbcx PRIVATE -w)
endif()
