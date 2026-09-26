// =============================================================================
//  _repro_wic.cpp —— 用「应用自己的函数」复现用户看到的那句报错
//
//  直接调 penhu::native::make_image_data_url()，并且**故意放在工作线程里**，
//  也就是 app.cpp:671 那个 std::thread 的处境（不初始化 COM）。
//  输入是一张真实 PNG。这条路径就是「扫描识别」点下去的第一件事。
//
//  修 image_util.cpp 之前：应当返回 false，err = 图片子系统初始化失败
//  修之后：             应当返回 true，data_url 是以 data:image/jpeg;base64, 开头的一大串
// =============================================================================

#include "app/image_util.hpp"

#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

namespace {

/// 控制台是 UTF-8（用 /utf-8 编的），宽字符直接 printf 会乱码，转一下。
std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                     nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: _repro_wic <图片路径>\n");
        return 2;
    }
    // 命令行给的是 UTF-8 窄串，这里按 ANSI(GBK) 之外的方式简单展开成宽串：
    // 直接用 MultiByteToWideChar(CP_UTF8) 更稳妥，但为少引一个头文件，
    // 这里就逐字节拷贝（测试用的路径是纯 ASCII）。
    std::string narrow(argv[1]);
    std::wstring path(narrow.begin(), narrow.end());

    bool ok = false;
    std::string url;
    std::wstring err;

    std::thread worker([&] {
        // 这里**故意不** CoInitializeEx —— 复刻识别线程的真实处境
        ok = penhu::native::make_image_data_url(path, 1600, url, &err);
    });
    worker.join();

    std::printf("make_image_data_url 返回: %s\n", ok ? "true（成功）" : "false（失败）");
    if (ok) {
        std::printf("data_url 长度: %zu 字节\n", url.size());
        std::printf("前缀: %.40s\n", url.c_str());
    } else {
        std::printf("err = %s\n", to_utf8(err).c_str());
    }
    return ok ? 0 : 1;
}
