#include "ui/platform_input.hpp"

#include <windows.h>

#include <cstring>

namespace penhu::native::ui {
namespace {

/// 剪贴板的全局锁。别的进程正在用的时候 OpenClipboard 会直接失败，
/// 所以必须重试 —— 一次失败就放弃的话，用户遇到的是「偶尔粘不上」，
/// 既无法稳定复现，也没有任何报错可查。
class ClipboardLock {
public:
    ClipboardLock() {
        for (int i = 0; i < 8; ++i) {
            if (OpenClipboard(nullptr) != FALSE) {
                ok_ = true;
                return;
            }
            Sleep(8);
        }
    }
    ~ClipboardLock() {
        if (ok_) CloseClipboard();
    }
    bool ok() const { return ok_; }

    ClipboardLock(const ClipboardLock&) = delete;
    ClipboardLock& operator=(const ClipboardLock&) = delete;

private:
    bool ok_{false};
};

}  // namespace

std::wstring clipboard_read_text() {
    ClipboardLock lock;
    if (!lock.ok()) return {};

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) return {};

    const auto* src = static_cast<const wchar_t*>(GlobalLock(handle));
    if (src == nullptr) return {};

    // 剪贴板里是 NUL 结尾的串；构造 wstring 会在第一个 NUL 处停下，
    // 正好把「NUL 之后还有内容」这种脏数据挡掉。
    std::wstring text(src);
    GlobalUnlock(handle);
    return text;
}

bool clipboard_write_text(const std::wstring& text) {
    if (text.empty()) return false;

    ClipboardLock lock;
    if (!lock.ok()) return false;
    if (EmptyClipboard() == FALSE) return false;

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem == nullptr) return false;

    void* dst = GlobalLock(mem);
    if (dst == nullptr) {
        GlobalFree(mem);
        return false;
    }
    std::memcpy(dst, text.c_str(), bytes);
    GlobalUnlock(mem);

    // 成功后所有权归系统，不能再 GlobalFree（那会变成双重释放）。
    if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
        GlobalFree(mem);
        return false;
    }
    return true;
}

/// 键码就是 Win32 的 VK 码（见 ui/keys.hpp 的说明），所以这里直接透传 ——
/// 没有任何映射表，也就没有「映射表写错一个键」这类 bug。
bool key_down(unsigned key) { return (GetKeyState(static_cast<int>(key)) & 0x8000) != 0; }

}  // namespace penhu::native::ui
