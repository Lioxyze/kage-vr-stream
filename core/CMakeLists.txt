# kage_core: platform-neutral protocol, networking, sync and telemetry.
# Built for Windows (host) and Android arm64 (Quest client, via the NDK).
add_library(kage_core STATIC
    src/packet.cpp
    src/messages.cpp
    src/video_fragment.cpp
    src/clock_sync.cpp
    src/udp_socket.cpp
    src/log.cpp
    src/host_service.cpp
    src/client_link.cpp
    src/secure.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher/monocypher.c
)
add_library(kage::core ALIAS kage_core)

target_include_directories(kage_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher)
target_compile_features(kage_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(kage_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(kage_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(kage_core PUBLIC ws2_32 bcrypt)
else()
    target_compile_options(kage_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ANDROID)
    find_library(ANDROID_LOG_LIB log)
    target_link_libraries(kage_core PUBLIC ${ANDROID_LOG_LIB})
endif()

find_package(Threads REQUIRED)
target_link_libraries(kage_core PUBLIC Threads::Threads)

if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/monocypher/monocypher.c PROPERTIES COMPILE_OPTIONS "/W3;/wd4244")
endif()
