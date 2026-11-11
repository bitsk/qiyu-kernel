#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
[[ $# -ge 1 && $# -le 2 ]] || {
    echo "Usage: $0 NEW_OUTPUT_DIRECTORY [default|xr-scheduling|usb-pd-5v]" >&2; exit 1;
}
profile=${2:-default}
case "$profile" in
    default) expected_release=7.3.0-qiyu-mix ;;
    xr-scheduling) expected_release=7.3.0-qiyu-mix-scheduling ;;
    usb-pd-5v) expected_release=7.3.0-qiyu-mix-usb-pd5v ;;
    *) echo 'Unknown A081 build profile' >&2; exit 1 ;;
esac
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
fragments=()
if [[ "$profile" == xr-scheduling ]]; then
    fragments=("$root/devices/a081/config/xr-scheduling.config")
elif [[ "$profile" == usb-pd-5v ]]; then
    fragments=("$root/devices/a081/config/xr-cameras.config"
               "$root/devices/a081/config/usb-pd-5v.config")
fi
if [[ ${#fragments[@]} -gt 0 ]]; then
    "$root/scripts/kconfig/merge_config.sh" -m -O "$out/build" \
        "$root/devices/a081/config/kernel.config" \
        "${fragments[@]}"
fi
make -C "$root" O="$out/build" olddefconfig
if [[ "$profile" == default ]]; then
    cmp "$root/devices/a081/config/kernel.config" "$out/build/.config"
else
    # Reject requested settings dropped by Kconfig dependencies.
    while IFS= read -r setting; do
        if [[ "$setting" == CONFIG_*=* || "$setting" == '# CONFIG_'*' is not set' ]]; then
            grep -Fxq "$setting" "$out/build/.config" || {
                echo "Unresolved profile setting: $setting" >&2; exit 1;
            }
        fi
    done < <(cat "${fragments[@]}")
fi
make -C "$root" O="$out/build" -j"${A081_BUILD_JOBS:-12}" \
    Image modules qcom/sm8250-iqiyi-a081.dtb
release=$(cat "$out/build/include/config/kernel.release")
[[ "$release" == "$expected_release" ]]
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
