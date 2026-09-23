cmake_minimum_required(VERSION 3.15)

project(Slowscan
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# much faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(SLOWSCAN_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor in Slowscan.cpp and nothing ever references that
# object by name, so in an archive the linker is entitled to drop the whole
# translation unit -- and the result is a bundle that loads, exports plugMain,
# and reports that it contains no plugins. Do not "tidy" it to STATIC.
# ---------------------------------------------------------------------------
add_library(slowscan_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/Controls.h
    source/StoatworksAbout.h
    source/StoatworksAboutLinks.h
    source/StoatworksAboutParams.h
    source/Shaders.h
    source/shaders/Vertex.cpp
    source/shaders/Readback.cpp
    source/shaders/Compose.cpp
    source/sstv/Modes.h
    source/sstv/Modes.cpp
    source/sstv/Rng.h
    source/sstv/Transmitter.h
    source/sstv/Transmitter.cpp
    source/sstv/Channel.h
    source/sstv/Channel.cpp
    source/sstv/Receiver.h
    source/sstv/Receiver.cpp
    source/sstv/Engine.h
    source/sstv/Engine.cpp
    source/Slowscan.h
    source/Slowscan.cpp
)

target_include_directories(slowscan_core PUBLIC source)
target_link_libraries(slowscan_core PUBLIC ffgl::sdk)
target_compile_definitions(slowscan_core PUBLIC SLOWSCAN_VERSION="${PROJECT_VERSION}")
set_target_properties(slowscan_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers pull in <GL/glew.h>. The SDK links GLEW
# privately, so consumers need it too. macOS uses the system OpenGL framework
# and needs none of this. GLEW arrives through vcpkg.json.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(slowscan_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin. FFGL resolves one plugMain per binary.
# ---------------------------------------------------------------------------
function(slowscan_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    target_link_libraries(${target} PRIVATE slowscan_core)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL"
            "-framework Carbon"
            "-framework AppKit"
        )
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

slowscan_add_plugin(Slowscan source/PluginEntry.cpp
    "Slowscan" "com.stoatworks.ffgl.slowscan")

# ---------------------------------------------------------------------------
# The harness: the real plugin class in a real GL context, with no host.
#
# macOS only, because it stands up a CGL context directly. Most of what it
# checks needs no context at all -- the timing, the slant, the levels, the
# threshold, the header and the sync are all CPU code in source/sstv/ -- and
# those modes deliberately run before any GL call.
# ---------------------------------------------------------------------------
if(SLOWSCAN_BUILD_TOOLS AND APPLE)
    enable_testing()

    add_executable(sstest tools/sstest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(sstest PRIVATE slowscan_core z
        "-framework OpenGL"
        "-framework AppKit"
    )

    add_test(NAME timing      COMMAND sstest --timing)
    add_test(NAME slant       COMMAND sstest --slant)
    add_test(NAME levels      COMMAND sstest --levels)
    add_test(NAME threshold   COMMAND sstest --threshold)
    add_test(NAME progressive COMMAND sstest --progressive)
    add_test(NAME vis         COMMAND sstest --vis)
    add_test(NAME sync        COMMAND sstest --sync)
    add_test(NAME clock       COMMAND sstest --clock)
    add_test(NAME names       COMMAND sstest --names)
    add_test(NAME negative    COMMAND sstest --negative)
    add_test(NAME render      COMMAND sstest --render)
    add_test(NAME raster      COMMAND sstest --raster)
    add_test(NAME offline     COMMAND sstest --offline)
endif()

# ---------------------------------------------------------------------------
# `cmake --install build` drops the bundle where Arena reads it. Avenue reads
# ~/Documents/Resolume Avenue/Extra Effects instead; pass --prefix for that.
# ---------------------------------------------------------------------------
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()
install(TARGETS Slowscan
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
