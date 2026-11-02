#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
[[ $# -eq 1 ]] || { echo "Usage: $0 NEW_OUTPUT_DIRECTORY" >&2; exit 1; }
out=$(realpath -m "$1")
[[ "$out" != "$root" && "$out" != "$root/"* ]] || {
    echo 'Choose an output directory outside the source tree' >&2; exit 1;
}
mkdir "$out"
export ARCH=arm64 CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
export LOCALVERSION=
export SOURCE_DATE_EPOCH=0
export KCFLAGS="${KCFLAGS:-} -ffile-prefix-map=$root=/usr/src/linux -fdebug-prefix-map=$root=/usr/src/linux"
export KBUILD_BUILD_USER=qiyu KBUILD_BUILD_HOST=build
export KBUILD_BUILD_TIMESTAMP='1970-01-01 00:00:00 +0000' KBUILD_BUILD_VERSION=1
mkdir "$out/build" "$out/artifacts"
cp "$root/devices/a081/config/kernel.config" "$out/build/.config"
make -C "$root" O="$out/build" olddefconfig
cmp "$root/devices/a081/config/kernel.config" "$out/build/.config"
make -C "$root" O="$out/build" -j"${A081_BUILD_JOBS:-12}" \
    Image modules qcom/sm8250-iqiyi-a081.dtb
release=$(cat "$out/build/include/config/kernel.release")
[[ "$release" == 7.3.0-qiyu-mix ]]
make -C "$root" O="$out/build" INSTALL_MOD_PATH="$out/modules" \
    INSTALL_MOD_STRIP=1 modules_install
rm -f "$out/modules/lib/modules/$release/build" "$out/modules/lib/modules/$release/source"
tar --sort=name --mtime='@0' --owner=0 --group=0 \
    --numeric-owner --mode=go-w -C "$out/modules" -czf "$out/artifacts/modules.tar.gz" \
    "lib/modules/$release"
cp "$out/build/arch/arm64/boot/Image" "$out/artifacts/Image"
cp "$out/build/arch/arm64/boot/dts/qcom/sm8250-iqiyi-a081.dtb" "$out/artifacts/source.dtb"
cp "$out/build/.config" "$out/artifacts/kernel.config"
(cd "$out/artifacts" && sha256sum Image source.dtb kernel.config modules.tar.gz > SHA256SUMS)
echo "Build complete: $out/artifacts"
