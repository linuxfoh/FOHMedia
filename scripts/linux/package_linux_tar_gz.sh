#!/usr/bin/env bash
set -euo pipefail

EXE_PATH="${1:-}"
OUTPUT_TAR="${2:-}"
SOURCE_DIR="${3:-}"
QMAKE_PATH="${4:-}"

if [ -z "$EXE_PATH" ] || [ -z "$OUTPUT_TAR" ] || [ -z "$SOURCE_DIR" ]; then
    echo "Usage: $0 <ExecutablePath> <OutputTarGzPath> <SourceDir> [QMakePath]"
    exit 1
fi

STAGING_DIR="$(mktemp -d /tmp/fohmedia_linux_staging.XXXXXX)"
trap 'rm -rf "$STAGING_DIR"' EXIT

APP_DIR="$STAGING_DIR/FOHMedia-Linux"
mkdir -p "$APP_DIR/usr/bin"
mkdir -p "$APP_DIR/usr/share/applications"
mkdir -p "$APP_DIR/usr/share/icons/hicolor/256x256/apps"

echo "Staging AppDir contents at $APP_DIR..."

# Copy executable
cp "$EXE_PATH" "$APP_DIR/usr/bin/FOHMedia"

# Copy resources required next to the binary
cp "${SOURCE_DIR}/Media/logo0.png" "$APP_DIR/usr/bin/" 2>/dev/null || true
cp "${SOURCE_DIR}/Media/BebasNeue-Regular.ttf" "$APP_DIR/usr/bin/" 2>/dev/null || true

# Copy desktop file and icon
cp "${SOURCE_DIR}/FOHMedia.desktop" "$APP_DIR/usr/share/applications/" 2>/dev/null || true
cp "${SOURCE_DIR}/Media/logo0.png" "$APP_DIR/usr/share/icons/hicolor/256x256/apps/FOHMedia.png" 2>/dev/null || true

LINUXDEPLOYQT_URL="https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage"
LINUXDEPLOYQT_BIN="$STAGING_DIR/linuxdeployqt.AppImage"

echo "Downloading linuxdeployqt..."
wget -q -c -O "$LINUXDEPLOYQT_BIN" "$LINUXDEPLOYQT_URL"
chmod a+x "$LINUXDEPLOYQT_BIN"

echo "Running linuxdeployqt..."
# We export NO_STRIP=1 in case stripping fails, and use -unsupported-allow-new-glibc for newer Ubuntu versions
export NO_STRIP=1

# linuxdeployqt will bundle the libraries into the AppDir.
if [ -n "$QMAKE_PATH" ]; then
    "$LINUXDEPLOYQT_BIN" "$APP_DIR/usr/bin/FOHMedia" -qmake="$QMAKE_PATH" -unsupported-allow-new-glibc -qmldir="${SOURCE_DIR}/qml"
else
    "$LINUXDEPLOYQT_BIN" "$APP_DIR/usr/bin/FOHMedia" -unsupported-allow-new-glibc -qmldir="${SOURCE_DIR}/qml"
fi

echo "Creating compressed tar.gz at $OUTPUT_TAR..."
rm -f "$OUTPUT_TAR"
tar -czf "$OUTPUT_TAR" -C "$STAGING_DIR" FOHMedia-Linux

echo "Successfully created tar.gz at $OUTPUT_TAR"
