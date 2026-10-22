# SPDX-License-Identifier: GPL-2.0-or-later
set(THEME rosepen)
install(FILES "index.theme" "LICENSE-TraceDraw-MIT" DESTINATION ${INKSCAPE_SHARE_INSTALL}/icons/${THEME} COMPONENT themes)
FILE(GLOB PIXMAP_FILES ${CMAKE_CURRENT_SOURCE_DIR}/scalable/actions/*.svg)
install(FILES ${PIXMAP_FILES} DESTINATION ${INKSCAPE_SHARE_INSTALL}/icons/${THEME}/scalable/actions COMPONENT themes)
