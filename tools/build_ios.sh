#!/usr/bin/env bash
# Build Ultimate UJL for iOS.
#
#   bash tools/build_ios.sh [disc images...]   simulator: build, install on the booted simulator, and copy disc
#                                              images into the app's Documents folder (an APFS clone where
#                                              possible, so a 700 MB image takes no extra space)
#   bash tools/build_ios.sh --ipa              device: an unsigned build/ios-device/UltimateUJL.ipa for
#                                              sideloading tools that sign it with your own Apple ID
#                                              (AltStore, Sideloadly, ...). Never share it: it is built from your
#                                              disc's program (the IPA is gitignored).
#
# Needs build/gen_jp (tools/prepare.sh), Xcode (with an iOS Simulator runtime for the simulator), cmake and ninja.
# For a signed device build see docs/ARCHITECTURE.md, "iOS".
set -euo pipefail
cd "$(dirname "$0")/.."
BUNDLE_ID=$(grep -o 'UJL_IOS_BUNDLE_ID "[^"]*"' CMakeLists.txt | cut -d'"' -f2)

configure() {   # build dir, sdk, extra args
  local dir=$1 sdk=$2
  shift 2
  if [ ! -f "$dir/build.ninja" ]; then
    cmake -S . -B "$dir" -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdk" \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "$@"
  fi
}

if [ "${1:-}" = "--device" ]; then
  # A development build on a connected iPhone/iPad, for testing and profiling (Instruments can attach: the
  # profile grants get-task-allow). It installs as a separate app, bundle id <id>.dev named "UJL Dev", so a
  # sideloaded copy and its saves stay untouched. Signed with the first "Apple Development" identity and the
  # team's wildcard iOS profile from Xcode (override: UJL_IOS_IDENTITY, UJL_IOS_PROFILE). Optional: the device
  # UDID (default: the first connected iPhone/iPad) and disc images to copy into the app's Documents.
  shift
  DEVICE=""
  if [ -n "${1:-}" ] && [ ! -f "$1" ]; then DEVICE=$1; shift; fi
  if [ -z "$DEVICE" ]; then
    DEVICE=$(xcrun devicectl list devices 2>/dev/null | awk '/available \(paired\)/ && /(iPhone|iPad)/ && /physical/ {for (i=1;i<=NF;i++) if ($i ~ /^[0-9A-F]{8}-[0-9A-F]{16}$/ || $i ~ /^[0-9a-f]{40}$/) {print $i; exit}}')
  fi
  [ -n "$DEVICE" ] || { echo "no connected iPhone/iPad (xcrun devicectl list devices)"; exit 1; }
  IDENTITY=${UJL_IOS_IDENTITY:-$(security find-identity -v -p codesigning | sed -n 's/.*"\(Apple Development: [^"]*\)".*/\1/p' | head -1)}
  [ -n "$IDENTITY" ] || { echo "no Apple Development signing identity (sign in to Xcode with your Apple ID)"; exit 1; }
  TMP=$(mktemp -d)
  trap 'rm -rf "$TMP"' EXIT
  PROFILE=${UJL_IOS_PROFILE:-}
  if [ -z "$PROFILE" ]; then
    for p in "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"/*.mobileprovision \
             "$HOME/Library/MobileDevice/Provisioning Profiles"/*.mobileprovision; do
      [ -f "$p" ] || continue
      security cms -D -i "$p" > "$TMP/p.plist" 2>/dev/null || continue
      /usr/libexec/PlistBuddy -c "Print :Platform" "$TMP/p.plist" 2>/dev/null | grep -qx '    iOS' || continue
      /usr/libexec/PlistBuddy -c "Print :Entitlements:application-identifier" "$TMP/p.plist" | grep -q '\.\*$' || continue
      PROFILE=$p
      break
    done
  fi
  [ -n "$PROFILE" ] || { echo "no wildcard iOS development profile (build any app for a device in Xcode once)"; exit 1; }
  security cms -D -i "$PROFILE" > "$TMP/p.plist"
  TEAM=$(/usr/libexec/PlistBuddy -c "Print :Entitlements:com.apple.developer.team-identifier" "$TMP/p.plist")
  BUILD=build/ios-device
  configure "$BUILD" iphoneos -DUJL_IOS_SIGN_IDENTITY=
  cmake --build "$BUILD" --target ujl
  APP="$BUILD/dev/UltimateUJL.app"
  rm -rf "$BUILD/dev"
  mkdir -p "$BUILD/dev"
  cp -R "$BUILD/UltimateUJL.app" "$APP"
  /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $BUNDLE_ID.dev" -c "Set :CFBundleDisplayName UJL Dev" "$APP/Info.plist"
  cp "$PROFILE" "$APP/embedded.mobileprovision"
  /usr/libexec/PlistBuddy -x -c "Print :Entitlements" "$TMP/p.plist" > "$TMP/ent.plist"
  /usr/libexec/PlistBuddy -c "Set :application-identifier $TEAM.$BUNDLE_ID.dev" "$TMP/ent.plist"
  codesign --force --sign "$IDENTITY" --entitlements "$TMP/ent.plist" --timestamp=none "$APP"
  xcrun devicectl device install app --device "$DEVICE" "$APP"
  for f in "$@"; do
    xcrun devicectl device copy to --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$BUNDLE_ID.dev" \
      --source "$f" --destination "Documents/$(basename "$f")"
    echo "copied $(basename "$f") to the app's Documents"
  done
  echo "installed $BUNDLE_ID.dev on $DEVICE; start it with: xcrun devicectl device process launch --device $DEVICE $BUNDLE_ID.dev"
  exit 0
fi

if [ "${1:-}" = "--ipa" ]; then
  BUILD=build/ios-device
  configure "$BUILD" iphoneos -DUJL_IOS_SIGN_IDENTITY=
  cmake --build "$BUILD" --target ujl
  STAGE="$BUILD/ipa"
  rm -rf "$STAGE" "$BUILD/UltimateUJL.ipa"
  mkdir -p "$STAGE/Payload"
  cp -R "$BUILD/UltimateUJL.app" "$STAGE/Payload/"
  # the linker signs arm64 executables ad hoc by itself; strip that too so the IPA is plainly unsigned
  codesign --remove-signature "$STAGE/Payload/UltimateUJL.app/UltimateUJL" 2>/dev/null || true
  rm -rf "$STAGE/Payload/UltimateUJL.app/_CodeSignature"
  (cd "$STAGE" && zip -qry ../UltimateUJL.ipa Payload)
  rm -rf "$STAGE"
  echo "wrote $BUILD/UltimateUJL.ipa (unsigned, bundle id $BUNDLE_ID)"
  exit 0
fi

BUILD=build/ios-sim
configure "$BUILD" iphonesimulator
cmake --build "$BUILD" --target ujl
xcrun simctl install booted "$BUILD/UltimateUJL.app"
DOCS="$(xcrun simctl get_app_container booted "$BUNDLE_ID" data)/Documents"
mkdir -p "$DOCS"
for f in "$@"; do
  cp -c "$f" "$DOCS/" 2>/dev/null || cp "$f" "$DOCS/"
  echo "copied $(basename "$f") to the app's Documents"
done
echo "installed $BUNDLE_ID; start it with: xcrun simctl launch booted $BUNDLE_ID"
