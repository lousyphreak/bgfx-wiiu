# Original bgfx applications, sharing one SDL3/GX2 entry implementation.
find_package(SDL3 REQUIRED CONFIG)
set(BGFX_WIIU_EXAMPLES "00-helloworld;01-cubes" CACHE STRING "Numbered example directories to build")
set(common_dir "${BGFX_DIR}/examples/common")
set(example_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/example-generated")

file(GLOB imgui_sources CONFIGURE_DEPENDS "${BGFX_DIR}/3rdparty/dear-imgui/*.cpp")
file(GLOB meshoptimizer_sources CONFIGURE_DEPENDS "${BGFX_DIR}/3rdparty/meshoptimizer/src/*.cpp")
add_library(example-common STATIC
    ${common_dir}/entry/entry.cpp
    ${common_dir}/entry/entry_wiiu.cpp
    ${common_dir}/entry/cmd.cpp
    ${common_dir}/entry/input.cpp
    ${common_dir}/entry/dialog.cpp
    ${common_dir}/bgfx_utils.cpp
    ${common_dir}/camera.cpp
    ${common_dir}/example-glue.cpp
    ${common_dir}/imgui/imgui.cpp
    ${imgui_sources}
    ${meshoptimizer_sources})
target_include_directories(example-common PUBLIC "${common_dir}" PRIVATE "${BGFX_DIR}/3rdparty")
target_link_libraries(example-common PUBLIC bgfx SDL3::SDL3)
target_compile_options(example-common PRIVATE -fno-exceptions -fno-rtti)
# ImGui stores colors in native uint32_t values; GX2 consumes RGBA bytes.
target_compile_definitions(example-common PUBLIC USE_ENTRY=1 ENTRY_CONFIG_IMPLEMENT_MAIN=1
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
    IM_COL32_R_SHIFT=24 IM_COL32_G_SHIFT=16 IM_COL32_B_SHIFT=8 IM_COL32_A_SHIFT=0
    IM_COL32_A_MASK=0x000000ff)

# Supply GX2 variants alongside the upstream embedded shader tables without
# changing their checked-in, cross-platform headers or common renderer sources.
set(imgui_embedded "#include <stdint.h>\n")
foreach(shader IN ITEMS vs_ocornut_imgui fs_ocornut_imgui vs_imgui_image fs_imgui_image)
    if(shader MATCHES "^vs_")
        set(stage vertex)
    else()
        set(stage fragment)
    endif()
    set(output "${example_generated_dir}/${shader}.gx2.bin.h")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${example_generated_dir}"
        COMMAND "${BGFX_SHADERC}" -f "${common_dir}/imgui/${shader}.sc" -o "${output}"
            --type ${stage} --platform wiiu -p gx2 --bin2c ${shader}_gx2
            --varyingdef "${common_dir}/imgui/varying.def.sc"
            -i "${BGFX_DIR}/src;${common_dir}" --depends
        DEPENDS "${BGFX_SHADERC}" "${common_dir}/imgui/${shader}.sc"
            "${common_dir}/imgui/varying.def.sc" "${BGFX_DIR}/src/bgfx_shader.sh"
        DEPFILE "${output}.d"
        VERBATIM)
    target_sources(example-common PRIVATE "${output}")
    string(APPEND imgui_embedded "#include \"${output}\"\n")
endforeach()
file(GENERATE OUTPUT "${example_generated_dir}/imgui-gx2.h" CONTENT "${imgui_embedded}")
set_source_files_properties("${common_dir}/imgui/imgui.cpp" PROPERTIES
    COMPILE_OPTIONS "-include;${example_generated_dir}/imgui-gx2.h")

foreach(example IN LISTS BGFX_WIIU_EXAMPLES)
    set(example_dir "${BGFX_DIR}/examples/${example}")
    set(package_dir "${CMAKE_CURRENT_BINARY_DIR}/${example}")
    file(GLOB sources CONFIGURE_DEPENDS "${example_dir}/*.cpp")
    add_executable(example-${example} ${sources})
    target_link_libraries(example-${example} PRIVATE example-common)
    target_include_directories(example-${example} PRIVATE "${BGFX_DIR}/3rdparty")
    set_target_properties(example-${example} PROPERTIES OUTPUT_NAME "${example}"
        RUNTIME_OUTPUT_DIRECTORY "${package_dir}/code")
    add_custom_command(TARGET example-${example} POST_BUILD
        COMMAND "${WUT_ELF2RPL_EXE}" "$<TARGET_FILE:example-${example}>"
            "${package_dir}/code/${example}.rpx"
        BYPRODUCTS "${package_dir}/code/${example}.rpx"
        VERBATIM)

    file(GLOB shader_sources CONFIGURE_DEPENDS "${example_dir}/vs_*.sc" "${example_dir}/fs_*.sc" "${example_dir}/cs_*.sc")
    # Embedded example shaders need the same GX2 symbols as common ImGui.
    set(example_source_text "")
    foreach(source IN LISTS sources)
        file(READ "${source}" source_text)
        string(APPEND example_source_text "${source_text}")
    endforeach()
    set(example_embedded "#include <stdint.h>\n")
    set(shader_outputs "")
    foreach(source IN LISTS shader_sources)
        get_filename_component(shader "${source}" NAME_WE)
        if(shader MATCHES "^vs_")
            set(stage vertex)
        elseif(shader MATCHES "^fs_")
            set(stage fragment)
        else()
            set(stage compute)
        endif()
        set(bin2c_options "")
        string(FIND "${example_source_text}" "${shader}.bin.h" embedded_include)
        if(NOT embedded_include EQUAL -1)
            set(output "${example_generated_dir}/${example}/${shader}.gx2.bin.h")
            set(bin2c_options --bin2c ${shader}_gx2)
            string(APPEND example_embedded "#include \"${output}\"\n")
        else()
            set(output "${package_dir}/content/shaders/gx2/${shader}.bin")
        endif()
        get_filename_component(output_dir "${output}" DIRECTORY)
        add_custom_command(OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
            COMMAND "${BGFX_SHADERC}" -f "${source}" -o "${output}"
                --type ${stage} --platform wiiu -p gx2 ${bin2c_options}
                --varyingdef "${example_dir}/varying.def.sc"
                -i "${BGFX_DIR}/src;${common_dir}" --depends
            DEPENDS "${BGFX_SHADERC}" "${source}" "${example_dir}/varying.def.sc"
                "${BGFX_DIR}/src/bgfx_shader.sh"
            DEPFILE "${output}.d"
            VERBATIM)
        list(APPEND shader_outputs "${output}")
    endforeach()
    file(GENERATE OUTPUT "${example_generated_dir}/${example}/embedded.h" CONTENT "${example_embedded}")
    target_compile_options(example-${example} PRIVATE
        -include "${example_generated_dir}/${example}/embedded.h")
    # Keep the original runtime asset paths under Cemu's /vol/content mount.
    foreach(asset_dir IN ITEMS font images meshes text textures)
        file(GLOB_RECURSE assets CONFIGURE_DEPENDS
            RELATIVE "${BGFX_DIR}/examples/runtime" "${BGFX_DIR}/examples/runtime/${asset_dir}/*")
        foreach(asset IN LISTS assets)
            configure_file("${BGFX_DIR}/examples/runtime/${asset}"
                "${package_dir}/content/${asset}" COPYONLY)
        endforeach()
    endforeach()
    add_custom_target(example-${example}-content
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${package_dir}/content"
        DEPENDS ${shader_outputs})
    add_dependencies(example-${example} example-${example}-content)
    install(FILES "${package_dir}/code/${example}.rpx" DESTINATION "${example}/code")
    install(DIRECTORY "${package_dir}/content/" DESTINATION "${example}/content")
endforeach()
