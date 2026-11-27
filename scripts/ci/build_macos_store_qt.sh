#!/usr/bin/env bash
# Build the universal SDK one module at a time to fit a hosted runner's disk.
set -euo pipefail
prefix=${1:?Pass the absolute SDK install prefix}
[[ "$prefix" = /* ]]
# Qt rejects source and build paths containing symlinks (on macOS /var is one).
mkdir -p "$prefix"
prefix=$(cd -P "$prefix" && pwd -P)
script_dir=$(cd -P "$(dirname "$0")" && pwd -P)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
flags=(-G Ninja -DCMAKE_BUILD_TYPE=Release '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
       "-DCMAKE_INSTALL_PREFIX=$prefix"
       "-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOS_DEPLOYMENT_TARGET:?}"
       '-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local'
       -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF)
while read -r sha module; do
    name="$module-everywhere-src-6.11.0"
    (
        cd -P "$work"
        curl --fail --location --retry 3 -o source.tar.xz \
            "https://download.qt.io/archive/qt/6.11/6.11.0/submodules/$name.tar.xz"
        echo "$sha  source.tar.xz" | shasum -a 256 -c -
        tar -xf source.tar.xz
        if [[ "$module" == qtbase ]]; then
            patch -d "$name" -p1 -F 0 < "$script_dir/patches/qtbase-6.11.0-arm-yield.patch"
            cmake -S "$name" -B build "${flags[@]}" \
                -DFEATURE_appstore_compliant=ON -DFEATURE_pkg_config=OFF \
                -DFEATURE_icu=OFF -DFEATURE_openssl=OFF
        else
            module_flags=("${flags[@]}")
            if [[ "$module" == qttools ]]; then
                module_flags+=(-DFEATURE_qdoc=OFF -DFEATURE_designer=OFF -DFEATURE_assistant=OFF)
            fi
            "$prefix/bin/qt-cmake" -S "$name" -B build "${module_flags[@]}"
        fi
        cmake --build build --parallel 3
        cmake --install build
        rm -rf build "$name" source.tar.xz
    )
done <<'MODULES'
231ad85979864d914dc9568a1b71c91d6cf20d7b2021d059103bf0eb51cb755e qtbase
597aa25e7c4d6f4f82c9d57e5d855d509b8ffe9ddae8a6442fb1f0bff1a8b34f qttasktree
e43cb1ae8809b2a858281ee269f98da59d0fc1bcf958ca5510c81f7ad3d2e14a qtshadertools
dfa8d653be07087d9407ed4a4ebae847f8953e0b7abd829f089803ab652a30e6 qtsvg
4eece569431ddf8324e7d322fa27001916570b23df535f8fb28aba445eedfde9 qtdeclarative
d3adb02ac5e2fe24068dbdaee0d7cc68cc3fa8553291c1bfce77c9fe8e940cc8 qtimageformats
cfb1993d7a10848965b01b9cf33a54b8a4ba4e5e3a6d28d59483e73f10d9fc76 qttools
MODULES
grep -Eq '^QT\.global\.enabled_features =.* appstore-compliant( |$)' "$prefix/mkspecs/qconfig.pri"
