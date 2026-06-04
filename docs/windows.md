# Halo —— Windows 方案

> 状态：已实现，**待真机验证**。代码在 macOS 上撰写、无法本地编译，请在 Windows 上按下文编译并核对观感后，再把本说明的状态改为"可用"。

Windows 版用 C++ 调用系统原生框架实现：Win32 负责窗口，DirectComposition 负责把内容做成真透明的合成层，Direct2D（D3D11/DXGI 后端）负责在 GPU 上绘制发光动画。整份实现是单个 C++ 源文件加一个编译脚本，没有任何第三方依赖——Direct2D、DirectComposition、Direct3D 11 都是系统自带。

## 环境要求

- Windows 10 1607 或更高（用到 `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` 与组合交换链）
- Visual Studio（含 C++ 工具集）或 Build Tools for Visual Studio，确保 `cl.exe` 可用
- 无需任何第三方库

## 编译

在 **"x64 Native Tools Command Prompt for VS"**（或先运行过 `vcvars64.bat` 的命令行）里，进入 Windows 实现目录运行脚本：

```bat
cd src\windows
build.bat
```

脚本会编译 `halo.cpp`，把 `halo.exe` 输出到仓库的 `dist\windows\halo.exe`。它是个 GUI 子系统程序（`/SUBSYSTEM:WINDOWS`），所以被钩子调用或双击时都不会弹出控制台窗口。

## 测试运行

```bat
:: 彩色流光（默认风格）
dist\windows\halo.exe --duration 3 --style rainbow

:: 单色呼吸闪烁
dist\windows\halo.exe --duration 3 --style pulse --color "#FF3B30"
```

正确表现：屏幕边缘亮起一圈发光，持续指定秒数后淡出消失。发光期间，鼠标点击应当正常穿透到下面的窗口。

## 命令行参数

与所有平台一致，完整定义见 [cli-spec.md](cli-spec.md)：

| 参数 | 说明 | 默认值 |
|---|---|---|
| `--duration <秒>` | 发光持续时间，浮点数，最小 0.3 | `3` |
| `--style <名称>` | `rainbow` 彩色流光，或 `pulse` 单色呼吸 | `rainbow` |
| `--color <#RRGGBB>` | 仅 `pulse` 模式使用的颜色 | `#3B82F6` |

不认识的参数会被静默忽略，Codex 追加的 JSON 尾参也能正确处理。

## 实现原理

### 透明、穿透、置顶的全屏窗口

发光效果的地基是一个特殊窗口，关键的扩展样式：

- `WS_EX_NOREDIRECTIONBITMAP` —— 不分配重定向位图，交给 DirectComposition 直接合成（真透明的前提）
- `WS_EX_TRANSPARENT` —— 鼠标点击穿透到下层窗口
- `WS_EX_TOPMOST` —— 盖在普通窗口之上
- `WS_EX_TOOLWINDOW` + `WS_EX_NOACTIVATE` —— 不进任务栏、不进 Alt+Tab、不抢焦点

窗口本身用 `WS_POPUP`（无边框），覆盖主显示器矩形。

### GPU 合成链路

为什么不用传统的分层窗口（`UpdateLayeredWindow`）：那条路每帧都要 CPU 把位图 blit 上去，动画不够丝滑。这里改用现代链路：

- **Direct3D 11** 创建设备 → **Direct2D** 在其上建设备上下文
- **DXGI 组合交换链**（`CreateSwapChainForComposition`，预乘 alpha）作为绘制目标
- **DirectComposition** 把交换链作为可视内容挂到窗口，由桌面合成器（DWM）在 GPU 上合成

动画靠 `Present(1, 0)` 跟随 vsync 推进，和 macOS 的 Core Animation 同级别的流畅，且 CPU 占用极低。

### 彩色流光（rainbow）

启动时用 CPU 在一张 1024×1024 位图里按角度填入一圈彩虹色（锥形渐变），只填一次。之后每帧把这张位图绕屏幕中心旋转（放大到屏幕对角线尺寸，保证旋转到任意角度都盖满），再用"光带几何"裁出屏幕边缘那一圈。旋转一圈 3 秒，于是颜色沿边框流动。

### 单色呼吸（pulse）

直接用指定颜色填充光带几何，透明度在 0.35 与 1.0 之间往返（整周期 1.8 秒）形成呼吸，配合较大的高斯模糊做出外发光。

### 光带几何：外直角 + 内圆角

光带区域 = 整屏直角矩形 **减去** 一个向内缩的圆角矩形（Direct2D 的 `D2D1_COMBINE_MODE_EXCLUDE`）。外缘是直角，能贴满外接显示器的直角；笔记本圆角屏会把超出物理圆角的角像素自然裁掉，于是同一份几何在两种屏上都贴合。详见 macOS 文档里同名设计说明，两平台行为一致。

### 柔光与 DPI

柔和感来自 Direct2D 的高斯模糊效果（`CLSID_D2D1GaussianBlur`）。程序声明为 per-monitor DPI 感知，光带宽度与圆角半径按显示器 DPI 缩放，保证高分屏上观感一致。

### 自动退出

计时循环到达 `--duration` 时刻前的一小段触发整体淡出，到时退出进程。生命周期就是"启动 → 发光 → 淡出 → 退出"，不残留后台进程。

## 接入 AI 代理

把下面命令里的路径换成你本机 `halo.exe` 的绝对路径（注意 Windows 路径在 JSON 里要转义反斜杠）。

### Claude Code

编辑 `%USERPROFILE%\.claude\settings.json`，配置 `Stop` 钩子：

```json
{
  "hooks": {
    "Stop": [
      { "matcher": "*", "hooks": [
        { "type": "command", "command": "C:\\绝对路径\\halo\\dist\\windows\\halo.exe --duration 3 --style rainbow" }
      ]}
    ]
  }
}
```

### Codex CLI

编辑 `%USERPROFILE%\.codex\config.toml`，`notify` 是根键，必须放在所有 `[表]` 之前：

```toml
notify = ["C:\\绝对路径\\halo\\dist\\windows\\halo.exe", "--duration", "3", "--style", "rainbow"]

[tui]
notifications = true
```

## 已知限制

### 仅主显示器发光

当前实现只覆盖主显示器（`MONITOR_DEFAULTTOPRIMARY`），和 macOS 版一致。若要所有显示器同时亮起，需为每块屏各开一个窗口。

### 待真机验证

本实现尚未在 Windows 上实际编译运行过。若编译报错或观感异常（旋转方向、模糊强度、圆角大小等），请反馈以便调整。

## 排错

发光没出现：先在命令行单独跑一次 `halo.exe`，确认能看到效果。能看到说明程序正常，问题出在钩子配置——检查 `command` 是不是绝对路径、JSON 里反斜杠是否转义。

编译报错：确认在 VS 的原生工具命令行里运行（`cl.exe` 在 PATH 中），且安装了 Windows SDK（Direct2D / DirectComposition 头文件随 SDK 提供）。
