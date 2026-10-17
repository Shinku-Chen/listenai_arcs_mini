# 06 与上游同步（镜像机制）

## 1. 这是什么仓

| 项 | 值 |
|---|---|
| 上游 | `https://cloud.listenai.com/CSKG836746/arcs-sdk/public/arcs_mini`（公开，匿名 HTTPS 可读） |
| 本仓 | `https://github.com/Shinku-Chen/listenai_arcs_mini` —— 上游的 GitHub 镜像 + 本项目 AI 文档 |
| 上游版本 | `master` @ `23fca91e`（= tag `mini-v3.0.3`） |
| 默认分支 | `master`（唯一分支） |

**为什么需要镜像**：上游在 `cloud.listenai.com`，GitHub 侧（AI 工具、搜索、issue、Actions）
访问不到；镜像让代码可被检索、可被 AI 读、可在 PR 里讨论。

## 2. 快照式同步（不是逐提交镜像）

`master` 上每个 `sync:` 提交 = **上游 master 当时的完整快照** + 本仓的文档覆盖层。

```text
sync: upstream master @23fca91e (2026-10-18)     ← tree = 上游内容 + docs/ai 覆盖层
sync: upstream master @<sha> (<date>)
...
```

好处：GitHub 上直接 diff 两次 sync 就能回答"上游这次改了什么"；不需要 force-push；
不受上游历史体积影响。

**代价**：没有上游的逐提交历史与 blame。要看历史请去上游仓。

## 3. 哪些内容不入镜像

**`arcs-sdk/samples/` 整个目录不入镜像**（约 188 MB 示例工程 + 4 个超大素材），
其中 `samples/subsys` 也一并去掉——**本项目做的是自定义固件，`CONFIG_MINIAPP=n`**，
用不到小程序的 Lua 绑定源码。

| 被剔除的路径 | 大小 | 原因 |
|---|---|---|
| `arcs-sdk/samples/algorithms/` | 32 MB（含 3×205 MB `algo_emmc.bin`，git 去重为 1 份） | 算法样例的 eMMC 镜像，超过 GitHub 单文件 100 MB 硬上限 |
| `arcs-sdk/samples/libraries/` | 79 MB（含 90 MB `fatfs_db.bin`） | sqlite3 等库样例的磁盘镜像 |
| `arcs-sdk/samples/{bluetooth,network,media,drivers,security}/` | 73 MB | 各外设/协议示例，构建 `apps/arcs-mini` 不需要 |
| `arcs-sdk/samples/subsys/` | 5.2 MB | 小程序 Lua 绑定源码；本项目 `CONFIG_MINIAPP=n`，不需要 |

### 同步时自动改的配置

镜像**不带 `samples/subsys`**，而上游 `apps/arcs-mini/prj.conf` 默认 `CONFIG_MINIAPP=y`
（`apps/arcs-mini/miniapp/CMakeLists.txt` 引用 `samples/subsys/lua/lua_binding_lvgl/...`）。
所以同步脚本会把 `apps/arcs-mini/prj.conf` 里的 **`CONFIG_MINIAPP` 改成 `n`**，
保证镜像开箱即可编译自定义固件。

需要小程序能力时：把 `samples/subsys` 从上游取回来，再把 `CONFIG_MINIAPP=y` 打开。

需要这些示例时**直接去上游仓取**：
`https://cloud.listenai.com/CSKG836746/arcs-sdk/public/arcs_mini` → `arcs-sdk/samples/`。

## 4. 文档覆盖层（不会被同步覆盖）

同步只会替换上游内容，**以下文件由本仓维护，同步时保留**：

```text
docs/ai/**                        AI 开发文档（本目录）
tools/sync-upstream.ps1 / .sh     同步脚本
.github/workflows/sync-upstream.yml  定时同步
```

上游文件（`AGENTS.md`、`README.MD`、`docs/miniapp.md`…）保持原样，跟随上游更新。

## 5. 怎么同步

### 手动（本机）

```powershell
.\tools\sync-upstream.ps1              # 拉上游 → 重建快照 → 提交 → 推送
.\tools\sync-upstream.ps1 -DryRun      # 只看会改什么，不提交
```

```bash
./tools/sync-upstream.sh
```

脚本行为：
1. `git fetch upstream master`；
2. 用上游树重置工作区（tracked 文件），剔除 §3 的超大文件；
3. 从上一个 `master` 恢复 `docs/ai/` 与同步脚本（覆盖层）；
4. 有变化就提交 `sync: upstream master @<short-sha> (<date>)` 并 `git push origin master`；
   无变化直接退出。

### 自动（GitHub Actions）

`.github/workflows/sync-upstream.yml`：每周一 04:00 UTC 跑一次 + 支持手动 `Run workflow`。
它做同样的事，并用仓库自带的 `GITHUB_TOKEN` 推送。

## 6. 冲突与回滚

- 同步**只改上游内容**，理论上不会冲突；若上游删除了某个我们也在改的文件，脚本会报错退出，
  需要人工看一眼。
- 回滚：`git revert <sync 提交>`，或 `git reset --hard <上一个 sync>`。
- 想换成"完整历史镜像"（保留上游逐提交历史）也可以，但每次同步必须
  `filter-repo --strip-blobs-bigger-than 100M` 后 force-push，并且 `docs/ai` 覆盖层要靠
  rebase 重建——维护成本高，默认不做。
