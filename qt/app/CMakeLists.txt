cmake_minimum_required(VERSION 3.21)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" ROOM237_VERSION)
string(STRIP "${ROOM237_VERSION}" ROOM237_VERSION)
if(APPLE)
  set(EXTRA_LANGUAGES OBJCXX)
endif()
project(room237 VERSION 0.0.0 LANGUAGES CXX ${EXTRA_LANGUAGES})
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia MultimediaWidgets Svg Concurrent)
if(UNIX AND NOT APPLE)
  find_package(Qt6 REQUIRED COMPONENTS DBus)
endif()
# The QA driver (qa.cpp, qa_fence.cpp, qa_native_panel) is what qt/tests talk to. It is part of Debug builds and of
# builds configured with -DROOM237_QA=ON; release packages never contain it.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(ROOM237_QA_DEFAULT ON)
else()
  set(ROOM237_QA_DEFAULT OFF)
endif()
option(ROOM237_QA "Build the QA driver used by qt/tests" ${ROOM237_QA_DEFAULT})
set(SOURCES
  src/qa_fence.h
  src/main.cpp src/core_client.cpp src/core_client.h src/thumbs.cpp src/thumbs.h
  src/model.cpp src/model.h src/gallery.cpp src/gallery.h src/duplicates.cpp src/duplicates.h
  src/viewer.cpp src/viewer.h src/window.cpp src/window.h
  src/qatrace.cpp src/qatrace.h src/root_watcher.h src/theme.cpp src/theme.h src/i18n.cpp src/i18n.h
  src/platform.cpp src/platform.h src/widgets.cpp src/widgets.h src/sidebar.cpp src/sidebar.h
  src/accessibility.cpp src/accessibility.h src/panels.cpp src/panels.h src/overlays.cpp src/overlays.h
  src/updater.cpp src/updater.h src/secure_media.cpp src/secure_media.h src/security.cpp src/security.h resources/app.qrc)
if(APPLE)
  list(APPEND SOURCES src/platform_mac.mm src/root_watcher.mm)
  set_source_files_properties(resources/icon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  list(APPEND SOURCES resources/icon.icns)
endif()
list(APPEND SOURCES src/root_watcher.cpp)
if(ROOM237_QA)
  list(APPEND SOURCES src/qa.cpp src/qa.h src/qa_core_server.cpp src/qa_core_server.h src/qa_fence.cpp src/qa_native_panel.h src/qa_scroll_log.cpp src/qa_scroll_log.h)
  if(APPLE)
    list(APPEND SOURCES src/qa_native_panel_mac.mm)
  endif()
endif()
if(WIN32)
  list(APPEND SOURCES resources/app.rc)
  enable_language(RC)
endif()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_BINARY_DIR}/core)
qt_add_executable(room237 WIN32 MACOSX_BUNDLE ${SOURCES})
target_compile_definitions(room237 PRIVATE ROOM237_VERSION="${ROOM237_VERSION}")
if(ROOM237_QA)
  target_compile_definitions(room237 PRIVATE ROOM237_QA)
endif()
target_link_libraries(room237 PRIVATE room237_core Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Svg)
# The update helper runs from the application directory, next to the executable.
add_dependencies(room237 room237-core)
add_custom_command(TARGET room237 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:room237-core> $<TARGET_FILE_DIR:room237>/$<TARGET_FILE_NAME:room237-core>)
if(APPLE)
  target_link_libraries(room237 PRIVATE "-framework CoreGraphics" "-framework ImageIO" "-framework Foundation" "-framework AppKit" "-framework CoreServices")
  set_target_properties(room237 PROPERTIES
    OUTPUT_NAME "Room237"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.dgazizullin.room237"
    MACOSX_BUNDLE_BUNDLE_NAME "Room237"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${ROOM237_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${ROOM237_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "icon.icns"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in")
elseif(UNIX)
  target_link_libraries(room237 PRIVATE Qt6::DBus)
endif()
