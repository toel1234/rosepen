# SPDX-License-Identifier: GPL-2.0-or-later

# Rosepen translation overlays: each fork/po/<lang>.po holds strings for
# fork-only UI. It is merged over the upstream po/<lang>.po (overlay entries
# win) and installed after it, replacing the upstream catalog for <lang>.
find_program(MSGCAT_EXECUTABLE msgcat REQUIRED)

file(GLOB OVERLAYS ${CMAKE_CURRENT_SOURCE_DIR}/*.po)
foreach(overlay ${OVERLAYS})
    get_filename_component(language ${overlay} NAME_WE)
    set(upstream ${CMAKE_SOURCE_DIR}/po/${language}.po)
    set(merged ${CMAKE_CURRENT_BINARY_DIR}/${language}.po)
    set(gmofile ${CMAKE_CURRENT_BINARY_DIR}/${language}.gmo)
    add_custom_command(OUTPUT ${gmofile}
        COMMAND ${MSGCAT_EXECUTABLE} --use-first -o ${merged} ${overlay} ${upstream}
        COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -o ${gmofile} ${merged}
        DEPENDS ${overlay} ${upstream}
        COMMENT "Merging Rosepen ${language} translations")
    add_custom_target(rosepen_po_${language} ALL DEPENDS ${gmofile})
    # install(FILES) would skip the copy as "Up-to-date" when the upstream
    # catalog was installed to the same path just before, so copy explicitly.
    set(destination "${PACKAGE_LOCALE_DIR}/${language}/LC_MESSAGES/${CMAKE_PROJECT_NAME}.mo")
    install(CODE "
        set(dest \"${destination}\")
        if(NOT IS_ABSOLUTE \"\${dest}\")
            set(dest \"\${CMAKE_INSTALL_PREFIX}/\${dest}\")
        endif()
        set(dest \"\$ENV{DESTDIR}\${dest}\")
        message(STATUS \"Installing Rosepen overlay: \${dest}\")
        file(COPY_FILE \"${gmofile}\" \"\${dest}\")"
        COMPONENT translations)
endforeach()
