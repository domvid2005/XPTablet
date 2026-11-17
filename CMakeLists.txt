cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: nothing to install next to the plugin
if(CMAKE_HOST_APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()
project(XPTablet LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- X-Plane SDK ------------------------------------------------------------------------------------------------------
# Download the SDK from https://developer.x-plane.com/sdk/plugin-sdk-downloads/ and unzip it.
# Pass the unzipped folder: cmake -DXPLANE_SDK=/path/to/SDK ...   (it contains CHeaders/ and Libraries/)
# Without it only the tablet's UI and its tests are built (XPT_BUILD_PLUGIN is switched off).
set(XPLANE_SDK "" CACHE PATH "Folder containing CHeaders/ and Libraries/ of the X-Plane SDK (3.0.1 or newer)")
option(XPT_BUILD_PLUGIN "Build the X-Plane plugin (needs XPLANE_SDK)" ON)
if(XPT_BUILD_PLUGIN AND (NOT XPLANE_SDK OR NOT EXISTS "${XPLANE_SDK}/CHeaders/XPLM/XPLMDisplay.h"))
  message(FATAL_ERROR "Set -DXPLANE_SDK=<SDK folder> (CHeaders/XPLM/XPLMDisplay.h missing), or -DXPT_BUILD_PLUGIN=OFF for the UI tests only.")
endif()

# --- Dear ImGui (third_party/imgui, MIT) and the fonts, embedded at build time ------------------------------------------
add_library(xpt_imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp)
target_include_directories(xpt_imgui PUBLIC third_party/imgui)
target_compile_definitions(xpt_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS IMGUI_DISABLE_WIN32_FUNCTIONS)
set(XPT_ASSET_FILES
  ${CMAKE_SOURCE_DIR}/third_party/fonts/IBMPlexSans-Regular.ttf ${CMAKE_SOURCE_DIR}/third_party/fonts/IBMPlexSans-SemiBold.ttf
  ${CMAKE_SOURCE_DIR}/third_party/fonts/IBMPlexSans-Bold.ttf ${CMAKE_SOURCE_DIR}/third_party/fonts/JetBrainsMono-Regular.ttf)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/xpt_assets.c
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/xpt_assets.c -P ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
  DEPENDS ${XPT_ASSET_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
  COMMENT "Embedding the tablet's fonts")
# The tablet itself: its screens and the triangle clipper. No X-Plane calls, so the tests build it too.
add_library(xpt_ui STATIC src/tabletui.cpp src/tabrender.cpp ${CMAKE_BINARY_DIR}/xpt_assets.c)
target_include_directories(xpt_ui PUBLIC src)
target_link_libraries(xpt_ui PUBLIC xpt_imgui)

# --- The plugin: <build>/package/XPTablet/<lin_x64|mac_x64|win_x64>/XPTablet.xpl ------------------------------------------
if(XPT_BUILD_PLUGIN)
  add_library(XPTablet MODULE src/plugin.cpp src/json_mini.cpp)
  target_include_directories(XPTablet PRIVATE "${XPLANE_SDK}/CHeaders/XPLM")
  target_compile_definitions(XPTablet PRIVATE XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1)
  target_link_libraries(XPTablet PRIVATE xpt_ui ${CMAKE_DL_LIBS})
  if(WIN32)
    set(XPT_PLATFORM_DIR win_x64)
    target_compile_definitions(XPTablet PRIVATE IBM=1 NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
    target_link_libraries(XPTablet PRIVATE "${XPLANE_SDK}/Libraries/Win/XPLM_64.lib")
  elseif(APPLE)
    set(XPT_PLATFORM_DIR mac_x64)
    target_compile_definitions(XPTablet PRIVATE APL=1)
    target_link_options(XPTablet PRIVATE -undefined dynamic_lookup)
  else()
    set(XPT_PLATFORM_DIR lin_x64)
    target_compile_definitions(XPTablet PRIVATE LIN=1)
    # XPLM symbols are resolved by X-Plane when the plugin loads, so undefined references are expected.
    target_link_options(XPTablet PRIVATE -static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL)
  endif()
  set(XPT_OUT "${CMAKE_BINARY_DIR}/package/XPTablet/${XPT_PLATFORM_DIR}")
  add_custom_command(TARGET XPTablet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/LICENSES.txt" "${XPT_OUT}/LICENSES.txt")
  set_target_properties(XPTablet PROPERTIES
    PREFIX "" OUTPUT_NAME "XPTablet" SUFFIX ".xpl"
    LIBRARY_OUTPUT_DIRECTORY "${XPT_OUT}" RUNTIME_OUTPUT_DIRECTORY "${XPT_OUT}")
  foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${cfg} CFG)
    set_target_properties(XPTablet PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${CFG} "${XPT_OUT}" RUNTIME_OUTPUT_DIRECTORY_${CFG} "${XPT_OUT}")
  endforeach()
endif()

# --- Tests (no X-Plane needed): ctest -------------------------------------------------------------------------------------
option(XPT_BUILD_TESTS "Build the off-line UI test" ON)
if(XPT_BUILD_TESTS)
  enable_testing()
  # every screen of the tablet, drawn off-line at several screen shapes (and as PNG pictures where zlib is available)
  add_executable(ui_shot tests/ui_shot.cpp)
  target_link_libraries(ui_shot PRIVATE xpt_ui)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_compile_definitions(ui_shot PRIVATE XPT_UI_PNG=1)
    target_link_libraries(ui_shot PRIVATE ZLIB::ZLIB)
  endif()
  add_test(NAME tablet_screens COMMAND ui_shot ${CMAKE_BINARY_DIR})
endif()
