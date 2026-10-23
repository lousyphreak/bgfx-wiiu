cmake_minimum_required(VERSION 3.21)
project(bgfx_wiiu LANGUAGES CXX)

get_filename_component(BGFX_DIR "../.." ABSOLUTE)
set(BX_DIR "${BGFX_DIR}/../bx-wiiu" CACHE PATH "bx source directory")
set(BIMG_DIR "${BGFX_DIR}/../bimg-wiiu" CACHE PATH "bimg source directory")
add_subdirectory(${BIMG_DIR}/scripts/wiiu ${CMAKE_CURRENT_BINARY_DIR}/bimg)

add_library(bgfx STATIC
    ${BGFX_DIR}/src/bgfx.cpp
    ${BGFX_DIR}/src/shader.cpp
    ${BGFX_DIR}/src/topology.cpp
    ${BGFX_DIR}/src/vertexlayout.cpp
    ${BGFX_DIR}/src/renderer_noop.cpp
    ${BGFX_DIR}/src/renderer_agc.cpp
    ${BGFX_DIR}/src/renderer_d3d11.cpp
    ${BGFX_DIR}/src/renderer_d3d12.cpp
    ${BGFX_DIR}/src/renderer_gnm.cpp
    ${BGFX_DIR}/src/renderer_gl.cpp
    ${BGFX_DIR}/src/renderer_mtl.cpp
    ${BGFX_DIR}/src/renderer_nvn.cpp
    ${BGFX_DIR}/src/renderer_vk.cpp
    ${BGFX_DIR}/src/renderer_webgpu.cpp
)
target_include_directories(bgfx PUBLIC ${BGFX_DIR}/include PRIVATE ${BGFX_DIR}/3rdparty)
target_compile_options(bgfx PRIVATE -fno-exceptions -fno-rtti)
target_compile_definitions(bgfx PUBLIC BGFX_CONFIG_MULTITHREADED=0
    PRIVATE BGFX_CONFIG_VIDEO=0 BGFX_CONFIG_MIP_GEN_FALLBACK=0 BGFX_CONFIG_BLIT_FALLBACK=0)
target_link_libraries(bgfx PUBLIC bimg)
