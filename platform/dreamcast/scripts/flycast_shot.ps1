# A PNG of the Flycast window running from the build's portable copy (the
# picture check while the game runs in Flycast; no input is sent). The
# emulator's window is found among the top-level windows of those processes
# (not its "Flycast Serial Output" console).
#   powershell -File platform/dreamcast/scripts/flycast_shot.ps1 OUT.png
param([Parameter(Mandatory = $true)][string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    public static IntPtr Find(uint[] pids) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (Array.IndexOf(pids, pid) < 0 || !IsWindowVisible(h)) return true;
            var t = new StringBuilder(256); GetWindowText(h, t, 256);
            if (t.ToString().Contains("Serial")) return true;
            RECT r; GetClientRect(h, out r);
            if (r.Right - r.Left < 320) return true;
            found = h; return false;
        }, IntPtr.Zero);
        return found;
    }
}
"@
$pids = @(Get-Process flycast -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "*build-daytona*" } |
    ForEach-Object { [uint32]$_.Id })
$h = if ($pids.Count) { [Win]::Find($pids) } else { [IntPtr]::Zero }
if ($h -eq [IntPtr]::Zero) { Write-Error "no Flycast window from the build's copy"; exit 1 }
$r = New-Object Win+RECT
[void][Win]::GetClientRect($h, [ref]$r)
$w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[void][Win]::PrintWindow($h, $dc, 3) # client area, PW_RENDERFULLCONTENT (Direct3D content too)
$g.ReleaseHdc($dc)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"$Out ${w}x$hh"
