#!/bin/sh
# Render blog-header.html to og/blog-header.jpg (2400x1350) with headless Chrome (macOS).
cd "$(dirname "$0")" || exit 1
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --disable-gpu --hide-scrollbars \
  --allow-file-access-from-files --force-device-scale-factor=1.5 --window-size=1600,900 \
  --virtual-time-budget=8000 --screenshot="$PWD/blog-header.png" "file://$PWD/blog-header.html" 2>/dev/null
sips -s format jpeg -s formatOptions 88 blog-header.png --out blog-header.jpg >/dev/null && rm blog-header.png
