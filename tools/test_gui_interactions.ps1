# Drives the real GUI through the interactions that produced past defects:
# Enter-to-run, Cancel during a run, and closing the window mid-run.
# Exits 0 only if every assertion passes.
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class W32i {
    [DllImport("user32.dll", CharSet = CharSet.Ansi)]
    public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageStr(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageGet(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$WM_SETTEXT = 0x000C; $WM_GETTEXT = 0x000D; $WM_GETTEXTLENGTH = 0x000E
$WM_CHAR = 0x0102; $WM_CLOSE = 0x0010; $BM_CLICK = 0x00F5
$CMD = 1001; $BTN_CANCEL = 1009; $STATUS = 1012

$script:failures = 0
function Assert([bool]$cond, [string]$name) {
    if ($cond) { Write-Host ("PASS  " + $name) }
    else { Write-Host ("FAIL  " + $name); $script:failures++ }
}

function Get-Text([IntPtr]$h) {
    $len = [int][W32i]::SendMessage($h, $WM_GETTEXTLENGTH, [IntPtr]::Zero, [IntPtr]::Zero)
    $sb = New-Object System.Text.StringBuilder ($len + 1)
    [void][W32i]::SendMessageGet($h, $WM_GETTEXT, [IntPtr]($len + 1), $sb)
    return $sb.ToString()
}

function Wait-Status([IntPtr]$h, [string]$pattern, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $s = Get-Text $h
        if ($s -match $pattern) { return $s }
        Start-Sleep -Milliseconds 100
    }
    return (Get-Text $h)
}

$binDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin'
$exe = Join-Path $binDir 'crr_gui.exe'
$workload = '"' + (Join-Path $binDir 'sleep_delay.exe') + '"'

$proc = Start-Process -FilePath $exe -WorkingDirectory $binDir -PassThru
try {
    $deadline = (Get-Date).AddSeconds(15); $h = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline -and $h -eq [IntPtr]::Zero) {
        $h = [W32i]::FindWindow('CRR_MainWindowClass', [NullString]::Value); Start-Sleep -Milliseconds 150
    }
    Assert ($h -ne [IntPtr]::Zero) 'Main window created'

    $cmd = [W32i]::GetDlgItem($h, $CMD)
    $status = [W32i]::GetDlgItem($h, $STATUS)
    $cancel = [W32i]::GetDlgItem($h, $BTN_CANCEL)

    # 1. Enter in the command field runs the command (EDIT subclass -> BN_CLICKED).
    [void][W32i]::SendMessageStr($cmd, $WM_SETTEXT, [IntPtr]::Zero, ($workload + ' 400'))
    [void][W32i]::PostMessage($cmd, $WM_CHAR, [IntPtr]13, [IntPtr]1)
    $s = Wait-Status $status 'Completed successfully' 10
    Assert ($s -match 'Completed successfully') ("Enter triggers run: " + $s)

    # 2. Cancel aborts a long run and reports cancellation.
    [void][W32i]::SendMessageStr($cmd, $WM_SETTEXT, [IntPtr]::Zero, ($workload + ' 6000'))
    [void][W32i]::PostMessage($cmd, $WM_CHAR, [IntPtr]13, [IntPtr]1)
    $s = Wait-Status $status 'Executing' 5
    Assert ($s -match 'Executing') 'Long run reaches Executing state'
    [void][W32i]::SendMessage($cancel, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    $s = Wait-Status $status 'cancelled' 6
    Assert ($s -match 'cancelled') ("Cancel reports cancellation: " + $s)

    # 3. Closing the window during a run must cancel the worker and exit quickly.
    [void][W32i]::SendMessageStr($cmd, $WM_SETTEXT, [IntPtr]::Zero, ($workload + ' 6000'))
    [void][W32i]::PostMessage($cmd, $WM_CHAR, [IntPtr]13, [IntPtr]1)
    $s = Wait-Status $status 'Executing' 5
    Assert ($s -match 'Executing') 'Run reaches Executing before close'
    $closeStart = Get-Date
    [void][W32i]::PostMessage($h, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    $exited = $proc.WaitForExit(10000)
    $elapsed = ((Get-Date) - $closeStart).TotalSeconds
    Assert $exited 'Window close terminates the process while a run is active'
    Assert ($elapsed -lt 6.0) ("Close-mid-run returns within 6 s (took {0:N2} s)" -f $elapsed)
    if ($exited) { Assert ($proc.ExitCode -eq 0) ("GUI exit code 0 (got " + $proc.ExitCode + ")") }
} finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}

Write-Host ("GUI interaction failures: " + $script:failures)
exit $(if ($script:failures -eq 0) { 0 } else { 1 })
