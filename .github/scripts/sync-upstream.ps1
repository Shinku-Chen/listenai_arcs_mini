<#
.SYNOPSIS
把上游（cloud.listenai.com 的 arcs_mini）同步成本仓 master 的一个快照提交。

.DESCRIPTION
机制见 ling 的 docs/firmware/upstream-mirror.md。要点：
  - master 上每个 sync 提交 = 上游 master 的完整树 + 一条指向 ling 文档的指针；
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
# 本脚本位于 <repo>/.github/scripts/，仓库根是上两级
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

# 上游的样例工程整个不入镜像：体积大、构建用不到，且含 205MB 的 algo_emmc.bin（GitHub 拒收）。
# 注意：镜像不带 arcs-sdk/samples/subsys，所以必须 CONFIG_MINIAPP=n（见下）。
$samplesDir = 'arcs-sdk/samples'
if (Test-Path -LiteralPath $samplesDir) {
    Remove-Item -LiteralPath $samplesDir -Recurse -Force
    Write-Host "[sync] 已剔除 arcs-sdk/samples/（含 subsys）"
}

# 镜像不带 samples/subsys，必须关掉小程序运行时，否则构建找不到 Lua 绑定源码。
# ⚠️ 必须以字节方式读写：prj.conf 是 UTF-8（含中文注释），
#    用 Set-Content / Out-File 会被转成系统 ANSI（GBK），Kconfig 直接报 Malformed utf-8。
$prj = 'apps/arcs-mini/prj.conf'
if (Test-Path -LiteralPath $prj) {
    $enc  = New-Object System.Text.UTF8Encoding($false)
    $text = $enc.GetString([System.IO.File]::ReadAllBytes($prj))
    if ($text -match '(?m)^CONFIG_MINIAPP=') {
        $text = [regex]::Replace($text, '(?m)^CONFIG_MINIAPP=.*$', 'CONFIG_MINIAPP=n')
    } else {
        $text = $text.TrimEnd() + "`r`n" + 'CONFIG_MINIAPP=n' + "`r`n"
    }
    [System.IO.File]::WriteAllText($prj, $text, $enc)
    Write-Host "[sync] prj.conf: CONFIG_MINIAPP=n（镜像不含 samples/subsys）"
}
# 本仓维护的覆盖层：同步时保留，不随上游覆盖（本仓不放文档，只留同步工具与指针）
$overlay = @('.github/scripts/sync-upstream.ps1', '.github/scripts/sync-upstream.sh', '.github/ai-docs-pointer.md', '.github/workflows/sync-upstream.yml')

Write-Host "[sync] 恢复覆盖层：$($overlay -join ', ')"
& git checkout HEAD -- @overlay 2>$null
# 在 AGENTS.md 末尾追加"去文档仓读文档"的指针（上游 AGENTS.md 本身不动，只在末尾追加）
$pointerFile = '.github/ai-docs-pointer.md'
if ((Test-Path -LiteralPath 'AGENTS.md') -and (Test-Path -LiteralPath $pointerFile)) {
    $pointer = Get-Content -LiteralPath $pointerFile -Raw
    Add-Content -LiteralPath 'AGENTS.md' -Value ("`n" + $pointer.TrimEnd() + "`n")
    Write-Host "[sync] AGENTS.md 追加 ling 文档指针"
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
