#!/bin/sh
# Render og.html to docs/assets/og-image.jpg with headless Chrome (macOS).
cd "$(dirname "$0")" || exit 1
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --disable-gpu --hide-scrollbars \
  --allow-file-access-from-files --force-device-scale-factor=1 --window-size=1200,630 \
  --virtual-time-budget=8000 --screenshot="$PWD/og-image.png" "file://$PWD/og.html" 2>/dev/null
sips -s format jpeg -s formatOptions 86 og-image.png --out ../docs/assets/og-image.jpg >/dev/null && rm og-image.png
