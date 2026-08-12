cmake_minimum_required(VERSION 3.24)
project(omasnap VERSION 1.10.0 LANGUAGES C CXX)
include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Core Gui Test Widgets)
find_package(LayerShellQt REQUIRED)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(PROTOCOL_XMLS
  /usr/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
  /usr/share/wayland-protocols/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
  /usr/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
)
set(PROTOCOL_SOURCES)
foreach(PROTOCOL_XML IN LISTS PROTOCOL_XMLS)
  get_filename_component(PROTOCOL_NAME "${PROTOCOL_XML}" NAME_WE)
  set(PROTOCOL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL_NAME}-client-protocol.h")
  set(PROTOCOL_CODE "${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL_NAME}-protocol.c")
  add_custom_command(
    OUTPUT "${PROTOCOL_HEADER}" "${PROTOCOL_CODE}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${PROTOCOL_XML}" "${PROTOCOL_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${PROTOCOL_XML}" "${PROTOCOL_CODE}"
    DEPENDS "${PROTOCOL_XML}"
    VERBATIM
  )
  list(APPEND PROTOCOL_SOURCES "${PROTOCOL_HEADER}" "${PROTOCOL_CODE}")
endforeach()

qt_add_executable(omasnap
  main.cpp
  cli-path.cpp
  cli-path.hpp
  icons.cpp
  icons.hpp
  pin.cpp
  pin.hpp
  capture.cpp
  capture.hpp
  surface-capture.cpp
  ${PROTOCOL_SOURCES}
  editor.cpp
  editor.hpp
)
qt_add_resources(omasnap "capture-fonts"
  PREFIX "/fonts"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/Neucha.ttf"
)

target_include_directories(omasnap PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(omasnap PRIVATE cxx_std_23)
target_compile_options(omasnap PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(omasnap PRIVATE
  OMASNAP_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(omasnap PRIVATE
  Qt6::Concurrent
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  LayerShellQt::Interface
  PkgConfig::WaylandClient
)


qt_add_executable(omasnap-smoke
  editor-smoke.cpp
  icons.cpp
  icons.hpp
  transform-smoke.cpp
  transform-smoke.hpp
  cli-path.cpp
  cli-path.hpp
  capture.cpp
  capture.hpp
  surface-capture.cpp
  ${PROTOCOL_SOURCES}
  editor.cpp
  editor.hpp
)
qt_add_resources(omasnap-smoke "capture-fonts-smoke"
  PREFIX "/fonts"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/Neucha.ttf"
)
target_include_directories(omasnap-smoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(omasnap-smoke PRIVATE cxx_std_23)
target_compile_options(omasnap-smoke PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(omasnap-smoke PRIVATE
  Qt6::Concurrent
  Qt6::Core
  Qt6::Gui
  Qt6::Test
  Qt6::Widgets
  PkgConfig::WaylandClient
)

install(TARGETS omasnap
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES assets/OFL.txt
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/omasnap"
  RENAME Neucha-OFL.txt
)
install(FILES omasnap.desktop
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
