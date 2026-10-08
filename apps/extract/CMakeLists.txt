# opennova-extract — write named entries out of a mounted game root.
#
# Mounts a retail install exactly as a launch with its flags does (the .pff set
# with the optional expansion and the /d loose-override policy, through the
# engine vfs: mount_install) and writes each named entry to a directory, so the
# EFFECTIVE bytes the runtime serves (base + expansion override) can be inspected
# offline. Replaces the adm/weapon.def/.ptl dump probes; no parallel resolver.

add_executable(opennova_extract main.cpp)
set_target_properties(opennova_extract PROPERTIES OUTPUT_NAME opennova-extract)

target_link_libraries(opennova_extract PRIVATE opennova_base)

target_compile_features(opennova_extract PRIVATE cxx_std_17)
