# Third-party libraries, as targets the engine links:
#   nova3::sdl3  nova3::sdl3_ttf  nova3::zlib  nova3::lua  nova3::shaderc  nova3::glm
#   nova3::shadercross (when NOVA3_SHADERCROSS)
# From the system (pkg-config / CMake packages) by default, or built from pinned sources
# with NOVA3_VENDORED (Windows and macOS builds, which have no package manager to lean on).

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

function(nova3_alias name target)
    if(NOT TARGET ${name})
        add_library(${name} INTERFACE IMPORTED GLOBAL)
        set_target_properties(${name} PROPERTIES INTERFACE_LINK_LIBRARIES ${target})
    endif()
endfunction()

if(NOVA3_VENDORED)
    # ---------------------------------------------------------------- SDL3
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.18
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(SDL3)
    nova3_alias(nova3::sdl3 SDL3::SDL3-static)

    # ---------------------------------------------------------------- SDL3_ttf (FreeType, HarfBuzz)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
    set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3_ttf
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
        GIT_TAG release-3.2.2
        GIT_SHALLOW ON
        GIT_SUBMODULES external/freetype external/harfbuzz)
    FetchContent_MakeAvailable(SDL3_ttf)
    # HarfBuzz turns some warnings into errors by pragma; newer compilers warn more.
    if(TARGET harfbuzz)
        target_compile_definitions(harfbuzz PRIVATE HB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR)
    endif()
    nova3_alias(nova3::sdl3_ttf SDL3_ttf::SDL3_ttf-static)

    # ---------------------------------------------------------------- zlib
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(zlib)
    target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    nova3_alias(nova3::zlib zlibstatic)

    # ---------------------------------------------------------------- Lua 5.1 (the game's)
    FetchContent_Declare(lua51
        URL https://www.lua.org/ftp/lua-5.1.5.tar.gz
        URL_HASH SHA256=2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333)
    FetchContent_MakeAvailable(lua51)
    file(GLOB LUA_SOURCES ${lua51_SOURCE_DIR}/src/*.c)
    list(REMOVE_ITEM LUA_SOURCES ${lua51_SOURCE_DIR}/src/lua.c ${lua51_SOURCE_DIR}/src/luac.c ${lua51_SOURCE_DIR}/src/print.c)
    add_library(lua51 STATIC ${LUA_SOURCES})
    target_include_directories(lua51 PUBLIC ${lua51_SOURCE_DIR}/src)
    if(UNIX AND NOT APPLE)
        target_compile_definitions(lua51 PRIVATE LUA_USE_POSIX)
    endif()
    nova3_alias(nova3::lua lua51)

    # ---------------------------------------------------------------- shaderc (glslang, SPIRV-Tools)
    FetchContent_Declare(spirv_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
        GIT_TAG vulkan-sdk-1.4.363.0
        GIT_SHALLOW ON
        SOURCE_SUBDIR do-not-add)
    FetchContent_Declare(spirv_tools
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
        GIT_TAG vulkan-sdk-1.4.363.0
        GIT_SHALLOW ON
        SOURCE_SUBDIR do-not-add)
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG vulkan-sdk-1.4.363.0
        GIT_SHALLOW ON
        SOURCE_SUBDIR do-not-add)
    FetchContent_Declare(shaderc
        GIT_REPOSITORY https://github.com/google/shaderc.git
        GIT_TAG v2026.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(spirv_headers spirv_tools glslang)
    set(SHADERC_SPIRV_HEADERS_DIR ${spirv_headers_SOURCE_DIR} CACHE PATH "" FORCE)
    set(SHADERC_SPIRV_TOOLS_DIR ${spirv_tools_SOURCE_DIR} CACHE PATH "" FORCE)
    set(SHADERC_GLSLANG_DIR ${glslang_SOURCE_DIR} CACHE PATH "" FORCE)
    set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
    # shaderc turns glslang's install on regardless (a generator expression for a bool), and
    # glslang's install exports need SPIRV-Tools': let all three install. The packages only
    # take the "game" component.
    set(SHADERC_SKIP_INSTALL OFF CACHE BOOL "" FORCE)
    set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
    set(SHADERC_ENABLE_WERROR_COMPILE OFF CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
    set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(shaderc)
    nova3_alias(nova3::shaderc shaderc)

    # ---------------------------------------------------------------- GLM (headers)
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 1.0.1
        GIT_SHALLOW ON
        SOURCE_SUBDIR do-not-add)
    FetchContent_MakeAvailable(glm)
    add_library(nova3_glm INTERFACE)
    target_include_directories(nova3_glm INTERFACE ${glm_SOURCE_DIR})
    nova3_alias(nova3::glm nova3_glm)
else()
    find_package(SDL3 REQUIRED CONFIG)
    find_package(ZLIB REQUIRED)
    find_path(GLM_INCLUDE_DIR glm/glm.hpp REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SHADERC REQUIRED IMPORTED_TARGET shaderc)
    # The game embeds Lua 5.1.4; its level scripts rely on 5.1 semantics.
    pkg_check_modules(LUA51 REQUIRED IMPORTED_TARGET lua5.1)
    pkg_check_modules(SDL3_TTF REQUIRED IMPORTED_TARGET sdl3-ttf)
    nova3_alias(nova3::sdl3 SDL3::SDL3)
    nova3_alias(nova3::sdl3_ttf PkgConfig::SDL3_TTF)
    nova3_alias(nova3::zlib ZLIB::ZLIB)
    nova3_alias(nova3::lua PkgConfig::LUA51)
    nova3_alias(nova3::shaderc PkgConfig::SHADERC)
    add_library(nova3_glm INTERFACE)
    target_include_directories(nova3_glm INTERFACE ${GLM_INCLUDE_DIR})
    nova3_alias(nova3::glm nova3_glm)
endif()

# SDL_shadercross: the SPIR-V shaders as MSL (Metal) or DXBC (Direct3D 12, through the
# system's d3dcompiler_47.dll) at run time. No releases yet: a pinned commit. Its own CMake
# wants every submodule (DXC included) or installed packages, so its one source file is
# built here against SPIRV-Cross's C API.
if(NOVA3_SHADERCROSS)
    set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(spirv_cross
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG vulkan-sdk-1.4.363.0
        GIT_SHALLOW ON)
    FetchContent_Declare(SDL3_shadercross
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_shadercross.git
        GIT_TAG 1ff05bec573988a98ef9e0260b4da44f512b8367
        GIT_SUBMODULES ""
        SOURCE_SUBDIR do-not-add)
    FetchContent_MakeAvailable(spirv_cross SDL3_shadercross)
    add_library(nova3_shadercross STATIC ${sdl3_shadercross_SOURCE_DIR}/src/SDL_shadercross.c)
    target_include_directories(nova3_shadercross PUBLIC ${sdl3_shadercross_SOURCE_DIR}/include)
    target_link_libraries(nova3_shadercross PUBLIC nova3::sdl3 PRIVATE spirv-cross-c)
    set_property(TARGET nova3_shadercross PROPERTY LINKER_LANGUAGE CXX)   # SPIRV-Cross is C++
    nova3_alias(nova3::shadercross nova3_shadercross)
endif()
