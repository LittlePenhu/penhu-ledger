# =============================================================================
#  native/CMakeLists.txt
#  原生自绘界面。两个平台后端，同一套 Widget 树与页面代码。
#
#    Windows: Win32 窗口 + Direct2D 绘制 + DirectWrite 文本 + WIC 图片
#    Linux:   Wayland 窗口 + Cairo 绘制 + Pango 文本 + libpng/libjpeg 图片
#
#  界面全部自绘：没有用系统标准控件，因为 M3 的圆角、状态层、48dp 触控目标
#  和自定义配色在标准控件上做不到（子类化改一个控件的外观，代价比自绘一个还大）。
#  Linux 上没有系统标题栏可谈，客户端装饰（CSD）本来就是这个应用的原生形态。
#
#  产物：penhu-native（Windows 下为 penhu-native.exe）—— 本项目的最终交付物。
# =============================================================================

# 迭代期用 GLOB 而不是逐个列文件名：加一个页面不必回来改构建脚本。
# CONFIGURE_DEPENDS 让 ninja 在检测到源文件增删时自动重新 configure，
# 否则「新建了 .cpp 但没生效」会浪费大量时间去查。
file(GLOB_RECURSE PENHU_NATIVE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# -----------------------------------------------------------------------------
#  平台源文件选择
#
#  GLOB 会把两个平台的实现都收进来，所以这里按平台剔掉对方那份。
#  命名约定：Windows 实现是「无后缀」的原名（renderer.cpp / shell.cpp / …），
#  Linux 实现带后缀（renderer_cairo.cpp / shell_linux.cpp / …）。
#  为什么不让 Windows 那份也叫 _win32：这些文件名已经写在文档和崩溃排查记录里
#  （「看 shell.cpp 的 WM_SIZE」），改名的收益抵不过翻旧记录的麻烦。
#
#  平台无关的部分（widget / controls / pages / app 的业务逻辑）两个平台共用，
#  不在这两个列表里。
# -----------------------------------------------------------------------------
set(PENHU_NATIVE_WIN_ONLY
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/shell.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/shell_window.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/shell_diag.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/image_util.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/platform_fs_win.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/platform_input.cpp)

set(PENHU_NATIVE_LINUX_ONLY
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main_linux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/shell_linux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/image_util_linux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/platform_fs_linux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/renderer_cairo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/platform_input_linux.cpp)

if(WIN32)
    list(REMOVE_ITEM PENHU_NATIVE_SOURCES ${PENHU_NATIVE_LINUX_ONLY})
else()
    list(REMOVE_ITEM PENHU_NATIVE_SOURCES ${PENHU_NATIVE_WIN_ONLY})
endif()

if(WIN32)
    add_executable(penhu-native WIN32
        ${PENHU_NATIVE_SOURCES}
        # 图标与顶栏 logo（任务栏 / Alt+Tab / exe 文件图标都来自这里的 ICON 资源）
        resources/app.rc)
else()
    # ---- Linux：Wayland 协议代码生成 ----
    # xdg-shell 是窗口的立身之本（toplevel、configure、最大化/全屏/还原）；
    # text-input-v3 是输入法（fcitx5 在 Wayland 下走这个协议）。
    # 这两份 .xml 由 wayland-protocols 提供，用 wayland-scanner 生成 C 代码。
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    set(PENHU_WL_GEN_DIR ${CMAKE_BINARY_DIR}/wayland-protocols)
    file(MAKE_DIRECTORY ${PENHU_WL_GEN_DIR})

    function(penhu_wl_protocol xml_rel name)
        set(_xml ${WAYLAND_PROTOCOLS_DIR}/${xml_rel})
        add_custom_command(
            OUTPUT ${PENHU_WL_GEN_DIR}/${name}-client-protocol.h
                   ${PENHU_WL_GEN_DIR}/${name}-protocol.c
            COMMAND ${WAYLAND_SCANNER} client-header ${_xml}
                    ${PENHU_WL_GEN_DIR}/${name}-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} private-code ${_xml}
                    ${PENHU_WL_GEN_DIR}/${name}-protocol.c
            DEPENDS ${_xml}
            COMMENT "wayland-scanner: ${name}"
            VERBATIM)
    endfunction()

    penhu_wl_protocol(stable/xdg-shell/xdg-shell.xml xdg-shell)
    penhu_wl_protocol(unstable/text-input/text-input-unstable-v3.xml text-input-unstable-v3)

    add_executable(penhu-native
        ${PENHU_NATIVE_SOURCES}
        ${PENHU_WL_GEN_DIR}/xdg-shell-protocol.c
        ${PENHU_WL_GEN_DIR}/text-input-unstable-v3-protocol.c)
    target_include_directories(penhu-native PRIVATE ${PENHU_WL_GEN_DIR})
endif()

# 链接时输出映射表（RVA -> 函数名）。用途只有一个：崩溃转储里只有地址没有符号，
# 有了它才能把「penhu-native+0x5dc87d」还原成具体的函数名。
# 它不影响代码生成，只是让链接器多写一个文本文件。
if(WIN32)
    target_link_options(penhu-native PRIVATE "/MAP:${CMAKE_BINARY_DIR}/penhu-native.map")
endif()

target_include_directories(penhu-native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
    target_link_libraries(penhu-native
        PRIVATE
            penhu::core
            # 界面四件套
            d2d1
            dwrite
            windowscodecs
            # Win32 基础设施
            user32
            gdi32
            ole32
            shell32
            shlwapi
            advapi32
            # 原生文件选择对话框（选支付截图）用 GetOpenFileNameW
            comdlg32
            # 输入法：中文备注要靠 ImmGetCompositionStringW 取上屏结果。
            # 少了这个库会报「无法解析的外部符号 ImmReleaseContext」，
            # 提示里完全看不出是缺 imm32。
            imm32
            # 窗口圆角 / 阴影这类系统外观接口
            dwmapi
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)
    pkg_check_modules(XKBCOMMON     REQUIRED IMPORTED_TARGET xkbcommon)
    pkg_check_modules(CAIRO         REQUIRED IMPORTED_TARGET cairo)
    pkg_check_modules(PANGO         REQUIRED IMPORTED_TARGET pango pangocairo)
    pkg_check_modules(FONTCONFIG    REQUIRED IMPORTED_TARGET fontconfig)
    find_package(PNG REQUIRED)
    find_package(JPEG REQUIRED)

    target_link_libraries(penhu-native
        PRIVATE
            penhu::core
            PkgConfig::WAYLAND_CLIENT
            PkgConfig::WAYLAND_CURSOR
            PkgConfig::XKBCOMMON
            PkgConfig::CAIRO
            PkgConfig::PANGO
            PkgConfig::FONTCONFIG
            PNG::PNG
            JPEG::JPEG
            # 本地模型托管：判活/拉起子进程 + 走 127.0.0.1，
            # 这些 core 已经封好了，native 这边只需要 dl 和 pthread。
            dl
            pthread
    )
    # 字体链（HarmonyOS Sans SC → Noto Sans SC → …）由 fontconfig 在运行时解析，
    # 不需要在构建期绑定任何字体文件。
endif()

set_target_properties(penhu-native PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

if(MSVC)
    if(PENHU_STATIC_RUNTIME)
        set_property(TARGET penhu-native
                     PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    target_link_options(penhu-native PRIVATE /SUBSYSTEM:WINDOWS)
    # /utf-8：源码里有中文字面量（界面文案、日志），不加这个 MSVC 会按
    # 本地代码页解读，中文会变成乱码。这个坑在 core 里已经踩过一次。
    target_compile_options(penhu-native PRIVATE /utf-8)
endif()
