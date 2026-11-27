# Native window checks for the scratch-only coop-dev/test_split.sh.
param(
    [Parameter(Mandatory)][int]$HostId,
    [int]$GuestId,
    [ValidateSet('top', 'left')][string]$Layout = 'top',
    [switch]$Resize,
    [switch]$Maximize,
    [switch]$InputCheck,
    [switch]$AltF4,
    [switch]$TwoWindows,
    [switch]$Fullscreen,
    [string]$Desktop
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class SplitWindows {
    public delegate bool EnumProc(IntPtr h, IntPtr param);
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect {
        public int Left, Top, Right, Bottom;
        public int Width { get { return Right - Left; } }
        public int Height { get { return Bottom - Top; } }
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct Point { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr param);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, EnumProc callback, IntPtr param);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint command);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll", EntryPoint="GetWindowLongW")] static extern int Style(IntPtr h, int index);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref Point point);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int height, bool repaint);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int command);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [StructLayout(LayoutKind.Sequential)]
    struct MonitorInfo { public uint size; public Rect bounds, work; public uint flags; }
    [DllImport("user32.dll")] static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
    [DllImport("user32.dll", EntryPoint="GetMonitorInfoW")] static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    public static bool Fullscreen(IntPtr h) {
        var info = new MonitorInfo { size = (uint)Marshal.SizeOf(typeof(MonitorInfo)) };
        Rect rect;
        return (Style(h, -16) & 0x00c00000) == 0 && GetWindowRect(h, out rect) &&
            GetMonitorInfo(MonitorFromWindow(h, 2), ref info) && rect.Equals(info.bounds);
    }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    public static void Foreground(IntPtr target) {
        var foreground = GetForegroundWindow();
        if (GetAncestor(foreground, 2) == target || SetForegroundWindow(target)) return;
        int ignored;
        uint foregroundThread = GetWindowThreadProcessId(foreground, out ignored);
        uint caller = GetCurrentThreadId();
        // Ensure that this PowerShell thread has a message queue for AttachThreadInput.
        Msg message;
        PeekMessage(out message, IntPtr.Zero, 0, 0, 0);
        bool attached = foregroundThread != 0 && AttachThreadInput(caller, foregroundThread, true);
        try { BringWindowToTop(target); SetForegroundWindow(target); }
        finally { if (attached) AttachThreadInput(caller, foregroundThread, false); }
    }
    [StructLayout(LayoutKind.Sequential)]
    struct Msg { public IntPtr h; public uint message; public IntPtr wp, lp; public uint time; public Point point; public uint extra; }
    [DllImport("user32.dll")] static extern bool PeekMessage(out Msg msg, IntPtr h, uint min, uint max, uint remove);
    [StructLayout(LayoutKind.Sequential)]
    struct ThreadInfo {
        public uint size, flags;
        public IntPtr active, focus, capture, menuOwner, moveSize, caret;
        public Rect caretRect;
    }
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread, ref ThreadInfo info);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern void mouse_event(uint flags, uint dx, uint dy, uint data, IntPtr extra);
    [DllImport("user32.dll")] static extern void keybd_event(byte key, byte scan, uint flags, IntPtr extra);
    public static IntPtr Focus() {
        var info = new ThreadInfo { size = (uint)Marshal.SizeOf(typeof(ThreadInfo)) };
        return GetGUIThreadInfo(0, ref info) ? info.focus : IntPtr.Zero;
    }
    public static void Click(IntPtr pane) {
        Rect rect;
        if (!GetWindowRect(pane, out rect) || !SetCursorPos(rect.Left + rect.Width / 2, rect.Top + rect.Height / 2))
            throw new InvalidOperationException("Could not point at the pane");
        mouse_event(2, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(80);
        mouse_event(4, 0, 0, 0, IntPtr.Zero);
    }
    public static void Key(byte key, byte modifier = 0) {
        if (modifier != 0) keybd_event(modifier, 0, 0, IntPtr.Zero);
        keybd_event(key, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(80);
        keybd_event(key, 0, 2, IntPtr.Zero);
        if (modifier != 0) keybd_event(modifier, 0, 2, IntPtr.Zero);
    }
    public static IntPtr[] TaskbarWindows(int host, int guest) {
        var result = new List<IntPtr>();
        EnumWindows((h, p) => {
            int pid;
            GetWindowThreadProcessId(h, out pid);
            if ((pid == host || pid == guest) && IsWindowVisible(h) && GetWindow(h, 4) == IntPtr.Zero &&
                (Style(h, -20) & 0x80) == 0)
                result.Add(h);
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
    public static IntPtr Pane(IntPtr container, int owner) {
        IntPtr result = IntPtr.Zero;
        EnumChildWindows(container, (h, p) => {
            int pid;
            GetWindowThreadProcessId(h, out pid);
            if (pid != owner || GetParent(h) != container || !IsWindowVisible(h)) return true;
            result = h;
            return false;
        }, IntPtr.Zero);
        return result;
    }
}
'@
[void][SplitWindows]::SetProcessDpiAwarenessContext([IntPtr](-4))
$windows = @([SplitWindows]::TaskbarWindows($HostId, $GuestId))
if ($TwoWindows) {
    $hostWindows = @([SplitWindows]::TaskbarWindows($HostId, 0))
    $guestWindows = @([SplitWindows]::TaskbarWindows($GuestId, 0))
    if ($hostWindows.Count -ne 1 -or $guestWindows.Count -ne 1) {
        throw 'Expected one separate top-level window per player.'
    }
    $first = New-Object SplitWindows+Rect
    $second = New-Object SplitWindows+Rect
    if (![SplitWindows]::GetWindowRect($hostWindows[0], [ref]$first) -or
        ![SplitWindows]::GetWindowRect($guestWindows[0], [ref]$second) -or
        $first.Left -ne $second.Left -or $first.Right -ne $second.Right -or
        $first.Bottom -ne $second.Top -or [Math]::Abs($first.Height - $second.Height) -gt 1) {
        throw 'Separate windows did not fill adjoining top and bottom halves.'
    }
    Write-Output "Two separate windows: $($first.Width)x$($first.Height) and $($second.Width)x$($second.Height)."
    return
}
if ($windows.Count -ne 1) { throw "Expected one taskbar-eligible window. Found $($windows.Count)." }
$container = $windows[0]
if (!$GuestId) {
    if ([SplitWindows]::Pane($container, $HostId) -ne [IntPtr]::Zero) { throw 'Player 1 remains embedded after removal.' }
    Write-Output 'One standalone Player 1 window.'
    return
}
if ($Resize) {
    $outer = New-Object SplitWindows+Rect
    if (![SplitWindows]::GetWindowRect($container, [ref]$outer) -or
        ![SplitWindows]::MoveWindow($container, $outer.Left + 25, $outer.Top + 20,
                                    ($outer.Width * 3 / 4), ($outer.Height * 3 / 4), $true)) {
        throw 'Could not move and resize the container.'
    }
    Start-Sleep -Seconds 2
}
if ($Maximize) {
    [void][SplitWindows]::ShowWindowAsync($container, 3)
    Start-Sleep -Seconds 2
    if (![SplitWindows]::IsZoomed($container)) { throw 'Container did not maximize.' }
}
if ($Fullscreen) {
    [SplitWindows]::Foreground($container)
    [SplitWindows]::Click([SplitWindows]::Pane($container, $HostId))
    Start-Sleep -Milliseconds 500
    [SplitWindows]::Key(0x7a, 0)  # F11
    Start-Sleep -Seconds 2
    if (![SplitWindows]::Fullscreen($container)) { throw 'Container did not enter borderless fullscreen.' }
    Write-Output 'Container fills the monitor in borderless fullscreen.'
}
$p1 = [SplitWindows]::Pane($container, $HostId)
$p2 = [SplitWindows]::Pane($container, $GuestId)
$bounds = New-Object SplitWindows+Rect
$origin = New-Object SplitWindows+Point
if ($p1 -eq [IntPtr]::Zero -or $p2 -eq [IntPtr]::Zero -or
    ![SplitWindows]::GetClientRect($container, [ref]$bounds) -or
    ![SplitWindows]::ClientToScreen($container, [ref]$origin)) { throw 'Two visible child windows required.' }
$half = if ($Layout -eq 'top') { [Math]::Floor($bounds.Height / 2) } else { [Math]::Floor($bounds.Width / 2) }
$i = 0
foreach ($pane in $p1, $p2) {
    $rect = New-Object SplitWindows+Rect
    if (![SplitWindows]::GetWindowRect($pane, [ref]$rect)) { throw 'Cannot get child dimensions.' }
    $x = $origin.X
    $y = $origin.Y
    $w = $bounds.Width
    $h = $bounds.Height
    if ($Layout -eq 'top') {
        if ($i) { $y += $half; $h -= $half } else { $h = $half }
    } else {
        if ($i) { $x += $half; $w -= $half } else { $w = $half }
    }
    if ($rect.Left -ne $x -or $rect.Top -ne $y -or $rect.Width -ne $w -or $rect.Height -ne $h) {
        throw "Pane $i expected (${x},$y) ${w}x$h but got ($($rect.Left),$($rect.Top)) $($rect.Width)x$($rect.Height)."
    }
    Write-Output "Pane $i at (${x},$y), ${w}x$h."
    $i++
}
Write-Output "One taskbar-eligible window. Container: $($bounds.Width)x$($bounds.Height)."
if ($Desktop -or $InputCheck -or $AltF4) {
    [SplitWindows]::Foreground($container)
    Start-Sleep -Milliseconds 500
    $foreground = [SplitWindows]::GetForegroundWindow()
    if ($foreground -ne $container -and [SplitWindows]::GetAncestor($foreground, 2) -ne $container) {
        throw "Could not bring the split-screen container to the foreground (wanted $container, foreground $foreground)."
    }
}
if ($InputCheck) {
    foreach ($pane in $p2, $p1) {
        [SplitWindows]::Click($pane)
        Start-Sleep -Milliseconds 500
        if ([SplitWindows]::Focus() -ne $pane) { throw "Click did not focus pane $pane (focus $([SplitWindows]::Focus()))." }
    }
    [SplitWindows]::Key(0x70, 0)  # F1 opens the host overlay
    [SplitWindows]::Key(0x09, 0x11)  # Ctrl+Tab focuses the guest
    Start-Sleep -Milliseconds 500
    if ([SplitWindows]::Focus() -ne $p2) { throw 'Ctrl+Tab did not focus Player 2.' }
    [SplitWindows]::Key(0x70, 0)  # F1 opens the guest overlay
    [SplitWindows]::Key(0x09, 0x11)
    Start-Sleep -Milliseconds 500
    if ([SplitWindows]::Focus() -ne $p1) { throw 'Ctrl+Tab did not return focus to Player 1.' }
    Write-Output 'Click and Ctrl+Tab focused both panes.'
}
if ($Desktop) {
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bitmap.Size)
        $bitmap.Save($Desktop, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    Write-Output "Saved $Desktop"
}
if ($AltF4) { [SplitWindows]::Key(0x73, 0x12) }
