# =============================================================================
#  penhu-core —— 分成两个目标，这不是为了好看，是为了依赖边界能被验证
#
#  penhu-core-portable
#      纯逻辑：结果类型、日志、路径、子进程、领域模型、统计、报告 prompt 构造。
#      零第三方依赖（nlohmann/json 是 vendored 的单头文件）。
#      可以脱离 sqlite / sodium / httplib 单独编译和测试 —— 所以
#      「金额算错了、日期算错了、异常判定错了」这类的 bug 能在几秒内被发现，
#      不用等重型依赖编完。
#
#  penhu-core
#      = portable + 加密(SQLCipher/libsodium) + 存储 + 应用服务 + LLM 客户端 + 部署框架
#
#  这个切分还有第二个用处：portable 部分就是将来若有需要可以往
#  WebAssembly / 移动端编的候选。注意「零三方依赖」不等于「零系统依赖」：
#  util/fs 与 util/process 会碰操作系统的文件与子进程 API —— 真要上别的
#  平台，换掉这两个文件的实现即可，接口已经把平台差异挡在了外面。
# =============================================================================

# -----------------------------------------------------------------------------
#  第三方依赖（只在需要时去找）
# -----------------------------------------------------------------------------
option(PENHU_WITH_NATIVE_DEPS
       "构建依赖 SQLCipher / libsodium / cpp-httplib 的部分（存储、加密、LLM、部署）"
       ON)

set(PENHU_VENDOR_INCLUDE ${CMAKE_SOURCE_DIR}/third_party)

# -----------------------------------------------------------------------------
#  portable 目标
# -----------------------------------------------------------------------------
set(PENHU_PORTABLE_SOURCES
    src/util/result.cpp
    src/util/log.cpp
    src/util/uuid.cpp
    src/util/fs.cpp
    src/util/process.cpp

    src/domain/money.cpp
    src/domain/date.cpp
    src/domain/record.cpp
    src/domain/category.cpp
    src/domain/user.cpp

    src/analytics/summary.cpp
    src/analytics/trend.cpp
    src/analytics/anomaly.cpp
    src/analytics/insight.cpp

    src/report/prompt_builder.cpp
)

add_library(penhu-core-portable STATIC ${PENHU_PORTABLE_SOURCES})
add_library(penhu::portable ALIAS penhu-core-portable)

target_include_directories(penhu-core-portable
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PENHU_VENDOR_INCLUDE}>
)

set_target_properties(penhu-core-portable PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

if(MSVC AND PENHU_STATIC_RUNTIME)
    set_property(TARGET penhu-core-portable
                 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${PENHU_PORTABLE_SOURCES})

message(STATUS "  core-portable: ${CMAKE_CURRENT_SOURCE_DIR} 的 ${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------------
#  完整目标（叠加原生依赖）
# -----------------------------------------------------------------------------
if(NOT PENHU_WITH_NATIVE_DEPS)
    message(STATUS "  native deps : 已关闭（只构建 portable 部分）")
    return()
endif()

# -----------------------------------------------------------------------------
#  第三方依赖：**在根 CMakeLists.txt 里查找**，这里只做校验
#
#  为什么不在这里 find_package：IMPORTED target 是目录作用域的，
#  在 core/ 里创建的目标同级目录（server/、desktop/）看不见。
#  详见根 CMakeLists.txt 里那段注释（那里踩过一次，
#  症状是 "Target penhu-server links to httplib::httplib but the target was
#  not found"，很容易误判成「包没装」）。
#
#  这里保留校验：万一有人单独构建 core/，也应该得到一句能读懂的报错，
#  而不是后面一堆莫名其妙的未定义符号。
# -----------------------------------------------------------------------------
if(NOT PENHU_SQLITE_TARGET OR NOT TARGET unofficial-sodium::sodium OR NOT TARGET httplib::httplib)
    message(FATAL_ERROR
        "原生依赖没有在根 CMakeLists.txt 里被找到。\n"
        "  请从项目根目录 configure（cmake -S . -B build ...），不要直接 configure core/。\n"
        "  并确认已执行：bash _vcpkg.sh install --triplet x64-windows-static")
endif()

set(PENHU_CORE_SOURCES
    src/crypto/secure_buffer.cpp
    src/crypto/password.cpp
    src/crypto/cipher.cpp
    src/crypto/session.cpp

    src/storage/database.cpp
    src/storage/migrations.cpp
    src/storage/user_repository.cpp
    src/storage/category_repository.cpp
    src/storage/record_repository.cpp
    src/storage/report_repository.cpp

    src/llm/provider.cpp

    src/report/report_service.cpp

    # 截图识别：调多模态模型把支付截图变成结构化支出草稿
    src/vision/receipt_scan.cpp

    src/deploy/local_model.cpp

    src/app/app_config.cpp
    src/app/llm_settings.cpp
    src/app/ledger_service.cpp
)

add_library(penhu-core STATIC ${PENHU_CORE_SOURCES})
add_library(penhu::core ALIAS penhu-core)

target_link_libraries(penhu-core
    PUBLIC
        penhu::portable
    PRIVATE
        ${PENHU_SQLITE_TARGET}
        unofficial-sodium::sodium
        httplib::httplib
)

set_target_properties(penhu-core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

if(MSVC AND PENHU_STATIC_RUNTIME)
    set_property(TARGET penhu-core
                 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# -----------------------------------------------------------------------------
#  TLS 探测
#  cpp-httplib 需要 OpenSSL 才能走 https。云端 LLM 和「自动获取本地运行时」
#  都依赖它。没有 TLS 时不是静默降级，而是明确报错（见 llm/provider.cpp 与
#  deploy/local_model.cpp）。
# -----------------------------------------------------------------------------
if(TARGET OpenSSL::SSL)
    target_compile_definitions(penhu-core PRIVATE PENHU_HTTPLIB_SSL)
    target_link_libraries(penhu-core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "  TLS         : 已启用（https 可用）")
else()
    message(STATUS "  TLS         : 未启用（云端 LLM 与运行时自动下载会被拒绝）")
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${PENHU_CORE_SOURCES})
