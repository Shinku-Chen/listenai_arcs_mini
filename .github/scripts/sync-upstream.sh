#!/usr/bin/env bash
# 把上游（cloud.listenai.com 的 arcs_mini）同步成本仓 master 的一个快照提交。
# 机制见 ling 的 docs/firmware/upstream-mirror.md；Windows 用 tools/sync-upstream.ps1。
#
#   ./tools/sync-upstream.sh            # 拉取、重建、提交、推送
#   ./tools/sync-upstream.sh --dry-run  # 只看会改什么
set -euo pipefail

REMOTE="${REMOTE:-upstream}"
BRANCH="${BRANCH:-master}"
PUSH_REMOTE="${PUSH_REMOTE:-origin}"
DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

cd "$(dirname "$0")/../.."   # 脚本在 <repo>/.github/scripts/

# 上游样例工程整个不入镜像：体积大、构建用不到，且含 >100MB 素材（GitHub 拒收）。
# 镜像不带 samples/subsys，所以下面会把 CONFIG_MINIAPP 关掉。
# 本仓维护的覆盖层
OVERLAY=(.github/scripts/sync-upstream.ps1 .github/scripts/sync-upstream.sh .github/ai-docs-pointer.md .github/workflows/sync-upstream.yml)

echo "[sync] fetch $REMOTE/$BRANCH ..."
git fetch --depth=1 "$REMOTE" "$BRANCH"
sha=$(git rev-parse FETCH_HEAD)
short=${sha:0:8}

git rev-parse --verify HEAD >/dev/null 2>&1 || { echo "当前分支还没有提交"; exit 1; }

echo "[sync] 上游 = $short，展开其树到工作区"
git read-tree -u --reset FETCH_HEAD

if [ -d arcs-sdk/samples ]; then
  rm -rf arcs-sdk/samples
  echo "[sync] 已剔除 arcs-sdk/samples/"
fi

# 镜像不带 samples/subsys，必须关掉小程序运行时（否则构建找不到 Lua 绑定源码）
PRJ=apps/arcs-mini/prj.conf
if [ -f "$PRJ" ]; then
  if grep -q '^CONFIG_MINIAPP=' "$PRJ"; then
    sed -i 's/^CONFIG_MINIAPP=.*$/CONFIG_MINIAPP=n/' "$PRJ"
  else
    printf 'CONFIG_MINIAPP=n
' >> "$PRJ"
  fi
  echo "[sync] prj.conf: CONFIG_MINIAPP=n"
fi
echo "[sync] 恢复覆盖层：${OVERLAY[*]}"
git checkout HEAD -- "${OVERLAY[@]}" 2>/dev/null || true
# 在 AGENTS.md 末尾追加"去文档仓读文档"的指针
if [ -f AGENTS.md ] && [ -f .github/ai-docs-pointer.md ]; then
  printf '
' >> AGENTS.md
  cat .github/ai-docs-pointer.md >> AGENTS.md
  echo "[sync] AGENTS.md 追加 ling 文档指针"
fi

git add -A

if git diff --cached --quiet; then
  echo "[sync] 上游无变化（$short），退出"
  exit 0
fi

msg="sync: upstream $BRANCH @$short ($(date +%Y-%m-%d))"
if [ "$DRY_RUN" = "1" ]; then
  echo "[sync] dry-run：将提交 \"$msg\" 并推送；未执行"
  git diff --cached --stat | tail -5
  git reset -q
  exit 0
fi

git commit -q -m "$msg"
echo "[sync] 推送 $PUSH_REMOTE $BRANCH ..."
git push "$PUSH_REMOTE" "$BRANCH"
echo "[sync] 完成：$msg"
