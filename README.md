# SnapHelix

`SnapHelix` 是一个基于 `Qt 6`、`Windows API` 与 `UI Automation` 的 Windows 截图工具原型，目标是复刻类似 Snipaste 的多屏截图、窗口悬停识别、标注编辑与置顶贴图体验。

当前仓库已经具备可运行的截图工作流，包括全屏遮罩、窗口/控件探测、局部截图、标注工具栏、剪贴板复制、保存文件以及贴图预览窗口。

## 功能特性

- 支持多屏环境下创建截图遮罩窗口
- 通过 `F2` 呼出截图模式
- 自动识别鼠标悬停的窗口区域
- 支持拖拽框选并调整截图范围
- 提供放大镜与像素取色辅助
- 支持绘制、箭头、矩形、椭圆、文字、模糊、橡皮擦等标注能力
- 支持撤销与重做
- 支持复制截图到剪贴板
- 支持将截图保存为图片文件
- 支持将截图置顶为可继续编辑的贴图窗口
- 支持通过 `Alt+S` 将剪贴板中的图片快速贴图

## 快捷键

- `F2`：开始截图 / 呼出截图层
- `Alt+S`：将剪贴板图片贴到桌面上方
- `Ctrl+C`：在截图层中复制当前像素颜色的十六进制值
- `Esc`：退出当前截图或关闭截图层
- 鼠标滚轮：在画笔、模糊、橡皮擦工具下调整笔刷大小

## 构建要求

- Windows
- MSVC 工具链
- CMake 3.21+
- Qt 6.8 或更高版本
- Qt 组件：
  - `Core`
  - `Gui`
  - `Widgets`
  - `Concurrent`

项目当前在 `CMakeLists.txt` 中显式要求使用 MSVC Qt Kit；若不是 MSVC 环境，配置阶段会直接报错。

## 构建方式

### 使用 Qt Creator

1. 使用 Qt Creator 打开项目根目录下的 `CMakeLists.txt`
2. 选择一个 `Qt 6.x (msvc2022_64)` 或等价的 MSVC Kit
3. 配置并构建项目
4. 运行生成的 `SnipasteLikeCapturer` 可执行文件

### 使用命令行

```powershell
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="C:\Qt\6.8.3\msvc2022_64"
cmake --build build --config Debug
```

如果你使用 Visual Studio 生成器，也可以改用对应的 generator 与 Qt 安装路径。

## 项目结构

```text
include/
  coordinateconverter.h
  screencapturer.h
  uiadetector.h
src/
  main.cpp
  coordinateconverter.cpp
  screencapturer_multi.cpp
  uiadetector.cpp
  uiadetector_impl.cpp
```

## 实现概览

- `ScreenCapturer`：负责截图遮罩、交互状态、工具栏、标注绘制、保存/复制/贴图等核心逻辑
- `UIADetector`：负责基于 UI Automation 识别鼠标位置下的控件或窗口区域
- `CoordinateConverter`：负责不同 DPI / 坐标系之间的换算

## 当前状态

这是一个偏原型性质但已经具备较完整交互链路的桌面工具项目，适合作为以下方向的继续迭代基础：

- 更完善的快捷键与配置系统
- 更稳定的 UIA 探测策略
- 更丰富的标注样式与文本编辑体验
- 截图历史、自动保存和导出能力
- 国际化与资源整理

## 许可证

本项目使用 [MIT License](LICENSE)。
