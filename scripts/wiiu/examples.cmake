# Original bgfx applications, sharing one SDL3/GX2 entry implementation.
find_package(SDL3 REQUIRED CONFIG)
set(BGFX_WIIU_EXAMPLES
    "00-helloworld;01-cubes;02-metaballs;03-raymarch;04-mesh;05-instancing;06-bump;07-callback;08-update;09-hdr;10-font;11-fontsdf;12-lod;13-stencil;14-shadowvolumes;15-shadowmaps-simple;16-shadowmaps;17-drawstress;18-ibl;19-oit;20-nanovg;21-deferred;22-windows;23-vectordisplay;25-c99;26-occlusion;27-terrain;28-wireframe;29-debugdraw;30-picking;31-rsm;32-particles;33-pom;34-mvs;35-dynamic;36-sky;38-bloom;39-assao;40-svt;42-bunnylod;43-denoise;44-sss;45-bokeh;47-pixelformats;49-hextile;50-headless;51-gpufont;52-layered;54-s2h"
    CACHE STRING "Numbered example directories to build")
if("25-c99" IN_LIST BGFX_WIIU_EXAMPLES OR "42-bunnylod" IN_LIST BGFX_WIIU_EXAMPLES)
    enable_language(C)
endif()
set(common_dir "${BGFX_DIR}/examples/common")
set(example_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/example-generated")

file(GLOB imgui_sources CONFIGURE_DEPENDS "${BGFX_DIR}/3rdparty/dear-imgui/*.cpp")
file(GLOB meshoptimizer_sources CONFIGURE_DEPENDS "${BGFX_DIR}/3rdparty/meshoptimizer/src/*.cpp")
add_library(example-common STATIC
    ${common_dir}/entry/entry.cpp
    ${common_dir}/entry/entry_sdl3.cpp
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
target_compile_definitions(example-common PUBLIC ENTRY_CONFIG_USE_SDL3=1)
target_compile_options(example-common PRIVATE -fno-exceptions -fno-rtti)
target_compile_definitions(example-common PUBLIC USE_ENTRY=1 ENTRY_CONFIG_IMPLEMENT_MAIN=1
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

# Supply GX2 variants alongside the upstream embedded shader tables without
# changing their checked-in, cross-platform headers or common renderer sources.
set(embedded_families imgui)
if("10-font" IN_LIST BGFX_WIIU_EXAMPLES OR "11-fontsdf" IN_LIST BGFX_WIIU_EXAMPLES)
    target_sources(example-common PRIVATE
        ${common_dir}/cube_atlas.cpp
        ${common_dir}/font/font_manager.cpp
        ${common_dir}/font/text_buffer_manager.cpp
        ${common_dir}/font/text_metrics.cpp)
    list(APPEND embedded_families font)
endif()
if("10-font" IN_LIST BGFX_WIIU_EXAMPLES OR "11-fontsdf" IN_LIST BGFX_WIIU_EXAMPLES
        OR "51-gpufont" IN_LIST BGFX_WIIU_EXAMPLES)
    target_sources(example-common PRIVATE ${common_dir}/font/utf8.cpp)
endif()
if("20-nanovg" IN_LIST BGFX_WIIU_EXAMPLES)
    target_sources(example-common PRIVATE
        ${common_dir}/nanovg/nanovg.cpp
        ${common_dir}/nanovg/nanovg_bgfx.cpp)
    list(APPEND embedded_families nanovg)
endif()
if("29-debugdraw" IN_LIST BGFX_WIIU_EXAMPLES OR "32-particles" IN_LIST BGFX_WIIU_EXAMPLES)
    target_sources(example-common PRIVATE ${common_dir}/debugdraw/debugdraw.cpp)
    list(APPEND embedded_families debugdraw)
endif()
if("32-particles" IN_LIST BGFX_WIIU_EXAMPLES)
    target_sources(example-common PRIVATE ${common_dir}/ps/particle_system.cpp)
    list(APPEND embedded_families ps)
endif()
foreach(family IN LISTS embedded_families)
    set(embedded "#include <stdint.h>\n")
    file(GLOB embedded_sources CONFIGURE_DEPENDS
        "${common_dir}/${family}/vs_*.sc" "${common_dir}/${family}/fs_*.sc")
    foreach(source IN LISTS embedded_sources)
        get_filename_component(shader "${source}" NAME_WE)
        if(shader MATCHES "^vs_")
            set(stage vertex)
        else()
            set(stage fragment)
        endif()
        set(output "${example_generated_dir}/${shader}.gx2.bin.h")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${example_generated_dir}"
            COMMAND "${BGFX_SHADERC}" -f "${source}" -o "${output}"
                --type ${stage} --platform wiiu -p $<TARGET_PROPERTY:bgfx,BGFX_GX2_SHADER_PROFILE> --bin2c ${shader}_gx2
                --varyingdef "${common_dir}/${family}/varying.def.sc"
                -i "${BGFX_DIR}/src;${common_dir}" --depends
            DEPENDS "${BGFX_SHADERC}" "${source}"
                "${common_dir}/${family}/varying.def.sc" "${BGFX_DIR}/src/bgfx_shader.sh"
            DEPFILE "${output}.d"
            VERBATIM)
        target_sources(example-common PRIVATE "${output}")
        string(APPEND embedded "#include \"${output}\"\n")
    endforeach()
    file(GENERATE OUTPUT "${example_generated_dir}/${family}-gx2.h" CONTENT "${embedded}")
    if(family STREQUAL "imgui")
        set(consumer "${common_dir}/imgui/imgui.cpp")
    elseif(family STREQUAL "font")
        set(consumer "${common_dir}/font/text_buffer_manager.cpp")
    elseif(family STREQUAL "nanovg")
        set(consumer "${common_dir}/nanovg/nanovg_bgfx.cpp")
    elseif(family STREQUAL "debugdraw")
        set(consumer "${common_dir}/debugdraw/debugdraw.cpp")
    else()
        set(consumer "${common_dir}/ps/particle_system.cpp")
    endif()
    set_source_files_properties("${consumer}" PROPERTIES
        COMPILE_OPTIONS "-include;${example_generated_dir}/${family}-gx2.h")
endforeach()

foreach(example IN LISTS BGFX_WIIU_EXAMPLES)
    set(example_dir "${BGFX_DIR}/examples/${example}")
    set(package_dir "${CMAKE_CURRENT_BINARY_DIR}/${example}")
    file(GLOB sources CONFIGURE_DEPENDS "${example_dir}/*.cpp" "${example_dir}/*.c")
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

    # Graphics assets are universal; native compute assets are selected explicitly.
    file(GLOB shader_sources CONFIGURE_DEPENDS "${example_dir}/vs_*.sc" "${example_dir}/fs_*.sc")
    if(BGFX_GX2_UNIFORM_REGISTERS AND example STREQUAL "21-deferred")
        # Graphics image access requires native uniform-block mode. The original
        # deferred example loads these variants only when IMAGE_RW is advertised.
        list(FILTER shader_sources EXCLUDE REGEX "_uav\\.sc$")
        file(REMOVE "${package_dir}/content/shaders/gx2/fs_deferred_clear_uav.bin"
            "${package_dir}/content/shaders/gx2/fs_deferred_light_uav.bin")
    endif()
    if(example MATCHES "^(08-update|39-assao|46-fsr|48-drawindirect|53-sky2|54-s2h)$")
        file(GLOB compute_sources CONFIGURE_DEPENDS "${example_dir}/cs_*.sc")
        list(APPEND shader_sources ${compute_sources})
    endif()
    if(example STREQUAL "39-assao")
        # Package the 23 supported original stages. Adaptive quality still needs
        # a global atomicAdd in this stage, which native R700 cannot compile.
        # The example selects its original nonadaptive quality levels on GX2.
        list(FILTER shader_sources EXCLUDE REGEX "cs_assao_postprocess_importance_map_b\\.sc$")
    endif()
    if(example MATCHES "^(05-instancing|22-windows|26-occlusion|34-mvs|35-dynamic|50-headless)$")
        list(APPEND shader_sources
            "${BGFX_DIR}/examples/01-cubes/vs_cubes.sc"
            "${BGFX_DIR}/examples/01-cubes/fs_cubes.sc")
    endif()
    if(example STREQUAL "48-drawindirect")
        list(APPEND shader_sources
            "${BGFX_DIR}/examples/05-instancing/vs_instancing.sc"
            "${BGFX_DIR}/examples/05-instancing/fs_instancing.sc")
    endif()
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
        get_filename_component(shader_dir "${source}" DIRECTORY)
        set(shader_profile $<TARGET_PROPERTY:bgfx,BGFX_GX2_SHADER_PROFILE>)
        if(shader MATCHES "^vs_")
            set(stage vertex)
        elseif(shader MATCHES "^cs_")
            set(stage compute)
            set(shader_profile gx2)
        else()
            set(stage fragment)
        endif()
        set(varying_file "")
        set(varying_options "")
        if(NOT stage STREQUAL "compute")
            set(varying_file "${shader_dir}/varying.def.sc")
            set(varying_options --varyingdef "${varying_file}")
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
                --type ${stage} --platform wiiu -p ${shader_profile} ${bin2c_options}
                ${varying_options}
                -i "${BGFX_DIR}/src;${common_dir}" --depends
            DEPENDS "${BGFX_SHADERC}" "${source}" ${varying_file}
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
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${package_dir}/content/temp"
        DEPENDS ${shader_outputs})
    add_dependencies(example-${example} example-${example}-content)
    install(FILES "${package_dir}/code/${example}.rpx" DESTINATION "${example}/code")
    install(DIRECTORY "${package_dir}/content/" DESTINATION "${example}/content")
endforeach()
