cmake_minimum_required(VERSION 3.20)
project(RTSS_Clone LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only.")
endif()

if(MSVC)
    add_compile_options(/W3 /WX-)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
endif()

include(FetchContent)

FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master)
FetchContent_MakeAvailable(minhook)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4)
FetchContent_MakeAvailable(imgui)

# ImGui static lib — includes OpenGL3 backend for Psych Engine
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC
    d3d9 d3d11 d3d12 dxgi dxguid opengl32)

# Shared headers
add_library(shared_headers INTERFACE)
target_include_directories(shared_headers INTERFACE ${CMAKE_SOURCE_DIR}/Shared)

# Injector.exe
add_executable(Injector WIN32 Injector/main.cpp)
target_link_libraries(Injector PRIVATE shared_headers psapi gdiplus comctl32)
set_target_properties(Injector PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME "Injector")

# HookDLL.dll
add_library(HookDLL SHARED HookDLL/dllmain.cpp HookDLL/hooks.cpp)
target_include_directories(HookDLL PRIVATE
    ${CMAKE_SOURCE_DIR}/Shared
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${minhook_SOURCE_DIR}/include)
target_compile_definitions(HookDLL PRIVATE NOMINMAX)
target_link_libraries(HookDLL PRIVATE
    shared_headers imgui_lib minhook
    d3d9 d3d11 d3d12 dxgi dxguid opengl32 pdh)
set_target_properties(HookDLL PROPERTIES OUTPUT_NAME "HookDLL")

# SystemStats.exe
add_executable(SystemStats SystemStats/SystemStats.cpp)
target_link_libraries(SystemStats PRIVATE shared_headers pdh wbemuuid ole32 oleaut32)
set_target_properties(SystemStats PROPERTIES WIN32_EXECUTABLE FALSE OUTPUT_NAME "SystemStats")

# OverlayViewer.exe (external debug GUI)
add_executable(OverlayViewer WIN32 Overlay/overlay_viewer.cpp)
target_link_libraries(OverlayViewer PRIVATE shared_headers gdiplus msimg32)
set_target_properties(OverlayViewer PROPERTIES OUTPUT_NAME "OverlayViewer")


# ---- OverlayEditor.exe (live config editor) ----
add_executable(OverlayEditor WIN32 Overlay/overlay_editor.cpp)
target_include_directories(OverlayEditor PRIVATE ${CMAKE_SOURCE_DIR}/Shared)
target_link_libraries(OverlayEditor PRIVATE shared_headers gdiplus comctl32 comdlg32)
set_target_properties(OverlayEditor PROPERTIES OUTPUT_NAME "OverlayEditor")


# ---- RTSSClone.exe (all-in-one launcher with embedded DLL) ----
# The .rc file embeds HookDLL.dll as a binary resource.
# CMake + MSVC handle .rc files automatically.
add_executable(RTSSClone WIN32
    Launcher/launcher.cpp
    Launcher/launcher.rc
)
target_include_directories(RTSSClone PRIVATE
    ${CMAKE_SOURCE_DIR}/Shared
    ${CMAKE_SOURCE_DIR}/Launcher
)
target_link_libraries(RTSSClone PRIVATE
    shared_headers gdiplus comctl32 comdlg32 psapi
)
set_target_properties(RTSSClone PROPERTIES OUTPUT_NAME "RTSSClone")
# HookDLL must be built first so the .rc can embed it
add_dependencies(RTSSClone HookDLL)
# After HookDLL builds, copy it next to the .rc so the resource compiler finds it
add_custom_command(TARGET RTSSClone PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:HookDLL>
        ${CMAKE_SOURCE_DIR}/Launcher/HookDLL.dll
    COMMENT "Copying HookDLL.dll for resource embedding"
)

install(TARGETS Injector HookDLL SystemStats OverlayViewer OverlayEditor RTSSClone
    RUNTIME DESTINATION bin LIBRARY DESTINATION bin)

message(STATUS "Configured: Injector | HookDLL | SystemStats | OverlayViewer")
