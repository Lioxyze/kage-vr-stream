cmake_minimum_required(VERSION 3.22.1)
project(kage_quest C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared core library (protocol, networking, sync) from the monorepo root.
set(KAGE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)
add_subdirectory(${KAGE_ROOT}/core ${CMAKE_BINARY_DIR}/kage_core)

# Khronos OpenXR loader, provided as a prefab package by the AAR dependency.
find_package(OpenXR REQUIRED CONFIG)

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(kage_quest SHARED main.cpp xr_app.cpp renderer.cpp video_decoder.cpp stream_client.cpp)
target_include_directories(kage_quest PRIVATE ${KAGE_ROOT}/third_party/stb)
# OpenXR structs are idiomatically initialised as T x{XR_TYPE_...}; the omitted
# fields (next = nullptr, ...) are intentionally zero.
target_compile_options(kage_quest PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
# Keep ANativeActivity_onCreate exported from the static glue library.
target_link_options(kage_quest PRIVATE "-u" "ANativeActivity_onCreate")
target_link_libraries(kage_quest PRIVATE native_app_glue kage::core OpenXR::openxr_loader android EGL GLESv3 log mediandk nativewindow)
target_compile_options(kage_quest PRIVATE -O2)
