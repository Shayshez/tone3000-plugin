#!/bin/bash
# Build a macOS .pkg installer wizard for TONE3000 Plum (Standalone, VST3, AU, AAX, CLAP).
#
# Two modes:
#
# 1. Ad-hoc (default, no env vars), for local unsigned builds:
#    Bundles inside are ad-hoc codesigned, the .pkg envelope is unsigned.
#    Recipients right-click the .pkg and choose Open the first time.
#
# 2. Developer ID + notarization (when env vars are set):
#    Bundles are signed with `Developer ID Application` + Hardened Runtime
#    + secure timestamp; the .pkg is signed with `Developer ID Installer`;
#    the .pkg is then submitted to `xcrun notarytool` and stapled.
#    Recipients can double-click the .pkg with no Gatekeeper warning.
#
# Usage:
#   # Ad-hoc:
#   ./script/create-pkg.sh
#
#   # Fully signed + notarized:
#   SIGN_ID_APP='Developer ID Application: Your Name (TEAMID)' \
#   SIGN_ID_PKG='Developer ID Installer:   Your Name (TEAMID)' \
#   NOTARY_PROFILE='tone3000-notary' \
#     ./script/create-pkg.sh
#
# Environment overrides:
#   RELEASE=path           override the Release artefacts dir
#   STAGE=path             override the temp staging dir
#   SIGN_ID_APP=identity   Developer ID Application identity (enables real signing)
#   SIGN_ID_PKG=identity   Developer ID Installer identity (signs the .pkg envelope)
#   NOTARY_PROFILE=name    `notarytool store-credentials` profile name to use
#   ENTITLEMENTS=path      Standalone app entitlements (defaults to plugin/TONE3000-Standalone.entitlements)
#
# Prereqs for Developer ID mode:
#   - Both certs in your login keychain (`security find-identity -v -p basic`).
#   - notarytool credentials saved once:
#       xcrun notarytool store-credentials "tone3000-notary" \
#         --apple-id "you@example.com" --team-id "TEAMID" \
#         --password "app-specific-password"

set -euo pipefail

# Fork identity: must match plugin/CMakeLists.txt (T3K_PRODUCT_NAME,
# PLUGIN_NAME, T3K_BUNDLE_ID) and plugin/include/AppIdentity.h (data folder).
PRODUCT="TONE3000-Plum"          # bundle file names
DISPLAY_NAME="TONE3000 Plum"     # installer titles
PKG_ID="com.plumaudio.tone3000plum"
DATA_FOLDER="TONE3000 Plum"

# Upstream VERSION it's based on + this fork's build number (commits since
# FORK_BASE, same count cmake/ForkVersion.cmake compiles into the plugin).
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UPSTREAM_VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION")"
FORK_BUILD="$(git -C "$REPO_ROOT" rev-list --count "$(tr -d '[:space:]' < "$REPO_ROOT/FORK_BASE")..HEAD")"
# pkg receipt version: numeric and increasing with every fork build.
VERSION="${UPSTREAM_VERSION}.${FORK_BUILD}"
PKG_NAME="${PRODUCT}-build${FORK_BUILD}-macos"
RELEASE="${RELEASE:-./build/plugin/TONE3000_artefacts/Release}"
STAGE="${STAGE:-./build/pkg-stage}"
COMPONENTS_DIR="${COMPONENTS_DIR:-./build/pkg-components}"
OUTPUT_PKG="./build/${PKG_NAME}.pkg"
INSTALLER_DIR="./script/installer"
FACTORY_PRESETS_SRC="./resources/factory-presets"
ENTITLEMENTS="${ENTITLEMENTS:-./plugin/TONE3000-Standalone.entitlements}"

SIGN_ID_APP="${SIGN_ID_APP:-}"
SIGN_ID_PKG="${SIGN_ID_PKG:-}"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"

if [[ "$(uname)" != "Darwin" ]]; then
  echo "This script must run on macOS (pkgbuild / productbuild)."
  exit 1
fi

if [[ ! -d "$RELEASE" ]]; then
  echo "Release build dir not found: $RELEASE"
  echo "Build Release first (see README)."
  exit 1
fi

if [[ ! -d "$RELEASE/Standalone/${PRODUCT}.app" ]]; then
  echo "Standalone build not found at $RELEASE/Standalone/${PRODUCT}.app"
  exit 1
fi

if [[ -n "$SIGN_ID_APP" && ! -f "$ENTITLEMENTS" ]]; then
  echo "Entitlements file not found: $ENTITLEMENTS"
  echo "Set ENTITLEMENTS=path or ensure plugin/TONE3000-Standalone.entitlements exists."
  exit 1
fi

if [[ -n "$SIGN_ID_APP" ]]; then
  echo "Signing mode: Developer ID"
  echo "  App:       $SIGN_ID_APP"
  echo "  Installer: ${SIGN_ID_PKG:-<unset; .pkg envelope will be unsigned!>}"
  echo "  Notary:    ${NOTARY_PROFILE:-<unset; skipping notarization>}"
else
  echo "Signing mode: ad-hoc"
fi

# 0. Helpers: sign one bundle / detect a PACE-signed AAX

# AAX bundles that CI already ran through PACE's wraptool carry both the
# PACE signature and the Developer ID signature (wraptool applies the
# hardened-runtime codesign itself, see script/sign-aax-macos.sh).
# Re-signing such a bundle here would invalidate the PACE signature and
# Pro Tools would refuse to load it, so it must be left untouched.
aax_already_signed() {
  codesign -dvv "$1" 2>&1 | grep -q "Authority=Developer ID Application"
}

sign_bundle() {
  local bundle="$1"
  local extra_args=()

  if [[ "$bundle" == *.app ]]; then
    extra_args=( --entitlements "$ENTITLEMENTS" )
  fi

  if [[ -n "$SIGN_ID_APP" ]]; then
    codesign --force --deep --options runtime --timestamp \
      ${extra_args[@]+"${extra_args[@]}"} \
      --sign "$SIGN_ID_APP" \
      "$bundle"
  else
    codesign --force --deep --sign - "$bundle"
  fi
}

echo "Cleaning stage..."
rm -rf "$STAGE" "$COMPONENTS_DIR"
mkdir -p "$STAGE/standalone/Applications"
mkdir -p "$STAGE/vst3"
mkdir -p "$STAGE/au"
mkdir -p "$STAGE/aax"
mkdir -p "$STAGE/clap"
mkdir -p "$COMPONENTS_DIR"

# 1. Stage artefacts at their final layout

echo "Staging Standalone..."
ditto "$RELEASE/Standalone/${PRODUCT}.app" "$STAGE/standalone/Applications/${PRODUCT}.app"

HAVE_VST3=0
HAVE_AU=0
HAVE_AAX=0
HAVE_CLAP=0

if [[ -d "$RELEASE/VST3/${PRODUCT}.vst3" ]]; then
  echo "Staging VST3..."
  ditto "$RELEASE/VST3/${PRODUCT}.vst3" "$STAGE/vst3/${PRODUCT}.vst3"
  HAVE_VST3=1
fi

if [[ -d "$RELEASE/AU/${PRODUCT}.component" ]]; then
  echo "Staging AU..."
  ditto "$RELEASE/AU/${PRODUCT}.component" "$STAGE/au/${PRODUCT}.component"
  HAVE_AU=1
fi

if [[ -d "$RELEASE/AAX/${PRODUCT}.aaxplugin" ]]; then
  echo "Staging AAX..."
  ditto "$RELEASE/AAX/${PRODUCT}.aaxplugin" "$STAGE/aax/${PRODUCT}.aaxplugin"
  HAVE_AAX=1
fi

if [[ -d "$RELEASE/CLAP/${PRODUCT}.clap" ]]; then
  echo "Staging CLAP..."
  ditto "$RELEASE/CLAP/${PRODUCT}.clap" "$STAGE/clap/${PRODUCT}.clap"
  HAVE_CLAP=1
fi

HAVE_PRESETS=0
if compgen -G "${FACTORY_PRESETS_SRC}/*.t3kpreset" > /dev/null; then
  echo "Staging factory presets..."
  mkdir -p "$STAGE/presets/Library/Application Support/${DATA_FOLDER}/Presets/Factory"
  cp "${FACTORY_PRESETS_SRC}"/*.t3kpreset \
    "$STAGE/presets/Library/Application Support/${DATA_FOLDER}/Presets/Factory/"
  HAVE_PRESETS=1
else
  echo "No factory presets in $FACTORY_PRESETS_SRC (skipping)."
fi

xattr -cr "$STAGE" 2>/dev/null || true

# 2. Sign each staged bundle

echo "Signing staged bundles..."
sign_bundle "$STAGE/standalone/Applications/${PRODUCT}.app"
[[ $HAVE_VST3 -eq 1 ]] && sign_bundle "$STAGE/vst3/${PRODUCT}.vst3"
[[ $HAVE_AU   -eq 1 ]] && sign_bundle "$STAGE/au/${PRODUCT}.component"
if [[ $HAVE_AAX -eq 1 ]]; then
  if aax_already_signed "$STAGE/aax/${PRODUCT}.aaxplugin"; then
    echo "AAX already signed by PACE wraptool; leaving its signature intact."
  else
    sign_bundle "$STAGE/aax/${PRODUCT}.aaxplugin"
  fi
fi
[[ $HAVE_CLAP -eq 1 ]] && sign_bundle "$STAGE/clap/${PRODUCT}.clap"

# 3. Build component .pkg files (one per install location)

echo "Building component packages..."

pkgbuild \
  --root "$STAGE/standalone" \
  --identifier "${PKG_ID}.standalone" \
  --version "$VERSION" \
  --install-location "/" \
  "$COMPONENTS_DIR/_standalone.pkg"

if [[ $HAVE_VST3 -eq 1 ]]; then
  pkgbuild \
    --root "$STAGE/vst3" \
    --identifier "${PKG_ID}.vst3" \
    --version "$VERSION" \
    --install-location "/Library/Audio/Plug-Ins/VST3" \
    "$COMPONENTS_DIR/_vst3.pkg"
fi

if [[ $HAVE_AU -eq 1 ]]; then
  pkgbuild \
    --root "$STAGE/au" \
    --identifier "${PKG_ID}.au" \
    --version "$VERSION" \
    --install-location "/Library/Audio/Plug-Ins/Components" \
    "$COMPONENTS_DIR/_au.pkg"
fi

if [[ $HAVE_AAX -eq 1 ]]; then
  pkgbuild \
    --root "$STAGE/aax" \
    --identifier "${PKG_ID}.aax" \
    --version "$VERSION" \
    --install-location "/Library/Application Support/Avid/Audio/Plug-Ins" \
    "$COMPONENTS_DIR/_aax.pkg"
fi

if [[ $HAVE_CLAP -eq 1 ]]; then
  pkgbuild \
    --root "$STAGE/clap" \
    --identifier "${PKG_ID}.clap" \
    --version "$VERSION" \
    --install-location "/Library/Audio/Plug-Ins/CLAP" \
    "$COMPONENTS_DIR/_clap.pkg"
fi

if [[ $HAVE_PRESETS -eq 1 ]]; then
  # The preinstall script clears the previous factory set so this install
  # fully replaces it (pkg payloads overlay and never delete old files).
  pkgbuild \
    --root "$STAGE/presets" \
    --scripts "$INSTALLER_DIR/macos/presets-scripts" \
    --identifier "${PKG_ID}.factorypresets" \
    --version "$VERSION" \
    --install-location "/" \
    "$COMPONENTS_DIR/_presets.pkg"
fi

# 4. Generate distribution.xml for the components we actually built

DIST_XML="$COMPONENTS_DIR/distribution.xml"
RES="$INSTALLER_DIR/Resources"

{
  echo '<?xml version="1.0" encoding="utf-8"?>'
  echo '<installer-gui-script minSpecVersion="2">'
  echo "  <title>${DISPLAY_NAME} (build ${FORK_BUILD})</title>"
  # hostArchitectures: without arm64 listed, Installer.app on Apple Silicon
  # evaluates the distribution under Rosetta 2 and prompts to install it.
  # productbuild only injects this default when it synthesizes the XML
  # itself; a hand-written --distribution file must declare it.
  echo '  <options customize="always" allow-external-scripts="no" rootVolumeOnly="false" hostArchitectures="arm64,x86_64" />'
  echo '  <domains enable_localSystem="true" />'

  # Branding: a logo-only image (transparent elsewhere) anchored bottom-left
  # under the sidebar, so the window keeps its native appearance. Each TIFF
  # carries 1x + 2x reps; the light variant puts the wordmark on a black chip.
  # To regenerate: rasterize design/tone3000-wordmark.svg (140 pt wide, 16/14
  # pt margins), then pair the sizes with `tiffutil -cathidpicheck 1x 2x`.
  [[ -f "$RES/background.tiff" ]] &&
    echo '  <background file="background.tiff" mime-type="image/tiff" alignment="bottomleft" scaling="none" />'
  [[ -f "$RES/background-dark.tiff" ]] &&
    echo '  <background-darkAqua file="background-dark.tiff" mime-type="image/tiff" alignment="bottomleft" scaling="none" />'

  # readme (not license): shows the MIT text without forcing an Agree dialog.
  # No <conclusion>: omitting it gives the native Summary pane with the
  # green checkmark ("The installation was successful.").
  [[ -f "$RES/welcome.html" ]] && echo '  <welcome file="welcome.html" mime-type="text/html" />'
  [[ -f "$RES/readme.html"  ]] && echo '  <readme file="readme.html" mime-type="text/html" />'

  echo '  <choices-outline>'
  echo '    <line choice="standalone" />'
  [[ $HAVE_VST3 -eq 1 ]] && echo '    <line choice="vst3" />'
  [[ $HAVE_AU   -eq 1 ]] && echo '    <line choice="au" />'
  [[ $HAVE_AAX  -eq 1 ]] && echo '    <line choice="aax" />'
  [[ $HAVE_CLAP -eq 1 ]] && echo '    <line choice="clap" />'
  [[ $HAVE_PRESETS -eq 1 ]] && echo '    <line choice="presets" />'
  echo '  </choices-outline>'

  cat <<XML
  <choice id="standalone" title="Standalone App" description="Installs ${PRODUCT}.app to /Applications.">
    <pkg-ref id="${PKG_ID}.standalone" />
  </choice>
  <pkg-ref id="${PKG_ID}.standalone" version="${VERSION}" auth="root">_standalone.pkg</pkg-ref>
XML

  if [[ $HAVE_VST3 -eq 1 ]]; then
    cat <<XML
  <choice id="vst3" title="VST3 Plug-In" description="Installs ${PRODUCT}.vst3 to /Library/Audio/Plug-Ins/VST3.">
    <pkg-ref id="${PKG_ID}.vst3" />
  </choice>
  <pkg-ref id="${PKG_ID}.vst3" version="${VERSION}" auth="root">_vst3.pkg</pkg-ref>
XML
  fi

  if [[ $HAVE_AU -eq 1 ]]; then
    cat <<XML
  <choice id="au" title="Audio Unit (AU)" description="Installs ${PRODUCT}.component to /Library/Audio/Plug-Ins/Components.">
    <pkg-ref id="${PKG_ID}.au" />
  </choice>
  <pkg-ref id="${PKG_ID}.au" version="${VERSION}" auth="root">_au.pkg</pkg-ref>
XML
  fi

  if [[ $HAVE_AAX -eq 1 ]]; then
    cat <<XML
  <choice id="aax" title="AAX (Pro Tools)" description="Installs ${PRODUCT}.aaxplugin to /Library/Application Support/Avid/Audio/Plug-Ins.">
    <pkg-ref id="${PKG_ID}.aax" />
  </choice>
  <pkg-ref id="${PKG_ID}.aax" version="${VERSION}" auth="root">_aax.pkg</pkg-ref>
XML
  fi

  if [[ $HAVE_CLAP -eq 1 ]]; then
    cat <<XML
  <choice id="clap" title="CLAP Plug-In" description="Installs ${PRODUCT}.clap to /Library/Audio/Plug-Ins/CLAP.">
    <pkg-ref id="${PKG_ID}.clap" />
  </choice>
  <pkg-ref id="${PKG_ID}.clap" version="${VERSION}" auth="root">_clap.pkg</pkg-ref>
XML
  fi

  if [[ $HAVE_PRESETS -eq 1 ]]; then
    cat <<XML
  <choice id="presets" title="Factory Presets" description="Installs read-only ${DISPLAY_NAME} presets to /Library/Application Support/${DATA_FOLDER}/Presets/Factory.">
    <pkg-ref id="${PKG_ID}.factorypresets" />
  </choice>
  <pkg-ref id="${PKG_ID}.factorypresets" version="${VERSION}" auth="root">_presets.pkg</pkg-ref>
XML
  fi

  echo '</installer-gui-script>'
} > "$DIST_XML"

# 5. Build the final wizard pkg

RESOURCES_ARG=()
if [[ -d "$INSTALLER_DIR/Resources" ]]; then
  RESOURCES_ARG=( --resources "$INSTALLER_DIR/Resources" )
fi

PRODUCTBUILD_SIGN=()
if [[ -n "$SIGN_ID_PKG" ]]; then
  PRODUCTBUILD_SIGN=( --sign "$SIGN_ID_PKG" )
fi

mkdir -p ./build
rm -f "$OUTPUT_PKG"
echo "Building final installer..."
productbuild \
  --distribution "$DIST_XML" \
  --package-path "$COMPONENTS_DIR" \
  ${RESOURCES_ARG[@]+"${RESOURCES_ARG[@]}"} \
  ${PRODUCTBUILD_SIGN[@]+"${PRODUCTBUILD_SIGN[@]}"} \
  "$OUTPUT_PKG"

xattr -cr "$OUTPUT_PKG" 2>/dev/null || true

# 6. Notarize + staple (optional)

if [[ -n "$SIGN_ID_APP" && -n "$NOTARY_PROFILE" ]]; then
  if [[ -z "$SIGN_ID_PKG" ]]; then
    echo "Skipping notarization: NOTARY_PROFILE set but SIGN_ID_PKG is not."
    echo "Apple will reject an unsigned .pkg envelope."
  else
    echo "Submitting to notarytool (this can take a few minutes)..."
    xcrun notarytool submit "$OUTPUT_PKG" \
      --keychain-profile "$NOTARY_PROFILE" \
      --wait

    echo "Stapling notarization ticket..."
    xcrun stapler staple "$OUTPUT_PKG"
    xcrun stapler validate "$OUTPUT_PKG"
  fi
fi

rm -rf "$STAGE" "$COMPONENTS_DIR"

echo ""
echo "Installer ready: $OUTPUT_PKG"
if [[ -n "$SIGN_ID_APP" && -n "$SIGN_ID_PKG" && -n "$NOTARY_PROFILE" ]]; then
  echo "Signed + notarized + stapled. Recipients can double-click with no warning."
elif [[ -n "$SIGN_ID_APP" ]]; then
  echo "Bundles signed with Developer ID."
  if [[ -z "$SIGN_ID_PKG" ]]; then
    echo "  .pkg envelope is unsigned; set SIGN_ID_PKG to sign it."
  fi
  if [[ -z "$NOTARY_PROFILE" ]]; then
    echo "  Not notarized; set NOTARY_PROFILE to notarize and staple."
  fi
else
  echo "Ad-hoc only. Recipients: right-click the .pkg in Finder → Open → Open."
fi
echo "After install, ${PRODUCT}.app lives in /Applications and the UI loads"
echo "normally (no App Translocation)."
