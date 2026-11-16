#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
[[ $# == 1 ]] || { echo "Usage: $0 NEW_OUTPUT_DIRECTORY" >&2; exit 1; }
out=$(realpath -m "$1")
[[ "$out" != "$root" && "$out" != "$root/"* && ! -e "$out" ]] || {
    echo 'Choose a new directory outside the source checkout' >&2; exit 1;
}
git -C "$root" diff --quiet && git -C "$root" diff --cached --quiet || {
    echo 'Commit kernel changes before building' >&2; exit 1;
}
mkdir -p "$out/build" "$out/artifacts"
export ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-} LOCALVERSION=
export SOURCE_DATE_EPOCH=0 KBUILD_BUILD_USER=xr KBUILD_BUILD_HOST=build
export KBUILD_BUILD_TIMESTAMP='1970-01-01 00:00:00 +0000' KBUILD_BUILD_VERSION=1
export KCFLAGS="${KCFLAGS:-} -ffile-prefix-map=$root=/usr/src/linux -fdebug-prefix-map=$root=/usr/src/linux"
cp "$root/devices/vr700/config/kernel.config" "$out/build/.config"
make -C "$root" O="$out/build" olddefconfig
make -C "$root" O="$out/build" -j"${JOBS:-12}" Image modules qcom/sm8250-iqiyi-vr700.dtb
release=$(cat "$out/build/include/config/kernel.release")
[[ "$release" == 7.3.0-xr-vr700-tracking ]]
make -C "$root" O="$out/build" INSTALL_MOD_PATH="$out/modules" INSTALL_MOD_STRIP=1 modules_install
rm -f "$out/modules/lib/modules/$release/build" "$out/modules/lib/modules/$release/source"
tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    -C "$out/modules" -czf "$out/artifacts/modules.tar.gz" "lib/modules/$release"
cp "$out/build/arch/arm64/boot/Image" "$out/artifacts/"
cp "$out/build/arch/arm64/boot/dts/qcom/sm8250-iqiyi-vr700.dtb" "$out/artifacts/source.dtb"
cp "$out/build/.config" "$out/artifacts/kernel.config"
git -C "$root" rev-parse HEAD > "$out/artifacts/source-commit"
(cd "$out/artifacts" && sha256sum Image source.dtb kernel.config modules.tar.gz source-commit > SHA256SUMS)
