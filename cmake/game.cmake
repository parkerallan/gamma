# cmake/game.cmake
# Included from the root CMakeLists.txt when ENGINE_BUILD_GAME=ON.
# Defines the standalone `game` executable target only.
# Engine-only targets (imgui, imnodeflow, imguizmo, engine) are NOT built.
#
# All large dependencies (SDL3, assimp) are statically linked so the
# distributed game folder contains no extra DLLs for those libraries.
# SDL_STATIC=ON and BUILD_SHARED_LIBS=OFF must be set before FetchContent
# populates those deps (handled in the root CMakeLists.txt).

# ---------------------------------------------------------------------------
# ImGui - still needed for VulkanContext's internal swapchain/surface helpers.
# Only include the minimal set of source files required for the Vulkan backend.
# ---------------------------------------------------------------------------
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui
    PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC SDL3::SDL3-static Vulkan::Vulkan)

# ---------------------------------------------------------------------------
# Game runtime shaders
# ---------------------------------------------------------------------------
set(GAME_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/shaders)
set(GAME_SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/shaders)
set(GAME_SHADER_FLAGS --target-env=vulkan1.2 --target-spv=spv1.4)

function(add_game_shader source_name shader_stage)
    if(SHADER_COMPILER_IS_GLSLANGVALIDATOR)
        add_custom_command(
            OUTPUT ${GAME_SHADER_BINARY_DIR}/${source_name}.spv
            COMMAND ${CMAKE_COMMAND} -E make_directory ${GAME_SHADER_BINARY_DIR}
            COMMAND ${GLSLC_EXECUTABLE} -V --target-env vulkan1.2 -S ${shader_stage}
                -o ${GAME_SHADER_BINARY_DIR}/${source_name}.spv
                ${GAME_SHADER_SOURCE_DIR}/${source_name}
            DEPENDS ${GAME_SHADER_SOURCE_DIR}/${source_name}
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT ${GAME_SHADER_BINARY_DIR}/${source_name}.spv
            COMMAND ${CMAKE_COMMAND} -E make_directory ${GAME_SHADER_BINARY_DIR}
            COMMAND ${GLSLC_EXECUTABLE} ${GAME_SHADER_FLAGS} -fshader-stage=${shader_stage}
                -o ${GAME_SHADER_BINARY_DIR}/${source_name}.spv
                ${GAME_SHADER_SOURCE_DIR}/${source_name}
            DEPENDS ${GAME_SHADER_SOURCE_DIR}/${source_name}
            VERBATIM
        )
    endif()
    list(APPEND GAME_SHADER_OUTPUTS ${GAME_SHADER_BINARY_DIR}/${source_name}.spv)
    set(GAME_SHADER_OUTPUTS ${GAME_SHADER_OUTPUTS} PARENT_SCOPE)
endfunction()

set(GAME_SHADER_OUTPUTS)
add_game_shader(standard_rt.rgen rgen)
add_game_shader(standard_rt.rmiss rmiss)
add_game_shader(standard_rt_shadow.rmiss rmiss)
add_game_shader(standard_rt.rchit rchit)
add_game_shader(standard_rt_cloud.rchit rchit)
add_game_shader(standard_rt_shadow.rchit rchit)
add_game_shader(standard_rt_primary.rahit rahit)
add_game_shader(standard_rt_shadow.rahit rahit)
add_game_shader(skinning.comp comp)
add_game_shader(taa.comp comp)
add_game_shader(overlay2d.vert vert)
add_game_shader(overlay2d.frag frag)
add_game_shader(effects_depth.vert vert)
add_game_shader(effects_depth.frag frag)
add_game_shader(water_surface.vert vert)
add_game_shader(water_surface.frag frag)
add_game_shader(water_underwater.vert vert)
add_game_shader(water_underwater.frag frag)

add_custom_target(game_shaders
    DEPENDS ${GAME_SHADER_OUTPUTS}
)

set(GAME_ICON_RC)
if(WIN32)
    # Path to an .ico selected from the engine build dialog. When provided,
    # we generate a tiny .rc file so the built game.exe has an embedded EXE icon.
    # This controls Explorer/taskbar executable branding (separate from SDL
    # runtime window icon, which is still loaded from assets.pak at startup).
    set(GAME_WINDOWS_ICON_PATH "${GAME_WINDOWS_ICON_PATH}" CACHE STRING "Optional absolute path to game EXE icon (.ico) on Windows")

    if(NOT GAME_WINDOWS_ICON_PATH STREQUAL "")
        if(EXISTS "${GAME_WINDOWS_ICON_PATH}")
            get_filename_component(GAME_WINDOWS_ICON_EXT "${GAME_WINDOWS_ICON_PATH}" EXT)
            string(TOLOWER "${GAME_WINDOWS_ICON_EXT}" GAME_WINDOWS_ICON_EXT_LOWER)
            if(GAME_WINDOWS_ICON_EXT_LOWER STREQUAL ".ico")
                file(TO_CMAKE_PATH "${GAME_WINDOWS_ICON_PATH}" GAME_WINDOWS_ICON_PATH_CMAKE)
                set(GAME_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/game_icon.rc")
                file(WRITE "${GAME_ICON_RC}" "IDI_GAME_ICON ICON \"${GAME_WINDOWS_ICON_PATH_CMAKE}\"\n")
                message(STATUS "Game EXE icon resource enabled: ${GAME_WINDOWS_ICON_PATH}")
            else()
                message(WARNING "GAME_WINDOWS_ICON_PATH is not an .ico file: ${GAME_WINDOWS_ICON_PATH} (skipping EXE icon embedding; runtime window icon from assets.pak still works)")
            endif()
        else()
            message(WARNING "GAME_WINDOWS_ICON_PATH does not exist: ${GAME_WINDOWS_ICON_PATH} (building game.exe without embedded icon)")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Game executable
# ---------------------------------------------------------------------------
add_executable(game
    ${GAME_ICON_RC}
    game/main.cpp
    game/GameApplication.cpp
    src/app/VulkanContext.cpp
    src/assets/AssetMetadata.cpp
    src/assets/AnimatorControllerAsset.cpp
    src/assets/FaceClipAsset.cpp
    src/assets/ModelAsset.cpp
    src/assets/ModelMetadata.cpp
    src/assets/SceneMetadata.cpp
    src/assets/PrefabAsset.cpp
    src/audio/AudioEngine.cpp
    src/input/ControllerMapping.cpp
    src/vfs/PakArchive.cpp
    src/render/Lighting.cpp
    src/render/Raytracing.cpp
    src/render/PhysicsWorld.cpp
    src/render/Scene2DRenderer.cpp
    src/render/SkyboxRenderer.cpp
    src/render/RuntimeRenderer.cpp
    src/render/CameraController.cpp
    src/render/RuntimeEffectsRenderer.cpp
    src/render/RuntimeScriptAPI.cpp
    src/render/BoneModifiers.cpp
    src/render/VideoPlaybackManager.cpp
    src/vfs/AssetVFS.cpp
    src/components/graph/NodeSpec.cpp
    src/components/graph/BuiltinNodes.cpp
    src/components/graph/GraphDocument.cpp
    src/components/graph/GraphTranspiler.cpp
)

target_include_directories(game PRIVATE
    src
    game
    ${stb_SOURCE_DIR}
    ${assimp_SOURCE_DIR}/include
    ${assimp_BINARY_DIR}/include
)

target_compile_definitions(game PRIVATE
    SDL_MAIN_HANDLED
    IMGUI_DEFINE_MATH_OPERATORS
    ENGINE_GAME_BUILD
)

target_link_libraries(game PRIVATE
    SDL3::SDL3-static
    imgui
    tinyexr
    lua_runtime
    joltphysics
    miniaudio
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
    nlohmann_json::nlohmann_json
    Vulkan::Vulkan
)

if(TARGET assimp::assimp)
    target_link_libraries(game PRIVATE assimp::assimp)
elseif(TARGET assimp)
    target_link_libraries(game PRIVATE assimp)
endif()

if(GAMMA_WITH_EFFEKSEER)
    target_compile_definitions(game PRIVATE GAMMA_WITH_EFFEKSEER)
    target_link_libraries(game PRIVATE EffekseerRendererVulkan)
endif()

add_dependencies(game game_shaders)

if(MSVC)
    target_compile_options(game PRIVATE
        /W0
        /permissive-
        /external:W0
        "/external:I${IMGUI_DIR}"
        "/external:I${IMGUI_DIR}/backends"
        "/external:I${assimp_SOURCE_DIR}/include"
        "/external:I${stb_SOURCE_DIR}"
    )
    
    # Hide console window for Release builds (Final build type) while still
    # using the regular main() entry point.
    target_link_options(game PRIVATE $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)
elseif(UNIX)
    # Linux/GCC build path.
    target_compile_options(game PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# Post-build: copy compiled shaders next to the game executable.
if(WIN32)
    add_custom_command(TARGET game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:game>/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${GAME_SHADER_OUTPUTS}
            $<TARGET_FILE_DIR:game>/shaders
        COMMAND_EXPAND_LISTS
    )

    # Copy DLLs from any SHARED IMPORTED dependencies (FFmpeg, etc.) next to
    # game.exe so the build dialog's stage step can pick them up. Without this
    # the game launches with "avformat-XX.dll was not found".
    add_custom_command(TARGET game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:game>
            $<TARGET_FILE_DIR:game>
        COMMAND_EXPAND_LISTS
    )
endif()
