# qiyu-kernel

奇遇设备的 Linux 内核适配。仓库保留完整内核源码，各型号的配置、构建入口和验证记录放在 `devices/<model>/`，后续型号沿用这一结构。

| 型号 | 硬件 | 内核版本 | 文档与构建入口 |
| --- | --- | --- | --- |
| 奇遇 MIX / A081 | Qualcomm SM8250 | `7.3.0-qiyu-mix` | [devices/a081](devices/a081/) |

## A081 构建

```sh
./devices/a081/build.sh /absolute/path/to/new-output
```

构建需要 arm64 GNU 交叉工具链及内核常用构建依赖，详见 [A081 文档](devices/a081/README.md)。完整 Git 历史来自 `torvalds/linux`，以真实提交 `c03c1a14507dfaf8b856c2608f9ac216021fc222` 为基底；A081 驱动按子系统提交在其后，便于以后 rebase。详见 [维护与 rebase](devices/a081/MAINTAINING.md)。

当前发布以已在 Debian / SteamOS 上验证的 A081 驱动源码为基础，版本后缀统一为 `-qiyu-mix`，保留 PREEMPT 配置。版本名修改后的内核、模块和启动镜像经过离线重建，尚未上机；原实机基线与摘要保留在型号目录的 `provenance/` 中。

Linux 及源文件遵循原有 [COPYING](COPYING)、`LICENSES/` 和 SPDX 声明。新增构建脚本遵循 GPL-2.0-only。启动镜像内的 BusyBox 和固件沿用各自许可证。原始内核文档见 [Documentation](Documentation/)。
