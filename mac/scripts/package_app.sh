#!/bin/zsh
set -euo pipefail

root_dir=${0:A:h:h}
dist_dir="$root_dir/dist"
app_path="$dist_dir/Vibe Keyboard.app"
archive_path="$dist_dir/VibeKeyboard-macOS-arm64.zip"
dmg_path="$dist_dir/VibeKeyboard-macOS-arm64.dmg"

if [[ -z ${DEVELOPER_DIR:-} && -d /Applications/Xcode-beta.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer
fi

cd "$root_dir"
swift build -c release --product VibeKeyboardApp -Xswiftc -strict-concurrency=complete
binary_path="$(swift build -c release --show-bin-path)/VibeKeyboardApp"

staging_dir=$(mktemp -d "${TMPDIR:-/tmp}/vibe-keyboard-package.XXXXXX")
trap 'rm -rf "$staging_dir"' EXIT

staged_app="$staging_dir/Vibe Keyboard.app"
mkdir -p "$staged_app/Contents/MacOS" "$staged_app/Contents/Resources"
cp "$root_dir/packaging/Info.plist" "$staged_app/Contents/Info.plist"
cp "$root_dir/packaging/VibeKeyboard.icns" "$staged_app/Contents/Resources/VibeKeyboard.icns"
cp "$binary_path" "$staged_app/Contents/MacOS/VibeKeyboardApp"
chmod 755 "$staged_app/Contents/MacOS/VibeKeyboardApp"

# A persistent signing identity gives Accessibility a stable code requirement.
# Developers without an Apple certificate can still build an ad-hoc local package.
signing_identity="${VIBE_KEYBOARD_SIGNING_IDENTITY:-}"
if [[ -z "$signing_identity" ]]; then
    signing_identity="$({ security find-identity -v -p codesigning 2>/dev/null || true; } | sed -nE 's/^[[:space:]]*[0-9]+\) ([A-F0-9]{40}) "Developer ID Application:.*/\1/p' | head -n 1)"
fi
if [[ -z "$signing_identity" ]]; then
    signing_identity="$({ security find-identity -v -p codesigning 2>/dev/null || true; } | sed -nE 's/^[[:space:]]*[0-9]+\) ([A-F0-9]{40}) "Apple Development:.*/\1/p' | head -n 1)"
fi
if [[ -z "$signing_identity" ]]; then
    signing_identity="-"
    echo "No Apple Development signing identity found; using ad-hoc signing."
else
    echo "Signing with $signing_identity"
fi
codesign --force --deep --sign "$signing_identity" "$staged_app"

mkdir -p "$dist_dir"
rm -rf "$app_path"
mv "$staged_app" "$app_path"
rm -f "$archive_path"
ditto -c -k --sequesterRsrc --keepParent "$app_path" "$archive_path"

dmg_stage="$staging_dir/dmg"
mkdir -p "$dmg_stage"
cp -R "$app_path" "$dmg_stage/Vibe Keyboard.app"
ln -s /Applications "$dmg_stage/Applications"
rm -f "$dmg_path"
hdiutil create -volname "Vibe Keyboard" -srcfolder "$dmg_stage" -ov -format UDZO "$dmg_path" >/dev/null

codesign --verify --deep --strict "$app_path"
plutil -lint "$app_path/Contents/Info.plist"
echo "$app_path"
echo "$archive_path"
echo "$dmg_path"
