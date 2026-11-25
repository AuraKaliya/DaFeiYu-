param([string]$App, [string]$Root, [switch]$WithPanel)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TrayWindows {
    [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string kind, string title);
    [DllImport("user32")] public static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr GetProp(IntPtr window, string name);
    [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    // 保留空指针通配标题，PowerShell 的空字符串会只匹配无标题窗口。
    public static IntPtr Next(IntPtr after, string kind) { return FindWindowEx(IntPtr.Zero, after, kind, null); }
}
'@
$base = [IO.Directory]::CreateDirectory($Root).FullName
$folder = [IO.Directory]::CreateDirectory((Join-Path $base ('tray-' + [guid]::NewGuid().ToString('N')))).FullName
$config = Join-Path $folder 'offline.json'
[IO.File]::WriteAllText($config, '{"provider":"deepseek","base_url":"https://offline.invalid","model":"fixture","api_key_env":"DFY_UNUSED_TRAY_TEST"}')
$owners = @()
$views = @()
$shell = $null

# 等候可独立观察的窗口、日志或进程回执，每次只返回第一个匹配结果。
function Wait-For([scriptblock]$Read, [string]$Action) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        $value = & $Read
        if ($value) { return $value }
        Start-Sleep -Milliseconds 50
    } while ($clock.Elapsed.TotalSeconds -lt 12)
    throw "Timed out waiting for $Action"
}

# 只读取本次测试目录下已经落盘的结构化事件，跳过尚未写完的行。
function Read-Event([string]$Data, [string]$Event) {
    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $Data 'logs') -Filter '*.jsonl' -ErrorAction SilentlyContinue)) {
        foreach ($line in Get-Content -Encoding UTF8 -LiteralPath $file.FullName) {
            try { $row = $line | ConvertFrom-Json } catch { continue }
            if ($row.event -eq $Event) { $row }
        }
    }
}

# 为一个隔离目录启动正式后台，保留原进程句柄及独立的配置、插件、Trace 路径。
function Start-Resident([string]$Name) {
    $data = [IO.Directory]::CreateDirectory((Join-Path $folder $Name)).FullName
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $App
    $info.Arguments = '--host "' + $config + '" --data "' + $data + '" --plugins "' + (Join-Path $data 'other plugins') + '" --trace "' + (Join-Path $data 'trace') + '"'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.EnvironmentVariables['DFY_UI_CHECK_TRAY_PANEL'] = '1'
    $processOwner = [Diagnostics.Process]::Start($info)
    $null = $processOwner.Handle
    return [pscustomobject]@{ Data=$data; Process=$processOwner; Instance=''; Tray=[IntPtr]::Zero }
}

# 寻找公布原本体进程身份的管理窗，或属于指定托盘窗口的本机确认框。
function Find-OwnedWindow($Owner, [bool]$Dialog = $false) {
    $kind = if ($Dialog) { '#32770' } else { 'DaFeiYuU01' }
    $window = [IntPtr]::Zero
    while (($window = [TrayWindows]::Next($window, $kind)) -ne [IntPtr]::Zero) {
        if ($Dialog) {
            if ([TrayWindows]::GetWindow($window,4) -eq $Owner.Tray) { return $window }
        } elseif ([TrayWindows]::GetProp($window,'DaFeiYu.Management.ResidentPid').ToInt64() -eq $Owner.Process.Id) { return $window }
    }
}

# 向已核实的测试窗口投递单个 Win32 消息，不操作系统输入设备。
function Send-Native([IntPtr]$Window, [uint32]$Message, [int]$Wparam, [int]$Lparam = 0) {
    if (![TrayWindows]::PostMessage($Window,$Message,[IntPtr]$Wparam,[IntPtr]$Lparam)) { throw 'Native message was refused' }
}

# 等待真正由托盘启动的 WebView2 窗口通过原生来源与页面就绪检查。
function Wait-Panel($Owner) {
    $window = Wait-For { Find-OwnedWindow $Owner } 'original resident panel'
    [uint32]$panelPid = 0
    $null = [TrayWindows]::GetWindowThreadProcessId($window,[ref]$panelPid)
    $process = [Diagnostics.Process]::GetProcessById($panelPid)
    $null = $process.Handle
    $marker = Join-Path $Owner.Data ("tray-check-" + $panelPid)
    $null = Wait-For { Test-Path -LiteralPath ($marker + '.ready') } 'trusted panel document'
    return [pscustomobject]@{ Window=$window; Process=$process; Marker=$marker }
}

# 独立观察 Explorer 打开的路径，仅匹配本轮新建的日志目录。
function Find-LogView([string]$Path) {
    foreach ($view in $shell.Windows()) {
        try {
            $uri = [uri]$view.LocationURL
            if ($uri.IsFile -and $uri.LocalPath.TrimEnd('\') -eq $Path) { return $view }
        } catch { continue }
    }
}

try {
    $owners += Start-Resident 'data A'
    $owners += Start-Resident 'data B'
    foreach ($owner in $owners) {
        $row = Wait-For { Read-Event $owner.Data 'tray.start' | Select-Object -First 1 } 'native registration'
        if ($row.pid -ne $owner.Process.Id) { throw 'Unexpected tray owner' }
        if ($row.result -ne 'ok') {
            if ($row.result -ne 'failed' -or $owner.Process.HasExited) { throw 'Headless resident unavailable' }
            Write-Output 'Desktop registration unavailable for this process'
            exit 77
        }
        $owner.Instance = $row.instance
        $owner.Tray = Wait-For {
            $handle = [TrayWindows]::FindWindowEx([IntPtr]::Zero, [IntPtr]::Zero, 'DaFeiYu.ResidentTray', $owner.Instance)
            if ($handle -ne [IntPtr]::Zero) { $handle }
        } 'bound tray window'
    }
    $a, $b = $owners
    if ($a.Tray -eq $b.Tray -or $a.Instance -eq $b.Instance) { throw 'Two profiles shared one tray' }

    $shell = New-Object -ComObject Shell.Application
    $priorWindows = @($shell.Windows() | ForEach-Object { $_.HWND })
    Send-Native $a.Tray 0x111 2
    $logView = Wait-For { Find-LogView (Join-Path $a.Data 'logs') } 'original log folder in Explorer'
    if ((Read-Event $a.Data 'tray.open_logs').result -ne 'accepted' -or (Read-Event $b.Data 'tray.open_logs')) { throw 'Incorrect log target' }

    Send-Native $a.Tray 0x8011 0 0x10400
    if ($WithPanel) {
        $first = Wait-Panel $a
        $views += $first
        Send-Native $b.Tray 0x111 1
        $other = Wait-Panel $b
        $views += $other
        Send-Native $a.Tray 0x111 1
        $null = Wait-For { @(Read-Event $a.Data 'tray.open_panel').Count -eq 2 } 'panel activation'
        if ((Find-OwnedWindow $a) -ne $first.Window -or (Find-OwnedWindow $b) -ne $other.Window) { throw 'Reactivation opened a different panel' }
        Send-Native $first.Window 0x10 0
        if (!$first.Process.WaitForExit(8000) -or $first.Process.ExitCode -ne 0 -or $a.Process.HasExited) { throw 'Closing panel stopped the resident' }
        Send-Native $a.Tray 0x8011 0 0x10400
        $reopened = Wait-Panel $a
        $views += $reopened
        if ($reopened.Process.Id -eq $first.Process.Id) { throw 'Panel did not reopen' }
        Send-Native $a.Tray 0x111 3
        [IO.File]::WriteAllText($reopened.Marker + '.first','1')
        $null = Wait-For { Test-Path -LiteralPath ($reopened.Marker + '.cancelled') } 'cancelled shared confirmation'
        if ($a.Process.HasExited -or (Read-Event $a.Data 'stop.submitted')) { throw 'Cancel submitted a stop' }
        Send-Native $a.Tray 0x111 3
        [IO.File]::WriteAllText($reopened.Marker + '.second','1')
    } else {
        $missing = Wait-For { Read-Event $a.Data 'tray.open_panel' } 'missing-panel result'
        if ($missing.result -ne 'failed' -or $missing.systemError -ne 2) { throw 'Missing panel did not report its launch error' }
        Send-Native $a.Tray 0x111 3
        $dialog = Wait-For { Find-OwnedWindow $a $true } 'native confirmation'
        Send-Native $dialog 0x111 2
        $null = Wait-For { !(Find-OwnedWindow $a $true) } 'native cancellation'
        if ($a.Process.HasExited -or (Read-Event $a.Data 'stop.submitted')) { throw 'Native cancel submitted a stop' }
        Send-Native $a.Tray 0x111 3
        $dialog = Wait-For { Find-OwnedWindow $a $true } 'second native confirmation'
        Send-Native $dialog 0x111 6
    }
    if (!$a.Process.WaitForExit(8000) -or $a.Process.ExitCode -ne 0 -or $b.Process.HasExited) { throw 'Exit crossed profile boundaries or failed' }
    if ((Read-Event $a.Data 'tray.removed').result -ne 'ok' -or (Read-Event $a.Data 'stop.accepted').operation -ne ('stop-' + $a.Instance)) {
        throw 'Missing original shutdown or icon removal'
    }
    if ($WithPanel -and (!$reopened.Process.WaitForExit(8000) -or $reopened.Process.ExitCode -ne 0 -or $other.Process.HasExited)) {
        throw 'Panel shutdown crossed profile boundaries'
    }
} catch {
    foreach ($owner in $owners) {
        foreach ($event in @('tray.start','tray.open_panel','tray.open_logs','tray.confirm_exit')) {
            Read-Event $owner.Data $event | Select-Object event,result,systemError,pid | Out-Host
        }
    }
    throw
} finally {
    if ($logView -and $priorWindows -notcontains $logView.HWND) {
        try { if (([uri]$logView.LocationURL).LocalPath.TrimEnd('\') -eq (Join-Path $a.Data 'logs')) { $logView.Quit() } } catch {}
    }
    foreach ($owner in $owners) {
        if (!$owner.Process.HasExited) {
            & $App --stop-host $owner.Data 2>$null | Out-Null
            if (!$owner.Process.WaitForExit(6000)) { $owner.Process.Kill(); $owner.Process.WaitForExit() }
        }
        $owner.Process.Dispose()
    }
    foreach ($view in $views) { $null = $view.Process.WaitForExit(6000); $view.Process.Dispose() }
    $created = Get-Item -LiteralPath $folder
    if ($created.Parent.FullName -eq $base -and $created.Name -match '^tray-[0-9a-f]{32}$' -and
        !($created.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Remove-Item -LiteralPath $created.FullName -Recurse -Force }
}
