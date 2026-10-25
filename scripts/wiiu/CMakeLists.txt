cmake_minimum_required(VERSION 3.21)
project(bgfx_wiiu LANGUAGES CXX)

get_filename_component(BGFX_DIR "../.." ABSOLUTE)
set(BX_DIR "${BGFX_DIR}/../bx-wiiu" CACHE PATH "bx source directory")
set(BIMG_DIR "${BGFX_DIR}/../bimg-wiiu" CACHE PATH "bimg source directory")
set(BGFX_SHADERC "${BGFX_DIR}/../../bin/shaderc" CACHE FILEPATH "Host shaderc with CafeGLSL/GX2 support")
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
    ${BGFX_DIR}/src/renderer_gx2.cpp
)

set(generated_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(embedded_includes "")
foreach(shader IN ITEMS
    cs_gx2_indirect cs_gx2_indirect_indexed cs_gx2_indirect_dispatch
    vs_debugfont fs_debugfont vs_resolve_depth fs_resolve_depth fs_resolve_depth_array
    fs_gx2_mip_2d fs_gx2_mip_array fs_gx2_mip_3d
    fs_gx2_copy_depth_2d fs_gx2_copy_depth_array fs_gx2_copy_depth_3d
    fs_gx2_copy_uint_2d fs_gx2_copy_uint_array fs_gx2_copy_uint_3d
    fs_gx2_copy_int_2d fs_gx2_copy_int_array fs_gx2_copy_int_3d
    fs_gx2_clear_uint fs_gx2_clear_int
    fs_gx2_resolve_color_2d fs_gx2_resolve_color_array
    fs_gx2_copy_msaa_2d fs_gx2_copy_msaa_array
    vs_clear fs_clear0 fs_clear1 fs_clear2 fs_clear3 fs_clear4 fs_clear5 fs_clear6 fs_clear7)
    if(shader MATCHES "^cs_")
        set(stage compute)
    elseif(shader MATCHES "^vs_")
        set(stage vertex)
    else()
        set(stage fragment)
    endif()
    set(raw_options "")
    set(depfile_options DEPFILE ${generated_dir}/${shader}.gx2.bin.h.d)
    if(shader MATCHES "^fs_gx2_(copy|clear|resolve)_")
        set(raw_options --raw)
        set(depfile_options "")
    endif()
    set(output ${generated_dir}/${shader}.gx2.bin.h)
    add_custom_command(OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${generated_dir}
        COMMAND ${BGFX_SHADERC} -f ${BGFX_DIR}/src/${shader}.sc -o ${output}
            ${raw_options} --type ${stage} --platform wiiu -p gx2 --bin2c ${shader}_gx2
            --varyingdef ${BGFX_DIR}/src/varying.def.sc -i ${BGFX_DIR}/src --depends
        DEPENDS ${BGFX_SHADERC} ${BGFX_DIR}/src/${shader}.sc
            ${BGFX_DIR}/src/varying.def.sc ${BGFX_DIR}/src/bgfx_shader.sh
        ${depfile_options}
        VERBATIM)
    target_sources(bgfx PRIVATE ${output})
    string(APPEND embedded_includes "#include \"${shader}.gx2.bin.h\"\n")
endforeach()
file(GENERATE OUTPUT ${generated_dir}/gx2_embedded.bin.h CONTENT "${embedded_includes}")
target_include_directories(bgfx PUBLIC ${BGFX_DIR}/include PRIVATE ${BGFX_DIR}/3rdparty ${generated_dir})
target_compile_options(bgfx PRIVATE -fno-exceptions -fno-rtti)
target_compile_definitions(bgfx PUBLIC BGFX_CONFIG_MULTITHREADED=0
    PRIVATE BGFX_CONFIG_VIDEO=0 BGFX_CONFIG_MIP_GEN_FALLBACK=0 BGFX_CONFIG_BLIT_FALLBACK=0)
target_link_libraries(bgfx PUBLIC bimg)

option(BGFX_BUILD_EXAMPLES "Build original bgfx examples with the Wii U SDL3 entry" OFF)
if(BGFX_BUILD_EXAMPLES)
    include("${CMAKE_CURRENT_LIST_DIR}/examples.cmake")
endif()
