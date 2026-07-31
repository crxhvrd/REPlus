cmake_minimum_required(VERSION 3.15)

project(RockstarEditorPlus)

# STATIC CRT. Not a preference - a correctness requirement for a .asi.
#
# An ASI is injected into someone else's process and does not get to choose which
# msvcp140.dll is already loaded there. MSVC's STL requires the runtime DLL to be
# AT LEAST AS NEW as the toolset that compiled the headers, and a host is free to
# ship an older one: FiveM carries msvcp140 14.34 in FiveM.app\bin, which wins
# over the system's 14.51, so a build made with the current toolset binds its
# inlined 14.51 STL to a 2022-era runtime. That combination crashed on the first
# std::mutex lock the mod performed - inside MSVCP140!_Mtx_lock, reproducibly,
# and immune to every timing and threading change tried, because it is an ABI
# mismatch rather than a race.
#
# Linking the CRT statically ends the whole class of problem: the mod carries its
# own STL and no longer cares what the host loaded. It also drops the VC++
# redistributable from the install requirements, which is worth having on its
# own for singleplayer users.
#
# Everything linked in must agree, hence CMAKE_MSVC_RUNTIME_LIBRARY set before
# any target is declared (minhook included) rather than per-target.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB_RECURSE PROJECT_SOURCE_FILES "src/*.h" "src/*.cpp")
# Exclude the vendored minhook tree from the ASI target (it builds separately).
list(FILTER PROJECT_SOURCE_FILES EXCLUDE REGEX "src/vendor/")

macro(GroupSources curdir groupindex)
    file(GLOB children RELATIVE ${curdir} ${curdir}/*)
    foreach(child ${children})
        if(IS_DIRECTORY ${curdir}/${child})
            GroupSources(${curdir}/${child} ${groupindex}/${child})
        else()
            string(REPLACE "/" "\\" groupname ${groupindex})
            source_group(${groupname} FILES ${curdir}/${child})
        endif()
    endforeach()
endmacro()

GroupSources(${CMAKE_SOURCE_DIR}/src "Source Files")

add_subdirectory("src/vendor/minhook")

add_library(RockstarEditorPlus SHARED ${PROJECT_SOURCE_FILES} fx_asi_build.rc)
add_dependencies(RockstarEditorPlus minhook)

include_directories(
    ./src
)

set_target_properties(RockstarEditorPlus PROPERTIES
    OUTPUT_NAME "RockstarEditorPlus"
    SUFFIX ".asi"
    CXX_STANDARD 20
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BIN/"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/BIN/Debug/"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/BIN/Release/"
)

# Put the ini next to the built .asi on every build.
#
# The ini is the mod's whole configuration surface and carries the documentation
# for every option, so the authoritative copy lives in the repo. But it also has
# to sit BESIDE the .asi - that is where the mod reads it from, and where the
# in-editor menu writes toggles back to - so shipping it means copying it there.
#
# Doing it as a post-build step rather than keeping the only copy in BIN/Release
# is deliberate: that directory is build output and a clean wipes it, which is
# exactly how the file went missing once already.
add_custom_command(TARGET RockstarEditorPlus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/RockstarEditorPlus.ini"
            "$<TARGET_FILE_DIR:RockstarEditorPlus>/RockstarEditorPlus.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Render.ini"
            "$<TARGET_FILE_DIR:RockstarEditorPlus>/Render.ini"
    COMMENT "Copying the shipped .ini files next to the .asi")

set_target_properties( minhook
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/LIB"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/LIB"
)

# smoothblend.cpp guards the detour body with __try/__except; MSVC needs SEH
# enabled and it must not be compiled with /EHsc-only semantics there.
target_compile_options(RockstarEditorPlus PRIVATE /utf-8)

set(CMAKE_CONFIGURATION_TYPES Debug Release RelWithDebInfo)
if(ENABLE_LOGS)
    target_compile_definitions(RockstarEditorPlus PRIVATE _LOG)
endif()

target_link_directories(RockstarEditorPlus PUBLIC "${CMAKE_BINARY_DIR}/LIB")
target_link_libraries(RockstarEditorPlus version.lib shell32.lib debug minhook.x64d.lib optimized minhook.x64.lib)
