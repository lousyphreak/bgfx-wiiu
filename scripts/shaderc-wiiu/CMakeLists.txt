cmake_minimum_required(VERSION 3.21)
project(shaderc_wiiu_host LANGUAGES CXX)

get_filename_component(BGFX_DIR "../.." ABSOLUTE)
set(BX_DIR "${BGFX_DIR}/../bx-wiiu" CACHE PATH "bx source directory")
set(BIMG_DIR "${BGFX_DIR}/../bimg-wiiu" CACHE PATH "bimg source directory")
find_package(cafeglsl CONFIG REQUIRED)

add_library(bx_host STATIC ${BX_DIR}/src/amalgamated.cpp)
target_compile_features(bx_host PUBLIC cxx_std_20)
target_compile_definitions(bx_host PUBLIC BX_CONFIG_DEBUG=0 BX_CONFIG_SUPPORTS_THREADING=0)
target_compile_options(bx_host PRIVATE -ffunction-sections -fdata-sections)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i.86)$")
    target_compile_options(bx_host PUBLIC -msse4.2)
endif()
target_include_directories(bx_host PUBLIC ${BX_DIR}/include PRIVATE ${BX_DIR}/3rdparty)
target_link_libraries(bx_host PUBLIC ${CMAKE_DL_LIBS})

add_executable(shaderc
    ${BGFX_DIR}/tools/shaderc/pp.cpp
    ${BGFX_DIR}/tools/shaderc/shaderc.cpp
    ${BGFX_DIR}/tools/shaderc/shaderc_gx2.cpp
    ${BGFX_DIR}/tools/shaderc/shaderc_glsl.cpp
    ${BGFX_DIR}/tools/shaderc/shaderc_hlsl.cpp
    ${BGFX_DIR}/tools/shaderc/shaderc_dxil.cpp
    ${BGFX_DIR}/tools/shaderc/shaderc_metal.cpp
    ${BGFX_DIR}/tools/shaderc/shaderc_pssl.cpp
    ${BGFX_DIR}/tools/shaderc/shaderc_spirv.cpp
    ${BGFX_DIR}/tools/shaderc/shaderc_wgsl.cpp
    ${BGFX_DIR}/src/shader.cpp
    ${BGFX_DIR}/src/vertexlayout.cpp
)
target_include_directories(shaderc PRIVATE ${BGFX_DIR}/include ${BGFX_DIR}/3rdparty ${BIMG_DIR}/include)
target_compile_definitions(shaderc PRIVATE SHADERC_CONFIG_HAS_CAFEGLSL=1
    SHADERC_CONFIG_HAS_D3DCOMPILER=0 SHADERC_CONFIG_HAS_DXC=0
    SHADERC_CONFIG_HAS_GLSLANG=0 SHADERC_CONFIG_HAS_TINT=0
    BGFX_CONFIG_RENDERER_OPENGL=0 BGFX_CONFIG_MULTITHREADED=0)
target_compile_options(shaderc PRIVATE -ffunction-sections -fdata-sections)
target_link_options(shaderc PRIVATE -Wl,--gc-sections)
target_link_libraries(shaderc PRIVATE bx_host cafeglsl::cafeglsl)
install(TARGETS shaderc RUNTIME DESTINATION bin)
