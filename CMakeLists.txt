cmake_minimum_required(VERSION 3.23)
project(ChanterClient LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Keep dependency versions fixed so a future upstream update cannot silently
# change the ABI or APIs used by this client.
set(LIBHAT_TESTING OFF CACHE BOOL "" FORCE)
set(LIBHAT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBHAT_INSTALL_TARGET OFF CACHE BOOL "" FORCE)
set(LIBHAT_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.14.0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    libhat
    GIT_REPOSITORY https://github.com/BasedInc/libhat.git
    GIT_TAG v0.11.1
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.3
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    expected_lite
    GIT_REPOSITORY https://github.com/nonstd-lite/expected-lite.git
    GIT_TAG v0.9.0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(entt libhat minhook expected_lite nlohmann_json)

# MinHook v1.3.3 does not ship a CMakeLists.txt. Build its x64 sources
# directly instead of relying on a prebuilt library from another machine.
if(NOT TARGET minhook)
    add_library(minhook STATIC
        "${minhook_SOURCE_DIR}/src/buffer.c"
        "${minhook_SOURCE_DIR}/src/hook.c"
        "${minhook_SOURCE_DIR}/src/trampoline.c"
        "${minhook_SOURCE_DIR}/src/hde/hde64.c"
    )
    target_include_directories(minhook
        PUBLIC "${minhook_SOURCE_DIR}/include"
        PRIVATE
            "${minhook_SOURCE_DIR}/src"
            "${minhook_SOURCE_DIR}/src/hde"
    )
    set_target_properties(minhook PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        OUTPUT_NAME "MinHook"
    )
endif()

add_subdirectory(external/imgui)

file(GLOB_RECURSE CHANTERCLIENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE CHANTERCLIENT_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_library(ChanterClient SHARED
    ${CHANTERCLIENT_SOURCES}
    ${CHANTERCLIENT_HEADERS}
)

target_include_directories(ChanterClient PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/d3d12"
    "${minhook_SOURCE_DIR}/include"
    "${expected_lite_SOURCE_DIR}/include"
)

target_link_libraries(ChanterClient PRIVATE
    imgui
    minhook
    libhat::libhat
    EnTT::EnTT
    nlohmann_json::nlohmann_json
    d3d11
    d3d12
    dxgi
    Psapi
    windowscodecs
    ole32
    shell32
)

target_compile_definitions(ChanterClient PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set_target_properties(ChanterClient PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ChanterClient"
)

if(MSVC)
    target_compile_options(ChanterClient PRIVATE
        /W4 /permissive- /utf-8 /Zc:preprocessor /FIWindows.h /EHa
        $<$<CONFIG:Release>:/O2 /Ob2 /Oi /Ot /Oy /Zi /DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:/O2 /Ob2 /Oi /Ot /Oy /Zi /DNDEBUG>
    )
    target_link_options(ChanterClient PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FULL /OPT:REF /OPT:ICF>
        $<$<CONFIG:RelWithDebInfo>:/OPT:REF /OPT:ICF /DEBUG>
    )
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES
    ${CHANTERCLIENT_SOURCES}
    ${CHANTERCLIENT_HEADERS}
)
