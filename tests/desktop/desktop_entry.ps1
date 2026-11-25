# 从其他工作目录打开正式桌面入口，按原实例的托盘命令恢复最小化的管理中心。
param([Parameter(Mandatory)][string]$Panel, [Parameter(Mandatory)][string]$Root)
$ErrorActionPreference = 'Stop'
$panelPath = (Resolve-Path -LiteralPath $Panel).Path
$hostPath = Join-Path (Split-Path -Parent $panelPath) 'dafeiyu-host.exe'
$data = [IO.Directory]::CreateDirectory((Join-Path $Root ('desktop ' + [guid]::NewGuid().ToString('N')))).FullName
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class DesktopEntryWindow {
    [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string kind, string title);
    [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr GetProp(IntPtr window, string name);
    [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32")] public static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder path, ref int size);
    // 按原进程句柄读取映像路径，兼容从 32 位 PowerShell 验证 64 位程序。
    public static string ImagePath(IntPtr process) {
        int size = 32768;
        var path = new StringBuilder(size);
        if (!QueryFullProcessImageName(process, 0, path, ref size)) throw new System.ComponentModel.Win32Exception();
        return path.ToString();
    }
}
'@

# 等待属于本用例的可观察结果，超时报告具体阶段。
function Wait-Entry([scriptblock]$Read, [string]$Description) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        $result = & $Read
        if ($result) { return $result }
        Start-Sleep -Milliseconds 50
    } while ($clock.Elapsed.TotalSeconds -lt 15)
    throw "Desktop entry timed out waiting for $Description"
}

# 查找已经和原后台握手的正式面板，可限制到本次启动的窗口进程。
function Find-EntryPanel([int]$ResidentPid, [int]$WindowPid = 0) {
    $window = [IntPtr]::Zero
    while (($window = [DesktopEntryWindow]::FindWindowEx([IntPtr]::Zero,$window,'DaFeiYuU01','大肥鱼管理中心')) -ne [IntPtr]::Zero) {
        [uint32]$observedPid = 0
        $null = [DesktopEntryWindow]::GetWindowThreadProcessId($window,[ref]$observedPid)
        if ((!$WindowPid -or $observedPid -eq $WindowPid) -and
            [DesktopEntryWindow]::GetProp($window,'DaFeiYu.Management.ResidentPid').ToInt64() -eq $ResidentPid) { return $window }
    }
}

$info = [Diagnostics.ProcessStartInfo]::new()
$info.FileName = $panelPath
$info.Arguments = '--data "' + $data + '"'
$info.WorkingDirectory = Join-Path $env:SystemRoot 'System32'
$info.UseShellExecute = $false
$info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$info.EnvironmentVariables.Remove('DEEPSEEK_API_KEY')
$info.EnvironmentVariables.Remove('DASHSCOPE_API_KEY')
$panelProcess = [Diagnostics.Process]::Start($info)
$resident = $null
$reopened = $null
try {
    $status = Wait-Entry {
        if ($panelProcess.HasExited) { throw "Desktop exited early with $($panelProcess.ExitCode)" }
        try { $state = (& $hostPath --host-status $data 2>$null | Out-String) | ConvertFrom-Json } catch { return }
        if ($state.state -eq 'running') { $state }
    } 'the resident'
    $resident = [Diagnostics.Process]::GetProcessById([int]$status.pid)
    $imagePath = [DesktopEntryWindow]::ImagePath($resident.Handle)
    if ($imagePath -ne $hostPath) { throw "Expected resident at $hostPath, observed $imagePath" }
    $panelWindow = Wait-Entry { Find-EntryPanel $resident.Id $panelProcess.Id } 'the main panel bound to this resident'

    $trayState = Wait-Entry {
        Get-ChildItem -LiteralPath (Join-Path $data 'logs') -Filter '*.jsonl' | ForEach-Object {
            foreach ($line in Get-Content -LiteralPath $_.FullName -Encoding UTF8) {
                if ($line -notmatch 'tray\.start') { continue }
                try { $row = $line | ConvertFrom-Json } catch { continue }
                if ($row.pid -eq $resident.Id -and $row.event -eq 'tray.start') { return $row }
            }
        } | Select-Object -First 1
    } 'the Shell registration receipt'
    if ($trayState.result -ne 'ok') { Write-Output 'Shell notification area unavailable'; exit 77 }
    $tray = Wait-Entry {
        $handle = [DesktopEntryWindow]::FindWindowEx([IntPtr]::Zero,[IntPtr]::Zero,'DaFeiYu.ResidentTray',$status.instance)
        if ($handle -ne [IntPtr]::Zero) { $handle }
    } 'the original tray'
    # 在本用例的窗口上核对最小化与原句柄恢复。
    $null = [DesktopEntryWindow]::ShowWindowAsync($panelWindow,7)
    $null = Wait-Entry { [DesktopEntryWindow]::IsIconic($panelWindow) } 'minimization'
    if (![DesktopEntryWindow]::PostMessage($tray,0x111,[IntPtr]1,[IntPtr]::Zero)) { throw 'Tray command rejected' }
    $null = Wait-Entry { ![DesktopEntryWindow]::IsIconic($panelWindow) -and !$panelProcess.HasExited } 'restoration of the original window'
    $null = [DesktopEntryWindow]::PostMessage($panelWindow,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    if (!$panelProcess.WaitForExit(12000) -or $panelProcess.ExitCode -ne 0 -or $resident.HasExited) { throw 'Closing the panel lost the resident' }
    if (![DesktopEntryWindow]::PostMessage($tray,0x111,[IntPtr]1,[IntPtr]::Zero)) { throw 'Second tray command rejected' }
    $next = Wait-Entry { Find-EntryPanel $resident.Id } 'a new main panel for the original resident'
    [uint32]$nextPid = 0
    $null = [DesktopEntryWindow]::GetWindowThreadProcessId($next,[ref]$nextPid)
    $reopened = [Diagnostics.Process]::GetProcessById($nextPid)
    $imagePath = [DesktopEntryWindow]::ImagePath($reopened.Handle)
    if ($imagePath -ne $panelPath -or $nextPid -eq $panelProcess.Id) { throw "Expected a new $panelPath, observed $imagePath (PID $nextPid)" }
} finally {
    try {
        if ($resident -and !$resident.HasExited) {
            & $hostPath --stop-host $data $status.instance | Out-Null
            if (!$resident.WaitForExit(10000) -or $resident.ExitCode -ne 0) { throw 'Resident did not exit normally' }
        }
        if (!$panelProcess.WaitForExit(12000) -or $panelProcess.ExitCode -ne 0) { throw 'Desktop did not release its window' }
        if ($reopened -and (!$reopened.WaitForExit(12000) -or $reopened.ExitCode -ne 0)) { throw 'Reopened panel did not exit normally' }
    } finally {
        if (!$panelProcess.HasExited) { $panelProcess.Kill(); $panelProcess.WaitForExit() }
        $panelProcess.Dispose()
        if ($resident) { $resident.Dispose() }
        if ($reopened) { $reopened.Dispose() }
    }
}
