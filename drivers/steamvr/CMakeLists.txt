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

# Optional Rift CV1 full body support: needs the LibOVR headers + shim (Oculus SDK,
# not redistributed here). scripts/fetch-libovr.ps1 downloads them into tools/.
set(KAGE_LIBOVR_DIR "${CMAKE_SOURCE_DIR}/tools/otsl/CustomHMD/LibOVR" CACHE PATH "LibOVR (Include + Shim)")
if(EXISTS "${KAGE_LIBOVR_DIR}/Shim/OVR_CAPIShim.c")
    target_sources(driver_kage PRIVATE src/rift_tracking.cpp "${KAGE_LIBOVR_DIR}/Shim/OVR_CAPIShim.c")
    target_include_directories(driver_kage PRIVATE "${KAGE_LIBOVR_DIR}/Include")
    target_compile_definitions(driver_kage PRIVATE KAGE_WITH_RIFT=1)
    message(STATUS "KAGE: Rift CV1 full body support enabled (${KAGE_LIBOVR_DIR})")
else()
    message(STATUS "KAGE: LibOVR not found, Rift CV1 full body disabled (run scripts/fetch-libovr.ps1)")
endif()
