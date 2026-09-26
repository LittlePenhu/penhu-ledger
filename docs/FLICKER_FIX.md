# 窗口最大化/缩放闪烁修复报告（2026-09-23）

## 根因（三个，都有实证）

1. **缺 `SWP_NOCOPYBITS` 的几何 SetWindowPos** —— 「重绘错乱」的直接来源。
   不带这个标志时，系统先把旧帧像素**拷贝/拉伸**到新几何再发 WM_SIZE，
   合成器在「系统拷贝的拉伸帧」与「自绘的新帧」之间取样，就看到错位中间帧。
2. **resize 扩容余量太小** `min(256, w/8)` —— 「闪烁」的直接来源。
   最大化动画每帧窗口宽 +78px，余量 232px 只够撑 3 帧；
   超容量就 destroy + create WIC 位图 + D2D 软件 RT，每 1-2 帧一次重建 = 一闪。
3. **运动中途回收重建** —— 「抖动」（卡顿帧）的来源。
   半容量滞回回收在动画/拖拽进行中误触发，重分配出现在帧中途。

## 修复（13 处编辑，布局结构与交互行为零改动）

| # | 改动 | 位置 | 作用 |
|---|---|---|---|
| 1 | 动画前 `renderer_.reserve(起终点逐维 max)` | shell.cpp start_window_anim | 帧尺寸动画中永不超过容量 → 重建 0 次 |
| 2 | 扩容余量 `std::clamp(w/4, 384, 1024)` | renderer.cpp / renderer_cairo.cpp resize | 一次分配撑住整个动画跨度 |
| 3 | `resize(..., allow_reclaim)` 动画/拖拽中只增不减 | renderer.hpp/.cpp/.cairo + shell.cpp relayout | 运动中不回收 |
| 4 | 几何 SetWindowPos 一律 `SWP_NOCOPYBITS` | shell.cpp ×4（动画帧/DPI/诊断×2） | 消灭拉伸错位中间帧 |
| 5 | `Renderer::target_builds()` 计数器 + 动画日志「渲染目标重建 N 次」 | renderer.hpp/.cpp/.cairo + shell.cpp | 闪烁 → 可断言指标 |
| 6 | Linux renderer_cairo.cpp 对齐同款容量策略 | renderer_cairo.cpp | destroy_target 连 Pango 上下文一起销毁，configure 风暴会每帧重建 |

全项目 8 处 SetWindowPos 清点：4 处改变几何的全部带 NOCOPYBITS，4 处不动几何的（TOPMOST/NOTOPMOST/FRAMECHANGED）不带 —— 规则自洽。
Linux 跨档重建保持同步（Wayland 无动画信号，延迟重建会闪旧档布局帧）。

## 验证方式与结果（全绿）

| 验证 | 结果 |
|---|---|
| penhu-tests-portable | 50 用例 / 1852 断言 0 失败 |
| penhu-tests | 75 用例 / 4226 断言 0 失败 |
| CLI 自检 | 33 项 0 失败 |
| 完整诊断链 24 项（真实窗口+注入交互）×2 轮 | EXIT=0，全部交互断言通过 |
| **8 段窗口动画（最大化/还原/全屏/链①②③④）** | **渲染目标重建全部 0 次** |
| 窗口动画帧率 | 7-8 帧 23-26ms/帧 → 8-9 帧 20-25ms/帧（帧数↑帧耗↓） |
| 连续缩放 16 步 | 基线 187ms（11ms/步）→ **109ms（6ms/步）**，当场重绘 16/16 |
| 最大化矩形序列 | 单向平滑推进无回跳（t=16→203ms） |
| **反向验证**（撤 reserve+余量回退旧行为） | 最大化段报「重建 **1 次**」= 计数器断言有效；缩放退到 12ms/步 |
| 截图静态回归（修复前 vs 后 11 张） | **10/11 像素级完全一致**；差异 1 张 0.074% 恰在数据目录路径文本行（y641-655×x911-1105）——布局零变化 |
| Linux（WSLg）shot 11 张 | 渲染正常，中文/金额/布局无异常 |

对比数字可追溯：基线与修复后日志分别在（基线目录已清）与 `_flicker_data_win/logs/penhu-ledger.log`、
`_flicker_data_final/logs/penhu-ledger.log`；反向版数据在 `_flicker_data_rev/logs/penhu-ledger.log`。
逐像素对比脚本：`_compare_png.py`（纯 stdlib，unfilter 后逐通道比）。

## 局限（如实）

- 闪烁是**时序视觉现象**，自动化测的是代理指标（重建计数/帧耗/当场重绘/矩形序列单调）。
  「是否丝滑」的最终观感需要人眼确认 —— 请实际最大化/还原/拖拽缩放窗口体验一轮。
- 反向验证版保留了 `allow_reclaim` 门控（只撤了 reserve + 大余量），未复现修复前
  「每 1-2 帧重建」的完整量级（该数字是修复前的机理推理值，当时没有计数器）。
  断言有效性由「0 次 vs 1 次」的翻转证明。
- Linux 侧窗口动画/拖拽路径无法自动化（Wayland 无动画信号、诊断链是 Windows 专属），
  容量策略改动仅通过构建 + shot 核渲染验证，运动路径待人工体验。

## 诊断链已知边界（非本次引入，遗留）

- `--diag-scan-dir` 传入时 stage 15 走护栏分支 `PostQuitMessage(0)`，
  窗口链（stage 20-28）整段跳过 —— 跑窗口链必须不带该参数（护栏本意：不动用户目录文件）。
- 内置测试目录硬编码在 `shell_diag.cpp` 的目录扫描阶段 = 工作区根 `_scan_test`；每轮前需重铺 3 PNG+1txt
  （victim 进回收站）+ 全新数据目录（seen 指纹表须空）。
- first=1 场景 dedup 断言边界 bug（早前记录），未修。
