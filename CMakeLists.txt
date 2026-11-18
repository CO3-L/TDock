cmake_minimum_required(VERSION 3.20)
project(TDock VERSION 0.1.21 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "TDock requires the x64 Visual Studio C++ toolchain and Windows SDK.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(TDockNative SHARED
    src/TDockNative.cpp
    src/TaskbarCenter.cpp
    src/TDockNative.manifest
    third_party/minhook-1.3.4/src/buffer.c
    third_party/minhook-1.3.4/src/hook.c
    third_party/minhook-1.3.4/src/trampoline.c
    third_party/minhook-1.3.4/src/hde/hde64.c
)
target_compile_features(TDockNative PRIVATE cxx_std_20)
target_include_directories(TDockNative PRIVATE third_party/minhook-1.3.4/include)
target_compile_definitions(TDockNative PRIVATE
    UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00
    _WIN32_IE=0x0A00 _CRT_SECURE_NO_WARNINGS
)
target_compile_options(TDockNative PRIVATE /utf-8 "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
target_link_libraries(TDockNative PRIVATE comctl32 user32 gdi32 msimg32 ole32 oleaut32 oleacc shell32 propsys windowscodecs uuid)

install(TARGETS TDockNative RUNTIME DESTINATION .)
