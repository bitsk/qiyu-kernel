# 维护补丁栈与更新主线

本分支直接建立在主线 Linux 7.3 提交 `c03c1a14507dfaf8b856c2608f9ac216021fc222` 之上，保留其全部祖先提交。其后的 A081 驱动按子系统拆成 13 个提交，配置/构建工具和文档单独提交。原有快照仓库与 Release 保留为备份。

本次只是重新组织 Git 历史。最终驱动、DTS、内核配置和构建脚本与此前发布版本逐文件相同，不新增硬件验收结论。每个中间提交没有单独进行实机验证，顺序用于后续维护和冲突定位。

## 查看改动

```sh
git log --oneline --reverse c03c1a14507dfaf8b856c2608f9ac216021fc222..main
git diff --stat c03c1a14507dfaf8b856c2608f9ac216021fc222..main
```

## 后续 rebase

先确认工作树干净，添加上游 remote（已有时跳过添加），取得主线并保留当前分支备份：

```sh
git remote add upstream https://github.com/torvalds/linux.git
git fetch upstream master --tags
git switch main
git branch backup/a081-before-rebase
git rebase --onto upstream/master c03c1a14507dfaf8b856c2608f9ac216021fc222 main
```

发生冲突时按子系统处理，核对 binding、DTS、驱动接口后继续 `git rebase --continue`；需要回退时 `git rebase --abort`。后续换用其他基底时，应将命令中的旧基底替换为当轮实际主线基底。

更新主线后，重新生成配置，更新构建脚本的内核版本断言，完成内核/模块/initramfs 构建、涉及的 DT schema 检查和启动镜像封装；实机验收前不晋升设备基线。仅更新 Git 历史或离线构建不能代替设备功能验收。

`patches/a081-v7.3.patch` 是初始 7.3 基底的累计补丁归档，今后维护以独立 Git 提交为准。`provenance/` 保存历史事实，其中的旧版本名称和摘要不随 Git 历史重排而改写。
