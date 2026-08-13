cmake_minimum_required(VERSION 3.20)
project(RoughOcean LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

add_executable(RoughOcean WIN32
    src/main.cpp
    src/App.cpp
    src/Audio.cpp
    src/GpuContext.cpp
    src/Meteor.cpp
    src/Ocean.cpp
    src/Whirlpool.cpp
    src/Sky.cpp
    src/Buoy.cpp
    src/Post.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
)

target_include_directories(RoughOcean PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(RoughOcean PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(RoughOcean PRIVATE /W3 /MP /EHsc)
endif()

target_link_libraries(RoughOcean PRIVATE
    d3d12 dxgi d3dcompiler dxguid ole32 windowscodecs xaudio2
    mfplat mfreadwrite mfuuid
)

# Copy HLSL shaders and audio assets next to the built executable so it finds
# them at runtime.
add_custom_command(TARGET RoughOcean POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:RoughOcean>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:RoughOcean>/assets
)
