// Native-window diagnostics and capture for the verified audit GUI only.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public static class Pc11GuiNative
{
    public sealed class Control {
        public long Handle; public long Parent; public int Id; public string Class; public string Text;
    }
    public sealed class WindowDetails {
        public long Handle; public string PhysicalBounds,MonitorBounds,WorkBounds;
        public uint Dpi; public bool Maximized;
    }
    public sealed class MeasurementWindowLayout {
        public long MainHandle,WorkbenchHandle;
        public string MainBounds,WorkbenchBounds,WorkAreaBounds;
        public int MainWidth,WorkbenchWidth,WorkbenchHeight,TotalWidth;
        public bool TargetIsMain,NoOverlap,WithinWorkArea,SameMonitor,LayoutValid;
    }
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct MonitorInfo { public int Size; public Rect Monitor, Work; public uint Flags; }
    delegate bool Visitor(IntPtr hwnd, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(Visitor visitor, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr hwnd, Visitor visitor, IntPtr parameter);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out Rect rectangle);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr context, uint flags);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsZoomed(IntPtr hwnd);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd,int command);
    [DllImport("user32.dll")] static extern bool IsChild(IntPtr parent, IntPtr child);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromWindow(IntPtr hwnd,uint flags);
    [DllImport("user32.dll")] static extern bool GetMonitorInfo(IntPtr monitor,ref MonitorInfo info);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd,IntPtr after,int x,int y,int width,int height,uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr hwnd,uint flags);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint message,IntPtr wparam,string lparam,uint flags,uint timeout,out UIntPtr result);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint message,IntPtr wparam,StringBuilder lparam,uint flags,uint timeout,out UIntPtr result);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint message,IntPtr wparam,IntPtr lparam,uint flags,uint timeout,out UIntPtr result);

    static void Check(IntPtr hwnd, int pid) {
        uint owner; GetWindowThreadProcessId(hwnd, out owner);
        if (owner != pid || hwnd == IntPtr.Zero) throw new InvalidOperationException("Native window is not owned by the verified GUI PID");
    }
    static string RectText(Rect bounds) {
        return String.Format("{0},{1},{2},{3}",bounds.Left,bounds.Top,bounds.Right-bounds.Left,bounds.Bottom-bounds.Top);
    }
    static bool Inside(Rect inner,Rect outer) {
        return inner.Left>=outer.Left && inner.Top>=outer.Top && inner.Right<=outer.Right && inner.Bottom<=outer.Bottom;
    }
    static bool Overlap(Rect first,Rect second) {
        return first.Left<second.Right && first.Right>second.Left && first.Top<second.Bottom && first.Bottom>second.Top;
    }
    public static MeasurementWindowLayout ArrangeMeasurementWindows(long mainHandle,long workbenchHandle,int pid) {
        var main=new IntPtr(mainHandle); var workbench=new IntPtr(workbenchHandle);
        Check(main,pid); Check(workbench,pid);
        if(!IsWindowVisible(main) || !IsWindowVisible(workbench))
            throw new InvalidOperationException("Cannot lay out a hidden measurement window");
        Rect mainBounds,workbenchBounds;
        if(!GetWindowRect(main,out mainBounds) || !GetWindowRect(workbench,out workbenchBounds))
            throw new InvalidOperationException("Measurement window bounds unavailable before layout");
        if(main==workbench) {
            return new MeasurementWindowLayout { MainHandle=mainHandle,WorkbenchHandle=workbenchHandle,
                MainBounds=RectText(mainBounds),WorkbenchBounds=RectText(workbenchBounds),
                WorkAreaBounds=Details(mainHandle,pid).WorkBounds,MainWidth=mainBounds.Right-mainBounds.Left,
                WorkbenchWidth=workbenchBounds.Right-workbenchBounds.Left,WorkbenchHeight=workbenchBounds.Bottom-workbenchBounds.Top,
                TotalWidth=mainBounds.Right-mainBounds.Left,TargetIsMain=true,NoOverlap=true,WithinWorkArea=true,
                SameMonitor=true,LayoutValid=true };
        }
        if(GetAncestor(workbench,2)!=workbench)
            throw new InvalidOperationException("Measurement target is not a separate top-level window");
        if(IsZoomed(main)) ShowWindow(main,4); // SW_SHOWNOACTIVATE
        if(IsZoomed(workbench)) ShowWindow(workbench,4); // SW_SHOWNOACTIVATE
        if(IsZoomed(main) || IsZoomed(workbench))
            throw new InvalidOperationException("Could not restore measurement windows without activation");
        var monitor=MonitorFromWindow(main,2);
        var info=new MonitorInfo(); info.Size=Marshal.SizeOf(typeof(MonitorInfo));
        if(monitor==IntPtr.Zero || !GetMonitorInfo(monitor,ref info))
            throw new InvalidOperationException("Measurement monitor work area unavailable");
        int areaWidth=info.Work.Right-info.Work.Left, areaHeight=info.Work.Bottom-info.Work.Top;
        if(areaWidth<2340 || areaHeight<700)
            throw new InvalidOperationException("Monitor work area cannot fit the 1330px main window and 1000px workbench");
        // The workbench contains wide controls at high DPI. Give it all
        // remaining monitor width after the observed 1330px Qt main-window
        // minimum, then place and center using measured dimensions.
        int totalWidth=Math.Min(3200,areaWidth);
        int mainWidth=Math.Min(1330,totalWidth-1000);
        int workbenchWidth=totalWidth-mainWidth;
        int height=Math.Min(1200,areaHeight);
        int left=info.Work.Left+(areaWidth-totalWidth)/2;
        const uint swpNoZOrder=0x0004,swpNoActivate=0x0010;
        uint flags=swpNoZOrder|swpNoActivate;
        if(!SetWindowPos(main,IntPtr.Zero,left,info.Work.Top,mainWidth,height,flags))
            throw new InvalidOperationException("Could not place the main window without activation");
        if(!GetWindowRect(main,out mainBounds))
            throw new InvalidOperationException("Main window bounds unavailable after initial placement");
        int actualMainWidthBeforeWorkbench=mainBounds.Right-mainBounds.Left;
        int workbenchLeft=mainBounds.Left+actualMainWidthBeforeWorkbench;
        if(!SetWindowPos(workbench,IntPtr.Zero,workbenchLeft,info.Work.Top,workbenchWidth,height,flags))
            throw new InvalidOperationException("Could not place the workbench without activation");
        Check(main,pid); Check(workbench,pid);
        if(!IsWindowVisible(main) || !IsWindowVisible(workbench)
            || !GetWindowRect(main,out mainBounds) || !GetWindowRect(workbench,out workbenchBounds))
            throw new InvalidOperationException("Measurement window bounds unavailable after layout");
        int actualMainWidth=mainBounds.Right-mainBounds.Left;
        int actualWorkbenchWidth=workbenchBounds.Right-workbenchBounds.Left;
        int actualWorkbenchHeight=workbenchBounds.Bottom-workbenchBounds.Top;
        int actualTotalWidth=Math.Max(mainBounds.Right,workbenchBounds.Right)-Math.Min(mainBounds.Left,workbenchBounds.Left);
        if(actualTotalWidth>areaWidth)
            throw new InvalidOperationException("Actual minimum measurement-window widths exceed the monitor work area");
        int centeredLeft=info.Work.Left+(areaWidth-actualTotalWidth)/2;
        if(!SetWindowPos(main,IntPtr.Zero,centeredLeft,info.Work.Top,actualMainWidth,height,flags)
            || !SetWindowPos(workbench,IntPtr.Zero,centeredLeft+actualMainWidth,info.Work.Top,actualWorkbenchWidth,height,flags))
            throw new InvalidOperationException("Could not center the measured windows without activation");
        Check(main,pid); Check(workbench,pid);
        if(!IsWindowVisible(main) || !IsWindowVisible(workbench)
            || !GetWindowRect(main,out mainBounds) || !GetWindowRect(workbench,out workbenchBounds))
            throw new InvalidOperationException("Measurement window bounds unavailable after final placement");
        actualMainWidth=mainBounds.Right-mainBounds.Left;
        actualWorkbenchWidth=workbenchBounds.Right-workbenchBounds.Left;
        actualWorkbenchHeight=workbenchBounds.Bottom-workbenchBounds.Top;
        actualTotalWidth=Math.Max(mainBounds.Right,workbenchBounds.Right)-Math.Min(mainBounds.Left,workbenchBounds.Left);
        bool noOverlap=!Overlap(mainBounds,workbenchBounds);
        bool within=Inside(mainBounds,info.Work) && Inside(workbenchBounds,info.Work);
        bool sameMonitor=MonitorFromWindow(main,2)==monitor && MonitorFromWindow(workbench,2)==monitor;
        bool dimensions=actualMainWidth>=640 && actualWorkbenchWidth>=1000 && actualWorkbenchHeight>=700;
        bool widthBound=actualTotalWidth<=areaWidth;
        return new MeasurementWindowLayout { MainHandle=mainHandle,WorkbenchHandle=workbenchHandle,
            MainBounds=RectText(mainBounds),WorkbenchBounds=RectText(workbenchBounds),WorkAreaBounds=RectText(info.Work),
            MainWidth=actualMainWidth,WorkbenchWidth=actualWorkbenchWidth,WorkbenchHeight=actualWorkbenchHeight,
            TotalWidth=actualTotalWidth,TargetIsMain=false,NoOverlap=noOverlap,WithinWorkArea=within,
            SameMonitor=sameMonitor,LayoutValid=noOverlap && within && sameMonitor && dimensions && widthBound };
    }
    public static void FitWindow(long hwnd,int pid) { FitWindow(hwnd,pid,2000,1200); }
    public static void FitWindow(long hwnd,int pid,int maximumWidth,int maximumHeight) {
        var window=new IntPtr(hwnd); Check(window,pid);
        if(!IsWindowVisible(window)) throw new InvalidOperationException("Cannot resize a hidden product window");
        if(IsZoomed(window)) ShowWindow(window,4); // SW_SHOWNOACTIVATE: normal visible state, no focus change.
        var info=new MonitorInfo(); info.Size=Marshal.SizeOf(typeof(MonitorInfo));
        if(!GetMonitorInfo(MonitorFromWindow(window,2),ref info)) throw new InvalidOperationException("Owned window monitor bounds unavailable");
        Check(window,pid);
        int width=Math.Min(maximumWidth,info.Work.Right-info.Work.Left), height=Math.Min(maximumHeight,info.Work.Bottom-info.Work.Top);
        if(width<640 || height<480) throw new InvalidOperationException("Owned product viewport would be too small");
        if(!SetWindowPos(window,IntPtr.Zero,info.Work.Left,info.Work.Top,width,height,0x14))
            throw new InvalidOperationException("Cannot fit owned product window without activation");
    }
    public static string Bounds(long hwnd,int pid) {
        var window=new IntPtr(hwnd); Check(window,pid); Rect bounds;
        if(!GetWindowRect(window,out bounds)) throw new InvalidOperationException("Owned window rectangle unavailable");
        return String.Format("{0},{1},{2},{3}",bounds.Left,bounds.Top,bounds.Right-bounds.Left,bounds.Bottom-bounds.Top);
    }
    public static WindowDetails Details(long hwnd,int pid) {
        var window=new IntPtr(hwnd); Check(window,pid);
        var info=new MonitorInfo(); info.Size=Marshal.SizeOf(typeof(MonitorInfo));
        if(!GetMonitorInfo(MonitorFromWindow(window,2),ref info)) throw new InvalidOperationException("Owned monitor details unavailable");
        return new WindowDetails { Handle=hwnd,PhysicalBounds=Bounds(hwnd,pid),Dpi=GetDpiForWindow(window),Maximized=IsZoomed(window),
            MonitorBounds=String.Format("{0},{1},{2},{3}",info.Monitor.Left,info.Monitor.Top,info.Monitor.Right-info.Monitor.Left,info.Monitor.Bottom-info.Monitor.Top),
            WorkBounds=String.Format("{0},{1},{2},{3}",info.Work.Left,info.Work.Top,info.Work.Right-info.Work.Left,info.Work.Bottom-info.Work.Top) };
    }
    public static long Picker(int pid,string title) {
        long result=0;
        EnumWindows((hwnd,parameter)=> {
            uint owner; GetWindowThreadProcessId(hwnd,out owner); if(owner!=pid || !IsWindowVisible(hwnd)) return true;
            var name=new StringBuilder(1024); var kind=new StringBuilder(256);
            GetWindowText(hwnd,name,name.Capacity); GetClassName(hwnd,kind,kind.Capacity);
            if(name.ToString()==title && kind.ToString()=="#32770") {
                if(result!=0) throw new InvalidOperationException("Ambiguous owned native picker"); result=hwnd.ToInt64();
            }
            return true;
        },IntPtr.Zero);
        return result;
    }
    static void CheckPicker(long hwnd,int pid,string title) {
        Check(new IntPtr(hwnd),pid);
        if(Picker(pid,title)!=hwnd) throw new InvalidOperationException("Native picker title/visibility/ownership changed");
    }
    static string Text(IntPtr hwnd) {
        var text=new StringBuilder(4096); UIntPtr result;
        if(SendMessageTimeout(hwnd,0xD,new IntPtr(text.Capacity),text,2,1000,out result)==IntPtr.Zero)
            throw new InvalidOperationException("Owned native control text query timed out: hwnd="+hwnd.ToInt64());
        return text.ToString();
    }
    public static void CompletePicker(long hwnd,int pid,string title,string path,bool folder,bool save) {
        CheckPicker(hwnd,pid,title); var window=new IntPtr(hwnd); var controls=Controls(hwnd,pid);
        Control edit=null,button=null;
        foreach(var candidate in controls) {
            if(candidate.Class=="Edit") {
                foreach(var parent in controls) if(candidate.Parent==parent.Handle && parent.Id==1148) edit=candidate;
                if(candidate.Id==1148) edit=candidate;
                // Modern IFileDialog uses an actual direct child edit 1152.
                // Verify its adjacent native label before accepting this form.
                if(candidate.Id==1152 && candidate.Parent==hwnd) {
                    foreach(var labelControl in controls)
                        if(labelControl.Id==1090 && labelControl.Parent==hwnd && labelControl.Class=="Static"
                           && Text(new IntPtr(labelControl.Handle)).Replace("&","")==(folder?"Folder:":"File name:")) edit=candidate;
                }
            }
            if(candidate.Class=="Button" && candidate.Id==1) button=candidate;
        }
        if(edit==null || button==null) throw new InvalidOperationException("Native picker has no verified filename/folder edit and commit button");
        var label=Text(new IntPtr(button.Handle)).Replace("&","");
        bool expected=folder?(label=="Select Folder" || label=="Select folder" || label=="Choose"):save?label=="Save":label=="Open";
        if(!expected) throw new InvalidOperationException("Native picker commit label differs: "+label);
        CheckPicker(hwnd,pid,title); Check(new IntPtr(edit.Handle),pid);
        if(!IsChild(window,new IntPtr(edit.Handle)) || GetParent(new IntPtr(edit.Handle)).ToInt64()!=edit.Parent || GetDlgCtrlID(new IntPtr(edit.Handle))!=edit.Id)
            throw new InvalidOperationException("Native filename/folder edit changed before write");
        UIntPtr result;
        if(SendMessageTimeout(new IntPtr(edit.Handle),0xC,IntPtr.Zero,path,2,5000,out result)==IntPtr.Zero || result.ToUInt64()==0)
            throw new InvalidOperationException("Native filename/folder edit rejected WM_SETTEXT");
        if(Text(new IntPtr(edit.Handle))!=path) throw new InvalidOperationException("Native filename/folder edit readback differs");
        CheckPicker(hwnd,pid,title); Check(new IntPtr(button.Handle),pid);
        if(!IsChild(window,new IntPtr(button.Handle)) || GetDlgCtrlID(new IntPtr(button.Handle))!=1 || Text(new IntPtr(button.Handle)).Replace("&","")!=label)
            throw new InvalidOperationException("Native picker commit button changed before click");
        if(SendMessageTimeout(new IntPtr(button.Handle),0xF5,IntPtr.Zero,IntPtr.Zero,2,5000,out result)==IntPtr.Zero)
            throw new InvalidOperationException("Native picker commit click timed out");
    }
    public static Control[] Controls(long hwnd, int pid) {
        var window=new IntPtr(hwnd); Check(window,pid); var result=new List<Control>();
        EnumChildWindows(window,(child,parameter)=> {
            Check(child,pid); var text=new StringBuilder(1024); var kind=new StringBuilder(256);
            GetWindowText(child,text,text.Capacity); GetClassName(child,kind,kind.Capacity);
            result.Add(new Control { Handle=child.ToInt64(), Parent=GetParent(child).ToInt64(), Id=GetDlgCtrlID(child), Class=kind.ToString(), Text=text.ToString() });
            return true;
        },IntPtr.Zero);
        return result.ToArray();
    }
    public static void Capture(long hwnd, int pid, string path) {
        var window=new IntPtr(hwnd); Check(window,pid); Rect bounds;
        if (!GetWindowRect(window,out bounds)) throw new InvalidOperationException("Native window bounds unavailable");
        int width=bounds.Right-bounds.Left, height=bounds.Bottom-bounds.Top;
        if (width<=0 || height<=0 || width>16000 || height>16000) throw new InvalidOperationException("Invalid native window capture bounds");
        using(var bitmap=new Bitmap(width,height)) using(var graphics=Graphics.FromImage(bitmap)) {
            var context=graphics.GetHdc(); bool copied;
            try { copied=PrintWindow(window,context,2); } finally { graphics.ReleaseHdc(context); }
            if (!copied) throw new InvalidOperationException("PrintWindow failed for owned product window");
            bitmap.Save(path,ImageFormat.Png);
        }
    }
}
