#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
APP_NAME="lcs"
APP_SUMMARY="Lightweight cluster service for quorum-managed VIPs"
MAINTAINER="Petr Bena <petr@bena.rocks>"
RELEASE="1"
OUTPUT_DIR="$PROJECT_ROOT/packaging/output"

APP_VERSION="$(sed -n 's/^#define LCS_VERSION "\([^"]*\)"/\1/p' "$PROJECT_ROOT/src/common.h")"
NCPUS="$(nproc 2>/dev/null || echo 1)"

usage() {
    echo "Usage: $0 [--version x.y.z]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            if [ $# -lt 2 ]; then
                echo "Error: --version requires a value"
                usage
                exit 1
            fi
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

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "Error: rpmbuild not found in PATH"
    echo "Install the rpm-build package and re-run."
    exit 1
fi

if command -v rpm >/dev/null 2>&1; then
    missing=()

    require_pkg() {
        local pkg="$1"
        if ! rpm -q "$pkg" >/dev/null 2>&1; then
            missing+=("$pkg")
        fi
    }

    require_pkg rpm-build
    require_pkg gcc
    require_pkg make
    require_pkg tar
    require_pkg gzip

    if [ ${#missing[@]} -gt 0 ]; then
        echo "Missing build dependencies detected."
        echo ""
        if command -v dnf >/dev/null 2>&1; then
            echo "  sudo dnf install ${missing[*]}"
        elif command -v yum >/dev/null 2>&1; then
            echo "  sudo yum install ${missing[*]}"
        else
            for pkg in "${missing[@]}"; do
                echo "  - $pkg"
            done
        fi
        echo ""
        exit 1
    fi
fi

RPM_DIST="$(rpm --eval '%{?dist}' 2>/dev/null || true)"
DISPLAY_RELEASE="${RELEASE}${RPM_DIST}"

echo "================================"
echo "Building lcs RPM package"
echo "================================"
echo "CPUs: $NCPUS"
echo "Version: $APP_VERSION-$DISPLAY_RELEASE"
echo ""

RPM_TOPDIR="$(mktemp -d)"
trap 'rm -rf "$RPM_TOPDIR"' EXIT
mkdir -p "$RPM_TOPDIR"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

SOURCE_DIR_NAME="${APP_NAME}-${APP_VERSION}"
SOURCE_STAGE="$RPM_TOPDIR/SOURCES/$SOURCE_DIR_NAME"
SOURCE_TARBALL="$RPM_TOPDIR/SOURCES/${SOURCE_DIR_NAME}.tar.gz"
SPEC_PATH="$RPM_TOPDIR/SPECS/${APP_NAME}.spec"

echo "Step 1: Preparing source archive..."
mkdir -p "$SOURCE_STAGE"
tar \
    --exclude-vcs \
    --exclude='./packaging/output' \
    --exclude='./debian/.debhelper' \
    --exclude='./debian/lcs' \
    --exclude='./debian/*.debhelper.log' \
    --exclude='./debian/*.substvars' \
    --exclude='./debian/files' \
    --exclude='./debian/debhelper-build-stamp' \
    -C "$PROJECT_ROOT" -cf - . | tar -C "$SOURCE_STAGE" -xf -

make -C "$SOURCE_STAGE" clean >/dev/null 2>&1 || true
tar -C "$RPM_TOPDIR/SOURCES" -czf "$SOURCE_TARBALL" "$SOURCE_DIR_NAME"
rm -rf "$SOURCE_STAGE"

echo ""
echo "Step 2: Creating RPM spec file..."
cat > "$SPEC_PATH" <<EOF_SPEC
%{!?_unitdir:%global _unitdir /usr/lib/systemd/system}

Name:           $APP_NAME
Version:        $APP_VERSION
Release:        $RELEASE%{?dist}
Summary:        $APP_SUMMARY

License:        GPL-3.0-or-later
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc
BuildRequires:  make
Requires:       iproute

%description
lcs is a small Linux cluster service for quorum voting and safe virtual IP
address ownership.

This package provides the lcs CLI, the lcsd daemon, a systemd unit, and the
example configurations shipped by the Debian package.

%prep
%setup -q

%build
make -j$NCPUS

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot} bindir=%{_bindir} sbindir=%{_sbindir}
install -Dm644 packaging/lcsd.service %{buildroot}%{_unitdir}/lcsd.service
install -Dm644 README.md %{buildroot}%{_docdir}/%{name}/README.md
install -Dm644 LICENSE %{buildroot}%{_docdir}/%{name}/LICENSE
mkdir -p %{buildroot}%{_docdir}/%{name}/examples
cp -a examples/. %{buildroot}%{_docdir}/%{name}/examples/

%post
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    if [ \$1 -gt 1 ] && systemctl --quiet is-active lcsd.service; then
        systemctl try-restart lcsd.service >/dev/null 2>&1 || true
    fi
fi

%preun
if [ \$1 -eq 0 ] && command -v systemctl >/dev/null 2>&1; then
    systemctl --no-reload disable --now lcsd.service >/dev/null 2>&1 || true
fi

%postun
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

%files
%license %{_docdir}/%{name}/LICENSE
%doc %{_docdir}/%{name}/README.md
%{_docdir}/%{name}/examples
%{_bindir}/lcs
%{_sbindir}/lcsd
%{_unitdir}/lcsd.service

%changelog
* $(date '+%a %b %d %Y') $MAINTAINER - $APP_VERSION-$RELEASE
- Automated build.
EOF_SPEC

echo ""
echo "Step 3: Building RPM(s)..."
rpmbuild --define "_topdir $RPM_TOPDIR" -ba "$SPEC_PATH"

echo ""
echo "Step 4: Collecting RPM output..."
mkdir -p "$OUTPUT_DIR"

mapfile -t RPM_FILES < <(find "$RPM_TOPDIR/RPMS" "$RPM_TOPDIR/SRPMS" -type f \( -name "${APP_NAME}-${APP_VERSION}-${RELEASE}*.rpm" -o -name "${APP_NAME}-*-${APP_VERSION}-${RELEASE}*.rpm" \) -print)

if [ ${#RPM_FILES[@]} -eq 0 ]; then
    echo "Error: No .rpm artifacts found in $RPM_TOPDIR"
    exit 1
fi

for RPM_FILE in "${RPM_FILES[@]}"; do
    cp "$RPM_FILE" "$OUTPUT_DIR/"
done

mapfile -t PRIMARY_RPMS < <(find "$OUTPUT_DIR" -maxdepth 1 -type f -name "${APP_NAME}-${APP_VERSION}-${RELEASE}*.rpm" ! -name "*.src.rpm" -print)

echo ""
echo "================================"
echo "Build complete"
echo "================================"
echo "Package(s):"
for RPM_FILE in "$OUTPUT_DIR"/*.rpm; do
    [ -e "$RPM_FILE" ] || continue
    echo "  $RPM_FILE"
done
echo ""
if [ ${#PRIMARY_RPMS[@]} -gt 0 ]; then
    echo "To install:"
    for RPM_FILE in "${PRIMARY_RPMS[@]}"; do
        echo "  sudo dnf install $RPM_FILE"
    done
fi