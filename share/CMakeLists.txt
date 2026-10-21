# SPDX-License-Identifier: GPL-2.0-or-later
add_subdirectory(attributes)
add_subdirectory(branding)
add_subdirectory(examples)
add_subdirectory(filters)
add_subdirectory(fonts)
add_subdirectory(icons)
add_subdirectory(keys)
add_subdirectory(markers)
add_subdirectory(paint)
add_subdirectory(palettes)
add_subdirectory(screens)
add_subdirectory(symbols)
add_subdirectory(templates)
add_subdirectory(tutorials)
add_subdirectory(ui)
add_subdirectory(doc)


# extensions (git submodule!)
install(DIRECTORY extensions DESTINATION ${INKSCAPE_SHARE_INSTALL}
    COMPONENT extensions
    PATTERN ".git*" EXCLUDE
    PATTERN ".pylintrc" EXCLUDE
    PATTERN ".darglint" EXCLUDE
    PATTERN ".pre-commit-config.yaml" EXCLUDE
    PATTERN "tests" EXCLUDE
    PATTERN "MANIFEST.in" EXCLUDE
    PATTERN "pyproject.toml" EXCLUDE
    PATTERN "Makefile" EXCLUDE
    PATTERN "__pycache__" EXCLUDE
    PATTERN "tox.ini" EXCLUDE
    PATTERN "setup.cfg" EXCLUDE
    PATTERN "requirements.txt" EXCLUDE
    PATTERN "poetry.lock" EXCLUDE
    PATTERN "*.dox" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
    PATTERN "*.sh" EXCLUDE
    PATTERN "*.bat" EXCLUDE
    PATTERN "*.md" EXCLUDE
    PATTERN "print_win32_vector.*" EXCLUDE
    PATTERN "other/inkman" EXCLUDE
)

install(DIRECTORY extensions/other/inkman DESTINATION ${INKSCAPE_SHARE_INSTALL}/extensions/inkman/
    COMPONENT extension_manager
    PATTERN "tests" EXCLUDE
    PATTERN ".git*" EXCLUDE
    PATTERN "MANIFEST.in" EXCLUDE
    PATTERN "requirements.txt" EXCLUDE
    PATTERN "pyproject.toml" EXCLUDE
)

if(WIN32)
    install(
        FILES extensions/print_win32_vector.py extensions/print_win32_vector.inx
        DESTINATION ${INKSCAPE_SHARE_INSTALL}/extensions
        COMPONENT extensions
    )
  install(CODE
    "MESSAGE(\"Pre-compiling Python extensions to byte-code (.pyc files)\")
     execute_process(COMMAND \${CMAKE_INSTALL_PREFIX}/bin/python -m compileall -qq \${CMAKE_INSTALL_PREFIX}/${INKSCAPE_SHARE_INSTALL})"
    COMPONENT extensions)
endif()


# themes (git submodule!)
add_subdirectory(themes)


# appdata and desktop files, bash completions
if(UNIX)
  if (ENABLE_BINRELOC)
    set(INKSCAPE_ICONPATH "\${SNAP}/share/inkscape/branding/rosepen-logo.svg")
  else()
    set(INKSCAPE_ICONPATH "${FORK_APP_ID}")
  endif()
  set(INKSCAPE_MIMETYPE
	  "image/svg+xml"
	  "image/svg+xml-compressed"
	  "application/vnd.corel-draw"
	  "application/pdf"
	  "application/postscript"
	  "image/x-eps"
	  "application/illustrator"
	  "image/x-wmf"
	  "image/x-emf"
	  "application/x-xccx"
	  "application/x-xcdt"
	  "application/x-xcmx"
	  "image/x-xcdr"
	  "application/visio"
	  "application/x-visio"
	  "application/vnd.visio"
	  "application/vnd.ms-visio.viewer"
	  "application/visio.drawing"
	  "application/vsd"
	  "application/x-vsd"
	  "image/x-vsd"
	  ""
  )

  foreach(APP ${FORK_APP_ID} ${FORK_VIEWER_APP_ID})
    configure_file(${CMAKE_SOURCE_DIR}/fork/branding/${APP}.desktop.template
      ${CMAKE_BINARY_DIR}/${APP}.desktop.in)
  endforeach()

  if(ENABLE_NLS)
    foreach(APP ${FORK_APP_ID} ${FORK_VIEWER_APP_ID})
      add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${APP}.desktop
        DEPENDS ${LANGUAGES} ${CMAKE_BINARY_DIR}/${APP}.desktop.in
        COMMAND ${GETTEXT_MSGFMT_EXECUTABLE}
        --desktop
        --template ${CMAKE_BINARY_DIR}/${APP}.desktop.in
        -d ${CMAKE_SOURCE_DIR}/po
        -o ${CMAKE_BINARY_DIR}/${APP}.desktop
        --keyword=Name
        --keyword=GenericName
        --keyword=X-GNOME-FullName
        --keyword=Comment
        --keyword=Keywords
      )
    endforeach()
    add_custom_target(inkscape_desktop ALL DEPENDS
      ${CMAKE_BINARY_DIR}/${FORK_APP_ID}.desktop
      ${CMAKE_BINARY_DIR}/${FORK_VIEWER_APP_ID}.desktop
    )

    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${FORK_APP_ID}.metainfo.xml
      DEPENDS ${LANGUAGES} ${CMAKE_SOURCE_DIR}/fork/branding/${FORK_APP_ID}.metainfo.xml.in
      COMMAND ${GETTEXT_MSGFMT_EXECUTABLE}
      --xml
      --template ${CMAKE_SOURCE_DIR}/fork/branding/${FORK_APP_ID}.metainfo.xml.in
      -d ${CMAKE_SOURCE_DIR}/po -o ${CMAKE_BINARY_DIR}/${FORK_APP_ID}.metainfo.xml
    )
    add_custom_target(inkscape_appdata ALL DEPENDS ${CMAKE_BINARY_DIR}/${FORK_APP_ID}.metainfo.xml)
  else()
    configure_file(${CMAKE_SOURCE_DIR}/fork/branding/${FORK_APP_ID}.metainfo.xml.in ${CMAKE_BINARY_DIR}/${FORK_APP_ID}.metainfo.xml COPYONLY)
    foreach(APP ${FORK_APP_ID} ${FORK_VIEWER_APP_ID})
      configure_file(${CMAKE_BINARY_DIR}/${APP}.desktop.in
        ${CMAKE_BINARY_DIR}/${APP}.desktop COPYONLY)
    endforeach()
  endif()

  foreach(APP ${FORK_APP_ID} ${FORK_VIEWER_APP_ID})
    install(FILES ${CMAKE_BINARY_DIR}/${APP}.desktop
      DESTINATION ${SHARE_INSTALL}/applications)
  endforeach()

  install(FILES ${CMAKE_BINARY_DIR}/${FORK_APP_ID}.metainfo.xml
    DESTINATION ${SHARE_INSTALL}/metainfo)


  if(EXISTS ${CMAKE_SOURCE_DIR}/fork/branding/bash-completion/rosepen)
    install(FILES
      ${CMAKE_SOURCE_DIR}/fork/branding/bash-completion/rosepen
      DESTINATION ${SHARE_INSTALL}/bash-completion/completions)
  endif()

endif()


