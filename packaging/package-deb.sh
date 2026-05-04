#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DEBIAN_DIR="$PROJECT_ROOT/debian"
CHANGELOG_PATH="$DEBIAN_DIR/changelog"
APP_NAME="lcs"
MAINTAINER="Petr Bena <petr@bena.rocks>"

APP_VERSION="$(sed -n 's/^#define LCS_VERSION "\([^"]*\)"/\1/p' "$PROJECT_ROOT/src/common.h")"
NCPUS="$(nproc)"

usage() {
    echo "Usage: $0 [--version x.y.z]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            APP_VERSION="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

if [ -z "$APP_VERSION" ]; then
    echo "Error: unable to determine version from src/common.h"
    exit 1
fi

if [ ! -d "$DEBIAN_DIR" ]; then
    echo "Error: debian packaging metadata not found at $DEBIAN_DIR"
    exit 1
fi

echo "================================"
echo "Building lcs Debian package"
echo "================================"
echo "CPUs: $NCPUS"
echo "Version: $APP_VERSION"
echo ""

if command -v dpkg-query >/dev/null 2>&1; then
    missing=()

    require_pkg() {
        local pkg="$1"
        if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "install ok installed"; then
            missing+=("$pkg")
        fi
    }

    require_pkg build-essential
    require_pkg debhelper
    require_pkg dpkg-dev

    if [ ${#missing[@]} -gt 0 ]; then
        echo "Missing build dependencies detected."
        echo ""
        echo "Install the following packages and re-run:"
        if command -v apt-get >/dev/null 2>&1; then
            echo "  sudo apt-get install ${missing[*]}"
        else
            for pkg in "${missing[@]}"; do
                echo "  - $pkg"
            done
        fi
        echo ""
        exit 1
    fi
fi

DISTRO_TAG=""
if [ -r /etc/os-release ]; then
    . /etc/os-release
    if [ "${ID:-}" = "debian" ] && [ -n "${VERSION_ID:-}" ]; then
        DISTRO_TAG="deb${VERSION_ID}"
    elif [ "${ID:-}" = "ubuntu" ] && [ -n "${VERSION_ID:-}" ]; then
        DISTRO_TAG="ubuntu${VERSION_ID}"
    fi
fi

if [ -n "$DISTRO_TAG" ]; then
    DEB_VERSION="${APP_VERSION}-1~${DISTRO_TAG}"
else
    DEB_VERSION="${APP_VERSION}-1"
fi

BACKUP_CHANGELOG=""
if [ -f "$CHANGELOG_PATH" ]; then
    BACKUP_CHANGELOG="$(mktemp)"
    cp "$CHANGELOG_PATH" "$BACKUP_CHANGELOG"
fi

restore_changelog() {
    if [ -n "$BACKUP_CHANGELOG" ] && [ -f "$BACKUP_CHANGELOG" ]; then
        cp "$BACKUP_CHANGELOG" "$CHANGELOG_PATH"
        rm -f "$BACKUP_CHANGELOG"
    fi
}
trap restore_changelog EXIT

cat > "$CHANGELOG_PATH" <<EOF
${APP_NAME} (${DEB_VERSION}) unstable; urgency=medium

  * Automated build.

 -- ${MAINTAINER}  $(date -R)
EOF

echo "Step 1: Building package with debhelper..."
cd "$PROJECT_ROOT"
dpkg-buildpackage -b -us -uc

echo ""
echo "Step 2: Collecting .deb output..."
OUTPUT_DIR="$PROJECT_ROOT/packaging/output"
OUTPUT_PARENT="$(cd "$PROJECT_ROOT/.." && pwd)"
mkdir -p "$OUTPUT_DIR"

mapfile -t DEB_FILES < <(find "$OUTPUT_PARENT" "$PROJECT_ROOT" -maxdepth 1 -type f \( -name "${APP_NAME}_${DEB_VERSION}_*.deb" -o -name "${APP_NAME}-dbgsym_${DEB_VERSION}_*.deb" -o -name "${APP_NAME}-dbgsym_${DEB_VERSION}_*.ddeb" \) -print)

if [ ${#DEB_FILES[@]} -eq 0 ]; then
    echo "Error: No .deb artifacts found in $OUTPUT_PARENT or $PROJECT_ROOT"
    echo "Expected pattern: ${APP_NAME}_${DEB_VERSION}_*.deb"
    exit 1
fi

for DEB_FILE in "${DEB_FILES[@]}"; do
    mv "$DEB_FILE" "$OUTPUT_DIR/"
done

echo ""
echo "================================"
echo "Build complete"
echo "================================"
echo "Package(s):"
for DEB_FILE in "$OUTPUT_DIR"/*.deb "$OUTPUT_DIR"/*.ddeb; do
    [ -e "$DEB_FILE" ] || continue
    echo "  $DEB_FILE"
done
echo ""
echo "To install:"
echo "  sudo dpkg -i $OUTPUT_DIR/${APP_NAME}_${DEB_VERSION}_*.deb"
