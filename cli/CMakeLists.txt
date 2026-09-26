# =============================================================================
#  cli/CMakeLists.txt
#  命令行入口：自测与排障。不依赖任何 UI。
# =============================================================================

add_executable(penhu-cli src/main.cpp)

target_link_libraries(penhu-cli
    PRIVATE
        penhu::server
)

set_target_properties(penhu-cli PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

if(MSVC)
    if(PENHU_STATIC_RUNTIME)
        set_property(TARGET penhu-cli
                     PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # 控制台程序：保留 main（不是 WinMain）
    target_link_options(penhu-cli PRIVATE /SUBSYSTEM:CONSOLE)
endif()
