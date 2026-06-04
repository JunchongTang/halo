// halo.swift — macOS 全屏边框发光工具
//
// 编译: swiftc halo.swift -O -o halo -framework Cocoa -framework QuartzCore -framework CoreImage
// 运行: ./halo --duration 3 --style rainbow
//       ./halo --duration 3 --style pulse --color "#FF3B30"
//
// 行为: 开一个无边框、透明、鼠标穿透、最顶层的全屏窗口，
//       沿屏幕边缘发光，duration 秒后淡出并自动退出。

import Cocoa
import QuartzCore
import CoreImage

// MARK: - 命令行配置

struct Config {
    var duration: Double = 3.0          // 持续秒数
    var style: String = "rainbow"       // "rainbow"(彩色流光) | "pulse"(单色呼吸)
    var colorHex: String = "#3B82F6"    // pulse 模式的颜色
    var lineWidth: CGFloat = 16          // 边框光带宽度
    var corner: CGFloat = 36             // 圆角，贴合现代屏幕的圆角
}

// 解析参数。关键点：宽容对待不认识的参数。
// Codex 会额外塞一个 JSON 负载作为尾参，例如 {"type":"agent-turn-complete",...}
// 这里若检测到 JSON 且 type 不是 agent-turn-complete，就直接不发光退出（更精确）。
func parseArgs() -> Config? {
    var cfg = Config()
    let args = Array(CommandLine.arguments.dropFirst())
    var i = 0
    while i < args.count {
        let a = args[i]
        switch a {
        case "--duration":
            i += 1
            if i < args.count, let d = Double(args[i]) { cfg.duration = max(0.3, d) }
        case "--style":
            i += 1
            if i < args.count { cfg.style = args[i].lowercased() }
        case "--color":
            i += 1
            if i < args.count { cfg.colorHex = args[i] }
        default:
            // Codex 的 JSON 尾参：解析它，只在任务完成事件时发光
            if a.hasPrefix("{"),
               let data = a.data(using: .utf8),
               let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let type = obj["type"] as? String,
               type != "agent-turn-complete" {
                return nil   // 不是完成事件 → 不发光
            }
            // 其它不认识的参数一律忽略
        }
        i += 1
    }
    return cfg
}

// MARK: - 工具函数

func color(fromHex hex: String) -> NSColor {
    var s = hex.trimmingCharacters(in: .whitespaces)
    if s.hasPrefix("#") { s.removeFirst() }
    var v: UInt64 = 0
    Scanner(string: s).scanHexInt64(&v)
    guard s.count == 6 else {
        return NSColor(red: 0.23, green: 0.51, blue: 0.96, alpha: 1) // 兜底蓝色
    }
    let r = CGFloat((v & 0xFF0000) >> 16) / 255
    let g = CGFloat((v & 0x00FF00) >> 8) / 255
    let b = CGFloat(v & 0x0000FF) / 255
    return NSColor(red: r, green: g, blue: b, alpha: 1)
}

// 沿屏幕边缘的光带区域：外边界是整屏直角矩形，内边界是向内缩的圆角矩形，
// 用 even-odd 填充规则填中间那一圈。
//
// 为什么外直角、内圆角：外接显示器是直角，直角外缘能严丝合缝贴满屏幕角，无缺口；
// MacBook 屏是圆角，直角外缘的角像素正好落在物理圆角被裁掉的位置，会被硬件自动裁成
// 圆角，光带自然贴着物理圆角走。一份路径同时适配两种屏。
func bandPath(size: CGSize, lineWidth: CGFloat, corner: CGFloat) -> CGPath {
    let path = CGMutablePath()
    // 外边界：整屏直角矩形
    path.addRect(CGRect(origin: .zero, size: size))
    // 内边界：四边各向内缩 lineWidth 的圆角矩形（挖空）
    let inner = CGRect(x: lineWidth, y: lineWidth,
                       width: size.width - 2 * lineWidth,
                       height: size.height - 2 * lineWidth)
    path.addRoundedRect(in: inner, cornerWidth: corner, cornerHeight: corner)
    return path
}

func gaussianBlur(_ radius: Double) -> CIFilter? {
    guard let f = CIFilter(name: "CIGaussianBlur") else { return nil }
    f.setValue(radius, forKey: "inputRadius")
    return f
}

// MARK: - App 代理

class HaloDelegate: NSObject, NSApplicationDelegate {
    let cfg: Config
    var window: NSWindow!

    init(cfg: Config) { self.cfg = cfg }

    func applicationDidFinishLaunching(_ note: Notification) {
        // 在鼠标光标所在的屏幕发光：多屏时跟随你的注意力位置。
        // 找不到（极少见）就回退到主屏。
        let mouse = NSEvent.mouseLocation
        let chosen = NSScreen.screens.first(where: { $0.frame.contains(mouse) }) ?? NSScreen.main
        guard let screen = chosen else { NSApp.terminate(nil); return }
        let frame = screen.frame

        // —— 透明、无边框、穿透、置顶的全屏窗口 ——
        window = NSWindow(contentRect: frame,
                          styleMask: .borderless,
                          backing: .buffered,
                          defer: false)
        window.isOpaque = false
        window.backgroundColor = .clear
        window.ignoresMouseEvents = true              // 鼠标穿透
        window.level = .screenSaver                   // 盖在最上层
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary,
                                     .stationary, .ignoresCycle]
        window.hasShadow = false

        let view = NSView(frame: NSRect(origin: .zero, size: frame.size))
        view.wantsLayer = true
        view.layer?.backgroundColor = NSColor.clear.cgColor
        window.contentView = view

        switch cfg.style {
        case "pulse":   setupPulse(in: view, size: frame.size)
        default:        setupRainbow(in: view, size: frame.size)
        }

        window.orderFrontRegardless()                 // 显示但不抢焦点

        // 结束前淡出，然后退出进程
        let fade = min(0.5, cfg.duration * 0.3)
        DispatchQueue.main.asyncAfter(deadline: .now() + (cfg.duration - fade)) {
            let a = CABasicAnimation(keyPath: "opacity")
            a.fromValue = 1.0
            a.toValue = 0.0
            a.duration = fade
            a.fillMode = .forwards
            a.isRemovedOnCompletion = false
            view.layer?.add(a, forKey: "fadeout")
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + cfg.duration) {
            NSApp.terminate(nil)
        }
    }

    // —— 风格一：彩色流光（苹果 AI 那种） ——
    func setupRainbow(in view: NSView, size: CGSize) {
        let lw = cfg.lineWidth

        // 容器：负责承载固定不动的环形遮罩
        let container = CALayer()
        container.frame = CGRect(origin: .zero, size: size)

        // 锥形彩虹渐变，做成正方形且放大到对角线长度，
        // 这样无论旋转到什么角度都能盖满整个屏幕，边角不会露空
        let diag = (size.width * size.width + size.height * size.height).squareRoot()
        let grad = CAGradientLayer()
        grad.frame = CGRect(x: (size.width - diag) / 2,
                            y: (size.height - diag) / 2,
                            width: diag, height: diag)
        grad.type = .conic
        grad.startPoint = CGPoint(x: 0.5, y: 0.5)
        grad.endPoint = CGPoint(x: 1.0, y: 0.5)
        let hues = stride(from: 0.0, through: 1.0, by: 1.0 / 8.0).map { CGFloat($0) }
        grad.colors = hues.map {
            NSColor(hue: $0, saturation: 0.9, brightness: 1.0, alpha: 1.0).cgColor
        }
        container.addSublayer(grad)

        // 光带遮罩（只让屏幕边缘那圈显示），加一点模糊让光带柔和
        let mask = CAShapeLayer()
        mask.frame = container.bounds
        mask.path = bandPath(size: size, lineWidth: lw, corner: cfg.corner)
        mask.fillColor = NSColor.white.cgColor
        mask.fillRule = .evenOdd
        if let blur = gaussianBlur(10) { mask.filters = [blur] }  // 柔光；若环境不支持只会变成硬边
        container.mask = mask

        view.layer?.addSublayer(container)

        // 让渐变绕中心旋转 → 颜色沿边框流动
        let spin = CABasicAnimation(keyPath: "transform.rotation.z")
        spin.fromValue = 0
        spin.toValue = CGFloat.pi * 2
        spin.duration = 3.0
        spin.repeatCount = .infinity
        grad.add(spin, forKey: "spin")
    }

    // —— 风格二：单色呼吸闪烁 ——
    func setupPulse(in view: NSView, size: CGSize) {
        let lw = cfg.lineWidth
        let c = color(fromHex: cfg.colorHex)

        let ring = CAShapeLayer()
        ring.frame = CGRect(origin: .zero, size: size)
        ring.path = bandPath(size: size, lineWidth: lw, corner: cfg.corner)
        ring.fillColor = c.cgColor
        ring.fillRule = .evenOdd
        // 用阴影做发光
        ring.shadowColor = c.cgColor
        ring.shadowRadius = 22
        ring.shadowOpacity = 1.0
        ring.shadowOffset = .zero
        if let blur = gaussianBlur(6) { ring.filters = [blur] }
        view.layer?.addSublayer(ring)

        // 呼吸：透明度在 0.35 ↔ 1.0 间往返
        let pulse = CABasicAnimation(keyPath: "opacity")
        pulse.fromValue = 0.35
        pulse.toValue = 1.0
        pulse.duration = 0.9
        pulse.autoreverses = true
        pulse.repeatCount = .infinity
        pulse.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
        ring.add(pulse, forKey: "pulse")
    }
}

// MARK: - 入口

guard let cfg = parseArgs() else { exit(0) }   // 非完成事件 → 静默退出

let app = NSApplication.shared
app.setActivationPolicy(.accessory)             // 安静的后台型程序，不进 Dock、不抢焦点
let delegate = HaloDelegate(cfg: cfg)           // 顶层 let 持有，保证生命周期
app.delegate = delegate
app.run()
