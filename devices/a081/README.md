# 奇遇 MIX / A081

Qualcomm SM8250 板级适配，内核版本 `7.3.0-qiyu-mix`。驱动源码冻结于 r1，来源为 r1 已验收基线；r1 重启记录仍使用该原始基线。本次仅修改 CONFIG_LOCALVERSION，保留 PREEMPT。新版本名对应的内核与模块经过离线构建，尚未进行实机验证，也未晋升设备基线。

| 项目 | 路径 |
| --- | --- |
| 内核配置 | [config/kernel.config](config/kernel.config) |
| 累计补丁 | [patches/a081-v7.3.patch](patches/a081-v7.3.patch)，已应用 |
| 发布清单 | [manifest.json](manifest.json) |
| 原始构建与实机证据 | [provenance](provenance/) |
| 温控工具 | [thermal](thermal/) |

## 构建

Debian/Ubuntu 主机需 GNU make、GCC arm64 交叉工具链、binutils、bc、bison、flex、libssl-dev、libelf-dev、Python 3、rsync、cpio、device-tree-compiler。归档构建使用 GCC 15.2.0 / GNU ld 2.46。

在仓库根目录运行：

```sh
./devices/a081/build.sh /absolute/path/to/new-output
```

脚本仅接受源码树之外的新目录，生成 Image、源 DTB、配置与模块包，不访问设备。模块安装路径为 `lib/modules/7.3.0-qiyu-mix`，必须与相同版本的内核配套。设备树检查使用内核标准 dt_binding_check / dtbs_check，需 dtschema；原始通过日志保存在 provenance。版本后缀修改不涉及驱动、DTS 或 binding。

## 适配与验收范围

包含板级 DTS/binding、屏幕和 DRM、QCOM SEE 传感器、CAMSS 与 QCFT 元数据、OV7251 曝光/相位控制、CCI 批量写入与诊断、Nordic 72 字节控制器传输/复位、Venus 视频编解码、系统调用与 FUSE、原厂/性能两档温控等适配。

原厂档默认大核 75°C、GPU 95°C；性能档大核 85°C、GPU 95°C，使用 CPU/GPU performance governor。critical 保护保留，NTC 用于采温。设备保存的配置可覆盖默认档位。

原实机证据覆盖 Debian / SteamOS 启动、温控切换/持久化、Android B/root 返回与 recovery 完整读回。控制器间歇失效、物理标定、佩戴体验、长期稳定、高温阈值跨越和持续 72 FPS 尚未完整验收。完整根文件系统和用户态 VR 运行时需另行准备。

原实机 recovery SHA-256（历史追溯，不是本次更名后镜像的摘要）：

```text
70df861523a19b65879b0ca85c894143089f35b69bf2e38cff3f3e7df33e8fa2
```

## Release 附件

提供新版本的 Image、modules.tar.gz、kernel.config、source.dtb、boot.dtb、recovery-a081.bin、原始 initramfs、完整源码包、manifest.json 和 SHA256SUMS。启动镜像使用原始 initramfs/启动 DTB/recovery DTBO，仅替换新版本内核。镜像用于已有 osstore Debian / SteamOS 双根安装，默认 Debian，不是系统安装器。

```sh
sha256sum -c SHA256SUMS
```

源码 DTB 与启动 DTB 分别保存；后者含既有启动适配，不能混用。内核与全部模块需一同更新。上机前确认 A081 身份、既有 osstore 安装和可用回退；启动分区仅允许 recovery_b，完整读回一致后才启动。boot A/B、ABL、DTBO、vbmeta、GPT 和 userdata 不在写入范围。仓库不提供自动刷写，本次发布未操作设备。
