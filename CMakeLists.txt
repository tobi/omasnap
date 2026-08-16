cmake_minimum_required(VERSION 3.24)
project(omasnap VERSION 1.12.0 LANGUAGES C CXX)
include(GNUInstallDirs)

option(OMASNAP_KDE "Build the KDE Plasma (KWin) backend" OFF)

find_package(PkgConfig REQUIRED)
find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Core Gui Test Widgets)
if(OMASNAP_KDE)
  find_package(Qt6 6.8 REQUIRED COMPONENTS DBus)
endif()
find_package(LayerShellQt REQUIRED)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(WAYLAND_PROTOCOLS_DIR "/usr/share/wayland-protocols" CACHE PATH
  "Root of the wayland-protocols XML tree")
set(PROTOCOL_XMLS
  ${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
  ${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
  ${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
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
  pin-file.cpp
  pin-file.hpp
  pin-layout.cpp
  pin-layout.hpp
  capture.cpp
  capture.hpp
  kwin.hpp
  eyedropper.cpp
  eyedropper.hpp
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
  kwin.hpp
  kwin-smoke.hpp
  surface-capture-smoke.cpp
  surface-capture-smoke.hpp
  ${PROTOCOL_SOURCES}
  editor.cpp
  editor.hpp
  eyedropper.cpp
  eyedropper.hpp
  pin-file.cpp
  pin-file.hpp
  pin-layout.cpp
  pin-layout.hpp
  pin-lifecycle-smoke.cpp
  pin-lifecycle-smoke.hpp
  pin-layout-smoke.cpp
  pin-layout-smoke.hpp
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

if(OMASNAP_KDE)
  # kwin.cpp declares a moc'd QObject for its DBus window-list sink.
  foreach(KDE_TARGET omasnap omasnap-smoke)
    target_sources(${KDE_TARGET} PRIVATE kwin.cpp)
    target_compile_definitions(${KDE_TARGET} PRIVATE OMASNAP_KDE)
    target_link_libraries(${KDE_TARGET} PRIVATE Qt6::DBus)
    set_target_properties(${KDE_TARGET} PROPERTIES AUTOMOC ON)
  endforeach()
  target_sources(omasnap-smoke PRIVATE kwin-smoke.cpp)
endif()

install(TARGETS omasnap
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES assets/OFL.txt
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/omasnap"
  RENAME Neucha-OFL.txt
)
if(OMASNAP_KDE)
  # KWin authorizes the ScreenShot2 DBus interface by resolving a desktop
  # entry whose Exec matches the calling binary, so Exec must be absolute.
  configure_file(omasnap-kde.desktop.in
    "${CMAKE_CURRENT_BINARY_DIR}/omasnap.desktop" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/omasnap.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
  )
else()
  install(FILES omasnap.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
  )
endif()
