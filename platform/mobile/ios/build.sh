#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build/ios}"
GEN="${M2_GEN_ROOT:-$ROOT/build/gen}"
UNSIGNED="${UNSIGNED:-0}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "iOS builds require macOS with Xcode." >&2
    exit 1
fi
if [[ ! -f "$GEN/daytona93/gen_table.cpp" ]]; then
    echo "Missing generated game sources in $GEN." >&2
    echo "Run the normal host recompile first, then rerun this script." >&2
    exit 1
fi

python3 "$ROOT/scripts/setup.py" --no-build

mkdir -p "$BUILD"
BUILD="$(cd "$BUILD" && pwd)"
GEN="$(cd "$GEN" && pwd)"
set -- -S "$ROOT" -B "$BUILD" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DM2_GEN_ROOT="$GEN"
if [[ "$UNSIGNED" == 1 ]]; then
    set -- "$@" -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
        -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO \
        -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY=
fi
cmake "$@"

cmake --build "$BUILD" --config Release --target daytona
echo
echo "Built: $BUILD/Release-iphoneos/daytona.app"
if [[ "$UNSIGNED" == 1 ]]; then
    STAGE="$(mktemp -d "$BUILD/ipa.XXXXXX")"
    mkdir "$STAGE/Payload"
    ditto --norsrc --noextattr --noqtn "$BUILD/Release-iphoneos/daytona.app" "$STAGE/Payload/daytona.app"
    if codesign -d "$STAGE/Payload/daytona.app" >/dev/null 2>&1; then
        codesign --remove-signature "$STAGE/Payload/daytona.app"
    fi
    /usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$STAGE/Payload/daytona.app/Info.plist"
    (cd "$STAGE" && ditto -c -k --norsrc --noextattr --noqtn --keepParent Payload "$BUILD/Daytona-unsigned.ipa")
    echo "Unsigned IPA: $BUILD/Daytona-unsigned.ipa (sign with AltStore before installing)"
else
    echo "Open $BUILD/daytona_recomp.xcodeproj to select your signing team and deploy."
fi
