# Build targets for the existing editor/rest CTest partition. Keep registration
# independent of selection so the suite inventory still sees every test.
function(opennova_collect_build_targets directory output)
    get_property(collected DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        opennova_collect_build_targets("${child}" child_targets)
        list(APPEND collected ${child_targets})
    endforeach()
    set(${output} "${collected}" PARENT_SCOPE)
endfunction()

opennova_collect_build_targets("${PROJECT_SOURCE_DIR}/tests" all_test_targets)
opennova_collect_build_targets("${PROJECT_SOURCE_DIR}/tests/editor" editor_test_targets)
set(rest_test_targets ${all_test_targets})
list(REMOVE_ITEM rest_test_targets ${editor_test_targets})

# Some test commands invoke an app directly or through $<TARGET_FILE:...>.
# Preserve the full root build's app coverage as well as those runtime inputs.
get_property(root_directories DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)
foreach(directory IN LISTS root_directories)
    string(FIND "${directory}" "${PROJECT_SOURCE_DIR}/apps/" apps_prefix)
    if(apps_prefix EQUAL 0)
        opennova_collect_build_targets("${directory}" app_targets)
        foreach(target IN LISTS app_targets)
            get_target_property(kind "${target}" TYPE)
            if(kind STREQUAL "EXECUTABLE")
                list(APPEND rest_test_targets "${target}")
            endif()
        endforeach()
    endif()
endforeach()

add_custom_target(opennova_tests_editor DEPENDS ${editor_test_targets})
add_custom_target(opennova_tests_rest DEPENDS ${rest_test_targets})
