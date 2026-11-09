add_executable(kage-host
    src/main.cpp
    src/probe.cpp
)
target_link_libraries(kage-host PRIVATE kage::core dxgi mfplat mfuuid iphlpapi wlanapi ole32)
if(MSVC)
    target_compile_options(kage-host PRIVATE /W4 /permissive- /utf-8)
endif()
set_target_properties(kage-host PROPERTIES OUTPUT_NAME "kage-host")

# Rift CV1 reader for full body (LibOVR headers + shim, see scripts/fetch-libovr.ps1).
set(KAGE_LIBOVR_DIR "${CMAKE_SOURCE_DIR}/tools/otsl/CustomHMD/LibOVR" CACHE PATH "LibOVR (Include + Shim)")
if(EXISTS "${KAGE_LIBOVR_DIR}/Shim/OVR_CAPIShim.c")
    target_sources(kage-host PRIVATE src/rift_reader.cpp "${KAGE_LIBOVR_DIR}/Shim/OVR_CAPIShim.c")
    target_include_directories(kage-host PRIVATE "${KAGE_LIBOVR_DIR}/Include")
    target_compile_definitions(kage-host PRIVATE KAGE_WITH_RIFT=1)
    set_source_files_properties("${KAGE_LIBOVR_DIR}/Shim/OVR_CAPIShim.c" PROPERTIES COMPILE_OPTIONS "/W0")
endif()
