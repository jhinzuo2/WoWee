#!/bin/bash
set -euo pipefail

APP_PATH="${1:?usage: create_asset_extractor_app.sh <app-path> <version> <icon.icns>}"
VERSION="${2:?usage: create_asset_extractor_app.sh <app-path> <version> <icon.icns>}"
ICON="${3:?usage: create_asset_extractor_app.sh <app-path> <version> <icon.icns>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

rm -rf "${APP_PATH}"
osacompile -o "${APP_PATH}" "${SCRIPT_DIR}/asset_extractor.applescript"
cp "${SCRIPT_DIR}/asset_extractor_launcher.sh" \
    "${APP_PATH}/Contents/Resources/extract-assets-terminal.sh"
chmod +x "${APP_PATH}/Contents/Resources/extract-assets-terminal.sh"
cp "${ICON}" "${APP_PATH}/Contents/Resources/Wowee.icns"

PLIST="${APP_PATH}/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier com.wowee.asset-extractor" "${PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleName Wowee Asset Extractor" "${PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName Wowee Asset Extractor" "${PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleIconFile Wowee.icns" "${PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion ${VERSION}" "${PLIST}"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${VERSION}" "${PLIST}"
/usr/libexec/PlistBuddy -c "Add :LSUIElement bool true" "${PLIST}" 2>/dev/null || \
    /usr/libexec/PlistBuddy -c "Set :LSUIElement true" "${PLIST}"
