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
mkdir -p "$APP_DIR/usr/share/icons/hicolor/192x192/apps"

echo "Staging AppDir contents at $APP_DIR..."

# Copy executable
cp "$EXE_PATH" "$APP_DIR/usr/bin/FOHMedia"

# Copy resources required next to the binary
cp "${SOURCE_DIR}/Media/logo0.png" "$APP_DIR/usr/bin/" 2>/dev/null || true
cp "${SOURCE_DIR}/Media/BebasNeue-Regular.ttf" "$APP_DIR/usr/bin/" 2>/dev/null || true

# Use 192x192 icon to comply with linuxdeploy icon size validation
mkdir -p "$APP_DIR/usr/share/icons/hicolor/192x192/apps"
cp "${SOURCE_DIR}/Media/logo0_192.png" "$APP_DIR/usr/share/icons/hicolor/192x192/apps/FOHMedia.png" 2>/dev/null || true

# Copy desktop file
if [ -f "${EXE_PATH%/*}/FOHMedia.desktop" ]; then
    cp "${EXE_PATH%/*}/FOHMedia.desktop" "$APP_DIR/usr/share/applications/" 2>/dev/null || true
else
    cp "${SOURCE_DIR}/FOHMedia.desktop" "$APP_DIR/usr/share/applications/" 2>/dev/null || true
fi

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

LINUXDEPLOY_BIN="$STAGING_DIR/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT_BIN="$STAGING_DIR/linuxdeploy-plugin-qt-x86_64.AppImage"

echo "Downloading linuxdeploy and qt plugin..."
wget -q -c -O "$LINUXDEPLOY_BIN" "$LINUXDEPLOY_URL"
wget -q -c -O "$LINUXDEPLOY_QT_BIN" "$LINUXDEPLOY_QT_URL"
chmod a+x "$LINUXDEPLOY_BIN" "$LINUXDEPLOY_QT_BIN"

echo "Extracting linuxdeploy tools to avoid FUSE issues..."
cd "$STAGING_DIR"
"$LINUXDEPLOY_BIN" --appimage-extract >/dev/null
mv squashfs-root linuxdeploy-ext
"$LINUXDEPLOY_QT_BIN" --appimage-extract >/dev/null
mv squashfs-root linuxdeploy-plugin-qt-ext
cd - >/dev/null

export PATH="$STAGING_DIR/linuxdeploy-ext/usr/bin:$STAGING_DIR/linuxdeploy-plugin-qt-ext/usr/bin:$PATH"

echo "Running linuxdeploy..."
# We use EXTRA_QT_PLUGINS to include multimedia explicitly
export EXTRA_QT_PLUGINS="multimedia;qml"
export QML_SOURCES_PATHS="${SOURCE_DIR}/qml"

linuxdeploy --appdir "$APP_DIR" -e "$APP_DIR/usr/bin/FOHMedia" -d "$APP_DIR/usr/share/applications/FOHMedia.desktop" -i "$APP_DIR/usr/share/icons/hicolor/192x192/apps/FOHMedia.png" --plugin qt

echo "Creating compressed tar.gz at $OUTPUT_TAR..."
rm -f "$OUTPUT_TAR"
tar -czf "$OUTPUT_TAR" -C "$STAGING_DIR" FOHMedia-Linux

echo "Successfully created tar.gz at $OUTPUT_TAR"
