#Requires -Version 5.1
<#
.SYNOPSIS
    Captures reproducible screenshots of the CRR GUI for the project report.

.DESCRIPTION
    Launches bin\crr_gui.exe, drives it through the real Win32 message interface
    (BM_CLICK on the CPU preset and Run buttons), waits for completion, and saves
    PNGs of the live window via PrintWindow. Nothing is mocked: the screenshots
    show metrics produced by an actual child-process execution.

    Output files (default docs\screenshots\):
      01_gui_idle.png       - control panel before any run
      02_gui_results.png    - single 250 ms CPU workload executed
      03_gui_benchmark.png  - 3-run benchmark with mean/std-dev rows
      04_gui_launch_failure.png - nonexistent command reported as FAILED

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture_gui.ps1
#>
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\bin\crr_gui.exe'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..\docs\screenshots')
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class CrrWin32 {
    [DllImport("user32.dll", CharSet = CharSet.Ansi)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr hDlg, int nIDDlgItem);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageStr(IntPtr hWnd, uint msg, IntPtr wParam, string lParam);

    [DllImport("user32.dll", CharSet = CharSet.Ansi)]
    public static extern int GetWindowTextA(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool SetProcessDPIAware();

    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

# Without DPI awareness GetWindowRect reports virtualized (scaled-down) bounds and
# PrintWindow renders only part of the real window into the bitmap.
[void][CrrWin32]::SetProcessDpiAwarenessContext([IntPtr](-4))  # PER_MONITOR_AWARE_V2
[void][CrrWin32]::SetProcessDPIAware()

$script:BM_CLICK       = 0x00F5
$script:WM_SETTEXT     = 0x000C
$script:WM_CLOSE       = 0x0010
$script:IDC_BTN_RUN    = 1008
$script:IDC_REPEAT     = 1005
$script:IDC_STATUS     = 1012
$script:IDC_PRESET_CPU = 1013
$script:IDC_LIST       = 1020
$script:IDC_CMD        = 1001

function Get-StatusText([IntPtr]$hwnd) {
    $label = [CrrWin32]::GetDlgItem($hwnd, $script:IDC_STATUS)
    $sb = New-Object System.Text.StringBuilder 512
    [void][CrrWin32]::GetWindowTextA($label, $sb, $sb.Capacity)
    return $sb.ToString()
}

function Save-WindowShot([IntPtr]$hwnd, [string]$path) {
    $rect = New-Object CrrWin32+RECT
    if (-not [CrrWin32]::GetWindowRect($hwnd, [ref]$rect)) {
        throw "GetWindowRect failed"
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bmp = New-Object System.Drawing.Bitmap $width, $height
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    try {
        # PW_RENDERFULLCONTENT (2) captures the client area of a layered/visible window
        if (-not [CrrWin32]::PrintWindow($hwnd, $hdc, 2)) {
            throw "PrintWindow failed"
        }
    } finally {
        $gfx.ReleaseHdc($hdc)
        $gfx.Dispose()
    }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("saved {0} ({1}x{2})" -f $path, $width, $height)
}

function Wait-ForWindow([int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $hwnd = [CrrWin32]::FindWindow('CRR_MainWindowClass', [NullString]::Value)
        if ($hwnd -ne [IntPtr]::Zero) { return $hwnd }
        Start-Sleep -Milliseconds 150
    }
    throw "GUI window did not appear within $timeoutSec s"
}

function Wait-ForStatus([IntPtr]$hwnd, [string]$needle, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $status = Get-StatusText $hwnd
        if ($status -like "*$needle*") { return $status }
        Start-Sleep -Milliseconds 150
    }
    throw "Timed out waiting for status containing '$needle' (last: '$(Get-StatusText $hwnd)')"
}

$exePath = [System.IO.Path]::GetFullPath($Exe)
$outPath = [System.IO.Path]::GetFullPath($OutDir)
if (-not (Test-Path $exePath)) { throw "GUI executable not found: $exePath" }
if (-not (Test-Path $outPath)) { New-Item -ItemType Directory -Force -Path $outPath | Out-Null }

$proc = Start-Process -FilePath $exePath -WorkingDirectory (Split-Path $exePath) -PassThru
try {
    $hwnd = Wait-ForWindow 15
    [void][CrrWin32]::ShowWindow($hwnd, 5)   # SW_SHOW
    [void][CrrWin32]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 700

    Save-WindowShot $hwnd (Join-Path $outPath '01_gui_idle.png')

    # Single run: CPU preset (250 ms) -> Run
    [void][CrrWin32]::SendMessage([CrrWin32]::GetDlgItem($hwnd, $script:IDC_PRESET_CPU), $script:BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    [void][CrrWin32]::SendMessage([CrrWin32]::GetDlgItem($hwnd, $script:IDC_BTN_RUN), $script:BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    $status = Wait-ForStatus $hwnd 'Status: Completed' 30
    Write-Host "single run: $status"
    Start-Sleep -Milliseconds 400
    Save-WindowShot $hwnd (Join-Path $outPath '02_gui_results.png')

    # Three-run benchmark -> Run
    $repeatBox = [CrrWin32]::GetDlgItem($hwnd, $script:IDC_REPEAT)
    [void][CrrWin32]::SendMessageStr($repeatBox, $script:WM_SETTEXT, [IntPtr]::Zero, '3')
    Start-Sleep -Milliseconds 200
    [void][CrrWin32]::SendMessage([CrrWin32]::GetDlgItem($hwnd, $script:IDC_BTN_RUN), $script:BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    $status = Wait-ForStatus $hwnd 'Status: Benchmark complete' 60
    Write-Host "benchmark: $status"
    Start-Sleep -Milliseconds 400
    Save-WindowShot $hwnd (Join-Path $outPath '03_gui_benchmark.png')

    # Launch failure: an unresolvable command must report FAILED, not SUCCESS (Exit 1)
    [void][CrrWin32]::SendMessageStr([CrrWin32]::GetDlgItem($hwnd, $script:IDC_REPEAT), $script:WM_SETTEXT, [IntPtr]::Zero, '1')
    [void][CrrWin32]::SendMessageStr([CrrWin32]::GetDlgItem($hwnd, $script:IDC_CMD), $script:WM_SETTEXT, [IntPtr]::Zero, 'crr_missing_command_xyz')
    Start-Sleep -Milliseconds 200
    [void][CrrWin32]::SendMessage([CrrWin32]::GetDlgItem($hwnd, $script:IDC_BTN_RUN), $script:BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    $status = Wait-ForStatus $hwnd 'Status: Launch failed' 20
    Write-Host "launch failure: $status"
    Start-Sleep -Milliseconds 400
    Save-WindowShot $hwnd (Join-Path $outPath '04_gui_launch_failure.png')

    [void][CrrWin32]::PostMessage($hwnd, $script:WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $proc.WaitForExit(5000)) { throw "GUI did not exit after WM_CLOSE" }
    Write-Host "GUI exited with code $($proc.ExitCode)"
} finally {
    if (-not $proc.HasExited) { $proc.Kill() }
}
