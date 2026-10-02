#!/bin/bash
# Wave Emulation macOS release builder.
#
# Produces a universal standalone/AU/VST3/AAX installer and optional DMG. The AAX bundle
# is PACE-wrapped with the Wave product configuration before it is packaged.
# Apple credentials are read from the keychain or environment; no passwords
# are stored in this script.
#
# Common overrides:
#   VERSION=0.1.12
#   BUILD_JOBS=8
#   SKIP_PLUGIN_BUILD=1
#   SKIP_WRAP=1
#   SKIP_PACE_PREFLIGHT=1
#   SKIP_SIGN=1
#   SKIP_NOTARIZE=1
#   CONTINUE_ON_NOTARIZE_FAILURE=1
#   SKIP_DMG=1
#   NOTARIZE_PROFILE=wave-notary
#   MANUAL_PDF=/absolute/path/to/manual.pdf
#   CODESIGN_IDENTITY="Developer ID Application: ... (YOUR_TEAM_ID)"
#   INSTALLER_IDENTITY="Developer ID Installer: ... (YOUR_TEAM_ID)"

set -euo pipefail
export COPYFILE_DISABLE=1

is_truthy() {
  case "${1:-}" in
  1 | true | TRUE | yes | YES | on | ON) return 0 ;;
  *) return 1 ;;
  esac
}

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Local account defaults stay outside version control. Environment overrides
# can be preserved by using ${VARIABLE:-default} assignments in this file.
if [ -f "$PROJECT_ROOT/Installer/.env.local" ]; then
  source "$PROJECT_ROOT/Installer/.env.local"
fi
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
INSTALLER_DIR="$PROJECT_ROOT/Installer"
WORK_DIR="$INSTALLER_DIR/.release-work"
STAGING_DIR="$WORK_DIR/staging"
PACKAGES_DIR="$WORK_DIR/packages"
RESOURCES_DIR="$WORK_DIR/resources"
DMG_STAGING_DIR="$WORK_DIR/dmg"

VERSION="${VERSION:-0.1.12}"
TEAM_ID="${TEAM_ID:-}"
BUILD_CONFIG="${BUILD_CONFIG:-Release}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-$BUILD_CONFIG}"
CMAKE_OSX_ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES:-arm64;x86_64}"
CMAKE_OSX_DEPLOYMENT_TARGET="${CMAKE_OSX_DEPLOYMENT_TARGET:-13.0}"
CMAKE_OSX_SYSROOT="${CMAKE_OSX_SYSROOT:-$(xcrun --sdk macosx --show-sdk-path)}"
BUILD_JOBS="${BUILD_JOBS:-8}"

TARGET="WaveEmulation"
BUNDLE_NAME="Wave Emulation"
PRODUCT_NAME="Wave Emulation"
PRODUCT_IDENTIFIER="${PRODUCT_IDENTIFIER:-com.djw.waveemulation}"
ARTEFACT_DIR="$BUILD_DIR/${TARGET}_artefacts/$BUILD_CONFIG"

APP_NAME="$BUNDLE_NAME.app"
APP_SOURCE="$ARTEFACT_DIR/Standalone/$APP_NAME"
AU_NAME="$BUNDLE_NAME.component"
VST3_NAME="$BUNDLE_NAME.vst3"
AAX_NAME="$BUNDLE_NAME.aaxplugin"
AAX_WRAPPED_NAME="$BUNDLE_NAME.wrapped.aaxplugin"

AU_SOURCE="$ARTEFACT_DIR/AU/$AU_NAME"
VST3_SOURCE="$ARTEFACT_DIR/VST3/$VST3_NAME"
AAX_SOURCE="$ARTEFACT_DIR/AAX/$AAX_NAME"
AAX_WRAPPED="$ARTEFACT_DIR/AAX/$AAX_WRAPPED_NAME"

PACE_ACCOUNT="${PACE_ACCOUNT:-}"
PACE_WCGUID="${PACE_WCGUID:-}"
PACE_SIGNID="${PACE_SIGNID:-$TEAM_ID}"
WRAPTOOL="${WRAPTOOL:-/Applications/PACEAntiPiracy/Eden/Fusion/Versions/5/bin/wraptool}"

NOTARIZE_PROFILE="${NOTARIZE_PROFILE-wave-notary}"
NOTARIZE_APPLE_ID="${NOTARIZE_APPLE_ID:-${APPLE_ID:-}}"
NOTARIZE_PASSWORD="${NOTARIZE_PASSWORD:-${APP_SPECIFIC_PASSWORD:-}}"
NOTARIZE_TEAM_ID="${NOTARIZE_TEAM_ID:-$TEAM_ID}"
NOTARIZE_DMG="${NOTARIZE_DMG:-1}"

FINAL_PKG_NAME="${FINAL_PKG_NAME:-DJW $PRODUCT_NAME Installer $VERSION.pkg}"
FINAL_DMG_NAME="${FINAL_DMG_NAME:-DJW $PRODUCT_NAME $VERSION.dmg}"
FINAL_PKG="$INSTALLER_DIR/$FINAL_PKG_NAME"
FINAL_DMG="$PROJECT_ROOT/$FINAL_DMG_NAME"

find_signing_identities() {
  if is_truthy "${SKIP_SIGN:-}"; then
    return
  fi

  : "${TEAM_ID:?Set TEAM_ID for release signing, or use SKIP_SIGN=1}"

  if [ -z "${CODESIGN_IDENTITY:-}" ]; then
    CODESIGN_IDENTITY="$(security find-identity -v -p codesigning |
      grep "Developer ID Application" | grep "$TEAM_ID" | head -1 |
      sed -E 's/.*"([^"]+)".*/\1/' || true)"
  fi
  if [ -z "${INSTALLER_IDENTITY:-}" ]; then
    INSTALLER_IDENTITY="$(security find-identity -v |
      grep "Developer ID Installer" | grep "$TEAM_ID" | head -1 |
      sed -E 's/.*"([^"]+)".*/\1/' || true)"
  fi

  if [ -z "$CODESIGN_IDENTITY" ] || [ -z "$INSTALLER_IDENTITY" ]; then
    echo "Error: Developer ID Application and Installer certificates are required for team $TEAM_ID." >&2
    echo "Use SKIP_SIGN=1 only for a local development package." >&2
    exit 1
  fi
}

bundle_executable() {
  /usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$1/Contents/Info.plist"
}

validate_bundle() {
  local bundle="$1"
  if [ ! -d "$bundle" ]; then
    echo "Error: missing bundle: $bundle" >&2
    exit 1
  fi
  plutil -lint "$bundle/Contents/Info.plist" >/dev/null
  local bundle_version
  bundle_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$bundle/Contents/Info.plist")"
  if [ "$bundle_version" != "$VERSION" ]; then
    echo "Error: $bundle has version $bundle_version; expected $VERSION. Rebuild before packaging." >&2
    exit 1
  fi
  local executable
  executable="$(bundle_executable "$bundle")"
  local binary="$bundle/Contents/MacOS/$executable"
  if [ ! -f "$binary" ]; then
    echo "Error: missing bundle executable: $binary" >&2
    exit 1
  fi
  local architectures
  architectures="$(lipo -archs "$binary")"
  for required in arm64 x86_64; do
    if ! [[ " $architectures " == *" $required "* ]]; then
      echo "Error: $bundle is missing $required (found: $architectures)." >&2
      exit 1
    fi
    local minimum_os
    minimum_os="$(xcrun vtool -arch "$required" -show-build "$binary" | awk '$1 == "minos" { print $2 }')"
    if [ "$minimum_os" != "$CMAKE_OSX_DEPLOYMENT_TARGET" ]; then
      echo "Error: $bundle ($required) targets macOS $minimum_os; expected $CMAKE_OSX_DEPLOYMENT_TARGET. Rebuild before packaging." >&2
      exit 1
    fi
  done
}

has_pace_payload() {
  [ -e "$1/Contents/__Pace_Eden.bundle" ] ||
    [ -e "$1/Contents/Resources/__Pace_Eden" ]
}

validate_pace_wrap_config() {
  if is_truthy "${SKIP_WRAP:-}" || is_truthy "${SKIP_PACE_PREFLIGHT:-}"; then
    return
  fi
  if [ ! -x "$WRAPTOOL" ]; then
    echo "Error: PACE wraptool is unavailable: $WRAPTOOL" >&2
    exit 1
  fi

  echo "Refreshing PACE Fusion v5 Wrap Configs..."
  local pace_sync
  if ! pace_sync="$($WRAPTOOL sync --account "$PACE_ACCOUNT" 2>&1)"; then
    echo "$pace_sync" >&2
    echo "Error: PACE could not refresh Wrap Configs for account '$PACE_ACCOUNT'." >&2
    exit 1
  fi

  echo "Checking PACE Fusion v5 Wrap Config..."
  local pace_configs
  if ! pace_configs="$($WRAPTOOL list --account "$PACE_ACCOUNT" 2>&1)"; then
    echo "$pace_configs" >&2
    echo "Error: PACE could not list Wrap Configs for account '$PACE_ACCOUNT'." >&2
    exit 1
  fi

  if ! grep -Fqi "GUID:    $PACE_WCGUID" <<<"$pace_configs"; then
    echo "Error: PACE account '$PACE_ACCOUNT' has no Fusion v5 Wrap Config with GUID:" >&2
    echo "  $PACE_WCGUID" >&2
    echo >&2
    echo "PACE reports this GUID as belonging to an incorrect Wrap Config version." >&2
    echo "Create or upgrade the Wave AAX Wrap Config to Fusion v5 in the PACE portal," >&2
    echo "then run this script with the new GUID, for example:" >&2
    echo "  PACE_WCGUID=NEW-V5-WRAP-CONFIG-GUID ./Installer/build_installer.sh" >&2
    echo >&2
    echo "Fusion v5 Wrap Configs currently available to this account:" >&2
    awk '
      /^  Product:/ { product=$0; sub(/^  Product: /, "", product) }
      /^  Name:/    { name=$0; sub(/^  Name:    /, "", name) }
      /^  GUID:/    { guid=$0; sub(/^  GUID:    /, "", guid); print "  " product " | " name " | " guid }
    ' <<<"$pace_configs" >&2
    exit 1
  fi
}

build_plugins() {
  if is_truthy "${SKIP_PLUGIN_BUILD:-}"; then
    echo "Skipping plug-in build."
    return
  fi

  # A previous PACE output cannot be linked over safely. Remove only the
  # generated AAX artefacts before asking CMake for a fresh unwrapped input.
  if [ -d "$AAX_SOURCE" ] && has_pace_payload "$AAX_SOURCE"; then
    rm -rf "$AAX_SOURCE" "$AAX_WRAPPED"
  fi

  cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCMAKE_OSX_ARCHITECTURES="$CMAKE_OSX_ARCHITECTURES" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$CMAKE_OSX_DEPLOYMENT_TARGET" \
    -DCMAKE_OSX_SYSROOT="$CMAKE_OSX_SYSROOT" \
    -DWAVE_SIGN_RELEASE_ARTIFACTS=OFF \
    -DWAVE_EMBED_PRIVATE_ASSETS=OFF
  cmake --build "$BUILD_DIR" \
    --target "${TARGET}_Standalone" "${TARGET}_AU" "${TARGET}_VST3" "${TARGET}_AAX" \
    --config "$BUILD_CONFIG" -j "$BUILD_JOBS"
}

wrap_aax() {
  if is_truthy "${SKIP_WRAP:-}"; then
    echo "Skipping PACE AAX wrapping."
    return
  fi
  if [ ! -x "$WRAPTOOL" ]; then
    echo "Error: PACE wraptool is unavailable: $WRAPTOOL" >&2
    exit 1
  fi
  if has_pace_payload "$AAX_SOURCE"; then
    echo "Error: AAX input is already PACE-wrapped; rebuild it or use SKIP_WRAP=1." >&2
    exit 1
  fi

  rm -rf "$AAX_WRAPPED"
  "$WRAPTOOL" sign --verbose \
    --account "$PACE_ACCOUNT" \
    --wcguid "$PACE_WCGUID" \
    --signid "$PACE_SIGNID" \
    --in "$AAX_SOURCE" \
    --out "$AAX_WRAPPED" \
    --autoinstall on

  validate_bundle "$AAX_WRAPPED"
  if ! has_pace_payload "$AAX_WRAPPED"; then
    echo "Error: wraptool completed without producing a PACE payload." >&2
    exit 1
  fi
  codesign --verify --deep --strict --verbose=2 "$AAX_WRAPPED"
  rm -rf "$AAX_SOURCE"
  mv "$AAX_WRAPPED" "$AAX_SOURCE"
}

stage_plugins() {
  rm -rf "$WORK_DIR"
  mkdir -p \
    "$STAGING_DIR/standalone/Applications" \
    "$STAGING_DIR/au/Library/Audio/Plug-Ins/Components" \
    "$STAGING_DIR/vst3/Library/Audio/Plug-Ins/VST3" \
    "$STAGING_DIR/aax/Library/Application Support/Avid/Audio/Plug-Ins" \
    "$PACKAGES_DIR" "$RESOURCES_DIR" "$DMG_STAGING_DIR"

  /usr/bin/ditto --noextattr --norsrc "$APP_SOURCE" \
    "$STAGING_DIR/standalone/Applications/$APP_NAME"
  /usr/bin/ditto --noextattr --norsrc "$AU_SOURCE" \
    "$STAGING_DIR/au/Library/Audio/Plug-Ins/Components/$AU_NAME"
  /usr/bin/ditto --noextattr --norsrc "$VST3_SOURCE" \
    "$STAGING_DIR/vst3/Library/Audio/Plug-Ins/VST3/$VST3_NAME"
  /usr/bin/ditto --noextattr --norsrc "$AAX_SOURCE" \
    "$STAGING_DIR/aax/Library/Application Support/Avid/Audio/Plug-Ins/$AAX_NAME"

  local notices="$STAGING_DIR/standalone/Library/Application Support/DJW/Wave Emulation"
  mkdir -p "$notices"
  for file in LICENSE THIRD_PARTY_NOTICES.md; do
    /usr/bin/ditto --noextattr --norsrc "$PROJECT_ROOT/$file" "$notices/$file"
  done
  /usr/bin/ditto --noextattr --norsrc "$PROJECT_ROOT/LICENSES" "$notices/LICENSES"
  cat >"$notices/BUILD.txt" <<BUILD
Wave Emulation $VERSION — macOS universal arm64/x86_64
Source repository: https://github.com/mo0kid/wave
Corresponding source archive: Wave-Emulation-$VERSION-Source.zip
Instrument firmware and disk images are supplied separately by the user.
BUILD
}

sign_staged_plugins() {
  if is_truthy "${SKIP_SIGN:-}"; then
    return
  fi
  local au="$STAGING_DIR/au/Library/Audio/Plug-Ins/Components/$AU_NAME"
  local vst3="$STAGING_DIR/vst3/Library/Audio/Plug-Ins/VST3/$VST3_NAME"
  local app="$STAGING_DIR/standalone/Applications/$APP_NAME"
  for bundle in "$app" "$au" "$vst3"; do
    codesign --force --deep --timestamp --options runtime \
      --sign "$CODESIGN_IDENTITY" "$bundle"
    codesign --verify --deep --strict --verbose=2 "$bundle"
  done
  # wraptool owns the final AAX signature. Re-signing it here would invalidate
  # the PACE envelope.
}

clean_payload_metadata() {
  local root="$1"
  xattr -cr "$root" 2>/dev/null || true
  dot_clean -m "$root" 2>/dev/null || true
  find "$root" -name '._*' -type f -delete
}

build_component_package() {
  local root="$1"
  local identifier="$2"
  local output="$3"
  local args=(--root "$root" --identifier "$identifier" --version "$VERSION"
              --install-location "/")
  if ! is_truthy "${SKIP_SIGN:-}"; then
    args+=(--sign "$INSTALLER_IDENTITY" --timestamp)
  fi
  if [ "$identifier" = "$PRODUCT_IDENTIFIER.standalone" ]; then
    local components="$WORK_DIR/standalone-components.plist"
    pkgbuild --analyze --root "$root" "$components"
    # Always install into /Applications, even if an older development copy
    # with the same bundle identifier exists elsewhere on the machine.
    /usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$components" 2>/dev/null ||
      /usr/libexec/PlistBuddy -c "Add :0:BundleIsRelocatable bool false" "$components"
    args+=(--component-plist "$components")
  fi
  pkgbuild "${args[@]}" "$output"
}

build_packages() {
  for payload in "$STAGING_DIR/standalone" "$STAGING_DIR/au" "$STAGING_DIR/vst3" "$STAGING_DIR/aax"; do
    clean_payload_metadata "$payload"
  done

  build_component_package "$STAGING_DIR/standalone" "$PRODUCT_IDENTIFIER.standalone" \
    "$PACKAGES_DIR/WaveEmulation_Standalone.pkg"
  build_component_package "$STAGING_DIR/au" "$PRODUCT_IDENTIFIER.au" \
    "$PACKAGES_DIR/WaveEmulation_AU.pkg"
  build_component_package "$STAGING_DIR/vst3" "$PRODUCT_IDENTIFIER.vst3" \
    "$PACKAGES_DIR/WaveEmulation_VST3.pkg"
  build_component_package "$STAGING_DIR/aax" "$PRODUCT_IDENTIFIER.aax" \
    "$PACKAGES_DIR/WaveEmulation_AAX.pkg"

  cat >"$RESOURCES_DIR/welcome.html" <<WELCOME
<!doctype html><html><head><meta charset="utf-8"></head>
<body style="font-family:-apple-system,Helvetica Neue,sans-serif;padding:20px">
<h1>DJW $PRODUCT_NAME</h1>
<p>This installer provides the Wave Emulation standalone app in /Applications, plus AU, VST3 and AAX instrument formats.</p>
<p style="color:#666;font-size:12px">Version $VERSION</p>
</body></html>
WELCOME

  cat >"$WORK_DIR/distribution.xml" <<DISTRIBUTION
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>DJW $PRODUCT_NAME</title>
  <organization>com.djw</organization>
  <welcome file="welcome.html"/>
  <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <volume-check><allowed-os-versions><os-version min="$CMAKE_OSX_DEPLOYMENT_TARGET"/></allowed-os-versions></volume-check>
  <choices-outline>
    <line choice="standalone"/><line choice="au"/><line choice="vst3"/><line choice="aax"/>
  </choices-outline>
  <choice id="standalone" title="Standalone Application"><pkg-ref id="$PRODUCT_IDENTIFIER.standalone"/></choice>
  <choice id="au" title="Audio Unit"><pkg-ref id="$PRODUCT_IDENTIFIER.au"/></choice>
  <choice id="vst3" title="VST3"><pkg-ref id="$PRODUCT_IDENTIFIER.vst3"/></choice>
  <choice id="aax" title="AAX"><pkg-ref id="$PRODUCT_IDENTIFIER.aax"/></choice>
  <pkg-ref id="$PRODUCT_IDENTIFIER.standalone" version="$VERSION">WaveEmulation_Standalone.pkg</pkg-ref>
  <pkg-ref id="$PRODUCT_IDENTIFIER.au" version="$VERSION">WaveEmulation_AU.pkg</pkg-ref>
  <pkg-ref id="$PRODUCT_IDENTIFIER.vst3" version="$VERSION">WaveEmulation_VST3.pkg</pkg-ref>
  <pkg-ref id="$PRODUCT_IDENTIFIER.aax" version="$VERSION">WaveEmulation_AAX.pkg</pkg-ref>
</installer-gui-script>
DISTRIBUTION

  local args=(--distribution "$WORK_DIR/distribution.xml"
              --resources "$RESOURCES_DIR" --package-path "$PACKAGES_DIR")
  if ! is_truthy "${SKIP_SIGN:-}"; then
    args+=(--sign "$INSTALLER_IDENTITY" --timestamp)
  fi
  productbuild "${args[@]}" "$FINAL_PKG"
  if ! is_truthy "${SKIP_SIGN:-}"; then
    pkgutil --check-signature "$FINAL_PKG"
  fi
}

notary_arguments() {
  NOTARY_ARGS=()
  if [ -n "$NOTARIZE_PROFILE" ]; then
    NOTARY_ARGS=(--keychain-profile "$NOTARIZE_PROFILE")
  elif [ -n "$NOTARIZE_APPLE_ID" ] && [ -n "$NOTARIZE_PASSWORD" ]; then
    NOTARY_ARGS=(--apple-id "$NOTARIZE_APPLE_ID"
                 --password "$NOTARIZE_PASSWORD"
                 --team-id "$NOTARIZE_TEAM_ID")
  fi
}

notarize_package() {
  if is_truthy "${SKIP_NOTARIZE:-}" || is_truthy "${SKIP_SIGN:-}"; then
    echo "Skipping package notarization."
    return
  fi
  notary_arguments
  if [ "${#NOTARY_ARGS[@]}" -eq 0 ]; then
    echo "Error: no notarization keychain profile or credentials were supplied." >&2
    exit 1
  fi
  if ! xcrun notarytool submit "$FINAL_PKG" "${NOTARY_ARGS[@]}" --wait; then
    if is_truthy "${CONTINUE_ON_NOTARIZE_FAILURE:-}"; then
      echo "Warning: package notarization failed; continuing as requested." >&2
      SKIP_NOTARIZE=1
      return
    fi
    echo "Error: package notarization failed." >&2
    exit 1
  fi
  xcrun stapler staple "$FINAL_PKG"
  xcrun stapler validate "$FINAL_PKG"
}

find_manual() {
  if [ -n "${MANUAL_PDF:-}" ]; then
    return
  fi
  local candidate
  for candidate in "$PROJECT_ROOT"/docs/*.[pP][dD][fF]; do
    if [ -f "$candidate" ]; then
      MANUAL_PDF="$candidate"
      return
    fi
  done
  MANUAL_PDF=""
}

build_dmg() {
  if is_truthy "${SKIP_DMG:-}"; then
    echo "Skipping DMG creation."
    return
  fi
  find_manual
  rm -rf "$DMG_STAGING_DIR"
  mkdir -p "$DMG_STAGING_DIR"
  /usr/bin/ditto --noextattr --norsrc "$FINAL_PKG" \
    "$DMG_STAGING_DIR/$FINAL_PKG_NAME"
  /usr/bin/ditto --noextattr --norsrc \
    "$STAGING_DIR/standalone/Library/Application Support/DJW/Wave Emulation" \
    "$DMG_STAGING_DIR/License and Source Information"
  if [ -n "$MANUAL_PDF" ]; then
    /usr/bin/ditto --noextattr --norsrc "$MANUAL_PDF" \
      "$DMG_STAGING_DIR/$(basename "$MANUAL_PDF")"
  fi
  rm -f "$FINAL_DMG"
  hdiutil create -volname "DJW $PRODUCT_NAME" -srcfolder "$DMG_STAGING_DIR" \
    -ov -format UDZO "$FINAL_DMG"

  if ! is_truthy "${SKIP_SIGN:-}"; then
    codesign --force --timestamp --sign "$CODESIGN_IDENTITY" "$FINAL_DMG"
  fi
  if ! is_truthy "${SKIP_SIGN:-}" && ! is_truthy "${SKIP_NOTARIZE:-}" \
     && is_truthy "$NOTARIZE_DMG"; then
    notary_arguments
    xcrun notarytool submit "$FINAL_DMG" "${NOTARY_ARGS[@]}" --wait
    xcrun stapler staple "$FINAL_DMG"
    xcrun stapler validate "$FINAL_DMG"
  fi
}

echo "=== DJW $PRODUCT_NAME Release Builder ==="
echo "Version:        $VERSION"
echo "Architectures:  $CMAKE_OSX_ARCHITECTURES"
echo "PACE account:   $PACE_ACCOUNT"
echo "PACE WCGUID:    $PACE_WCGUID"
echo "Team ID:        $TEAM_ID"

if ! is_truthy "${SKIP_WRAP:-}"; then
  : "${PACE_ACCOUNT:?Set PACE_ACCOUNT for AAX wrapping}"
  if ! [[ "$PACE_WCGUID" =~ ^[[:xdigit:]]{8}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{12}$ ]]; then
    echo "Error: PACE_WCGUID is not a valid GUID: $PACE_WCGUID" >&2
    exit 1
  fi
fi

find_signing_identities
validate_pace_wrap_config
rm -f "$FINAL_PKG" "$FINAL_DMG"
build_plugins
validate_bundle "$APP_SOURCE"
validate_bundle "$AU_SOURCE"
validate_bundle "$VST3_SOURCE"
validate_bundle "$AAX_SOURCE"
wrap_aax
stage_plugins
sign_staged_plugins
build_packages
notarize_package
build_dmg
rm -rf "$WORK_DIR"

echo "=== Release complete ==="
echo "Installer: $FINAL_PKG"
if ! is_truthy "${SKIP_DMG:-}"; then
  echo "Disk image: $FINAL_DMG"
fi
