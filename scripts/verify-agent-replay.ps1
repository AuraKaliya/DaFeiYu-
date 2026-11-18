# 比较两套测试构建中同一批固定案例的实际行为，并保存原证据。
# 步骤：解析构建目录 → 按明确案例运行双方 → 保存证据及差异 → 汇总失败和不可比。
param(
    [Parameter(Mandatory)][string]$BaselineBuild,
    [string]$CandidateBuild = $BaselineBuild,
    [ValidateSet('Debug','Release')][string]$BaselineConfig = 'Debug',
    [ValidateSet('Debug','Release')][string]$CandidateConfig = 'Release',
    [string[]]$Declared = @('build')
)
$ErrorActionPreference = 'Stop'
$BaselineBuild = (Resolve-Path -LiteralPath $BaselineBuild).Path
$CandidateBuild = (Resolve-Path -LiteralPath $CandidateBuild).Path
$root = Split-Path -Parent $PSScriptRoot
$results = Join-Path $root ('build/agent-replay-check/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $results | Out-Null

$groups = @(
    @{ Target='dfy_replay_evaluation'; Material='confirmed-counter.json'; Cases=@('confirmed-counter') },
    @{ Target='dfy_observation_controller_tests'; Cases=@('observation_dispatch_gap') },
    @{ Target='dfy_session_observation_rebind_tests'; Cases=@('observation_rebind','observation_rebind_cancel','observation_rebind_expire','observation_rebind_goal_change') },
    @{ Target='dfy_session_goal_revision_tests'; Cases=@('goal_revision','goal_revision_confirmed','goal_revision_limit') },
    @{ Target='dfy_session_replanning_tests'; Cases=@('plan_tail_revision','plan_tail_invalid_prefix','plan_tail_cancel','plan_tail_expire') },
    @{ Target='dfy_native_action_feedback_tests'; Fixture='tests/fixtures/tools_v2/{0}'; Cases=@('native_action_satisfied','native_action_mismatch','native_action_unknown','native_observation_action','native_observation_revoked') },
    @{ Target='dfy_tool_verification_tests'; Fixture='plugins/examples/status/{0}/example.status'; Cases=@('app_task_reconnect','app_task_mismatch','app_task_early_change','app_task_replaced','app_task_denied') }
)

# 只运行明确的测试入口；原生 DLL 路径由构建和已登记的夹具确定。
# 步骤：定位二进制及材料 → 采集指定案例 → 保存原输出并核对案例身份。
function Save-Case([string]$Build, [string]$Config, [hashtable]$Group, [string]$Case, [string]$Side) {
    $binary = Join-Path $Build "tests/$Config/$($Group.Target).exe"
    if (!(Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Missing capture binary: $binary" }
    $arguments = @('--capture',$Case)
    $label = "$Case-$Side"
    if ($Group.Material) {
        $arguments = @('--capture',(Join-Path $root "tests/fixtures/scenarios/$($Group.Material)"))
    } elseif ($Group.Fixture) {
        $package = Join-Path $Build ($Group.Fixture -f $Config)
        $arguments = @($package,(Join-Path $results "$label-data")) + $arguments
    }

    $output = & $binary @arguments 2> (Join-Path $results "$label.err")
    $code = $LASTEXITCODE
    $path = Join-Path $results "$label.json"
    $output | Set-Content -LiteralPath $path -Encoding utf8NoBOM
    if ($code -ne 0) { throw "Capture failed for $label with exit $code. See $results" }
    $value = $output | ConvertFrom-Json -AsHashtable
    if ($value.case_id -cne $Case -or $value.case_version -lt 1) { throw "Unexpected case identity in $path" }
    return $path
}

$compare = Join-Path $CandidateBuild "tests/$CandidateConfig/dfy_replay_evaluation.exe"
$failures = @()
$count = 0
foreach ($group in $groups) {
    foreach ($case in $group.Cases) {
        $baseline = Save-Case $BaselineBuild $BaselineConfig $group $case 'baseline'
        $candidate = Save-Case $CandidateBuild $CandidateConfig $group $case 'candidate'
        $report = & $compare --compare $baseline $candidate @Declared 2> (Join-Path $results "$case-compare.err")
        $code = $LASTEXITCODE
        $report | Set-Content -LiteralPath (Join-Path $results "$case-compare.txt") -Encoding utf8NoBOM
        if ($code -ne 0) { $failures += "$case ($code)" }
        ++$count
    }
}
if ($failures.Count) { throw ("Replay differences or incomparable cases: " + ($failures -join ', ') + ". See $results") }
Write-Output "$count offline cases matched. Evidence: $results"
