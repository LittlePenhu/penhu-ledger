# =============================================================================
#  PenHu Ledger — 每日花销统计与汇总应用
#
#  分层（严格遵守：依赖只能自上而下）
#    core    纯 C++ 静态库，零 UI 依赖          <- 所有业务逻辑的唯一归属
#    native  原生自绘界面                       <- 最终交付物
#             Windows: Win32 + Direct2D + DirectWrite + WIC
#             Linux:   Wayland + Cairo + Pango
#    cli     自测与运维入口，只依赖 core
#
#  定位：单机本地应用，交付一个可执行文件。
#  （更早的版本是 WebView2 壳 + 进程内 HTTP 服务，那条路已经废弃：
#    渲染崩在 Intel 核显驱动里，且 WebView2 运行时需要单独分发。
#    server/ 目录保留但默认不构建。）
#
#  跨平台：core 与 native/pages 是平台无关的；平台差异集中在
#    · core/util/{fs,process}.cpp   —— 已有 _WIN32 / POSIX 两套分支
#    · native/src/ui/renderer       —— Direct2D 封装 ↔ Cairo 实现
#    · native/src/app/shell         —— Win32 消息循环 ↔ Wayland 事件循环
# =============================================================================
cmake_minimum_required(VERSION 3.21)

project(PenHuLedger
        VERSION 0.1.0
        DESCRIPTION "Daily expense tracking, analytics and LLM-summarised reporting"
        # C 是必需的，不是随手加的：Linux 侧用 wayland-scanner 生成的协议胶水
        # 代码是 **.c** 文件（wayland 官方的生成器只出 C）。只启用 CXX 的话
        # CMake 不会去编译它们，表现是链接期一堆
        # 「undefined reference to `xdg_wm_base_interface'」——
        # 看着像「协议没生成」，其实是「生成了但没编」。
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 统一输出目录，方便 Windows 下直接跑 exe（DLL 与 exe 同目录即可加载）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/lib)
endforeach()

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /MP)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    # sqlite3 等第三方头会触发大量警告，屏蔽噪声但保留我们自己的告警
    add_compile_options(/wd4244 /wd4267 /wd4996)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_compile_definitions(PENHU_LEDGER_VERSION="${PROJECT_VERSION}")

# -----------------------------------------------------------------------------
#  依赖边界的总开关
#
#  OFF 时只构建 penhu-core-portable + 纯逻辑测试：零三方依赖，
#  几秒编完。用于快速验证「金额/日期/统计/异常判定」这些最容易错的地方，
#  不必等 SQLCipher 那一大坨从源码编完。
# -----------------------------------------------------------------------------
option(PENHU_WITH_NATIVE_DEPS
       "构建依赖 SQLCipher / libsodium / cpp-httplib 的部分（存储、加密、LLM、部署）"
       ON)

# 单文件 exe：静态链接 CRT，目标是双击就跑、不带一堆 DLL。
option(PENHU_STATIC_RUNTIME "静态链接 MSVC 运行时（/MT）" ON)

# -----------------------------------------------------------------------------
#  组件开关
#
#  注意：这几个 option 必须在这里显式声明。CMake 里未声明的变量展开成空串，
#  `if(PENHU_BUILD_SERVER)` 会静默为假——结果是子目录一个都没加进构建，
#  但 configure 阶段一声不响。这种沉默比报错更难查，所以宁可显式写出来。
# -----------------------------------------------------------------------------
option(PENHU_BUILD_SERVER  "构建进程内 HTTP/REST 服务"   ON)
option(PENHU_BUILD_NATIVE  "构建原生界面（最终 exe，纯 Win32 + Direct2D，无 WebView）" ON)
option(PENHU_BUILD_CLI     "构建 CLI 自测/运维工具"      ON)
option(PENHU_BUILD_TESTS   "构建测试"                    ON)

# -----------------------------------------------------------------------------
#  第三方依赖查找：**必须放在根目录**，这是 CMake 的一个语义陷阱
#
#  find_package(CONFIG) 通过 Targets.cmake 建立的 IMPORTED target 是
#  **目录作用域**的：它只在「创建它的那个目录及其子目录」可见，兄弟目录看不见。
#  所以如果把 find_package(httplib) 写在 core/ 里，core 自己能链接，
#  但同级的 server/ 链接 httplib::httplib 时就会报：
#      Target "penhu-server" links to: httplib::httplib
#      but the target was not found.
#  这个报错会把人往「包名写错了 / 没装」方向带，其实包装得好好的 ——
#  只是作用域不对。放在根目录就一次性对所有子目录可见。
#
#  依赖查找本身也有讲究：vcpkg 里这个包的 CMake 配置历史上换过命名，
#  见下面 SQLCipher 那段。
# -----------------------------------------------------------------------------
if(PENHU_WITH_NATIVE_DEPS)
    if(WIN32)
        # ---- Windows：vcpkg 提供的 CONFIG 包 ----
        # ---- SQLCipher（sqlite3 的加密分支）----
        set(PENHU_SQLITE_TARGET "")
        foreach(_pkg sqlcipher unofficial-sqlcipher SQLite3)
            find_package(${_pkg} CONFIG QUIET)
            foreach(_candidate sqlcipher::sqlcipher unofficial-sqlcipher::sqlcipher SQLite::SQLite3)
                if(TARGET ${_candidate})
                    set(PENHU_SQLITE_TARGET ${_candidate})
                    break()
                endif()
            endforeach()
            if(PENHU_SQLITE_TARGET)
                message(STATUS "  SQLCipher   : 包 ${_pkg}，目标 ${PENHU_SQLITE_TARGET}")
                break()
            endif()
        endforeach()

        if(NOT PENHU_SQLITE_TARGET)
            # 走到这里说明既没装 SQLCipher 也没装普通 SQLite。
            # 明确报错，不要静默降级成「没有加密」——那是最危险的一种「成功」。
            message(FATAL_ERROR
                "找不到 SQLCipher。本项目要求整库加密，不接受用普通 SQLite 顶替。\n"
                "请先执行：bash _vcpkg.sh install --triplet x64-windows-static")
        endif()

        # ---- libsodium（Argon2id / AEAD / CSPRNG）----
        find_package(unofficial-sodium CONFIG REQUIRED)

        # ---- cpp-httplib（LLM 客户端 + 本机 REST 服务）----
        # 注意它自带的 config 会去 find_dependency(OpenSSL) 和 unofficial-brotli，
        # 所以「TLS 是否可用」由这里决定。core 里靠 TARGET OpenSSL::SSL 判断。
        find_package(httplib CONFIG REQUIRED)
    else()
        # ---- Linux / 其它 Unix：用发行版提供的库 ----
        #
        # 这里**故意伪造出与 vcpkg 同名的 target**（sqlcipher::sqlcipher /
        # unofficial-sodium::sodium / httplib::httplib）——
        # core/ 与 cli/ 的 CMakeLists 一行都不用改，两个平台共用同一份链接声明。
        # 少一套 #ifdef 就少一处「只在某个平台编不过」的隐患。
        find_package(PkgConfig REQUIRED)
        find_package(OpenSSL REQUIRED)

        # ---- SQLCipher ----
        # 头文件在 <prefix>/include/sqlcipher/sqlite3.h（Arch 与 Debian 都是这个布局），
        # 所以 include 目录要指到 sqlcipher 子目录，代码里的 #include <sqlite3.h> 才找得到。
        find_library(PENHU_SQLCIPHER_LIBRARY NAMES sqlcipher REQUIRED)
        find_path(PENHU_SQLCIPHER_INCLUDE_DIR NAMES sqlite3.h
                  PATH_SUFFIXES sqlcipher)
        if(NOT PENHU_SQLCIPHER_INCLUDE_DIR)
            message(FATAL_ERROR
                "找到 libsqlcipher 但没找到 sqlite3.h。\n"
                "  Arch:   sudo pacman -S sqlcipher\n"
                "  Debian: sudo apt install libsqlcipher-dev")
        endif()
        add_library(sqlcipher::sqlcipher UNKNOWN IMPORTED)
        set_target_properties(sqlcipher::sqlcipher PROPERTIES
            IMPORTED_LOCATION "${PENHU_SQLCIPHER_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PENHU_SQLCIPHER_INCLUDE_DIR}")
        set(PENHU_SQLITE_TARGET sqlcipher::sqlcipher)
        message(STATUS "  SQLCipher   : ${PENHU_SQLCIPHER_LIBRARY}")

        # ---- libsodium ----
        pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
        add_library(unofficial-sodium::sodium ALIAS PkgConfig::SODIUM)

        # ---- cpp-httplib（header-only）----
        find_path(PENHU_HTTPLIB_INCLUDE_DIR NAMES httplib.h)
        if(NOT PENHU_HTTPLIB_INCLUDE_DIR)
            message(FATAL_ERROR
                "找不到 httplib.h。\n"
                "  Arch:   sudo pacman -S cpp-httplib\n"
                "  Debian: sudo apt install libcpp-httplib-dev")
        endif()
        add_library(httplib::httplib INTERFACE IMPORTED)
        set_target_properties(httplib::httplib PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${PENHU_HTTPLIB_INCLUDE_DIR}")
        message(STATUS "  httplib     : ${PENHU_HTTPLIB_INCLUDE_DIR}")
    endif()
endif()

add_subdirectory(core)

# 下面这些全部依赖 native deps（HTTP 服务、WebView2、CLI 都要用 core 的完整版）
if(PENHU_WITH_NATIVE_DEPS)
    if(PENHU_BUILD_SERVER)
        add_subdirectory(server)
    endif()
    if(PENHU_BUILD_NATIVE)
        add_subdirectory(native)
    endif()
    if(PENHU_BUILD_CLI)
        add_subdirectory(cli)
    endif()
endif()

if(PENHU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
#  安装规则
#
#  Linux 打包（PKGBUILD / deb / rpm / AppImage）都靠这一节：
#  没有 install() 规则的话，打包脚本只能手工 cp，一旦文件位置变了就静默漏装。
#  Windows 下也可以用 install 出一个干净的发布目录。
# -----------------------------------------------------------------------------
include(GNUInstallDirs)

if(PENHU_WITH_NATIVE_DEPS AND PENHU_BUILD_NATIVE AND TARGET penhu-native)
    install(TARGETS penhu-native RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    if(UNIX)
        # 桌面入口与图标：Wayland 下的任务栏/启动器图标**只认 .desktop + hicolor**，
        # 没有运行时 API 可以设置（这点和 Windows 的 wc.hIcon 完全不同）。
        install(FILES ${CMAKE_SOURCE_DIR}/packaging/penhu-ledger.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/packaging/icons/hicolor/
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
    endif()
endif()

if(PENHU_WITH_NATIVE_DEPS AND PENHU_BUILD_CLI AND TARGET penhu-cli)
    install(TARGETS penhu-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()


message(STATUS "")
message(STATUS "PenHu Ledger ${PROJECT_VERSION}")
message(STATUS "  compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  native deps : ${PENHU_WITH_NATIVE_DEPS}")
message(STATUS "  server      : ${PENHU_BUILD_SERVER}")
message(STATUS "  native ui   : ${PENHU_BUILD_NATIVE}")
message(STATUS "  cli         : ${PENHU_BUILD_CLI}")
message(STATUS "  tests       : ${PENHU_BUILD_TESTS}")
message(STATUS "")
