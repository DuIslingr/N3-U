cmake_minimum_required(VERSION 3.24)
project(nova3 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Linux uses its packages; Windows and macOS build every library from source.
if(WIN32 OR APPLE)
    set(NOVA3_PORTABLE_DEFAULT ON)
else()
    set(NOVA3_PORTABLE_DEFAULT OFF)
endif()
option(NOVA3_VENDORED "Build SDL3, SDL3_ttf, zlib, Lua 5.1, shaderc and GLM from pinned sources" ${NOVA3_PORTABLE_DEFAULT})
option(NOVA3_SHADERCROSS "Direct3D 12 and Metal through SDL_shadercross (else Vulkan only)" ${NOVA3_PORTABLE_DEFAULT})
include(cmake/Dependencies.cmake)

if(MSVC)
    set(NOVA3_WARNINGS /W4 /utf-8)
else()
    set(NOVA3_WARNINGS -Wall -Wextra)
endif()

# Engine core: file system, asset decoders and the game-shader compiler (no rendering).
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/assets/*.cpp src/shader/*.cpp src/script/*.cpp)
add_library(nova3core STATIC ${CORE_SOURCES})
target_include_directories(nova3core PUBLIC src)
target_link_libraries(nova3core PUBLIC nova3::zlib nova3::shaderc nova3::lua nova3::glm)
target_compile_options(nova3core PRIVATE ${NOVA3_WARNINGS})

# The renderer's own shaders: GLSL -> SPIR-V at build time, with the build machine's glslc
# (cross builds too); other GPU formats come from the SPIR-V at run time.
find_program(GLSLC glslc)
if(NOT GLSLC)
    if(TARGET glslc_exe AND NOT CMAKE_CROSSCOMPILING)
        set(GLSLC $<TARGET_FILE:glslc_exe>)
    else()
        message(FATAL_ERROR "glslc not found: install shaderc (or the Vulkan SDK) on the build machine")
    endif()
endif()
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

# The game.
file(GLOB VIEWER_SOURCES CONFIGURE_DEPENDS src/main.cpp src/app/*.cpp src/render/*.cpp src/game/*.cpp src/audio/*.cpp)
add_executable(nova3 ${VIEWER_SOURCES})
set_target_properties(nova3 PROPERTIES WIN32_EXECUTABLE ON)   # no console window of its own
target_link_libraries(nova3 PRIVATE nova3core nova3::sdl3 nova3::sdl3_ttf)
target_compile_options(nova3 PRIVATE ${NOVA3_WARNINGS})
if(NOVA3_SHADERCROSS)
    target_link_libraries(nova3 PRIVATE nova3::shadercross)
    target_compile_definitions(nova3 PRIVATE NOVA3_SHADERCROSS=1)
endif()
add_dependencies(nova3 shaders)
# The shaders sit beside the executable (SDL_GetBasePath()/shaders).
add_custom_command(TARGET nova3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:nova3>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADER_OUTPUTS} $<TARGET_FILE_DIR:nova3>/shaders
    VERBATIM)

# Command-line checks of the decoders against the Python reference tools.
add_executable(nova3-check tools/check.cpp)
target_link_libraries(nova3-check PRIVATE nova3core)
if(NOVA3_SHADERCROSS)
    target_link_libraries(nova3-check PRIVATE nova3::shadercross nova3::sdl3)
    target_compile_definitions(nova3-check PRIVATE NOVA3_SHADERCROSS=1)
endif()

# Installation and packages: the game, its shaders and the checks. The game's own data
# is never part of them; players point --data at their copy.
include(GNUInstallDirs)
if(WIN32 OR APPLE)
    set(NOVA3_INSTALL_BIN .)
else()
    set(NOVA3_INSTALL_BIN ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS nova3 nova3-check RUNTIME DESTINATION ${NOVA3_INSTALL_BIN} COMPONENT game)
install(FILES ${SHADER_OUTPUTS} DESTINATION ${NOVA3_INSTALL_BIN}/shaders COMPONENT game)
install(FILES ${CMAKE_SOURCE_DIR}/../README.md DESTINATION ${NOVA3_INSTALL_BIN} COMPONENT game OPTIONAL)
# Vendored libraries add install rules of their own (headers, static libraries); the
# packages hold only the game.
set(CPACK_COMPONENTS_ALL game)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_PACKAGE_NAME nova3)
set(CPACK_PACKAGE_VENDOR "N.O.V.A. 3 reimplementation")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A native reimplementation of N.O.V.A. 3 that runs the original game's files")
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
