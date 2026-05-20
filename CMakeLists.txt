cmake_minimum_required(VERSION 3.15)

# Detect version from git tags
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT GIT_TAG OR GIT_TAG STREQUAL "")
    set(GIT_TAG "0.1.0")
endif()
if(NOT GIT_COMMIT OR GIT_COMMIT STREQUAL "")
    set(GIT_COMMIT "unknown")
endif()

# Parse version components from tag (strip leading 'v' if present)
string(REGEX REPLACE "^v" "" VERSION_STRING "${GIT_TAG}")
string(REPLACE "." ";" VERSION_LIST "${VERSION_STRING}")
list(LENGTH VERSION_LIST VERSION_PARTS)

list(GET VERSION_LIST 0 VERSION_MAJOR)
if(VERSION_PARTS GREATER 1)
    list(GET VERSION_LIST 1 VERSION_MINOR)
else()
    set(VERSION_MINOR 0)
endif()
if(VERSION_PARTS GREATER 2)
    list(GET VERSION_LIST 2 VERSION_PATCH)
else()
    set(VERSION_PATCH 0)
endif()
set(VERSION_TWEAK 0)

project(GoPlayAlongSync
    VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.${VERSION_TWEAK}"
    LANGUAGES CXX
)

set(CMAKE_PROJECT_VERSION_COMMIT "${GIT_COMMIT}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate config header
configure_file(config.h.in config.h)

# Dependencies via FetchContent
include(FetchContent)

FetchContent_Declare(reaper-sdk
    GIT_REPOSITORY https://github.com/justinfrankel/reaper-sdk.git
    GIT_TAG        main
)

FetchContent_Declare(WDL
    GIT_REPOSITORY https://github.com/justinfrankel/WDL.git
    GIT_TAG        main
)

FetchContent_Declare(GSL
    GIT_REPOSITORY https://github.com/microsoft/GSL.git
    GIT_TAG        main
)

FetchContent_MakeAvailable(reaper-sdk WDL GSL)

# FetchContent sets lowercase variable names; retrieve them explicitly
FetchContent_GetProperties(WDL)
FetchContent_GetProperties(reaper-sdk)

# Shared library (REAPER plugin)
add_library(${PROJECT_NAME} SHARED)

target_include_directories(${PROJECT_NAME}
    PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${reaper-sdk_SOURCE_DIR}/sdk
    ${wdl_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
    Microsoft.GSL::GSL
)

add_subdirectory(src)

# Compiler flags
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /EHsc /O2 /W4 /wd4996)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -fstack-protector -fdiagnostics-color=always -Wall -Wextra)
endif()

# Strip "lib" prefix on all platforms
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

# Determine architecture suffix
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH_SUFFIX "x86_64")
else()
    set(ARCH_SUFFIX "x86")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "reaper_${PROJECT_NAME}-${ARCH_SUFFIX}"
)

# Install into REAPER UserPlugins directory
if(WIN32)
    set(REAPER_PLUGIN_DIR "$ENV{APPDATA}/REAPER/UserPlugins")
elseif(APPLE)
    set(REAPER_PLUGIN_DIR "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
else()
    set(REAPER_PLUGIN_DIR "$ENV{HOME}/.config/REAPER/UserPlugins")
endif()

install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION "${REAPER_PLUGIN_DIR}"
    RUNTIME DESTINATION "${REAPER_PLUGIN_DIR}"
)

# Packaging
set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
include(CPack)
