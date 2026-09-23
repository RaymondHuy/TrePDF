#!/bin/bash
# Rebuild the pinned prebuilt dependency; normal TrePDF builds never run this.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || $(uname -s) != Darwin ]]; then
    echo "Usage: $0 <Qt macOS prefix> [output directory]" >&2
    exit 1
fi

trepdf_qt_prefix=$(cd "$1" && pwd)
trepdf_repo=$(cd "$(dirname "$0")/.." && pwd)
trepdf_output=${2:-"$trepdf_repo/third_party/qpdf"}
trepdf_cmake=${TREPDF_CMAKE_BIN:-cmake}
trepdf_ninja=${TREPDF_NINJA_BIN:-ninja}
trepdf_version=12.4.1
trepdf_source_sha=f045aa277be2356ff53a89a8622945958291177d2483afc20ede7c8a8cd3873c
trepdf_source_url="https://github.com/qpdf/qpdf/releases/download/v$trepdf_version/qpdf-$trepdf_version.tar.gz"
trepdf_work=$(mktemp -d "${TMPDIR:-/tmp}/trepdf-qpdf-build.XXXXXX")
trap 'rm -rf "$trepdf_work"' EXIT

trepdf_jpeg="$trepdf_qt_prefix/lib/libQt6BundledLibjpeg.a"
if [[ ! -f "$trepdf_jpeg" || ! -f "$trepdf_qt_prefix/include/QtJpeg/jpeglib.h" ]]; then
    echo "The selected Qt SDK must contain its bundled static JPEG library and headers." >&2
    exit 1
fi

trepdf_archive="$trepdf_work/source.tar.gz"
if [[ -n ${TREPDF_QPDF_ARCHIVE:-} ]]; then
    cp "$TREPDF_QPDF_ARCHIVE" "$trepdf_archive"
else
    curl --fail --location --proto '=https' --tlsv1.2 "$trepdf_source_url" -o "$trepdf_archive"
fi
trepdf_actual_sha=$(shasum -a 256 "$trepdf_archive" | awk '{print $1}')
if [[ "$trepdf_actual_sha" != "$trepdf_source_sha" ]]; then
    echo "qpdf source checksum mismatch; refusing to build." >&2
    exit 1
fi
tar -xzf "$trepdf_archive" -C "$trepdf_work"
trepdf_source="$trepdf_work/qpdf-$trepdf_version"
trepdf_sdk=$(xcrun --sdk macosx --show-sdk-path)
mkdir -p "$trepdf_work/empty-pkgconfig"

PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$trepdf_work/empty-pkgconfig" \
"$trepdf_cmake" -S "$trepdf_source" -B "$trepdf_work/build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$trepdf_ninja" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_INSTALL_NAME_DIR=@rpath \
    -DCMAKE_BUILD_WITH_INSTALL_NAME_DIR=ON \
    -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF \
    -DBUILD_DOC=OFF -DINSTALL_MANUAL=OFF -DINSTALL_EXAMPLES=OFF \
    -DENABLE_QTC=OFF -DUSE_IMPLICIT_CRYPTO=OFF \
    -DREQUIRE_CRYPTO_NATIVE=ON -DREQUIRE_CRYPTO_OPENSSL=OFF \
    -DREQUIRE_CRYPTO_GNUTLS=OFF -DDEFAULT_CRYPTO=native \
    -DUSE_INSECURE_RANDOM=OFF -DSKIP_OS_SECURE_RANDOM=OFF -DZOPFLI=OFF \
    -DLIBJPEG_LIB_PATH="$trepdf_jpeg" \
    -DLIBJPEG_H_PATH="$trepdf_qt_prefix/include/QtJpeg" \
    -DZLIB_LIB_PATH="$trepdf_sdk/usr/lib/libz.tbd" \
    -DZLIB_H_PATH="$trepdf_sdk/usr/include"
"$trepdf_cmake" --build "$trepdf_work/build" --target libqpdf --parallel 8

trepdf_package="$trepdf_work/package"
mkdir -p "$trepdf_package/lib" "$trepdf_package/include/qpdf"
cp -L "$trepdf_work/build/libqpdf/libqpdf.dylib" "$trepdf_package/lib/libqpdf.dylib"
install_name_tool -id @rpath/libqpdf.dylib "$trepdf_package/lib/libqpdf.dylib"
strip -x "$trepdf_package/lib/libqpdf.dylib"
codesign --force --sign - "$trepdf_package/lib/libqpdf.dylib"

# qpdf-c.h also includes public C++ interop headers when consumed by C++.
# Ship the upstream public headers unchanged, as with the PDFium package.
cp "$trepdf_source/include/qpdf/"*.h "$trepdf_source/include/qpdf/"*.hh \
    "$trepdf_package/include/qpdf/"
printf '%s\n' "$trepdf_version" > "$trepdf_package/VERSION"
for trepdf_notice in NOTICE.md LICENSE.txt Artistic-2.0; do
    if [[ -f "$trepdf_source/$trepdf_notice" ]]; then
        cp "$trepdf_source/$trepdf_notice" "$trepdf_package/"
    fi
done
(
    cd "$trepdf_package"
    shasum -a 256 lib/libqpdf.dylib include/qpdf/*.h include/qpdf/*.hh > SHA256SUMS
)
# No SDK, Homebrew or temporary build directory may be required at runtime.
trepdf_dependencies=$(otool -L "$trepdf_package/lib/libqpdf.dylib" | tail -n +2 | awk '{print $1}')
while IFS= read -r trepdf_dependency; do
    case "$trepdf_dependency" in
        @rpath/libqpdf.dylib|/usr/lib/*|/System/Library/*) ;;
        *) echo "Unexpected runtime dependency: $trepdf_dependency" >&2; exit 1 ;;
    esac
done <<< "$trepdf_dependencies"
mkdir -p "$trepdf_output"
cp -R "$trepdf_package/." "$trepdf_output/"
echo "Built qpdf $trepdf_version for macOS arm64 in $trepdf_output"
