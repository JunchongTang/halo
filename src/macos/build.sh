#!/bin/bash
# build.sh — 编译 halo.swift 并打包成 Halo.app
# 用法: chmod +x build.sh && ./build.sh
# 产物输出到仓库的 dist/macos/Halo.app（预编译产物，已提交进仓库）

set -e

# 以脚本所在目录为基准定位，便于从任意路径调用
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST="$SCRIPT_DIR/../../dist/macos"
APP="$DIST/Halo.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"

echo "编译中..."
swiftc "$SCRIPT_DIR/halo.swift" -O \
  -o "$APP/Contents/MacOS/halo" \
  -framework Cocoa \
  -framework QuartzCore \
  -framework CoreImage

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Halo</string>
  <key>CFBundleIdentifier</key><string>com.example.halo</string>
  <key>CFBundleExecutable</key><string>halo</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>LSUIElement</key><true/>
  <key>LSMinimumSystemVersion</key><string>10.15</string>
</dict>
</plist>
PLIST

echo "完成 → $APP"
echo ""
echo "测试运行（彩色流光）:"
echo "  $APP/Contents/MacOS/halo --duration 3 --style rainbow"
echo "测试运行（单色呼吸）:"
echo "  $APP/Contents/MacOS/halo --duration 3 --style pulse --color \"#FF3B30\""
