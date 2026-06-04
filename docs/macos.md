# Halo —— macOS 方案

macOS 版用 Swift 调用系统原生框架实现：Cocoa（AppKit）负责窗口，Core Animation 负责发光动画。整份实现是单个 Swift 源文件加一个打包脚本，没有任何第三方依赖，编译产物不到 100 K。

## 环境要求

- macOS 10.15 (Catalina) 或更高
- 安装了 Swift 编译器 `swiftc`。装了 Xcode 或 Xcode Command Line Tools 即自带。若没有，运行 `xcode-select --install`
- 无需任何第三方库

## 编译

仓库已在 `dist/macos/Halo.app` 提供预编译产物（约 90 K），可直接使用，无需编译。若想自行编译，进入 macOS 实现目录运行打包脚本：

```bash
cd src/macos
chmod +x build.sh
./build.sh
```

脚本会编译 `halo.swift` 并打包成 `Halo.app`（一个标准的 macOS 应用包），输出到仓库的 `dist/macos/Halo.app`。

如果你只想要一个裸二进制、不需要应用包，可以直接编译：

```bash
swiftc halo.swift -O -o halo -framework Cocoa -framework QuartzCore -framework CoreImage
```

## 测试运行

```bash
# 彩色流光（默认风格）
./dist/macos/Halo.app/Contents/MacOS/halo --duration 3 --style rainbow

# 单色呼吸闪烁
./dist/macos/Halo.app/Contents/MacOS/halo --duration 3 --style pulse --color "#FF3B30"
```

正确表现：屏幕边缘亮起一圈发光，持续指定秒数后淡出消失。发光期间，鼠标点击应当正常穿透到下面的窗口——也就是说这个发光层完全不挡你操作。

## 命令行参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| `--duration <秒>` | 发光持续时间，浮点数，最小 0.3 | `3` |
| `--style <名称>` | `rainbow` 彩色流光，或 `pulse` 单色呼吸 | `rainbow` |
| `--color <#RRGGBB>` | 仅 `pulse` 模式使用的颜色 | `#3B82F6` |

不认识的参数会被静默忽略。完整的跨平台接口约定见 [cli-spec.md](cli-spec.md)。

## 实现原理

### 透明、穿透、置顶的全屏窗口

发光效果的地基是一个特殊窗口。它无边框、背景透明、不响应鼠标、且盖在所有窗口之上。关键设置：

- `styleMask: .borderless` —— 无标题栏和边框
- `isOpaque = false` 配合 `backgroundColor = .clear` —— 背景透明
- `ignoresMouseEvents = true` —— 鼠标点击穿透到下层窗口
- `level = .screenSaver` —— 窗口层级抬到屏保级，盖在普通窗口和菜单栏之上

程序的激活策略设为 `.accessory`，使它成为一个安静的后台型程序：不进 Dock、不抢焦点、不打断你正在用的应用。

### 彩色流光（rainbow）

做法是一个锥形彩虹渐变层加一个环形遮罩：

- 一个 `CAGradientLayer`，类型为 `.conic`（锥形），填入一圈彩虹色
- 渐变层被放大到屏幕对角线的尺寸，并绕中心持续旋转。放大是为了旋转到任意角度时四个屏幕角都不会露空
- 一个固定不动的环形遮罩（`CAShapeLayer` 描边路径），只让屏幕边缘那一圈显示出来

渐变在转、遮罩不动，于是颜色就沿着屏幕边框"流动"起来，形成苹果设备上那种环绕发光的观感。

### 单色呼吸（pulse）

做法更简单：一个沿屏幕边缘描边的 `CAShapeLayer`，用阴影（`shadowRadius`）做出外发光，再用一个透明度在 0.35 与 1.0 之间往返的动画做出"呼吸"节奏。颜色由 `--color` 指定。

### 柔光

两种风格的光带柔和感都来自高斯模糊滤镜（`CIGaussianBlur`）。这个滤镜在 macOS 的图层上受支持。万一在某些环境下表现不如预期，最坏情况只是光带边缘变硬，程序本身不会出错。

### 自动退出

程序在结束前的一小段时间触发淡出动画，到达 `--duration` 指定的时刻后调用 `NSApp.terminate` 退出进程。整个生命周期就是"启动 → 发光 → 淡出 → 退出"，不残留任何后台进程。

## 接入 AI 代理

把下面命令里的路径换成你本机编译产物的绝对路径。

### Claude Code

编辑 `~/.claude/settings.json`，配置 `Stop` 钩子——它在 Claude Code 完成一轮响应时触发（被用户主动打断时不触发）：

```json
{
  "hooks": {
    "Stop": [
      { "matcher": "*", "hooks": [
        { "type": "command", "command": "/绝对路径/halo/dist/macos/Halo.app/Contents/MacOS/halo --duration 3 --style rainbow" }
      ]}
    ]
  }
}
```

### Codex CLI

编辑 `~/.codex/config.toml`。注意 `notify` 是根键，按 TOML 语法**必须放在所有 `[表]` 之前**：

```toml
notify = ["/绝对路径/halo/dist/macos/Halo.app/Contents/MacOS/halo", "--duration", "3", "--style", "rainbow"]

[tui]
notifications = true
```

Codex 在调用时会额外附带一个 JSON 负载作为最后一个参数，形如 `{"type":"agent-turn-complete",...}`。Halo 会解析它，只在事件类型为 `agent-turn-complete` 时发光，其它事件静默退出。这一行为无需你额外配置。

## 已知限制

### 全屏 Space 下覆盖不完整

`.screenSaver` 窗口层级能盖住普通窗口和菜单栏，但如果你的终端正处于 macOS 的"全屏 Space"（点绿色按钮进入的那种独占全屏），发光可能无法完整覆盖。这是系统对全屏 Space 的限制，不是程序缺陷。日常终端不全屏使用时不受影响。

### 仅主显示器发光

当前实现只在主显示器（`NSScreen.main`）发光。若需要所有显示器同时亮起，需改为遍历 `NSScreen.screens`，为每块屏各开一个窗口。

### 柔光在个别环境可能变硬边

见上文"柔光"一节。属于观感差异，不影响功能。

## 排错

发光没出现：先单独在终端跑一次命令，确认能看到效果。能看到说明程序正常，问题出在钩子配置——检查 `command` 里是不是写的绝对路径，以及路径是否指向真实存在的二进制。

钩子没触发：确认配置文件路径正确（Claude Code 是 `~/.claude/settings.json`，Codex 是 `~/.codex/config.toml`），且 JSON / TOML 语法没写错。Codex 还要特别检查 `notify` 是否放在了所有 `[表]` 之前。

权限相关：Halo 只是在屏幕上绘制，不捕获屏幕内容，因此不需要"屏幕录制"等隐私权限。如果系统提示未签名应用相关的安全拦截，可在"系统设置 → 隐私与安全性"中放行，或对二进制自签名。
