cmake_minimum_required(VERSION 3.20)
project(Ordis CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2 SDL2_image SDL2_mixer SDL2_ttf)
find_program(GLSLC glslc REQUIRED)

# Shaders: <source> <output name> [defines...]
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
set(SPIRV)
function(add_shader SRC OUT)
  set(DEFS)
  foreach(D ${ARGN})
    list(APPEND DEFS -D${D})
  endforeach()
  add_custom_command(
    OUTPUT ${SHADER_OUT}/${OUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O ${DEFS} ${SHADER_DIR}/${SRC} -o ${SHADER_OUT}/${OUT}
    DEPENDS ${SHADER_DIR}/${SRC} ${SHADER_DIR}/common.glsl
    VERBATIM)
  set(SPIRV ${SPIRV} ${SHADER_OUT}/${OUT} PARENT_SCOPE)
endfunction()

add_shader(world.vert world.vert.spv)
add_shader(world.frag world.frag.spv)
add_shader(world.frag world_rt.frag.spv RT=1)
add_shader(sky.vert sky.vert.spv)
add_shader(sky.frag sky.frag.spv)
add_shader(fullscreen.vert fullscreen.vert.spv)
add_shader(post.frag post.frag.spv)
add_shader(accum.comp accum.comp.spv)
add_shader(ui.vert ui.vert.spv)
add_shader(ui.frag ui.frag.spv)
add_custom_target(shaders DEPENDS ${SPIRV})

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(ordis ${SOURCES})
add_dependencies(ordis shaders)
target_compile_options(ordis PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_compile_definitions(ordis PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)
target_link_libraries(ordis PRIVATE Vulkan::Vulkan PkgConfig::SDL Threads::Threads)

add_custom_command(TARGET ordis POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/assets)
