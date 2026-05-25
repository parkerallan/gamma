cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

project(Gamma LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# When ON, only the standalone game target is built (no engine app).
# Set by the engine's build dialog: cmake -DENGINE_BUILD_GAME=ON ...
# The default cmake --build build command leaves this OFF and builds only the engine.
option(ENGINE_BUILD_GAME "Build the standalone game target instead of the engine app" OFF)

# Windows engine executable branding (ENGINE target only).
# This does not affect the standalone game target when ENGINE_BUILD_GAME=ON.
set(ENGINE_EXE_NAME "Gamma" CACHE STRING "Output executable name for the engine target on Windows")

find_package(Vulkan REQUIRED)

find_program(GLSLC_EXECUTABLE
    NAMES glslc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/Bin32"
)

set(SHADER_COMPILER_IS_GLSLANGVALIDATOR OFF)

if(NOT GLSLC_EXECUTABLE)
    file(GLOB VULKAN_SDK_GLSLC "C:/VulkanSDK/*/Bin/glslc.exe")
    if(VULKAN_SDK_GLSLC)
        list(SORT VULKAN_SDK_GLSLC)
        list(REVERSE VULKAN_SDK_GLSLC)
        list(GET VULKAN_SDK_GLSLC 0 GLSLC_EXECUTABLE)
    endif()
endif()

if(NOT GLSLC_EXECUTABLE)
    find_program(GLSLC_EXECUTABLE NAMES glslangValidator)
    if(GLSLC_EXECUTABLE)
        set(SHADER_COMPILER_IS_GLSLANGVALIDATOR ON)
    endif()
endif()

if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "No supported shader compiler was found. Install glslc or glslangValidator and add it to PATH.")
endif()

include(FetchContent)

# SDL3: shared for the engine app, static for the standalone game so that
# no SDL3.dll needs to be distributed with the game.
if(ENGINE_BUILD_GAME)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
endif()

# Disable unused SDL3 subsystems for both editor and game builds to reduce
# dependency compile time and binary size.
set(SDL_AUDIO      OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK   ON  CACHE BOOL "" FORCE)
set(SDL_HAPTIC     OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI     OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR     OFF CACHE BOOL "" FORCE)
set(SDL_POWER      OFF CACHE BOOL "" FORCE)
set(SDL_TRAY       OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG     OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA     OFF CACHE BOOL "" FORCE)
set(SDL_RENDER     OFF CACHE BOOL "" FORCE)
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
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG a15acd87a3f3241a29ea1363ceafc680dca3a96b
)
FetchContent_GetProperties(imguizmo)
if(NOT imguizmo_POPULATED)
    FetchContent_Populate(imguizmo)
endif()

FetchContent_Declare(
    imguicolortextedit
    GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(imguicolortextedit)
if(NOT imguicolortextedit_POPULATED)
    FetchContent_Populate(imguicolortextedit)
endif()

# imgui-node-editor (thedmd) — Blueprint-style node graph editor used by the
# NodeGraphComponent. Editor-only; the shipped game does not link this and
# instead consumes the .lua files generated from .graph documents at build time.
FetchContent_Declare(
    imgui_node_editor
    GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
    # Pinned to a master commit ahead of the v0.9.3 tag. v0.9.3 collides with
    # imgui >= 1.91 because the stock ImGui now defines ImVec2 operator==/!=
    # and removed GetKeyIndex; this commit has the compatibility fixes.
    GIT_TAG 021aa0ea4da13fed864bafb2a92d4c5205076866
    GIT_SHALLOW FALSE
)
FetchContent_GetProperties(imgui_node_editor)
if(NOT imgui_node_editor_POPULATED)
    FetchContent_Populate(imgui_node_editor)
endif()

set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
# Build zlib from Assimp's bundled sources so Linux/WSL does not require
# a preinstalled system zlib development package.
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)

# Disable all assimp importers/exporters by default, then enable only the
# formats we actually use. This keeps both editor and game build times lower.
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER  ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER  ON CACHE BOOL "" FORCE)

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
    tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG v1.0.13
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tinyexr)
if(NOT tinyexr_POPULATED)
    FetchContent_Populate(tinyexr)
endif()

add_library(tinyexr STATIC
    ${tinyexr_SOURCE_DIR}/tinyexr.cc
    ${tinyexr_SOURCE_DIR}/deps/miniz/miniz.c
)
target_include_directories(tinyexr PUBLIC
    ${tinyexr_SOURCE_DIR}
    ${tinyexr_SOURCE_DIR}/deps/miniz
)
target_compile_definitions(tinyexr PUBLIC
    TINYEXR_USE_THREAD=1
    TINYEXR_MAX_THREADS=8
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    lua_runtime
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG v5.4.7
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(lua_runtime)
if(NOT lua_runtime_POPULATED)
    FetchContent_Populate(lua_runtime)
endif()

set(LUA_RUNTIME_SOURCE_DIR ${lua_runtime_SOURCE_DIR})
add_library(lua_runtime STATIC
    ${LUA_RUNTIME_SOURCE_DIR}/lapi.c
    ${LUA_RUNTIME_SOURCE_DIR}/lauxlib.c
    ${LUA_RUNTIME_SOURCE_DIR}/lbaselib.c
    ${LUA_RUNTIME_SOURCE_DIR}/lcode.c
    ${LUA_RUNTIME_SOURCE_DIR}/lcorolib.c
    ${LUA_RUNTIME_SOURCE_DIR}/lctype.c
    ${LUA_RUNTIME_SOURCE_DIR}/ldblib.c
    ${LUA_RUNTIME_SOURCE_DIR}/ldebug.c
    ${LUA_RUNTIME_SOURCE_DIR}/ldo.c
    ${LUA_RUNTIME_SOURCE_DIR}/ldump.c
    ${LUA_RUNTIME_SOURCE_DIR}/lfunc.c
    ${LUA_RUNTIME_SOURCE_DIR}/lgc.c
    ${LUA_RUNTIME_SOURCE_DIR}/linit.c
    ${LUA_RUNTIME_SOURCE_DIR}/liolib.c
    ${LUA_RUNTIME_SOURCE_DIR}/llex.c
    ${LUA_RUNTIME_SOURCE_DIR}/lmathlib.c
    ${LUA_RUNTIME_SOURCE_DIR}/lmem.c
    ${LUA_RUNTIME_SOURCE_DIR}/loadlib.c
    ${LUA_RUNTIME_SOURCE_DIR}/lobject.c
    ${LUA_RUNTIME_SOURCE_DIR}/lopcodes.c
    ${LUA_RUNTIME_SOURCE_DIR}/loslib.c
    ${LUA_RUNTIME_SOURCE_DIR}/lparser.c
    ${LUA_RUNTIME_SOURCE_DIR}/lstate.c
    ${LUA_RUNTIME_SOURCE_DIR}/lstring.c
    ${LUA_RUNTIME_SOURCE_DIR}/lstrlib.c
    ${LUA_RUNTIME_SOURCE_DIR}/ltable.c
    ${LUA_RUNTIME_SOURCE_DIR}/ltablib.c
    ${LUA_RUNTIME_SOURCE_DIR}/ltm.c
    ${LUA_RUNTIME_SOURCE_DIR}/lundump.c
    ${LUA_RUNTIME_SOURCE_DIR}/lutf8lib.c
    ${LUA_RUNTIME_SOURCE_DIR}/lvm.c
    ${LUA_RUNTIME_SOURCE_DIR}/lzio.c
)

target_include_directories(lua_runtime PUBLIC
    ${LUA_RUNTIME_SOURCE_DIR}
)

FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif()
# Build miniaudio's single-header implementation as a C static library.
add_library(miniaudio STATIC
    ${CMAKE_SOURCE_DIR}/src/audio/miniaudio_impl.c
)
target_include_directories(miniaudio PUBLIC ${miniaudio_SOURCE_DIR})
if(MSVC)
    target_compile_options(miniaudio PRIVATE /W0)
else()
    target_compile_options(miniaudio PRIVATE -w)
endif()
if(WIN32)
    target_link_libraries(miniaudio PUBLIC ole32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(miniaudio PUBLIC dl pthread m)
elseif(APPLE)
    target_link_libraries(miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

# pl_mpeg removed in favor of FFmpeg below.

# FFmpeg (LGPL shared, BtbN's Windows prebuilds). Provides libavformat,
# libavcodec, libavutil, libswscale + matching DLLs. Used by
# VideoPlaybackManager to decode arbitrary video files for the Video2D
# scene attribute.
if(WIN32)
    set(FFMPEG_ARCHIVE_URL
        "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip"
        CACHE STRING "URL for prebuilt FFmpeg LGPL shared archive")

    FetchContent_Declare(
        ffmpeg
        URL ${FFMPEG_ARCHIVE_URL}
    )
    FetchContent_GetProperties(ffmpeg)
    if(NOT ffmpeg_POPULATED)
        message(STATUS "Downloading prebuilt FFmpeg from ${FFMPEG_ARCHIVE_URL} (~30 MB) ...")
        FetchContent_Populate(ffmpeg)
    endif()

    set(FFMPEG_ROOT "${ffmpeg_SOURCE_DIR}")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")
    set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")

    function(_add_ffmpeg_imported_lib NAME LIBFILE DLLPATTERN)
        add_library(FFmpeg::${NAME} SHARED IMPORTED)
        file(GLOB _ff_dll "${FFMPEG_BIN_DIR}/${DLLPATTERN}")
        list(GET _ff_dll 0 _ff_dll0)
        set_target_properties(FFmpeg::${NAME} PROPERTIES
            IMPORTED_LOCATION "${_ff_dll0}"
            IMPORTED_IMPLIB "${FFMPEG_LIB_DIR}/${LIBFILE}"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
        )
    endfunction()

    _add_ffmpeg_imported_lib(avformat avformat.lib  "avformat-*.dll")
    _add_ffmpeg_imported_lib(avcodec  avcodec.lib   "avcodec-*.dll")
    _add_ffmpeg_imported_lib(avutil   avutil.lib    "avutil-*.dll")
    _add_ffmpeg_imported_lib(swscale  swscale.lib   "swscale-*.dll")
    _add_ffmpeg_imported_lib(swresample swresample.lib "swresample-*.dll")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    add_library(FFmpeg::avformat   ALIAS PkgConfig::FFMPEG)
    add_library(FFmpeg::avcodec    ALIAS PkgConfig::FFMPEG)
    add_library(FFmpeg::avutil     ALIAS PkgConfig::FFMPEG)
    add_library(FFmpeg::swscale    ALIAS PkgConfig::FFMPEG)
    add_library(FFmpeg::swresample ALIAS PkgConfig::FFMPEG)
endif()

FetchContent_Declare(
    joltphysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.3.0
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(joltphysics)
if(NOT joltphysics_POPULATED)
    FetchContent_Populate(joltphysics)
endif()

set(JOLT_SOURCE_DIR ${joltphysics_SOURCE_DIR})
file(GLOB_RECURSE JOLT_SOURCES "${JOLT_SOURCE_DIR}/Jolt/*.cpp")
add_library(joltphysics STATIC ${JOLT_SOURCES})
target_include_directories(joltphysics PUBLIC ${JOLT_SOURCE_DIR})
if(MSVC)
    target_compile_options(joltphysics PRIVATE /W0)
    target_compile_definitions(joltphysics PUBLIC
        JPH_PROFILE_DISABLED
        JPH_DEBUG_RENDERER_DISABLED
    )
else()
    target_compile_options(joltphysics PRIVATE -w)
    target_compile_definitions(joltphysics PUBLIC
        JPH_PROFILE_DISABLED
        JPH_DEBUG_RENDERER_DISABLED
    )
endif()

set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUIZMO_DIR ${imguizmo_SOURCE_DIR})
set(IMGUICOLORTEXTEDIT_DIR ${imguicolortextedit_SOURCE_DIR})

# ---------------------------------------------------------------------------
# ENGINE_BUILD_GAME=ON  → build only the standalone game target
# ENGINE_BUILD_GAME=OFF → build only the engine app (default)
# ---------------------------------------------------------------------------
if(ENGINE_BUILD_GAME)

    include(cmake/game.cmake)

else() # engine app build ------------------------------------------------------

set(ENGINE_ICON_RC)
if(WIN32)
    set(ENGINE_ICON_RC src/app/engine_icon.rc)
endif()

set(ENGINE_SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/shaders)
set(ENGINE_SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/shaders)
set(ENGINE_UI_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/ui)
set(ENGINE_SHADER_FLAGS --target-env=vulkan1.2 --target-spv=spv1.4)

function(add_engine_shader source_name shader_stage)
    if(SHADER_COMPILER_IS_GLSLANGVALIDATOR)
        add_custom_command(
            OUTPUT ${ENGINE_SHADER_BINARY_DIR}/${source_name}.spv
            COMMAND ${CMAKE_COMMAND} -E make_directory ${ENGINE_SHADER_BINARY_DIR}
            COMMAND ${GLSLC_EXECUTABLE} -V --target-env vulkan1.2 -S ${shader_stage} -o ${ENGINE_SHADER_BINARY_DIR}/${source_name}.spv ${ENGINE_SHADER_SOURCE_DIR}/${source_name}
            DEPENDS ${ENGINE_SHADER_SOURCE_DIR}/${source_name}
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT ${ENGINE_SHADER_BINARY_DIR}/${source_name}.spv
            COMMAND ${CMAKE_COMMAND} -E make_directory ${ENGINE_SHADER_BINARY_DIR}
            COMMAND ${GLSLC_EXECUTABLE} ${ENGINE_SHADER_FLAGS} -fshader-stage=${shader_stage} -o ${ENGINE_SHADER_BINARY_DIR}/${source_name}.spv ${ENGINE_SHADER_SOURCE_DIR}/${source_name}
            DEPENDS ${ENGINE_SHADER_SOURCE_DIR}/${source_name}
            VERBATIM
        )
    endif()

    list(APPEND ENGINE_SHADER_OUTPUTS ${ENGINE_SHADER_BINARY_DIR}/${source_name}.spv)
    set(ENGINE_SHADER_OUTPUTS ${ENGINE_SHADER_OUTPUTS} PARENT_SCOPE)
endfunction()

set(ENGINE_SHADER_OUTPUTS)
add_engine_shader(standard_rt.rgen rgen)
add_engine_shader(standard_rt.rmiss rmiss)
add_engine_shader(standard_rt_shadow.rmiss rmiss)
add_engine_shader(standard_rt.rchit rchit)
add_engine_shader(standard_rt_shadow.rchit rchit)
add_engine_shader(standard_rt_primary.rahit rahit)
add_engine_shader(standard_rt_shadow.rahit rahit)
add_engine_shader(skinning.comp comp)
add_engine_shader(taa.comp comp)
add_engine_shader(overlay2d.vert vert)
add_engine_shader(overlay2d.frag frag)
add_engine_shader(animator_preview.vert vert)
add_engine_shader(animator_preview.frag frag)

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

add_library(imguizmo STATIC
    ${IMGUIZMO_DIR}/ImGuizmo.cpp
)

target_include_directories(imguizmo
    PUBLIC
        ${IMGUIZMO_DIR}
        ${IMGUI_DIR}
)

target_link_libraries(imguizmo PUBLIC imgui)

add_library(imguicolortextedit STATIC
    ${IMGUICOLORTEXTEDIT_DIR}/TextEditor.cpp
)
target_include_directories(imguicolortextedit PUBLIC ${IMGUICOLORTEXTEDIT_DIR})
target_link_libraries(imguicolortextedit PUBLIC imgui)

set(IMGUI_NODE_EDITOR_DIR ${imgui_node_editor_SOURCE_DIR})
set(IMGUI_NODE_EDITOR_UTILS_DIR ${IMGUI_NODE_EDITOR_DIR}/examples/blueprints-example/utilities)
# Note: builders.cpp from the blueprints-example is intentionally NOT compiled
# here because it requires the stack-layout fork of imgui (Spring/BeginHorizontal)
# which is not in upstream imgui. drawing.cpp + widgets.cpp depend only on
# standard imgui APIs and provide the pin-icon widgets we need.
add_library(imgui_node_editor STATIC
    ${IMGUI_NODE_EDITOR_DIR}/crude_json.cpp
    ${IMGUI_NODE_EDITOR_DIR}/imgui_canvas.cpp
    ${IMGUI_NODE_EDITOR_DIR}/imgui_node_editor.cpp
    ${IMGUI_NODE_EDITOR_DIR}/imgui_node_editor_api.cpp
    ${IMGUI_NODE_EDITOR_UTILS_DIR}/drawing.cpp
    ${IMGUI_NODE_EDITOR_UTILS_DIR}/widgets.cpp
)
target_include_directories(imgui_node_editor PUBLIC ${IMGUI_NODE_EDITOR_DIR} ${IMGUI_NODE_EDITOR_UTILS_DIR})
target_link_libraries(imgui_node_editor PUBLIC imgui)
if(MSVC)
    target_compile_options(imgui_node_editor PRIVATE /W0)
endif()

if(MSVC)
    target_compile_options(imguizmo PRIVATE /wd4100 /wd4244 /wd4456)
    target_compile_options(imguicolortextedit PRIVATE /W0)
endif()

add_executable(engine
    ${ENGINE_ICON_RC}
    src/app/main.cpp
    src/app/VulkanContext.cpp
    src/assets/AssetMetadata.cpp
    src/assets/AnimatorControllerAsset.cpp
    src/assets/ModelAsset.cpp
    src/app/EngineApplication.cpp
    src/assets/ModelMetadata.cpp
    src/assets/SceneMetadata.cpp
    src/audio/AudioEngine.cpp
    src/vfs/AssetVFS.cpp
    src/components/CreationMenu.cpp
    src/components/EditorComponent.cpp
    src/components/FileContextMenu.cpp
    src/components/NodeGraphComponent.cpp
    src/components/SceneObjectAttributesEditor.cpp
    src/components/BuildSettingsComponent.cpp
    src/components/graph/NodeSpec.cpp
    src/components/graph/BuiltinNodes.cpp
    src/components/graph/GraphDocument.cpp
    src/components/graph/GraphTranspiler.cpp
    src/dialogs/BuildGameDialog.cpp
    src/dialogs/NewProjectDialog.cpp
    src/dialogs/OpenProjectDialog.cpp
    src/vfs/PakArchive.cpp
    src/panels/FilesPanel.cpp
    src/panels/EffectsPanel.cpp
    src/panels/AnimatorPanel.cpp
    src/panels/AnimatorPreviewRenderer.cpp
    src/panels/InfoPanel.cpp
    src/panels/LogPanel.cpp
    src/panels/PerformancePanel.cpp
    src/panels/VersionControlPanel.cpp
    src/render/Lighting.cpp
    src/render/Raytracing.cpp
    src/render/RuntimeRenderer.cpp
    src/render/RuntimeScriptAPI.cpp
    src/render/PhysicsWorld.cpp
    src/render/Scene2DRenderer.cpp
    src/render/SkyboxRenderer.cpp
    src/render/SceneViewportRenderer.cpp
    src/render/VideoPlaybackManager.cpp
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
    imguicolortextedit
    imgui_node_editor
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
)

if(TARGET assimp::assimp)
    target_link_libraries(engine PRIVATE assimp::assimp)
elseif(TARGET assimp)
    target_link_libraries(engine PRIVATE assimp)
endif()

add_dependencies(engine engine_scene_shaders)

if(WIN32)
    target_link_libraries(engine PRIVATE Ole32 Shell32)

    # Apply Windows executable branding only to the engine target.
    # The logical CMake target name stays "engine"; only the produced .exe name changes.
    set_target_properties(engine PROPERTIES
        OUTPUT_NAME "${ENGINE_EXE_NAME}"
    )

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
        "/external:I${IMGUI_DIR}"
        "/external:I${IMGUI_DIR}/backends"
        "/external:I${assimp_SOURCE_DIR}/include"
        "/external:I${stb_SOURCE_DIR}"
        "/external:I${IMGUICOLORTEXTEDIT_DIR}"
    )

    # Hide console window for engine Release builds while keeping Debug
    # attached to a console for development logs.
    target_link_options(engine PRIVATE $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)
else()
    target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(engine PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

endif() # ENGINE_BUILD_GAME