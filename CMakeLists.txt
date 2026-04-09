cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

project(EngineSkeleton LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

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

set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

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

set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMNODEFLOW_DIR ${imnodeflow_SOURCE_DIR})

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)

target_include_directories(imgui
    PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC SDL3::SDL3)

add_library(imnodeflow STATIC
    ${IMNODEFLOW_DIR}/src/ImNodeFlow.cpp
)

target_include_directories(imnodeflow
    PUBLIC
        ${IMNODEFLOW_DIR}/include
    PRIVATE
        ${IMNODEFLOW_DIR}/src
)

target_compile_definitions(imnodeflow PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imnodeflow PUBLIC imgui)

if(MSVC)
    target_compile_options(imnodeflow PRIVATE /wd4100 /wd4244)
endif()

add_executable(engine
    src/app/main.cpp
    src/assets/AssetMetadata.cpp
    src/app/EngineApplication.cpp
    src/assets/ModelMetadata.cpp
    src/components/CreationMenu.cpp
    src/components/FileContextMenu.cpp
    src/dialogs/NewProjectDialog.cpp
    src/dialogs/OpenProjectDialog.cpp
    src/graph/NodeLibrary.cpp
    src/panels/FilesPanel.cpp
    src/panels/InfoPanel.cpp
    src/panels/LogPanel.cpp
    src/panels/SettingsPanel.cpp
    src/panels/WorkspacePanel.cpp
)

target_include_directories(engine PRIVATE src)

target_compile_definitions(engine PRIVATE
    SDL_MAIN_HANDLED
    IMGUI_DEFINE_MATH_OPERATORS
)

target_link_libraries(engine PRIVATE
    SDL3::SDL3
    imgui
    imnodeflow
)

if(TARGET assimp::assimp)
    target_link_libraries(engine PRIVATE assimp::assimp)
elseif(TARGET assimp)
    target_link_libraries(engine PRIVATE assimp)
endif()

if(WIN32)
    target_link_libraries(engine PRIVATE Ole32 Shell32)

    add_custom_command(TARGET engine POST_BUILD
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