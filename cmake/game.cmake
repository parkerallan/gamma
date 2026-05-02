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
add_game_shader(scene_viewport_rt.rgen rgen)
add_game_shader(scene_viewport_rt.rmiss rmiss)
add_game_shader(scene_viewport_rt_shadow.rmiss rmiss)
add_game_shader(scene_viewport_rt.rchit rchit)
add_game_shader(scene_viewport_rt_shadow.rchit rchit)
add_game_shader(scene_viewport_rt_primary.rahit rahit)
add_game_shader(scene_viewport_rt_shadow.rahit rahit)
add_game_shader(overlay2d.vert vert)
add_game_shader(overlay2d.frag frag)

add_custom_target(game_shaders
    DEPENDS ${GAME_SHADER_OUTPUTS}
)

# ---------------------------------------------------------------------------
# Game executable
# ---------------------------------------------------------------------------
add_executable(game
    game/main.cpp
    game/GameApplication.cpp
    src/app/VulkanContext.cpp
    src/assets/AssetMetadata.cpp
    src/assets/ModelAsset.cpp
    src/assets/ModelMetadata.cpp
    src/assets/SceneMetadata.cpp
    src/vfs/PakArchive.cpp
    src/render/Lighting.cpp
    src/render/Raytracing.cpp
    src/render/PhysicsWorld.cpp
    src/render/Scene2DRenderer.cpp
    src/render/SkyboxRenderer.cpp
    src/render/RuntimeRenderer.cpp
    src/render/RuntimeScriptAPI.cpp
    src/vfs/AssetVFS.cpp
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
    nlohmann_json::nlohmann_json
    Vulkan::Vulkan
)

if(TARGET assimp::assimp)
    target_link_libraries(game PRIVATE assimp::assimp)
elseif(TARGET assimp)
    target_link_libraries(game PRIVATE assimp)
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
endif()
