cmake_minimum_required(VERSION 3.24)
project(nova3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(ZLIB REQUIRED)
find_path(GLM_INCLUDE_DIR glm/glm.hpp REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SHADERC REQUIRED IMPORTED_TARGET shaderc)
# The game embeds Lua 5.1.4; its level scripts rely on 5.1 semantics.
pkg_check_modules(LUA51 REQUIRED IMPORTED_TARGET lua5.1)

# Engine core: file system, asset decoders and the game-shader compiler (no rendering).
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/assets/*.cpp src/shader/*.cpp src/script/*.cpp)
add_library(nova3core STATIC ${CORE_SOURCES})
target_include_directories(nova3core PUBLIC src ${GLM_INCLUDE_DIR})
target_link_libraries(nova3core PUBLIC ZLIB::ZLIB PkgConfig::SHADERC PkgConfig::LUA51)
target_compile_options(nova3core PRIVATE -Wall -Wextra)

# Shaders: GLSL -> SPIR-V (Vulkan). Metal/D3D12 builds will add other formats.
find_program(GLSLC glslc REQUIRED)
file(GLOB SHADER_SOURCES RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS src/render/shaders/*.vert src/render/shaders/*.frag)
set(SHADER_OUTPUTS)
foreach(src ${SHADER_SOURCES})
    get_filename_component(name ${src} NAME)
    set(out ${CMAKE_BINARY_DIR}/shaders/${name}.spv)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
        COMMAND ${GLSLC} -O ${CMAKE_SOURCE_DIR}/${src} -o ${out}
        DEPENDS ${src} VERBATIM)
    list(APPEND SHADER_OUTPUTS ${out})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})

# The level viewer.
file(GLOB VIEWER_SOURCES CONFIGURE_DEPENDS src/main.cpp src/render/*.cpp src/game/*.cpp)
if(VIEWER_SOURCES)
    add_executable(nova3 ${VIEWER_SOURCES})
    target_link_libraries(nova3 PRIVATE nova3core SDL3::SDL3)
    target_compile_options(nova3 PRIVATE -Wall -Wextra)
    add_dependencies(nova3 shaders)
endif()

# Command-line checks of the decoders against the Python reference tools.
add_executable(nova3-check tools/check.cpp)
target_link_libraries(nova3-check PRIVATE nova3core)
