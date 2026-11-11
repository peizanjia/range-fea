// Windows .NET UI Automation client for the actual Range FEA desktop process.
// No application state, provider, registry, or dataset is injected by this client.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Automation;

public static class Pc11GuiUia
{
    public sealed class WindowHit
    {
        public int X { get; set; }
        public int Y { get; set; }
        public long Handle { get; set; }
        public uint ProcessId { get; set; }
        public string Title { get; set; }
        public string Class { get; set; }
        public bool OwnedByTarget { get; set; }
        public bool OwnedByMain { get; set; }
    }
    public sealed class WindowExposureResult
    {
        public long TargetHandle { get; set; }
        public uint TargetProcessId { get; set; }
        public string TargetTitle { get; set; }
        public string TargetClass { get; set; }
        public long MainWindowHandle { get; set; }
        public bool MainOwnsTarget { get; set; }
        public bool AllSamplePointsOwned { get; set; }
        public WindowHit[] Hits { get; set; }
    }
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint first, uint second, bool attach);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr hwnd);
    delegate bool WindowVisitor(IntPtr hwnd, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(WindowVisitor visitor, IntPtr parameter);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder text, int size);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr hwnd, uint command);
    [StructLayout(LayoutKind.Sequential)] struct Point { public int X,Y; }
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left,Top,Right,Bottom; }
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] static extern bool IsChild(IntPtr parent, IntPtr child);
    [DllImport("user32.dll")] static extern bool ScreenToClient(IntPtr hwnd,ref Point point);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd,out Rect rectangle);
    [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
    public static volatile string LastActionError;
    public static AutomationElement Descendant(AutomationElement dialog, string id, string name, string type)
    {
        var matches=new List<AutomationElement>(); var seen=new HashSet<string>();
        foreach(AutomationElement element in dialog.FindAll(TreeScope.Descendants,Condition.TrueCondition)) {
            var c=element.Current;
            if(!String.IsNullOrEmpty(type) && c.ControlType.ProgrammaticName!="ControlType."+type) continue;
            if((!String.IsNullOrEmpty(id) && (c.AutomationId==id || c.AutomationId.EndsWith("."+id,StringComparison.Ordinal)))
               || (!String.IsNullOrEmpty(name) && c.Name==name)) {
                if(seen.Add(String.Join(",",element.GetRuntimeId()))) matches.Add(element);
            }
        }
        if(matches.Count>1) throw new InvalidOperationException("Ambiguous control inside owned dialog: "+id+" / "+name);
        return matches.Count==1?matches[0]:null;
    }
    public static void CheckDialog(AutomationElement dialog,int pid,string title)
    {
        var c=dialog.Current;
        if(c.ProcessId!=pid || c.ControlType!=ControlType.Window || !c.IsEnabled || c.IsOffscreen || (c.Name!=title && c.Name!=title+" - Range FEA"))
            throw new InvalidOperationException("Owned visible enabled dialog title/PID changed");
        var hwnd=new IntPtr(c.NativeWindowHandle); uint actualPid;
        GetWindowThreadProcessId(hwnd,out actualPid);
        if(hwnd==IntPtr.Zero || actualPid!=pid || !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd))
            throw new InvalidOperationException("Owned dialog native window identity/visibility changed");
        object pattern; bool modal=false;
        if(dialog.TryGetCurrentPattern(WindowPattern.Pattern,out pattern)) modal=((WindowPattern)pattern).Current.IsModal;
        // Qt providers can omit WindowPattern. A disabled native owner in the
        // exact same process supplies an independent modal-window check.
        var owner=GetWindow(hwnd,4); uint ownerPid=0;
        if(owner!=IntPtr.Zero) GetWindowThreadProcessId(owner,out ownerPid);
        if(!modal && !(owner!=IntPtr.Zero && ownerPid==pid && !IsWindowEnabled(owner)))
            throw new InvalidOperationException("Owned file dialog lacks verified modal state");
    }
    public static void EnableDpiAwareness() { SetThreadDpiAwarenessContext(new IntPtr(-4)); }
    public static bool CancelNativePicker(int pid, string title,bool allowQt=false)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hwnd, parameter) => { uint owner; GetWindowThreadProcessId(hwnd, out owner);
            if (owner != pid || !IsWindowVisible(hwnd)) return true;
            var name = new StringBuilder(1024); var kind = new StringBuilder(256);
            GetWindowText(hwnd,name,name.Capacity); GetClassName(hwnd,kind,kind.Capacity);
            bool matchingTitle=name.ToString()==title || name.ToString()==title+" - Range FEA";
            bool matchingClass=kind.ToString()=="#32770" || (allowQt && kind.ToString().StartsWith("Qt",StringComparison.Ordinal));
            if (matchingTitle && matchingClass && IsWindowEnabled(hwnd)) {
                if(found!=IntPtr.Zero) throw new InvalidOperationException("Ambiguous owned picker cancellation");
                found = hwnd;
            }
            return true; }, IntPtr.Zero);
        if (found == IntPtr.Zero) return false;
        if (!PostMessage(found, 0x10, IntPtr.Zero, IntPtr.Zero)) throw new InvalidOperationException("Owned native picker rejected normal WM_CLOSE");
        return true;
    }
    public static void RedirectLogs(Process process, string stdout, string stderr)
    {
        var gate = new object();
        process.OutputDataReceived += (s, e) => { if (e.Data != null) lock (gate) File.AppendAllText(stdout, e.Data + Environment.NewLine); };
        process.ErrorDataReceived += (s, e) => { if (e.Data != null) lock (gate) File.AppendAllText(stderr, e.Data + Environment.NewLine); };
        process.BeginOutputReadLine(); process.BeginErrorReadLine();
    }
    public static void ToggleOff(AutomationElement element)
    {
        var toggle = (TogglePattern)element.GetCurrentPattern(TogglePattern.Pattern);
        if (toggle.Current.ToggleState != ToggleState.Off) toggle.Toggle();
        if (toggle.Current.ToggleState != ToggleState.Off) throw new InvalidOperationException("Checkbox did not turn off");
    }
    public static AutomationElement NativeWindowFor(AutomationElement element)
    {
        while (element != null)
        {
            var hwnd = new IntPtr(element.Current.NativeWindowHandle);
            if (hwnd != IntPtr.Zero) return AutomationElement.FromHandle(GetAncestor(hwnd, 2));
            element = TreeWalker.ControlViewWalker.GetParent(element);
        }
        throw new InvalidOperationException("Accessible control has no native window ancestor");
    }
    public static bool ClassifyExposureHit(bool hitIsTarget,bool hitIsTargetChild,uint hitPid,uint targetPid)
    {
        return hitPid==targetPid && (hitIsTarget || hitIsTargetChild);
    }
    public static WindowExposureResult WindowExposure(long targetHandle,long mainWindowHandle,int pid)
    {
        var target=new IntPtr(targetHandle); var main=new IntPtr(mainWindowHandle);
        uint targetPid,mainPid; Rect bounds,mainBounds;
        GetWindowThreadProcessId(target,out targetPid);
        if(target==IntPtr.Zero || targetPid!=(uint)pid || !IsWindowVisible(target) || !GetWindowRect(target,out bounds))
            throw new InvalidOperationException("Exposure target is not an owned visible window with readable bounds");
        GetWindowThreadProcessId(main,out mainPid);
        if(main==IntPtr.Zero || mainPid!=(uint)pid || !IsWindowVisible(main) || !GetWindowRect(main,out mainBounds))
            throw new InvalidOperationException("Exposure main window is not an owned visible window with readable bounds");
        var targetRootOwner=GetAncestor(target,3); // GA_ROOTOWNER covers Qt MDI/tool windows owned by the main window.
        bool mainOwnsTarget=target==main || IsChild(main,target) || targetRootOwner==main;
        var width=bounds.Right-bounds.Left; var height=bounds.Bottom-bounds.Top;
        if(width<20 || height<20) throw new InvalidOperationException("Exposure target is too small to sample");
        var title=new StringBuilder(1024); var kind=new StringBuilder(256);
        GetWindowText(target,title,title.Capacity); GetClassName(target,kind,kind.Capacity);
        int x1=bounds.Left+width/4, x2=bounds.Left+(3*width)/4;
        int y1=bounds.Top+height/4, y2=bounds.Top+(3*height)/4;
        var points=new Point[] {
            new Point { X=bounds.Left+width/2, Y=bounds.Top+height/2 },
            new Point { X=x1, Y=y1 }, new Point { X=x2, Y=y1 },
            new Point { X=x1, Y=y2 }, new Point { X=x2, Y=y2 }
        };
        var hits=new List<WindowHit>(); bool allOwned=true;
        foreach(var point in points) {
            var hit=WindowFromPoint(point); uint hitPid=0;
            if(hit!=IntPtr.Zero) GetWindowThreadProcessId(hit,out hitPid);
            var hitTitle=new StringBuilder(1024); var hitClass=new StringBuilder(256);
            if(hit!=IntPtr.Zero) { GetWindowText(hit,hitTitle,hitTitle.Capacity); GetClassName(hit,hitClass,hitClass.Capacity); }
            bool inMain=hit==main || IsChild(main,hit);
            bool isTarget=hit==target;
            bool isTargetChild=hit!=target && IsChild(target,hit);
            bool owned=ClassifyExposureHit(isTarget,isTargetChild,hitPid,targetPid);
            allOwned=allOwned && owned;
            hits.Add(new WindowHit { X=point.X, Y=point.Y, Handle=hit.ToInt64(), ProcessId=hitPid,
                Title=hitTitle.ToString(), Class=hitClass.ToString(), OwnedByTarget=owned, OwnedByMain=inMain });
        }
        return new WindowExposureResult { TargetHandle=targetHandle, TargetProcessId=targetPid,
            TargetTitle=title.ToString(), TargetClass=kind.ToString(), MainWindowHandle=mainWindowHandle,
            MainOwnsTarget=mainOwnsTarget, AllSamplePointsOwned=allOwned, Hits=hits.ToArray() };
    }
    public static string PostControl(AutomationElement element,int pid)
    {
        Reveal(element); var control=element.Current;
        if(control.ProcessId!=pid || !control.IsEnabled || control.IsOffscreen)
            throw new InvalidOperationException("Posted gesture target is not owned/enabled/visible");
        var r=control.BoundingRectangle;
        if(r.IsEmpty || r.Width<=0 || r.Height<=0) throw new InvalidOperationException("Posted gesture target has no bounds");
        var window=NativeWindowFor(element); var w=window.Current; var hwnd=new IntPtr(w.NativeWindowHandle);
        uint owner; GetWindowThreadProcessId(hwnd,out owner);
        var title=new StringBuilder(1024); var kind=new StringBuilder(256);
        GetWindowText(hwnd,title,title.Capacity); GetClassName(hwnd,kind,kind.Capacity);
          // Qt accessibility normalizes captions (including mnemonic ampersands).
          // Pin the actual native caption and re-read it before dispatch instead.
          var nativeTitle=title.ToString();
        if(owner!=pid || w.ProcessId!=pid || !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)
              || String.IsNullOrEmpty(nativeTitle) || !kind.ToString().StartsWith("Qt",StringComparison.Ordinal))
            throw new InvalidOperationException("Posted gesture native owner/title/class changed");
        Rect before,after; if(!GetWindowRect(hwnd,out before)) throw new InvalidOperationException("Posted gesture window bounds unavailable");
        var point=new Point { X=(int)(r.Left+r.Width/2),Y=(int)(r.Top+r.Height/2) };
        if(point.X<before.Left || point.X>=before.Right || point.Y<before.Top || point.Y>=before.Bottom
            || !ScreenToClient(hwnd,ref point) || point.X<0 || point.Y<0 || point.X>32767 || point.Y>32767)
            throw new InvalidOperationException("Posted gesture target is outside owned client bounds");
        var current=element.Current;
        GetWindowThreadProcessId(hwnd,out owner); GetWindowText(hwnd,title,title.Capacity);
          if(owner!=pid || !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd) || title.ToString()!=nativeTitle
            || current.ProcessId!=pid || !current.IsEnabled || current.IsOffscreen || current.BoundingRectangle!=r
            || !GetWindowRect(hwnd,out after) || before.Left!=after.Left || before.Top!=after.Top || before.Right!=after.Right || before.Bottom!=after.Bottom)
            throw new InvalidOperationException("Posted gesture identity/geometry changed before dispatch");
        var coordinates=new IntPtr((point.Y<<16)|(point.X&0xffff));
        if(!PostMessage(hwnd,0x200,IntPtr.Zero,coordinates) || !PostMessage(hwnd,0x201,new IntPtr(1),coordinates)
            || !PostMessage(hwnd,0x202,IntPtr.Zero,coordinates)) throw new InvalidOperationException("Owned posted gesture failed");
          return String.Format("hwnd={0};pid={1};title={2};client={3},{4};target={5};globalInput=false;accessibleName={6}",hwnd.ToInt64(),pid,nativeTitle,point.X,point.Y,r,w.Name);
    }

    public static string CloseWindow(AutomationElement window,int pid)
    {
        var c=window.Current; var hwnd=new IntPtr(c.NativeWindowHandle);
        uint owner; GetWindowThreadProcessId(hwnd,out owner);
        var title=new StringBuilder(1024); var kind=new StringBuilder(256);
        GetWindowText(hwnd,title,title.Capacity); GetClassName(hwnd,kind,kind.Capacity);
        var nativeTitle=title.ToString(); Rect before,after;
        if(c.ProcessId!=pid || owner!=pid || c.ControlType!=ControlType.Window || hwnd==IntPtr.Zero
            || !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd) || String.IsNullOrEmpty(nativeTitle)
            || !kind.ToString().StartsWith("Qt",StringComparison.Ordinal) || !GetWindowRect(hwnd,out before))
            throw new InvalidOperationException("Normal Close target is not an owned visible enabled Qt window");
        GetWindowThreadProcessId(hwnd,out owner); GetWindowText(hwnd,title,title.Capacity);
        if(owner!=pid || title.ToString()!=nativeTitle || !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)
            || !GetWindowRect(hwnd,out after) || before.Left!=after.Left || before.Top!=after.Top
            || before.Right!=after.Right || before.Bottom!=after.Bottom)
            throw new InvalidOperationException("Normal Close identity/title/geometry changed");
        if(!PostMessage(hwnd,0x10,IntPtr.Zero,IntPtr.Zero)) throw new InvalidOperationException("Owned normal Close failed");
        return String.Format("hwnd={0};pid={1};title={2};method=normal-WM_CLOSE;globalInput=false",hwnd.ToInt64(),pid,nativeTitle);
    }

    public static AutomationElement[] Elements(int pid)
    {
        var result = new List<AutomationElement>();
        var handles = new List<IntPtr>();
        // Do not query accessibility providers belonging to other desktop apps.
        EnumWindows((hwnd, parameter) => { uint owner; GetWindowThreadProcessId(hwnd, out owner);
            // A Qt Invoke call can remain inside native modal exec(). Querying
            // its disabled owner provider blocks; the enabled native picker has
            // its own accessible provider and must be traversed directly.
            if (owner == pid && IsWindowVisible(hwnd) && IsWindowEnabled(hwnd)) handles.Add(hwnd); return true; }, IntPtr.Zero);
        foreach (var hwnd in handles)
        {
            try
            {
                var window = AutomationElement.FromHandle(hwnd);
                if (window == null || window.Current.ProcessId != pid) continue;
                result.Add(window);
                var children = window.FindAll(TreeScope.Descendants, Condition.TrueCondition);
                foreach (AutomationElement child in children) result.Add(child);
            }
            catch (ElementNotAvailableException) { }
            catch (COMException e) { if (!Transient(e)) throw; }
        }
        return result.ToArray();
    }
    static bool Transient(COMException e)
    {
        // Qt recreates native popup accessibility providers while menus expand.
        return e.ErrorCode == unchecked((int)0x80004005) || e.ErrorCode == unchecked((int)0x80010108)
            || e.ErrorCode == unchecked((int)0x80040201);
    }

    public static AutomationElement Find(int pid, string id, string name, string type, bool visible)
    {
        var matches = new List<AutomationElement>();
        var identities = new HashSet<string>();
        foreach (var element in Elements(pid))
        {
            try
            {
                var c = element.Current;
                if (visible && c.IsOffscreen) continue;
                if (!String.IsNullOrEmpty(type) && c.ControlType.ProgrammaticName != "ControlType." + type) continue;
                if ((!String.IsNullOrEmpty(id) && (c.AutomationId == id || c.AutomationId.EndsWith("." + id, StringComparison.Ordinal))) ||
                    (!String.IsNullOrEmpty(name) && (c.Name == name || c.Name == name + " - Range FEA")))
                {
                    var identity=String.Join(",",element.GetRuntimeId());
                    if(identities.Add(identity)) matches.Add(element);
                }
            }
            catch (ElementNotAvailableException) { }
            catch (COMException e) { if (!Transient(e)) throw; }
        }
        if (matches.Count > 1) throw new InvalidOperationException("Ambiguous accessible control: " + id + " / " + name);
        return matches.Count == 1 ? matches[0] : null;
    }

    public static void Foreground(AutomationElement window, bool maximize)
    {
        var hwnd = new IntPtr(window.Current.NativeWindowHandle);
        if (hwnd == IntPtr.Zero) throw new InvalidOperationException("Window has no native handle");
        ShowWindow(hwnd, maximize ? 3 : 9);
        SetForegroundWindow(hwnd);
        if (GetForegroundWindow() != hwnd)
        {
            uint ignored;
            var foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), out ignored);
            var clientThread = GetCurrentThreadId();
            bool attached = foregroundThread != 0 && foregroundThread != clientThread && AttachThreadInput(clientThread, foregroundThread, true);
            try { BringWindowToTop(hwnd); SetForegroundWindow(hwnd); }
            finally { if (attached) AttachThreadInput(clientThread, foregroundThread, false); }
        }
        // ShowWindow can already have activated the window, in which case the
        // subsequent SetForegroundWindow return value need not report a change.
        if (GetForegroundWindow() != hwnd) throw new InvalidOperationException("Cannot foreground verified window");
    }

    public static bool FullyVisible(AutomationElement element)
    {
        var c=element.Current; var r=c.BoundingRectangle;
        if(c.IsOffscreen || r.IsEmpty || r.Width<=0 || r.Height<=0) return false;
        var window=NativeWindowFor(element); var hwnd=new IntPtr(window.Current.NativeWindowHandle); Rect bounds;
        if(!IsWindowVisible(hwnd) || !GetWindowRect(hwnd,out bounds) || r.Left<bounds.Left || r.Top<bounds.Top || r.Right>bounds.Right || r.Bottom>bounds.Bottom) return false;
        var parent=TreeWalker.ControlViewWalker.GetParent(element);
        while(parent!=null) {
            var p=parent.Current;
            if(p.AutomationId.EndsWith(".qt_scrollarea_viewport",StringComparison.Ordinal)) {
                var clip=p.BoundingRectangle;
                if(clip.IsEmpty || !clip.Contains(r)) return false;
            }
            parent=TreeWalker.ControlViewWalker.GetParent(parent);
        }
        return true;
    }
    public static void Reveal(AutomationElement element)
    {
        object pattern;
        if (element.TryGetCurrentPattern(ScrollItemPattern.Pattern, out pattern))
            ((ScrollItemPattern)pattern).ScrollIntoView();
        if (FullyVisible(element)) return;
        // Qt scroll areas expose ScrollPattern on their accessible ancestors.
        var parent = TreeWalker.ControlViewWalker.GetParent(element);
        while (parent != null)
        {
            var p=parent.Current;
            if(p.ClassName=="QScrollArea" || p.AutomationId.EndsWith(".QScrollArea",StringComparison.Ordinal)) {
                for(int attempt=0;attempt<8 && !FullyVisible(element);attempt++) {
                    var viewport=parent.FindFirst(TreeScope.Descendants,new PropertyCondition(AutomationElement.AutomationIdProperty,p.AutomationId+".qt_scrollarea_viewport"));
                    var clip=viewport!=null?viewport.Current.BoundingRectangle:p.BoundingRectangle;
                    var target=element.Current.BoundingRectangle;
                    double delta=(target.Top+target.Height/2)-(clip.Top+clip.Height/2);
                    bool adjusted=false;
                    if(parent.TryGetCurrentPattern(ScrollPattern.Pattern,out pattern)) {
                        var scroll=(ScrollPattern)pattern; var s=scroll.Current;
                        if(s.VerticallyScrollable && s.VerticalViewSize>0 && s.VerticalViewSize<100 && clip.Height>0) {
                            double extent=clip.Height*(100/s.VerticalViewSize-1);
                            scroll.SetScrollPercent(ScrollPattern.NoScroll,Math.Max(0,Math.Min(100,s.VerticalScrollPercent+100*delta/extent)));
                            adjusted=true;
                        }
                    }
                    if(!adjusted) {
                        var bars=parent.FindAll(TreeScope.Descendants,new PropertyCondition(AutomationElement.ControlTypeProperty,ControlType.ScrollBar));
                        foreach(AutomationElement bar in bars) {
                            if(bar.Current.IsOffscreen || bar.Current.Orientation!=OrientationType.Vertical) continue;
                            if(bar.TryGetCurrentPattern(RangeValuePattern.Pattern,out pattern)) {
                                var range=(RangeValuePattern)pattern; var v=range.Current;
                                if(!v.IsReadOnly) {
                                    var hwnd=new IntPtr(NativeWindowFor(element).Current.NativeWindowHandle);
                                    double scale=Math.Max(1,GetDpiForWindow(hwnd)/96.0);
                                    range.SetValue(Math.Max(v.Minimum,Math.Min(v.Maximum,v.Value+delta/scale)));
                                    adjusted=true; break;
                                }
                            }
                        }
                    }
                    if(!adjusted) break;
                    Thread.Sleep(80);
                }
            }
            if(FullyVisible(element)) return;
            parent = TreeWalker.ControlViewWalker.GetParent(parent);
        }
        if (!FullyVisible(element)) throw new InvalidOperationException("Control is outside the actual owned window/scroll viewport");
    }

    public static void Act(AutomationElement element)
    {
        if (!element.Current.IsEnabled) throw new InvalidOperationException("Accessible control is disabled");
        object pattern;
        if (element.TryGetCurrentPattern(SelectionItemPattern.Pattern, out pattern))
        { ((SelectionItemPattern)pattern).Select(); return; }
        Reveal(element);
        if (element.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out pattern))
        { ((ExpandCollapsePattern)pattern).Expand(); return; }
        if (element.TryGetCurrentPattern(InvokePattern.Pattern, out pattern))
        {
            // A native modal file picker can keep Invoke blocked. The controlling
            // PowerShell thread must remain free to operate that visible picker.
            LastActionError = null;
            var invoke = (InvokePattern)pattern;
            var worker = new Thread(() => { try { invoke.Invoke(); } catch (Exception e) { LastActionError = e.ToString(); } });
            worker.IsBackground = true; worker.SetApartmentState(ApartmentState.MTA); worker.Start(); return;
        }
        // Qt menus may expose no pattern. Click only this exact accessible control.
        RequireForeground(element.Current.ProcessId);
        var r = element.Current.BoundingRectangle;
        if (r.IsEmpty || r.Width <= 0 || r.Height <= 0) throw new InvalidOperationException("Control has no visible bounds");
        if (!SetCursorPos((int)(r.Left + r.Width / 2), (int)(r.Top + r.Height / 2)))
            throw new InvalidOperationException("Cannot position pointer on control");
        mouse_event(2, 0, 0, 0, UIntPtr.Zero); mouse_event(4, 0, 0, 0, UIntPtr.Zero);
    }
    public static bool IsSelected(AutomationElement element)
    {
        object pattern;
        if(!element.TryGetCurrentPattern(SelectionItemPattern.Pattern,out pattern))
            throw new InvalidOperationException("Selected model tree item lacks SelectionItemPattern");
        return ((SelectionItemPattern)pattern).Current.IsSelected;
    }
    public static void RequireForeground(int pid)
    {
        uint currentPid; GetWindowThreadProcessId(GetForegroundWindow(), out currentPid);
        if (currentPid != pid) throw new InvalidOperationException("Verified GUI no longer owns the desktop foreground; no pointer or keyboard input was sent");
    }

    public static void SetText(AutomationElement element, string text)
    {
        object pattern;
        if (!element.TryGetCurrentPattern(ValuePattern.Pattern, out pattern))
            throw new InvalidOperationException("Accessible edit lacks ValuePattern");
        var value = (ValuePattern)pattern;
        if (value.Current.IsReadOnly) throw new InvalidOperationException("Accessible edit is read-only");
        value.SetValue(text);
    }
    public static string Value(AutomationElement element)
    {
        object pattern;
        if(element.TryGetCurrentPattern(ValuePattern.Pattern,out pattern)) return ((ValuePattern)pattern).Current.Value;
        if(element.TryGetCurrentPattern(RangeValuePattern.Pattern,out pattern)) return ((RangeValuePattern)pattern).Current.Value.ToString(System.Globalization.CultureInfo.InvariantCulture);
        throw new InvalidOperationException("Accessible control has no readable value pattern");
    }
}
