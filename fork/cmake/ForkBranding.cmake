# SPDX-License-Identifier: GPL-2.0-or-later

set(FORK_PRODUCT_NAME "Rosepen" CACHE STRING "Fork product name")
set(FORK_BINARY_NAME "rosepen" CACHE STRING "Fork binary name")
set(FORK_APP_ID "io.github.toel1234.Rosepen" CACHE STRING "Fork application ID")
set(FORK_CONFIG_DIR_NAME "rosepen" CACHE STRING "Fork configuration directory name")
set(FORK_GETTEXT_DOMAIN "rosepen" CACHE STRING "Fork gettext domain")
set(FORK_VIEWER_PRODUCT_NAME "Rosepen Viewer" CACHE STRING "Fork viewer product name")
set(FORK_VIEWER_BINARY_NAME "rosepen-view" CACHE STRING "Fork viewer binary name")
set(FORK_VIEWER_APP_ID "io.github.toel1234.RosepenView" CACHE STRING "Fork viewer application ID")
set(FORK_HOMEPAGE_URL "https://github.com/toel1234/rosepen" CACHE STRING "Fork homepage")
set(FORK_BUGTRACKER_URL "https://github.com/toel1234/rosepen/issues" CACHE STRING "Fork bug tracker")

if(NOT CMAKE_PROJECT_NAME STREQUAL FORK_GETTEXT_DOMAIN)
    message(FATAL_ERROR "project() name must equal FORK_GETTEXT_DOMAIN (po/ installs ${CMAKE_PROJECT_NAME}.mo)")
endif()

message(STATUS "Fork branding: ${FORK_PRODUCT_NAME} (${FORK_APP_ID})")
