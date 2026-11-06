add_executable(kage-host
    src/main.cpp
    src/probe.cpp
)
target_link_libraries(kage-host PRIVATE kage::core dxgi mfplat mfuuid iphlpapi)
if(MSVC)
    target_compile_options(kage-host PRIVATE /W4 /permissive- /utf-8)
endif()
set_target_properties(kage-host PROPERTIES OUTPUT_NAME "kage-host")
