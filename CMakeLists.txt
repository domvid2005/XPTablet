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
  ${CMAKE_SOURCE_DIR}/assets/wxplane_stacked.rgba ${CMAKE_SOURCE_DIR}/third_party/naturalearth/tzones.bin ${CMAKE_SOURCE_DIR}/third_party/naturalearth/basemap.bin ${CMAKE_SOURCE_DIR}/assets/xptablet_logo.rgba ${CMAKE_SOURCE_DIR}/assets/icons.rgba ${CMAKE_SOURCE_DIR}/assets/notify_48k_mono.pcm)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/xpt_assets.c
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/xpt_assets.c -P ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
  DEPENDS ${XPT_ASSET_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
  COMMENT "Embedding the tablet's fonts")
# The tablet itself: its screens and the triangle clipper. No X-Plane calls, so the tests build it too.
add_library(xpt_ui STATIC src/tabletui.cpp src/arcade.cpp src/acars.cpp src/localwire.cpp src/tabrender.cpp src/metar.cpp src/aptgeo.cpp src/airspace_parse.cpp src/airlines.cpp src/tz.cpp ${CMAKE_BINARY_DIR}/xpt_assets.c)
target_include_directories(xpt_ui PUBLIC src)
target_link_libraries(xpt_ui PUBLIC xpt_imgui)

# media controls: Windows' media transport controls, Linux's MPRIS (D-Bus, loaded at run time); none on macOS
if(WIN32)
  set(XPT_MEDIA_SRC src/media_win.cpp)
elseif(APPLE)
  set(XPT_MEDIA_SRC src/media_none.cpp)
else()
  set(XPT_MEDIA_SRC src/media_linux.cpp)
endif()

# --- Local Connect's server: HTTPS and WebSocket for the tablet on a phone (src/localserver.cpp), on Mbed TLS (Apache-2.0), built
# in as static libraries. One TLS library for the three platforms (the plugin's HTTPS client side differs on each).
option(XPT_LOCAL_CONNECT "Build Local Connect's server (fetches Mbed TLS)" ON)
if(XPT_LOCAL_CONNECT)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(GEN_FILES OFF CACHE BOOL "" FORCE)                 # (the release carries its generated sources: no Perl or Python needed)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
  set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
  set(MSVC_STATIC_RUNTIME ON CACHE BOOL "" FORCE)        # (the static CRT, as the plugin)
  set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG v3.6.8
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(mbedtls)
  add_library(xpt_local STATIC src/localserver.cpp src/localnet.cpp)
  target_include_directories(xpt_local PUBLIC src)
  target_link_libraries(xpt_local PUBLIC mbedtls mbedx509 mbedcrypto Threads::Threads)
  if(WIN32)
    target_compile_definitions(xpt_local PRIVATE NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
    target_link_libraries(xpt_local PUBLIC ws2_32 bcrypt)
  endif()
endif()

# --- The plugin: <build>/package/XPTablet/<lin_x64|mac_x64|win_x64>/XPTablet.xpl ------------------------------------------
if(XPT_BUILD_PLUGIN)
  add_library(XPTablet MODULE src/plugin.cpp src/json_mini.cpp src/http.cpp src/metarfetch.cpp src/browser.cpp src/tiles.cpp src/aptdat.cpp src/ourairports.cpp src/navdata.cpp src/vatsim.cpp src/vatsim_parse.cpp src/airspace.cpp src/simbrief.cpp src/media.cpp src/localnet.cpp ${XPT_MEDIA_SRC})
  target_include_directories(XPTablet PRIVATE "${XPLANE_SDK}/CHeaders/XPLM" third_party/stb)
  target_compile_definitions(XPTablet PRIVATE XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1)   # (400: X-Plane 12's sound, for the notification chime)
  target_link_libraries(XPTablet PRIVATE xpt_ui ${XPT_CURL} Threads::Threads ${CMAKE_DL_LIBS})
  if(XPT_LOCAL_CONNECT)
    target_link_libraries(XPTablet PRIVATE xpt_local)
    target_compile_definitions(XPTablet PRIVATE XPT_LOCAL_CONNECT=1)
  endif()
  if(WIN32)
    set(XPT_PLATFORM_DIR win_x64)
    target_compile_definitions(XPTablet PRIVATE IBM=1 NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
    target_link_libraries(XPTablet PRIVATE "${XPLANE_SDK}/Libraries/Win/XPLM_64.lib" ws2_32)
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
  add_custom_command(TARGET XPTablet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different   # XPTablet's own (freeware) licence
    "${CMAKE_SOURCE_DIR}/LICENSE.txt" "${XPT_OUT}/LICENSE.txt")
  # the aircraft top views: a data file read at run time (GPL-3.0, with its licence and README), next to the platform folders
  add_custom_command(TARGET XPTablet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets/shapes" "${CMAKE_BINARY_DIR}/package/XPTablet/shapes")
  # SkunkCrafts Updater: the plugin folder carries skunkcrafts_updater.cfg, which tells the updater app where XPTablet's update depot
  # is (the "release" branch of the GitHub repo, made by tools/skunkcrafts_release.sh) and which version this download is. Written by
  # the Linux build only (the CI bundle merges the three platforms' folders, one copy of it is enough).
  file(STRINGS "${CMAKE_SOURCE_DIR}/src/version.h" XPT_VERSION_LINE REGEX "#define XPT_VERSION ")
  string(REGEX REPLACE ".*\"(.*)\".*" "\\1" XPT_VERSION_STR "${XPT_VERSION_LINE}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/version.h")
  if(NOT WIN32 AND NOT APPLE)
    configure_file("${CMAKE_SOURCE_DIR}/tools/skunkcrafts_updater.cfg.in" "${CMAKE_BINARY_DIR}/package/XPTablet/skunkcrafts_updater.cfg" @ONLY NEWLINE_STYLE UNIX)
  endif()
  add_custom_command(TARGET XPTablet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory   # the Airbus checklists (GPL-2, with their licence)
    "${CMAKE_SOURCE_DIR}/assets/checklists" "${CMAKE_BINARY_DIR}/package/XPTablet/checklists")
  add_custom_command(TARGET XPTablet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory   # the user's folders, ready in the plugin's folder
    "${CMAKE_SOURCE_DIR}/assets/backgrounds" "${CMAKE_BINARY_DIR}/package/XPTablet/backgrounds")
  add_custom_command(TARGET XPTablet POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets/docs" "${CMAKE_BINARY_DIR}/package/XPTablet/docs")
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

# --- The emulator: the real plugin against a stand-in for X-Plane (emulator/), to try changes without X-Plane ------------------
# XPT_BUILD_EMULATOR: the desktop app (GLFW, fetched; needs the SDK headers only). XPT_BUILD_EMULATOR_HEADLESS: Linux, no screen (EGL),
# driven by a script; for tests and CI.
option(XPT_BUILD_EMULATOR "Build the desktop emulator (fetches GLFW)" OFF)
option(XPT_BUILD_EMULATOR_HEADLESS "Build the screen-less emulator (Linux, EGL)" OFF)
if((XPT_BUILD_EMULATOR OR XPT_BUILD_EMULATOR_HEADLESS) AND XPLANE_SDK AND EXISTS "${CMAKE_SOURCE_DIR}/emulator/xplm_emu.cpp")   # (the emulator is not on GitHub)
  # the plugin's own sources, built into the emulator, with the XPLM functions coming from the emulator instead of X-Plane
  add_library(xpt_emu_core STATIC
    src/plugin.cpp src/json_mini.cpp src/http.cpp src/metarfetch.cpp src/browser.cpp src/tiles.cpp src/aptdat.cpp src/ourairports.cpp
    src/navdata.cpp src/vatsim.cpp src/vatsim_parse.cpp src/airspace.cpp src/simbrief.cpp src/media.cpp ${XPT_MEDIA_SRC}
    emulator/xplm_emu.cpp emulator/emu_sim.cpp emulator/emu_panel.cpp emulator/emu_script.cpp emulator/emu_paths.cpp emulator/emu_png.cpp)
  target_include_directories(xpt_emu_core PUBLIC "${XPLANE_SDK}/CHeaders/XPLM" src emulator third_party/stb)
  target_compile_definitions(xpt_emu_core PUBLIC XPLM=1 XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1
    XPT_EMU_SOURCE_ASSETS="${CMAKE_SOURCE_DIR}/assets")
  target_link_libraries(xpt_emu_core PUBLIC xpt_ui ${XPT_CURL} Threads::Threads ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(xpt_emu_core PUBLIC IBM=1 NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
  elseif(APPLE)
    target_compile_definitions(xpt_emu_core PUBLIC APL=1)
  else()
    target_compile_definitions(xpt_emu_core PUBLIC LIN=1)
  endif()
  # XPTablet's files beside the emulator: its aircraft shapes (and the browser helper, where it is built)
  function(xpt_emu_files target)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/assets/shapes" "$<TARGET_FILE_DIR:${target}>/XPTablet/shapes")
    if(TARGET xptbrowser)
      add_dependencies(${target} xptbrowser)
      add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:xptbrowser>" "$<TARGET_FILE_DIR:${target}>/XPTablet/${XPT_PLATFORM_DIR}/$<TARGET_FILE_NAME:xptbrowser>")
    endif()
  endfunction()

  if(XPT_BUILD_EMULATOR)
    include(FetchContent)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)   # X11 on Linux (XWayland covers Wayland desktops)
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw)
    add_executable(xptablet_emulator emulator/main_glfw.cpp)
    target_link_libraries(xptablet_emulator PRIVATE xpt_emu_core glfw)
    if(WIN32)
      target_link_libraries(xptablet_emulator PRIVATE opengl32)
      set_property(TARGET xptablet_emulator PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif(APPLE)
      target_link_libraries(xptablet_emulator PRIVATE "-framework OpenGL")
    else()
      find_library(XPT_LIBGL NAMES GL libGL.so.1 REQUIRED)   # global, so the plugin finds the GL functions as in X-Plane
      target_link_libraries(xptablet_emulator PRIVATE ${XPT_LIBGL})
    endif()
    set_target_properties(xptablet_emulator PROPERTIES OUTPUT_NAME "XPTablet Emulator")
    xpt_emu_files(xptablet_emulator)
  endif()
  if(XPT_BUILD_EMULATOR_HEADLESS AND NOT WIN32 AND NOT APPLE)
    add_executable(xptablet_emu_headless emulator/main_headless.cpp)
    target_link_libraries(xptablet_emu_headless PRIVATE xpt_emu_core)
    xpt_emu_files(xptablet_emu_headless)
  endif()
endif()

# --- Tests (no X-Plane needed): ctest -------------------------------------------------------------------------------------
option(XPT_BUILD_TESTS "Build the off-line UI test" ON)
if(XPT_BUILD_TESTS)
  enable_testing()
  # every screen of the tablet, drawn off-line at several screen shapes (and as PNG pictures where zlib is available)
  add_executable(ui_shot tests/ui_shot.cpp src/simbrief.cpp src/json_mini.cpp)
  target_link_libraries(ui_shot PRIVATE xpt_ui)
  target_compile_definitions(ui_shot PRIVATE XPT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_compile_definitions(ui_shot PRIVATE XPT_UI_PNG=1)
    target_link_libraries(ui_shot PRIVATE ZLIB::ZLIB)
  endif()
  add_test(NAME tablet_screens COMMAND ui_shot ${CMAKE_BINARY_DIR})
  # Local Connect's wire format: the state and the requests go out and come back the same; what is cut short or made up is refused
  add_executable(wire_test tests/wire_test.cpp)
  target_link_libraries(wire_test PRIVATE xpt_ui)
  add_test(NAME local_connect_wire COMMAND wire_test)
  if(XPT_LOCAL_CONNECT)   # the server over real TLS on this machine: the page, pairing, the token, the refusals
    add_executable(localserver_test tests/localserver_test.cpp)
    target_link_libraries(localserver_test PRIVATE xpt_local)
    add_test(NAME local_connect_server COMMAND localserver_test)
  endif()
endif()
