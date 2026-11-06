cmake_minimum_required(VERSION 3.23)
project(JhonClient LANGUAGES CXX)

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
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(entt libhat minhook nlohmann_json)

add_subdirectory(external/imgui)

file(GLOB_RECURSE JHONCLIENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE JHONCLIENT_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_library(JhonClient SHARED
    ${JHONCLIENT_SOURCES}
    ${JHONCLIENT_HEADERS}
)

target_include_directories(JhonClient PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/d3d12"
    "${minhook_SOURCE_DIR}/include"
)

target_link_libraries(JhonClient PRIVATE
    imgui
    minhook
    libhat::libhat
    EnTT::EnTT
    nlohmann_json::nlohmann_json
    d3d11
    d3d12
    dxgi
    Psapi
)

target_compile_definitions(JhonClient PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set_target_properties(JhonClient PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "JhonClient"
)

if(MSVC)
    target_compile_options(JhonClient PRIVATE
        /W4 /permissive- /utf-8 /Zc:preprocessor /FIWindows.h /EHa
        $<$<CONFIG:Release>:/O2 /Ob2 /Oi /Ot /Oy /DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:/O2 /Ob2 /Oi /Ot /Oy /Zi /DNDEBUG>
    )
    target_link_options(JhonClient PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
        $<$<CONFIG:RelWithDebInfo>:/OPT:REF /OPT:ICF /DEBUG>
    )
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES
    ${JHONCLIENT_SOURCES}
    ${JHONCLIENT_HEADERS}
)
