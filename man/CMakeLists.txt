# SPDX-License-Identifier: GPL-2.0-or-later

# Load AUTHORS file contents into $INKSCAPE_AUTHORS
file(READ ${CMAKE_SOURCE_DIR}/AUTHORS content)
string(REGEX REPLACE "^([^\n#]+)\n" "\\1;\n" content_list "${content}")
join(INKSCAPE_AUTHORS "," "${content_list}")


file(GLOB podfiles ${CMAKE_SOURCE_DIR}/fork/man/*.pod.in)
foreach(podfile ${podfiles})
    get_filename_component(basename ${podfile} NAME)
    string(REPLACE ".pod.in" "" basename ${basename})

    set(POD_IN ${podfile})
    set(POD_OUT ${CMAKE_CURRENT_BINARY_DIR}/${basename}.pod)

    configure_file(${POD_IN} ${POD_OUT})

    if(basename MATCHES "^${FORK_VIEWER_BINARY_NAME}")
        pod2man(${POD_OUT} ${INKSCAPE_VERSION} 1 "Rosepen Viewer Commands Manual")
    elseif(basename MATCHES "^${FORK_BINARY_NAME}")
        pod2man(${POD_OUT} ${INKSCAPE_VERSION} 1 "Rosepen Commands Manual")
    else()
        message(WARNING "unexpected podfile: ${podfile}")
    endif()
    # Pod2man.cmake's custom command has no DEPENDS on the configured pod, so a
    # pod edit would never regenerate the man page. Fork-side fix (fork-owned glob).
    if(basename MATCHES "^${FORK_VIEWER_BINARY_NAME}" OR basename MATCHES "^${FORK_BINARY_NAME}")
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${basename}.tmp APPEND DEPENDS ${POD_OUT})
    endif()
endforeach()
