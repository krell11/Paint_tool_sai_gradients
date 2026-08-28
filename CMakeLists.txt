cmake_minimum_required(VERSION 3.20)
project(SaiGradientMap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)
if(MSVC)
    target_compile_options(imgui PRIVATE /W3 /utf-8)
endif()

add_executable(SaiGradientMap WIN32
    src/main.cpp
    src/image.cpp
    src/clipboard.cpp
    src/gradient.cpp
)
target_include_directories(SaiGradientMap PRIVATE
    src
    ${stb_SOURCE_DIR}
)
target_compile_definitions(SaiGradientMap PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_link_libraries(SaiGradientMap PRIVATE
    imgui
    comdlg32
    shell32
    ole32
)
if(MSVC)
    target_compile_options(SaiGradientMap PRIVATE /utf-8 /W4 /permissive-)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT SaiGradientMap)
endif()
