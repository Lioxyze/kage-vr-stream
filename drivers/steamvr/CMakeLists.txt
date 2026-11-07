# SteamVR driver "kage". Output folder layout is what vrpathreg expects:
#   <build>/driver/kage/driver.vrdrivermanifest
#   <build>/driver/kage/bin/win64/driver_kage.dll
#   <build>/driver/kage/resources/settings/default.vrsettings
add_library(driver_kage SHARED
    src/driver.cpp
    src/stream_server.cpp
    src/d3d_compositor.cpp
    src/amf_encoder.cpp
    src/audio_capture.cpp
    src/mic_player.cpp
)
target_include_directories(driver_kage PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/openvr
    ${CMAKE_SOURCE_DIR}/third_party/amf/include)
target_link_libraries(driver_kage PRIVATE kage::core d3d11 dxgi d3dcompiler ole32)
target_compile_options(driver_kage PRIVATE /W3 /permissive- /utf-8)

set(KAGE_DRIVER_DIR ${CMAKE_BINARY_DIR}/driver/kage)
set_target_properties(driver_kage PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${KAGE_DRIVER_DIR}/bin/win64
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${KAGE_DRIVER_DIR}/bin/win64
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${KAGE_DRIVER_DIR}/bin/win64)
add_custom_command(TARGET driver_kage POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/driver.vrdrivermanifest ${KAGE_DRIVER_DIR}/driver.vrdrivermanifest
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources ${KAGE_DRIVER_DIR}/resources)

# Rift CV1 full body: poses come from kage-host over 127.0.0.1 (no LibOVR in SteamVR).
target_sources(driver_kage PRIVATE src/rift_tracking.cpp)
target_compile_definitions(driver_kage PRIVATE KAGE_WITH_RIFT=1)
