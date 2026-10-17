<#
.SYNOPSIS
把上游（cloud.listenai.com 的 arcs_mini）同步成本仓 master 的一个快照提交。

.DESCRIPTION
机制见 docs/ai/06-upstream-sync.md。要点：
  - master 上每个 sync 提交 = 上游 master 的完整树 + docs/ai 覆盖层；
  - 自动剔除 GitHub 拒收的超大样例素材（>100MB）；
  - 无变化时不产生提交。

.EXAMPLE
.\tools\sync-upstream.ps1            # 拉取、重建、提交、推送
.\tools\sync-upstream.ps1 -DryRun    # 只看会改什么
#>
[CmdletBinding()]
param(
    [switch]$DryRun,
    [string]$Remote = 'upstream',
    [string]$Branch = 'master',
    [string]$PushRemote = 'origin'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# 上游的样例工程整个不入镜像：体积大、构建用不到，且含 205MB 的 algo_emmc.bin（GitHub 拒收）。
# 注意：镜像不带 arcs-sdk/samples/subsys，所以必须 CONFIG_MINIAPP=n（见下）。
$samplesDir = 'arcs-sdk/samples'
if (Test-Path -LiteralPath $samplesDir) {
    Remove-Item -LiteralPath $samplesDir -Recurse -Force
    Write-Host "[sync] 已剔除 arcs-sdk/samples/（含 subsys）"
}

# 镜像不带 samples/subsys，必须关掉小程序运行时，否则构建找不到 Lua 绑定源码
$prj = 'apps/arcs-mini/prj.conf'
if (Test-Path -LiteralPath $prj) {
    $lines = Get-Content -LiteralPath $prj
    if ($lines -match '^CONFIG_MINIAPP=') {
        $lines = $lines -replace '^CONFIG_MINIAPP=.*$', 'CONFIG_MINIAPP=n'
    } else {
        $lines += 'CONFIG_MINIAPP=n'
    }
    Set-Content -LiteralPath $prj -Value $lines -NoNewline:$false
    Write-Host "[sync] prj.conf: CONFIG_MINIAPP=n（镜像不含 samples/subsys）"
}
Write-Host "[sync] 恢复覆盖层：$($overlay -join ', ')"
& git checkout HEAD -- @overlay 2>$null
# 在 AGENTS.md 末尾追加指向 docs/ai 的说明（幂等；上游更新 AGENTS.md 不受影响）
$marker = '<!-- ai-docs:begin -->'
$agents = 'AGENTS.md'
if (Test-Path -LiteralPath $agents) {
    $text = Get-Content -LiteralPath $agents -Raw
    if ($text -notmatch [regex]::Escape($marker)) {
        $block = @'
$marker
> **本仓的 AI 开发文档**（快速开始 / 架构 / 烧录 / 踩坑 / 编码约定 / 上游同步）在 [`docs/ai/`](docs/ai/README.md)。
> 先读那里，再回到本文的项目规则；两边冲突时以本文件（上游规则）为准。
<!-- ai-docs:end -->
'@
        Add-Content -LiteralPath $agents -Value $block
        Write-Host "[sync] AGENTS.md 追加 docs/ai 指引"
    }
}

& git add -A

$changed = & git diff --cached --name-only
if (-not $changed) {
    Write-Host "[sync] 上游无变化（$short），退出"
    exit 0
}
Write-Host ("[sync] 变更 {0} 个文件" -f @($changed).Count)

$msg = "sync: upstream $Branch @$short ($(Get-Date -Format 'yyyy-MM-dd'))"
if ($DryRun) {
    Write-Host "[sync] dry-run：将提交 `"$msg`"，并推送；未执行"
    & git diff --cached --stat | Select-Object -Last 5
    & git reset -q
    exit 0
}

& git commit -q -m $msg
if ($LASTEXITCODE -ne 0) { throw "git commit 失败" }
Write-Host "[sync] 推送 $PushRemote $Branch ..."
& git push $PushRemote $Branch
if ($LASTEXITCODE -ne 0) { throw "git push 失败" }
Write-Host "[sync] 完成：$msg"
