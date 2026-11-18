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

find_package(Threads REQUIRED)
include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(XPT_BUILD_PLUGIN)
# --- libcurl ------------------------------------------------------------------------------------------------------------
# Linux: built from source with OpenSSL and zlib linked in statically, and kept private (-Wl,--exclude-libs,ALL below). Linking the
# system libcurl.so.4 broke under Steam's Linux runtime (2026-11-11): it puts its own libcurl.so.4 in the process, built with the
# CURL_OPENSSL_3 symbol versions, and the plugin wanted CURL_OPENSSL_4 ("version `CURL_OPENSSL_4' not found"). A bundled
# libcurl.so.4 would clash with the one already loaded. The certificate bundle is found at run time (http.cpp).
# Windows: vcpkg's static curl (Schannel). macOS: the system libcurl.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  option(XPT_BUNDLE_CURL "Build libcurl, OpenSSL and zlib into the plugin (Linux)" ON)
endif()
if(XPT_BUNDLE_CURL)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
  find_package(OpenSSL 3 REQUIRED)
  set(ZLIB_USE_STATIC_LIBS ON)
  foreach(opt BUILD_CURL_EXE BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL BUILD_EXAMPLES CURL_USE_LIBPSL CURL_USE_LIBSSH2
              CURL_USE_LIBSSH USE_LIBIDN2 USE_NGHTTP2 USE_NGTCP2 USE_QUICHE USE_MSH3 CURL_BROTLI CURL_ZSTD CURL_USE_GSSAPI ENABLE_ARES
              USE_LIBRTMP CURL_ENABLE_EXPORT_TARGET PICKY_COMPILER CURL_USE_PKGCONFIG CURL_CA_FALLBACK)
    set(${opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  foreach(opt CURL_USE_OPENSSL CURL_ZLIB CURL_DISABLE_LDAP CURL_DISABLE_LDAPS CURL_DISABLE_INSTALL CURL_DISABLE_DICT CURL_DISABLE_GOPHER
              CURL_DISABLE_IMAP CURL_DISABLE_MQTT CURL_DISABLE_POP3 CURL_DISABLE_RTSP CURL_DISABLE_SMB CURL_DISABLE_SMTP CURL_DISABLE_TELNET
              CURL_DISABLE_TFTP)
    set(${opt} ON CACHE BOOL "" FORCE)
  endforeach()
  set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
  set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
  FetchContent_Declare(curl
    URL https://github.com/curl/curl/releases/download/curl-8_14_1/curl-8.14.1.tar.xz
    URL_HASH SHA256=f4619a1e2474c4bbfedc88a7c2191209c8334b48fa1f4e53fd584cc12e9120dd)
  FetchContent_MakeAvailable(curl)
  if(TARGET CURL::libcurl_static)
    set(XPT_CURL CURL::libcurl_static)
  else()
    set(XPT_CURL CURL::libcurl)
  endif()
else()
  find_package(CURL REQUIRED)
  set(XPT_CURL CURL::libcurl)
endif()
endif()

# --- Dear ImGui (third_party/imgui, MIT) and the fonts, embedded at build time ------------------------------------------
add_library(xpt_imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp)
target_include_directories(xpt_imgui PUBLIC third_party/imgui)
target_compile_definitions(xpt_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS IMGUI_DISABLE_WIN32_FUNCTIONS)
set(XPT_ASSET_FILES
  ${CMAKE_SOURCE_DIR}/third_party/fonts/IBMPlexSans-Regular.ttf ${CMAKE_SOURCE_DIR}/third_party/fonts/IBMPlexSans-SemiBold.ttf
  ${CMAKE_SOURCE_DIR}/third_party/fonts/IBMPlexSans-Bold.ttf ${CMAKE_SOURCE_DIR}/third_party/fonts/JetBrainsMono-Regular.ttf
  ${CMAKE_SOURCE_DIR}/assets/wxplane_stacked.rgba ${CMAKE_SOURCE_DIR}/third_party/timezones/tzones.bin)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/xpt_assets.c
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/xpt_assets.c -P ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
  DEPENDS ${XPT_ASSET_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
  COMMENT "Embedding the tablet's fonts")
# The tablet itself: its screens and the triangle clipper. No X-Plane calls, so the tests build it too.
add_library(xpt_ui STATIC src/tabletui.cpp src/tabrender.cpp src/metar.cpp src/aptgeo.cpp src/tz.cpp ${CMAKE_BINARY_DIR}/xpt_assets.c)
target_include_directories(xpt_ui PUBLIC src)
target_link_libraries(xpt_ui PUBLIC xpt_imgui)

# --- The plugin: <build>/package/XPTablet/<lin_x64|mac_x64|win_x64>/XPTablet.xpl ------------------------------------------
if(XPT_BUILD_PLUGIN)
  add_library(XPTablet MODULE src/plugin.cpp src/json_mini.cpp src/http.cpp src/metarfetch.cpp src/browser.cpp src/tiles.cpp src/aptdat.cpp)
  target_include_directories(XPTablet PRIVATE "${XPLANE_SDK}/CHeaders/XPLM" third_party/stb)
  target_compile_definitions(XPTablet PRIVATE XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1)
  target_link_libraries(XPTablet PRIVATE xpt_ui ${XPT_CURL} Threads::Threads ${CMAKE_DL_LIBS})
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
    target_link_libraries(XPTablet PRIVATE rt)
  endif()
  set(XPT_OUT "${CMAKE_BINARY_DIR}/package/XPTablet/${XPT_PLATFORM_DIR}")
  add_custom_command(TARGET XPTablet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/LICENSES.txt" "${XPT_OUT}/LICENSES.txt")
  # the aircraft top views: a data file read at run time (GPL-3.0, with its licence and README), next to the platform folders
  add_custom_command(TARGET XPTablet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets/shapes" "${CMAKE_BINARY_DIR}/package/XPTablet/shapes")
  set_target_properties(XPTablet PROPERTIES
    PREFIX "" OUTPUT_NAME "XPTablet" SUFFIX ".xpl"
    LIBRARY_OUTPUT_DIRECTORY "${XPT_OUT}" RUNTIME_OUTPUT_DIRECTORY "${XPT_OUT}")
  foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${cfg} CFG)
    set_target_properties(XPTablet PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${CFG} "${XPT_OUT}" RUNTIME_OUTPUT_DIRECTORY_${CFG} "${XPT_OUT}")
  endforeach()

  # --- The web browser: xptbrowser, a program of its own beside the plugin, running X-Plane's own Chromium (CEF, loaded at
  # run time; only its C headers are needed to build). Not on macOS: X-Plane ships CEF there as a framework inside its app.
  if(NOT APPLE)
    add_executable(xptbrowser src/browser_helper.cpp)
    target_include_directories(xptbrowser PRIVATE src third_party/cef)
    target_compile_features(xptbrowser PRIVATE cxx_std_17)
    if(WIN32)
      target_compile_definitions(xptbrowser PRIVATE NOMINMAX=1 WIN32_LEAN_AND_MEAN=1 UNICODE=1 _UNICODE=1)
      # a windows program (no console window), started from main()
      set_target_properties(xptbrowser PROPERTIES WIN32_EXECUTABLE ON)
      if(MSVC)
        target_link_options(xptbrowser PRIVATE /ENTRY:mainCRTStartup)
        set_property(TARGET xptbrowser PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
      endif()
    else()
      target_link_libraries(xptbrowser PRIVATE ${CMAKE_DL_LIBS} rt Threads::Threads)
      target_link_options(xptbrowser PRIVATE -static-libgcc -static-libstdc++)
    endif()
    set_target_properties(xptbrowser PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${XPT_OUT}")
    foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER ${cfg} CFG)
      set_target_properties(xptbrowser PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} "${XPT_OUT}")
    endforeach()
    add_dependencies(XPTablet xptbrowser)
  endif()
endif()

# --- Tests (no X-Plane needed): ctest -------------------------------------------------------------------------------------
option(XPT_BUILD_TESTS "Build the off-line UI test" ON)
if(XPT_BUILD_TESTS)
  enable_testing()
  # every screen of the tablet, drawn off-line at several screen shapes (and as PNG pictures where zlib is available)
  add_executable(ui_shot tests/ui_shot.cpp)
  target_link_libraries(ui_shot PRIVATE xpt_ui)
  target_compile_definitions(ui_shot PRIVATE XPT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_compile_definitions(ui_shot PRIVATE XPT_UI_PNG=1)
    target_link_libraries(ui_shot PRIVATE ZLIB::ZLIB)
  endif()
  add_test(NAME tablet_screens COMMAND ui_shot ${CMAKE_BINARY_DIR})
endif()
