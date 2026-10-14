# we_window.ps1 - run in the VM through winvm.sh run: hides WE's UI pop-up and prints the wallpaper window's client
# rectangle in screen pixels ("x y w h"), what decode.py style frame readers need
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class W {
    public delegate bool EnumProc (IntPtr h, IntPtr p);
    [DllImport ("user32.dll")] public static extern bool EnumWindows (EnumProc f, IntPtr p);
    [DllImport ("user32.dll")] public static extern int GetWindowText (IntPtr h, StringBuilder s, int n);
    [DllImport ("user32.dll")] public static extern bool IsWindowVisible (IntPtr h);
    [DllImport ("user32.dll")] public static extern bool ShowWindow (IntPtr h, int cmd);
    [DllImport ("user32.dll")] public static extern bool GetClientRect (IntPtr h, out RECT r);
    [DllImport ("user32.dll")] public static extern bool ClientToScreen (IntPtr h, ref POINT p);
    [StructLayout (LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout (LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@
$found = New-Object System.Collections.ArrayList
$cb = [W+EnumProc] {
    param($h, $p)
    $s = New-Object System.Text.StringBuilder 256
    [W]::GetWindowText($h, $s, 256) | Out-Null
    if ([W]::IsWindowVisible($h)) { $found.Add(@($h, $s.ToString())) | Out-Null }
    return $true
}
[W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
foreach ($w in $found) {
    if ($w[1] -eq 'Wallpaper UI') { [W]::ShowWindow($w[0], 0) | Out-Null; 'hid Wallpaper UI' }
}
foreach ($w in $found) {
    if ($w[1] -eq 'Wallpaper Pop-out') {
        $r = New-Object W+RECT
        [W]::GetClientRect($w[0], [ref] $r) | Out-Null
        $pt = New-Object W+POINT
        [W]::ClientToScreen($w[0], [ref] $pt) | Out-Null
        "client $($pt.X) $($pt.Y) $($r.R - $r.L) $($r.B - $r.T)"
    }
}
