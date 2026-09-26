// =============================================================================
//  _repro_com.cpp —— 复现「图片子系统初始化失败」
//
//  用来证伪/证实一个判断：这个报错不是字体、不是驱动、不是权限，
//  而是 **COM 套间（apartment）是「按线程」算的** ——
//  主线程在 main() 里 CoInitializeEx 过，工作线程没有，
//  于是在工作线程里 CoCreateInstance(CLSID_WICImagingFactory) 直接失败。
//
//  本程序三段对照，互相独立：
//    1) 主线程（模拟 main.cpp）：先 CoInitializeEx(STA) 再建 WIC 工厂
//    2) 工作线程，不初始化 COM（模拟 app.cpp 的识别线程）
//    3) 同一线程，先 CoInitializeEx(MTA) 再建 WIC 工厂
//
//  预期：1 成功、2 失败且返回码 = CO_E_NOTINITIALIZED(0x800401F0)、3 成功。
//  如果 2 也成功，那我的判断就是错的，得换方向查。
// =============================================================================

#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <thread>

namespace {

const char* hr_name(HRESULT hr) {
    switch (hr) {
        case S_OK:                  return "S_OK";
        case S_FALSE:               return "S_FALSE";
        case CO_E_NOTINITIALIZED:   return "CO_E_NOTINITIALIZED";
        case RPC_E_CHANGED_MODE:    return "RPC_E_CHANGED_MODE";
        case REGDB_E_CLASSNOTREG:   return "REGDB_E_CLASSNOTREG";
        case E_NOINTERFACE:         return "E_NOINTERFACE";
        default:                    return "(其它)";
    }
}

void report(const char* where, HRESULT hr, bool got_object) {
    std::printf("  %-34s HRESULT=0x%08lX %-20s 拿到对象=%s\n",
                where, static_cast<unsigned long>(hr), hr_name(hr),
                got_object ? "是" : "否");
}

HRESULT try_wic(IWICImagingFactory** out) {
    *out = nullptr;
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(out));
}

}  // namespace

int main() {
    std::printf("== 图片子系统（WIC）初始化对照实验 ==\n\n");

    // ---- 1) 主线程：模拟 native/src/main.cpp 的做法 ----
    const HRESULT hr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::printf("[1] 主线程\n");
    report("CoInitializeEx(STA)", hr_init, true);

    IWICImagingFactory* f1 = nullptr;
    const HRESULT hr1 = try_wic(&f1);
    report("CoCreateInstance(WIC)", hr1, f1 != nullptr);
    if (f1 != nullptr) f1->Release();

    // ---- 2) & 3) 工作线程：模拟 app.cpp:671 的 std::thread ----
    std::printf("\n[2][3] 工作线程（识别跑在这里）\n");
    std::thread worker([] {
        IWICImagingFactory* f2 = nullptr;
        const HRESULT hr2 = try_wic(&f2);
        report("不初始化 COM", hr2, f2 != nullptr);
        if (f2 != nullptr) f2->Release();

        const HRESULT hr3_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* f3 = nullptr;
        const HRESULT hr3 = try_wic(&f3);
        report("先 CoInitializeEx(MTA) 之后", hr3, f3 != nullptr);
        if (f3 != nullptr) f3->Release();
        std::printf("  （该线程 CoInitializeEx 返回 0x%08lX %s）\n",
                    static_cast<unsigned long>(hr3_init), hr_name(hr3_init));
        if (hr3_init == S_OK) CoUninitialize();
    });
    worker.join();

    std::printf("\n结论看 [2]：失败则确认「工作线程缺 COM 初始化」是根因。\n");
    return 0;
}
