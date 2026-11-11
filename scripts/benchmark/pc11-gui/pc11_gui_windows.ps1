param([Parameter(Mandatory=$true)][int]$GuiProcessId)
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Pc11WindowProbe {
    delegate bool Visitor(IntPtr hwnd, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(Visitor visitor, IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder text, int size);
    public static void Read(int pid) {
        EnumWindows((hwnd, parameter) => {
            uint owner; GetWindowThreadProcessId(hwnd, out owner);
            if (owner == pid) {
                var title = new StringBuilder(1024); var kind = new StringBuilder(256);
                GetWindowText(hwnd,title,title.Capacity); GetClassName(hwnd,kind,kind.Capacity);
                Console.WriteLine(hwnd.ToInt64() + " | visible=" + IsWindowVisible(hwnd) + " | enabled=" + IsWindowEnabled(hwnd) + " | " + kind + " | " + title);
            }
            return true;
        },IntPtr.Zero);
    }
}
'@
[Pc11WindowProbe]::Read($GuiProcessId)
