cmake_minimum_required(VERSION 3.22)

project(KageVRStream VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Standard C file/env functions (fopen, fscanf, getenv) are used on purpose: the MSVC "_s"
# variants are not portable (core also builds for Android).
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

option(KAGE_BUILD_TESTS "Build unit and loopback integration tests" ON)
option(KAGE_BUILD_HOST "Build the Windows host application" ${WIN32})

add_subdirectory(core)

if(KAGE_BUILD_HOST)
    if(NOT WIN32)
        message(FATAL_ERROR "The host application targets Windows only")
    endif()
    add_subdirectory(apps/windows-host)
    add_subdirectory(drivers/steamvr)
endif()

if(KAGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
