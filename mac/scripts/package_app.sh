#!/bin/zsh
set -euo pipefail

root_dir=${0:A:h:h}
dist_dir="$root_dir/dist"
app_path="$dist_dir/Vibe Keyboard.app"
archive_path="$dist_dir/VibeKeyboard-macOS-arm64.zip"

if [[ -z ${DEVELOPER_DIR:-} && -d /Applications/Xcode-beta.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer
fi

cd "$root_dir"
swift build -c release --product VibeKeyboardApp -Xswiftc -strict-concurrency=complete
binary_path="$(swift build -c release --show-bin-path)/VibeKeyboardApp"

staging_dir=$(mktemp -d "${TMPDIR:-/tmp}/vibe-keyboard-package.XXXXXX")
trap 'rm -rf "$staging_dir"' EXIT

staged_app="$staging_dir/Vibe Keyboard.app"
mkdir -p "$staged_app/Contents/MacOS"
cp "$root_dir/packaging/Info.plist" "$staged_app/Contents/Info.plist"
cp "$binary_path" "$staged_app/Contents/MacOS/VibeKeyboardApp"
chmod 755 "$staged_app/Contents/MacOS/VibeKeyboardApp"
codesign --force --deep --sign - "$staged_app"

mkdir -p "$dist_dir"
rm -rf "$app_path"
mv "$staged_app" "$app_path"
rm -f "$archive_path"
ditto -c -k --sequesterRsrc --keepParent "$app_path" "$archive_path"

codesign --verify --deep --strict "$app_path"
plutil -lint "$app_path/Contents/Info.plist"
echo "$app_path"
echo "$archive_path"
