# VR700 / Qiyu DREAM Pro

The BID-15 board is identified independently as `iqiyi,vr700`. The initial
configuration enables SM8250 CPU/GPU, dual DSI display, UFS, USB2 peripheral
console/network, keys and PWM fan. Electrical parameters come from the selected
OEM runtime FDT. The display commands and supply sequence match the A081
assembly; the compatible preserves the distinct board identity.

The default device tree includes the tracking interfaces and the onboard
AW88261 speaker pair. Speaker playback uses the VR700 ADSP firmware and
the OEM amplifier profile. Import firmware and calibration for this model.

Build `devices/vr700/build.sh NEW_OUTPUT_DIRECTORY` from a committed source.
Output stays outside the source checkout. The boot image, root filesystem and
firmware are assembled separately by xr-kernel and openKylin XR integration.
Building or installing packages does not flash a boot partition.
