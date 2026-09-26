# =============================================================================
#  tests/ —— 分两套，对应两个依赖边界
#
#  penhu-tests-portable
#      只测纯逻辑（金额、日期、统计、异常、建议、prompt）。
#      零三方依赖，几秒就能编完跑完。这是日常改代码时该跑的那一套。
#
#  penhu-tests
#      加上加密、存储、多账号隔离、端到端。需要 SQLCipher/libsodium。
# =============================================================================

add_executable(penhu-tests-portable
    main.cpp
    test_domain.cpp
    test_analytics.cpp
)
target_link_libraries(penhu-tests-portable PRIVATE penhu::portable)
target_include_directories(penhu-tests-portable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC AND PENHU_STATIC_RUNTIME)
    set_property(TARGET penhu-tests-portable
                 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME portable COMMAND penhu-tests-portable)

if(NOT PENHU_WITH_NATIVE_DEPS)
    return()
endif()

add_executable(penhu-tests
    main.cpp
    test_domain.cpp
    test_analytics.cpp
    test_crypto.cpp
    test_integration.cpp
)
target_link_libraries(penhu-tests PRIVATE penhu::core)
target_include_directories(penhu-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC AND PENHU_STATIC_RUNTIME)
    set_property(TARGET penhu-tests
                 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_test(NAME full COMMAND penhu-tests)
