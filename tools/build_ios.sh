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
