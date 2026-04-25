cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

project(EngineSkeleton LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# When ON, only the standalone game target is built (no engine editor).
# Set by the engine's build dialog: cmake -DENGINE_BUILD_GAME=ON ...
# The default cmake --build build command leaves this OFF and builds only the engine.
option(ENGINE_BUILD_GAME "Build the standalone game target instead of the engine editor" OFF)

find_package(Vulkan REQUIRED)

find_program(GLSLC_EXECUTABLE
    NAMES glslc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/Bin32"
)

if(NOT GLSLC_EXECUTABLE)
    file(GLOB VULKAN_SDK_GLSLC "C:/VulkanSDK/*/Bin/glslc.exe")
    if(VULKAN_SDK_GLSLC)
        list(SORT VULKAN_SDK_GLSLC)
        list(REVERSE VULKAN_SDK_GLSLC)
        list(GET VULKAN_SDK_GLSLC 0 GLSLC_EXECUTABLE)
    endif()
endif()

if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc was not found. Install the Vulkan SDK or add glslc to PATH.")
endif()

include(FetchContent)

# SDL3: shared for the engine editor, static for the standalone game so that
# no SDL3.dll needs to be distributed with the game.
# For game builds, disable unused subsystems to reduce compile time and size.
if(ENGINE_BUILD_GAME)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    # Disable unused SDL3 subsystems for game builds
    set(SDL_AUDIO      OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK   OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC     OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI     OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR     OFF CACHE BOOL "" FORCE)
    set(SDL_POWER      OFF CACHE BOOL "" FORCE)
    set(SDL_TRAY       OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG     OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA     OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER     OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
endif()
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(sdl3)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.6-docking
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

FetchContent_Declare(
    imnodeflow
    GIT_REPOSITORY https://github.com/Fattorino/ImNodeFlow.git
    GIT_TAG 5c93f4822869223cc1abe5ce5fe51e6bee1f5925
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(imnodeflow)
if(NOT imnodeflow_POPULATED)
    FetchContent_Populate(imnodeflow)
endif()

FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(imguizmo)
if(NOT imguizmo_POPULATED)
    FetchContent_Populate(imguizmo)
endif()

set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

# For game builds: disable all assimp importers by default, then enable only
# the formats we actually use (FBX, GLTF, OBJ). This dramatically reduces
# compile time and binary size.
if(ENGINE_BUILD_GAME)
    set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_FBX_IMPORTER  ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_OBJ_IMPORTER  ON CACHE BOOL "" FORCE)
endif()

# Force assimp to produce a static library when building the game so the
# final staged output requires no assimp DLL.
if(ENGINE_BUILD_GAME)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v5.4.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(assimp)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMNODEFLOW_DIR ${imnodeflow_SOURCE_DIR})
set(IMGUIZMO_DIR ${imguizmo_SOURCE_DIR})

# ---------------------------------------------------------------------------
# ENGINE_BUILD_GAME=ON  → build only the standalone game target
# ENGINE_BUILD_GAME=OFF → build only the engine editor (default)
# ---------------------------------------------------------------------------
if(ENGINE_BUILD_GAME)

    include(cmake/game.cmake)

else() # engine editor build ---------------------------------------------------

set(ENGINE_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/shaders)
set(ENGINE_SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/shaders)
set(ENGINE_UI_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/ui)
set(ENGINE_SHADER_FLAGS --target-env=vulkan1.2 --target-spv=spv1.4)

function(add_engine_shader source_name shader_stage)
    add_custom_command(
        OUTPUT ${ENGINE_SHADER_BINARY_DIR}/${source_name}.spv
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ENGINE_SHADER_BINARY_DIR}
        COMMAND ${GLSLC_EXECUTABLE} ${ENGINE_SHADER_FLAGS} -fshader-stage=${shader_stage} -o ${ENGINE_SHADER_BINARY_DIR}/${source_name}.spv ${ENGINE_SHADER_SOURCE_DIR}/${source_name}
        DEPENDS ${ENGINE_SHADER_SOURCE_DIR}/${source_name}
        VERBATIM
    )

    list(APPEND ENGINE_SHADER_OUTPUTS ${ENGINE_SHADER_BINARY_DIR}/${source_name}.spv)
    set(ENGINE_SHADER_OUTPUTS ${ENGINE_SHADER_OUTPUTS} PARENT_SCOPE)
endfunction()

set(ENGINE_SHADER_OUTPUTS)
add_engine_shader(scene_viewport_rt.rgen rgen)
add_engine_shader(scene_viewport_rt.rmiss rmiss)
add_engine_shader(scene_viewport_rt_shadow.rmiss rmiss)
add_engine_shader(scene_viewport_rt.rchit rchit)
add_engine_shader(scene_viewport_rt_shadow.rchit rchit)
add_engine_shader(scene_viewport_rt_primary.rahit rahit)
add_engine_shader(scene_viewport_rt_shadow.rahit rahit)

add_custom_target(engine_scene_shaders
    DEPENDS ${ENGINE_SHADER_OUTPUTS}
)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui
    PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)

add_library(imnodeflow STATIC
    ${IMNODEFLOW_DIR}/src/ImNodeFlow.cpp
)

add_library(imguizmo STATIC
    ${IMGUIZMO_DIR}/ImGuizmo.cpp
)

target_include_directories(imnodeflow
    PUBLIC
        ${IMNODEFLOW_DIR}/include
    PRIVATE
        ${IMNODEFLOW_DIR}/src
)

target_compile_definitions(imnodeflow PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imnodeflow PUBLIC imgui)

target_include_directories(imguizmo
    PUBLIC
        ${IMGUIZMO_DIR}
        ${IMGUI_DIR}
)

target_link_libraries(imguizmo PUBLIC imgui)

if(MSVC)
    target_compile_options(imnodeflow PRIVATE /wd4100 /wd4244)
    target_compile_options(imguizmo PRIVATE /wd4100 /wd4244 /wd4456)
endif()

add_executable(engine
    src/app/main.cpp
    src/app/VulkanContext.cpp
    src/assets/AssetMetadata.cpp
    src/assets/ModelAsset.cpp
    src/app/EngineApplication.cpp
    src/assets/ModelMetadata.cpp
    src/assets/SceneMetadata.cpp
    src/vfs/AssetVFS.cpp
    src/components/CreationMenu.cpp
    src/components/FileContextMenu.cpp
    src/components/SceneObjectAttributesEditor.cpp
    src/components/BuildSettingsComponent.cpp
    src/dialogs/BuildGameDialog.cpp
    src/dialogs/NewProjectDialog.cpp
    src/dialogs/OpenProjectDialog.cpp
    src/vfs/PakArchive.cpp
    src/graph/GraphDocument.cpp
    src/graph/NodeLibrary.cpp
    src/panels/FilesPanel.cpp
    src/panels/InfoPanel.cpp
    src/panels/LogPanel.cpp
    src/render/Lighting.cpp
    src/render/Raytracing.cpp
    src/render/RuntimeRenderer.cpp
    src/render/SceneViewportRenderer.cpp
    src/panels/SettingsPanel.cpp
    src/render/FontInfoRenderer.cpp
    src/render/ImageInfoRenderer.cpp
    src/panels/WorkspacePanel.cpp
)

target_include_directories(engine PRIVATE
    src
)

target_compile_definitions(engine PRIVATE
    SDL_MAIN_HANDLED
    IMGUI_DEFINE_MATH_OPERATORS
)

target_link_libraries(engine PRIVATE
    SDL3::SDL3
    imgui
    imguizmo
    imnodeflow
    nlohmann_json::nlohmann_json
)

if(TARGET assimp::assimp)
    target_link_libraries(engine PRIVATE assimp::assimp)
elseif(TARGET assimp)
    target_link_libraries(engine PRIVATE assimp)
endif()

add_dependencies(engine engine_scene_shaders)

if(WIN32)
    target_link_libraries(engine PRIVATE Ole32 Shell32)

    add_custom_command(TARGET engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:engine>/shaders
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:engine>/ui
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ENGINE_SHADER_OUTPUTS}
            $<TARGET_FILE_DIR:engine>/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ENGINE_UI_SOURCE_DIR}/codicon.ttf
            $<TARGET_FILE_DIR:engine>/ui
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:engine>
            $<TARGET_FILE_DIR:engine>
        COMMAND_EXPAND_LISTS
    )
endif()

if(MSVC)
    target_compile_options(engine PRIVATE
        /W4
        /permissive-
        /external:W0
        "/external:I${IMNODEFLOW_DIR}/include"
        "/external:I${IMNODEFLOW_DIR}/src"
        "/external:I${IMGUI_DIR}"
        "/external:I${IMGUI_DIR}/backends"
        "/external:I${assimp_SOURCE_DIR}/include"
        "/external:I${stb_SOURCE_DIR}"
    )
else()
    target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(engine PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

endif() # ENGINE_BUILD_GAME