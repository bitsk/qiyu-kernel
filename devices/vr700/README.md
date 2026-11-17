# VR700 / Qiyu DREAM Pro

The BID-15 board is identified independently as `iqiyi,vr700`. The initial
configuration enables SM8250 CPU/GPU, dual DSI display, UFS, USB2 peripheral
console/network, keys and PWM fan. Electrical parameters come from the selected
OEM runtime FDT. The display commands and supply sequence match the A081
assembly; the compatible preserves the distinct board identity.

This is a bring-up candidate, not a hardware acceptance claim. QCA6390 radio,
DSP sensors, cameras, controller transport and audio are not enabled by this
board description. Do not install A081 calibration or its WCN6851 firmware.

Build `devices/vr700/build.sh NEW_OUTPUT_DIRECTORY` from a committed source.
Output stays outside the source checkout. The boot image, root filesystem and
firmware are assembled separately by xr-kernel and openKylin XR integration.
Building or installing packages does not flash a boot partition.
