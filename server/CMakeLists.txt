# =============================================================================
#  server/CMakeLists.txt
#  进程内 HTTP 服务。只依赖 core，不依赖任何 UI。
# =============================================================================

# -----------------------------------------------------------------------------
#  前端资源内嵌
#
#  web/ 下的三个文件在构建时被生成成一份 C++ 源文件，编进静态库。
#  这样做是为了「一个 exe」：exe 旁边不带任何散落的 html/css/js。
#
#  用 add_custom_command 而不是 configure_file：
#  configure_file 只在重新 configure 时更新，改了 CSS 但没重新 configure
#  就会嵌进旧内容——这种「改了没生效」最消耗排查时间。
#  add_custom_command 挂在 web/ 文件上，改一个字符就重新生成。
# -----------------------------------------------------------------------------
# -----------------------------------------------------------------------------
#  这里原来有一段 add_custom_command：把 web/ 目录下的 HTML/CSS/JS 内嵌进 exe，
#  交给 WebView2 壳子加载。
#
#  那个方案已经废弃 —— 界面改成原生自绘（native/，Win32 + Direct2D + DirectWrite），
#  没有前端资源需要内嵌，整段删除。HTTP 服务本身保留，只提供 /api/ 下的 JSON 接口，
#  作为 CLI 自测与端到端验证的载体。
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
#  penhu-server 静态库
# -----------------------------------------------------------------------------
set(PENHU_SERVER_SOURCES
    src/api_support.cpp
    src/routes_auth.cpp
    src/routes_ledger.cpp
    src/routes_insight.cpp
    src/routes_local_model.cpp
    src/http_server.cpp
)
add_library(penhu-server STATIC ${PENHU_SERVER_SOURCES})
add_library(penhu::server ALIAS penhu-server)

target_include_directories(penhu-server
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(penhu-server
    PUBLIC
        penhu::core
    PRIVATE
        httplib::httplib
)

set_target_properties(penhu-server PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

if(MSVC AND PENHU_STATIC_RUNTIME)
    set_property(TARGET penhu-server
                 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# source_group(TREE ...) 要求列表里**每个**文件都在给定前缀目录下，
# 否则 configure 直接失败：
#     source_group ROOT: <源目录> is not a prefix of file: <build>/generated/...
# 内嵌资源是构建期生成的，落在 build/ 里，不满足这个前提。
# 这个报错完全没提「生成文件」，很容易误以为是生成脚本或资源路径的问题。
# 所以这里把两类文件分开处理：源码树里的按目录树分组，生成文件单独一组。
set(_penhu_srv_in_tree "")
set(_penhu_srv_generated "")
foreach(_f IN LISTS PENHU_SERVER_SOURCES)
    string(FIND "${_f}" "${CMAKE_CURRENT_SOURCE_DIR}/" _pos)
    if(_pos EQUAL 0)
        list(APPEND _penhu_srv_in_tree "${_f}")
    else()
        list(APPEND _penhu_srv_generated "${_f}")
    endif()
endforeach()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${_penhu_srv_in_tree})
if(_penhu_srv_generated)
    source_group("generated" FILES ${_penhu_srv_generated})
endif()
