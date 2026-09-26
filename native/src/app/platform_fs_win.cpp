// =============================================================================
//  native/app/platform_fs_win.cpp
//  platform_fs 的 Windows 实现（原来散在 app.cpp 里的那几段）。
// =============================================================================

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>
#include <vector>

#include "app/platform_fs.hpp"

namespace penhu::native {

bool pick_image_files(std::vector<std::wstring>& out) {
    out.clear();

    // 多选需要一块够大的缓冲区：返回的是「目录\0文件1\0文件2\0\0」这种
    // 双 NUL 结尾的列表，不是单条路径。用 MAX_PATH 装多选必然截断。
    std::vector<wchar_t> buf(64 * 1024, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"图片文件\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff\0所有文件\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    // OFN_ALLOWMULTISELECT：一次选多张支付截图，这是「批量记账」的前提
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    ofn.lpstrTitle = L"选择支付截图（可按 Ctrl / Shift 多选）";

    if (!GetOpenFileNameW(&ofn)) return false;   // 用户取消

    // 解析返回值。单选时是一整条路径；多选时第一段是目录，后面每段一个文件名。
    const wchar_t* p = buf.data();
    const std::wstring first = p;
    p += first.size() + 1;
    if (*p == L'\0') {
        out.push_back(first);
    } else {
        while (*p != L'\0') {
            const std::wstring name = p;
            p += name.size() + 1;
            out.push_back(first + L"\\" + name);
        }
    }
    return true;
}

bool pick_directory(std::wstring& out) {
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        return false;
    }

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    // FOS_PICKFOLDERS 才是「选目录」；不带它就成了选文件
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"选择截图保存目录");

    bool ok = false;
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

bool file_dialog_available() {
    // Windows 的 GetOpenFileNameW / IFileDialog 是系统自带的，永远可用。
    return true;
}

bool file_stamp(const std::wstring& path, long long* size, long long* mtime) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) == FALSE) {
        return false;
    }
    *size = (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    const long long ticks =
        (static_cast<long long>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
        fad.ftLastWriteTime.dwLowDateTime;
    // FILETIME 是「1601-01-01 起的 100 纳秒数」
    *mtime = (ticks - 116444736000000000LL) / 10000000LL;
    return true;
}

bool move_to_trash(const std::wstring& path) {
    // pFrom 要的是**双 NUL 结尾**的路径列表
    std::vector<wchar_t> buf(path.begin(), path.end());
    buf.push_back(L'\0');
    buf.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = buf.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0;
}

uint64_t monotonic_ms() {
    // GetTickCount64 本身就是单调的（开机起的毫秒数），不受系统时间调整影响。
    return static_cast<uint64_t>(GetTickCount64());
}

std::wstring default_screenshot_dir() {
    // 用 KnownFolder 而不是拼 "C:\Users\<用户名>"：换台机器、改过图片位置都能用。
    PWSTR pics = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) {
        std::wstring dir = std::wstring(pics) + L"\\账本";
        CoTaskMemFree(pics);
        return dir;
    }
    return {};
}

}  // namespace penhu::native
