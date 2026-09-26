# 「图片子系统初始化失败」——根因与修复

> 2026-09-23。用户报告：在这台电脑上使用「扫描识别」时出现「图片子系统初始化失败」。

## 结论先说

**不是系统缺组件、不是权限、不是图片有问题。** 是代码 bug：

`CoCreateInstance(CLSID_WICImagingFactory)` 跑在**没有初始化 COM 的工作线程**上，
返回 `CO_E_NOTINITIALIZED (0x800401F0)`，工厂为空，被上层翻译成那句报错。

**含义**：只要识别是在工作线程里跑的，「扫描识别」这个功能**从来没成功过** ——
不是时好时坏，是必然失败。它在 4226 条断言全绿、10 张截图全对的情况下活着，
因为自动化链路把识别结果**伪造**掉了（详见文末「为什么没被测出来」）。

## 根因

`image_util.cpp` 里 `make_wic()` 直接：

```cpp
CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(...));
```

而 **COM 的套间（apartment）是按线程算的**：

- `native/src/main.cpp:138` 只在**主线程**做过 `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`。
- 「扫描识别」跑在 `native/src/app/app.cpp:671` 的 `std::thread` 里 ——
  那条线程从未初始化 COM。
- 未初始化 COM 的线程上调用 `CoCreateInstance` → `CO_E_NOTINITIALIZED`。

## 实测证据

### 1) 最小对照实验（`_repro_com.cpp` / `_repro_com.sh`）

同一进程内，唯一变量是「线程内是否初始化 COM」：

| 场景 | HRESULT | 拿到 WIC 工厂 |
|---|---|---|
| 主线程，先 `CoInitializeEx(STA)` | `0x00000000` S_OK | 是 |
| **工作线程，不初始化 COM** | **`0x800401F0` CO_E_NOTINITIALIZED** | **否** |
| 工作线程，先 `CoInitializeEx(MTA)` | `0x00000000` S_OK | 是 |

### 2) 用应用自己的函数复现用户看到的那句话（`_repro_wic.cpp` / `_repro_wic.sh`）

在工作线程里调 `penhu::native::make_image_data_url()`，输入一张真实 PNG（111138 字节）：

**修复前**

```
make_image_data_url 返回: false（失败）
err = 图片子系统初始化失败
```

**修复后**

```
make_image_data_url 返回: true（成功）
data_url 长度: 131523 字节
前缀: data:image/jpeg;base64,/9j/4AAQSkZJRgABA
```

（前缀是 JPEG 的 `FFD8FF` 魔数，说明真的解码 + 缩放到 1600 + 编码成 JPEG 了。）

复现命令：

```bash
cd penhu-ledger
bash _repro_com.sh                                   # 极简对照
REPRO_INPUT='C:\...\某张.png' bash _repro_wic.sh     # 走应用真实函数
```

## 修法

在 `image_util.cpp` 里加一个 RAII 守卫，放在**所有碰 WIC 的公开入口**开头：

```cpp
struct ComApartment {
    bool owned{false};
    ComApartment() noexcept {
        owned = (CoInitializeEx(nullptr, COINIT_MULTITHREADED) == S_OK);
    }
    ~ComApartment() { if (owned) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};
```

几个必须讲清楚的细节：

- **为什么加在 image_util 而不是调用方**：「要碰 WIC（= COM）就得先有套间」是
  image_util 自己的约束。放在这一层，以后再有新的工作线程调它不会重踩；
  放在 `std::thread` 里则每多一个调用点就要记得写一次。
- **为什么用 MTA 不是 STA**：WIC 工厂是 Both 线程模型，MTA 下可用；
  而工作线程不需要消息泵，没必要背 STA 的包袱。
- **主线程上会不会出问题**：主线程已经是 STA，这里返回 `RPC_E_CHANGED_MODE`，
  `owned` 保持 false（不归我们收尾），同一个进程里两种套间对 WIC 都成立，继续用即可。
- **`owned` 的判定条件必须是 `== S_OK`**：
  `S_FALSE`（本线程已初始化）和 `RPC_E_CHANGED_MODE`（别的套间已初始化）
  都不该由我们 `CoUninitialize` —— 多减一次引用会让 COM 提前反初始化。
- **守卫必须活得比 COM 对象久**：所以它是函数体里的第一个局部变量
  （局部对象按声明逆序析构），而不是塞进 `make_wic()` 里 ——
  那样工厂一返回守卫就析构了。

## 为什么没被测出来（这是更值得记的一条）

Windows 侧有一套 20+ 项的无人值守诊断链（`--diag-login/--diag-pass/--diag-scan-dir`，
注入真实消息、断言状态变化）。但在「扫描目录」那一步，它是这么写结果的：

```cpp
app.scan_items[0].draft.ready = true;
app.scan_items[0].draft.amount_minor = 1234;
app.scan_items[0].draft.category_name = "餐饮";
```

——**直接伪造识别结果**，绕过了 `recognize_one → make_image_data_url → WIC` 这条链路。
于是这条链路上「只在工作线程才暴露」的问题，一条都测不出来。

**教训**：断言数量不等于覆盖。4226 条断言全绿 + 10 张截图全对，
只能说明「被断言覆盖到的那些路径」是对的；
凡是自动化里被 stub / 伪造掉的分支，等于没测。

## 同源风险（已检查，当前无问题）

- `platform_fs_win.cpp:57` 的 `CoCreateInstance(CLSID_FileOpenDialog)`：
  文件对话框从 UI 回调调用，跑在主线程（STA），套间正确 —— 且 `IFileDialog`
  本来就要求 STA，这里是对的。
- `renderer.cpp:70` 的 WIC 工厂：只在主线程创建（`init` / `attach_window`），
  未发现工作线程调用点。**注意**：那里不要照搬这个守卫 ——
  工厂是成员变量、活得比函数长，守卫放在函数里会提前反初始化。
  真要在那边加，得把守卫做成 Renderer 的成员。
