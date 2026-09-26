# =============================================================================
#  server/cmake/generate_web_assets.cmake
#  把 web/ 下的静态资源生成成一份 C++ 源文件（编译进 exe）。
#
#  为什么用 CMake 脚本而不是 python / node：
#    构建链里多一个解释器就多一个「换台机器就编不过」的理由。CMake 一定在
#    （它本来就在跑构建），file(READ) + file(WRITE) 足够完成这件事。
#
#  用法：
#    cmake -DWEB_DIR=<web 目录> -DOUT=<输出 .cpp> -P generate_web_assets.cmake
#
#  安全点：用固定分隔符的 raw string literal（R"PENHUWEB(...)PENHUWEB"）。
#  如果某个资源文件里正好出现 )PENHUWEB" 就会截断字符串，所以这里显式检查，
#  一旦命中直接 FATAL_ERROR —— 让构建失败，而不是悄悄嵌进去一个坏掉的页面。
# =============================================================================

if(NOT DEFINED WEB_DIR)
    message(FATAL_ERROR "缺少 -DWEB_DIR=<web 资源目录>")
endif()
if(NOT DEFINED OUT)
    message(FATAL_ERROR "缺少 -DOUT=<输出文件>")
endif()

# 文件名::MIME 基础类型::访问路径
#
# 分隔符用 "::" 而不是 "|"，而且 MIME 里**不能带分号**：
# CMake 的 set() 会把值里的 ";" 当成列表分隔符，于是
# "index.html|text/html; charset=utf-8|/" 会被切成两个列表元素，
# list(GET ... 2) 直接越界。字符集在下面统一追加。
set(_assets
    # 不再单独嵌一份给 "/" —— web_assets.cpp 里的 find() 已经把
    # "/" 与 "/index.html" 视作等价，再嵌一遍等于白占 12KB（而且它是
    # 所有资源里第二大的字面量）。
    "index.html::text/html::/index.html"
    "styles.css::text/css::/styles.css"
    "app.js::application/javascript::/app.js"
)

set(_charset "; charset=utf-8")
set(_delim "PENHUWEB")
set(_body "// 本文件由 server/cmake/generate_web_assets.cmake 自动生成，请勿手改。\n")
string(APPEND _body "// 源资源目录: ${WEB_DIR}\n\n")
string(APPEND _body "#include \"penhu/server/web_assets.hpp\"\n\n")
string(APPEND _body "namespace penhu::server::web::gen {\n\n")
string(APPEND _body "const Asset kAssets[] = {\n")

set(_count 0)
foreach(_entry IN LISTS _assets)
    string(REPLACE "::" ";" _parts "${_entry}")
    list(GET _parts 0 _file)
    list(GET _parts 1 _mime)
    list(GET _parts 2 _route)

    set(_full "${WEB_DIR}/${_file}")
    if(NOT EXISTS "${_full}")
        message(FATAL_ERROR "前端资源缺失: ${_full}")
    endif()

    file(READ "${_full}" _content)

    string(FIND "${_content}" ")${_delim}\"" _hit)
    if(NOT _hit EQUAL -1)
        message(FATAL_ERROR
            "${_full} 中出现了 )${_delim}\" 序列，会截断 raw string literal。"
            "请更换 generate_web_assets.cmake 里的 _delim。")
    endif()

    string(LENGTH "${_content}" _len)

    # ---- 必须分段，否则 MSVC 会拒绝 ----
    # MSVC 的**单个字符串字面量**上限是 16380 字节，超了报
    #     error C2026: 字符串太大，已截断尾部字符
    # 实测：index.html（11796 字节）能过，styles.css（25118）、app.js（32108）
    # 直接编不过。而且错误落在生成文件里，第一眼看不出是「资源太大」。
    #
    # 办法是把内容切成多段**相邻的 raw string literal**：
    # C++ 里相邻字面量在编译期自动拼接成一个，所以语义完全不变，
    # 而每一段都远低于上限。段大小取 8000 —— 离上限有一倍余量，
    # 将来 MSVC 收紧也还在安全区，代价只是多几行字面量。
    set(_chunk 8000)
    set(_pieces "")
    set(_offset 0)
    while(_offset LESS _len)
        math(EXPR _remain "${_len} - ${_offset}")
        if(_remain GREATER _chunk)
            set(_take ${_chunk})
        else()
            set(_take ${_remain})
        endif()
        string(SUBSTRING "${_content}" ${_offset} ${_take} _piece)
        string(APPEND _pieces "           R\"${_delim}(${_piece})${_delim}\"\n")
        math(EXPR _offset "${_offset} + ${_take}")
    endwhile()

    # 空文件也要给一个字面量，否则 Asset 初始化列表会少一个成员
    if(_pieces STREQUAL "")
        set(_pieces "           R\"${_delim}()${_delim}\"\n")
    endif()

    string(APPEND _body
        "    Asset{\"${_route}\", \"${_mime}${_charset}\", ${_len},\n"
        "${_pieces}"
        "    },\n")
    math(EXPR _count "${_count} + 1")
endforeach()

string(APPEND _body "};\n\n")
string(APPEND _body "const std::size_t kAssetCount = ${_count};\n\n")
string(APPEND _body "}  // namespace penhu::server::web::gen\n")

file(WRITE "${OUT}" "${_body}")
message(STATUS "已嵌入 ${_count} 个前端资源 -> ${OUT}")
