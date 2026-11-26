---
name: dafeiyu-worktree
description: 大肥鱼项目在 ui、plugins、core 固定工作树中分工开发时使用。覆盖建立或复用工作树与 Session、按提交号交付、在主目录 master 保留历史合并，以及下一阶段同步主线。
---

# 大肥鱼多工作树开发

## 工作树与 Session

采用多工作树分工时，下表的四个目录同属一个本地 Git 仓库。以当前仓库 `git worktree list --porcelain` 的登记位置核对各目录和分支。每个开发 Session 固定使用自己的工作树，主工作树的 Session 负责集成。

| 工作区根目录下的目录 | 固定分支 | 职责 |
| --- | --- | --- |
| `DaFeiYu-Master` | `master` | 协调跨线接口、审查交付范围、合并与集成验收 |
| `DaFeiYu-UI` | `ui` | 管理中心、WebView2、原生窗口交互 |
| `DaFeiYu-Plugins` | `plugins` | MC 等领域插件、桥接、场景状态和业务回执 |
| `DaFeiYu-Core` | `core` | 本体、Agent、模型、SDK，以及 `modules/plugin_host` 和 `modules/plugin_tools` 等通用宿主能力 |

接手任务时，用 `git worktree list --porcelain` 核对目录与分支，再在目标目录用 `git status --short --branch` 核对现场。首次建树以用户认可的同一个本地 `master` 完整提交号为起点。已有匹配的工作树直接复用，已有分支保留其提交。若目录或分支已被占用，先查明归属。用户要求开 Session 时，绑定对应目录，交代分支、基线、本轮任务和停止条件。

## 开发与交付

1. 在本分支记下开工时的 `git rev-parse HEAD`。按当轮范围完成一个可独立验收的功能点，定向验证后独立提交。中文提交说明、离线测试和阶段审查遵循[仓库约定](../../../AGENTS.md)。
2. 跨线修改按事实与接口的拥有方分工。例如由本体维护 SDK 或管理 IPC，由使用方接入。需要新接口时，先约定输入、结果和兼容范围，由负责方交付可独立验收的接口提交，再由依赖方同步已集成的基线。[当前架构](../../../文档/设计/项目架构设计.md)
3. 使用本工作树的构建目录。并行启动面板或后台时，显式指定本工作树的 `--data`，使窗口连接预期的后台实例。[构建预设](../../../CMakePresets.json) · [桌面运行说明](../../../文档/使用说明/桌面管理中心说明.md)

交接消息写明 **分支、起点完整 SHA、交付完整 SHA、行为变化与阅读入口、前置提交、实际验证、未覆盖项和下一步**。同一分支继续开发时，先前交付的 SHA 仍指向原来的提交。

## 主目录按提交号合并

1. 在 `DaFeiYu-Master` 确认当前分支为 `master`，记录合并前的 `HEAD`。以干净的暂存区和已跟踪文件开始新的合并。若已有合并，核对发起任务与 `MERGE_HEAD`。属于本任务且与交付 SHA 一致时，直接继续解决冲突或验收暂存结果。其他合并保留现场。仅做审查时，到范围与验证结果的核对为止。
2. 将交接中的来源分支、起点和交付号分别记为 `$sourceBranch`、`$baseSha` 和 `$deliverySha`。集成既有交付时使用原 SHA。若用户明确指定某分支的当前进度，先将其尖端解析为完整 SHA。缺少阶段起点时，可用 `git merge-base HEAD $deliverySha` 作为本次审查的对比基线。以下两项祖先检查的退出码都应为 0。另用 `git merge-base --is-ancestor $deliverySha HEAD` 判断是否已经集成，已包含时报告现有结果。

   ```powershell
   git merge-base --is-ancestor $baseSha $deliverySha
   git merge-base --is-ancestor $deliverySha "refs/heads/$sourceBranch"
   ```

3. 用 `git log --reverse --oneline "HEAD..$deliverySha"` 审查将进入主线的全部提交，用 `git diff "HEAD...$deliverySha"` 查看交付侧的改动。核对前置提交和授权范围。若包含尚待审查的祖先，先处理该依赖，或按本轮已获准的较早交付号集成。
4. 依当轮集成授权执行 `git merge --no-ff --no-commit $deliverySha`。如有冲突，按两侧意图处理并暂存解决结果。用 `git diff --cached` 核对实际合并内容，运行受影响的定向验收。若验收失败且需要退回开发线修正，核实 `MERGE_HEAD` 仍是本次交付号，再用 `git merge --abort` 撤回本次待提交合并。记录失败证据，按修正后的新 SHA 重新验收。
5. 验收通过并核实 `MERGE_HEAD` 为交付 SHA 后，用中文说明合并内容和验证结果并提交。用 `git rev-list --parents -n 1 HEAD` 核对第一父提交为合并前的主线，第二父提交为交付 SHA。向用户报告合并 SHA 和验证结果，供开发 Session 下一轮同步。产品最终验收按仓库约定运行全量离线测试。

## 下一阶段同步

保留原分支、工作树与 Session。在阶段交接处将本地 `master` 的完整提交号记为 `$masterSha`。主线已包含开发分支尖端时，在开发工作树执行 `git merge --ff-only $masterSha`。开发分支已包含该主线时直接继续。双方都有独立提交时，在开发工作树执行 `git merge --no-ff --no-commit $masterSha`，核对结果、定向验证并用中文提交。这样其他 Session 手里的交付号在同步后仍可核对。
