# Run agentty under a real Windows pseudo console (ConPTY), wait for a frame,
# press Esc and type /quit, and expect a clean exit. Proves the TUI starts,
# draws, reads keys and shuts down on a real console, not just that main()
# gets far.
#   pwsh tests/windows/conpty_smoke.ps1 <agentty.exe>
param([Parameter(Mandatory)] [string] $Exe)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

public static class ConPty {
    [StructLayout(LayoutKind.Sequential)] struct COORD { public short X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)] struct STARTUPINFOEX { public STARTUPINFO StartupInfo; public IntPtr lpAttributeList; }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("kernel32.dll")] static extern int CreatePseudoConsole(COORD size, SafeFileHandle hInput, SafeFileHandle hOutput, uint flags, out IntPtr phPC);
    [DllImport("kernel32.dll")] static extern void ClosePseudoConsole(IntPtr hPC);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CreatePipe(out SafeFileHandle r, out SafeFileHandle w, IntPtr sa, int size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref IntPtr size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attr, IntPtr value, IntPtr size, IntPtr prev, IntPtr retSize);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFOEX si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, uint code);

    // Returns the exit code; -1 if it didn't exit. `screen` gets everything
    // the child wrote, `steps` what happened when.
    public static int Run(string cmdline, int startMs, string keys, int exitMs, out string screen, out string steps) {
        SafeFileHandle inR, inW, outR, outW;
        CreatePipe(out inR, out inW, IntPtr.Zero, 0);
        CreatePipe(out outR, out outW, IntPtr.Zero, 0);
        IntPtr pc;
        int hr = CreatePseudoConsole(new COORD { X = 120, Y = 40 }, inR, outW, 0, out pc);
        if (hr != 0) throw new Exception("CreatePseudoConsole failed: 0x" + hr.ToString("X"));

        IntPtr size = IntPtr.Zero;
        InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
        var si = new STARTUPINFOEX();
        si.StartupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFOEX));
        si.lpAttributeList = Marshal.AllocHGlobal(size);
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, ref size)) throw new Exception("attr list");
        if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, (IntPtr)0x20016 /*PSEUDOCONSOLE*/, pc, (IntPtr)IntPtr.Size, IntPtr.Zero, IntPtr.Zero))
            throw new Exception("UpdateProcThreadAttribute");
        PROCESS_INFORMATION pi;
        if (!CreateProcessW(null, cmdline, IntPtr.Zero, IntPtr.Zero, false, 0x00080000 /*EXTENDED_STARTUPINFO_PRESENT*/,
                            IntPtr.Zero, null, ref si, out pi))
            throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
        inR.Dispose(); outW.Dispose();   // the console owns its ends now

        var buf = new StringBuilder();
        var log = new StringBuilder();
        var reader = new Thread(() => {
            using (var s = new FileStream(outR, FileAccess.Read)) {
                var b = new byte[8192]; int n;
                try { while ((n = s.Read(b, 0, b.Length)) > 0) lock (buf) buf.Append(Encoding.UTF8.GetString(b, 0, n)); }
                catch (IOException) { }
            }
        }) { IsBackground = true };
        reader.Start();
        var input = new FileStream(inW, FileAccess.Write);

        // Wait for the first frame: the composer shows once the TUI is up.
        var t0 = DateTime.UtcNow;
        bool drew = false;
        while ((DateTime.UtcNow - t0).TotalMilliseconds < startMs) {
            if (WaitForSingleObject(pi.hProcess, 0) == 0) break;   // exited early
            lock (buf) if (buf.Length > 200) { drew = true; break; }
            Thread.Sleep(100);
        }
        log.AppendFormat("drew={0} after {1}ms; ", drew, (int)(DateTime.UtcNow - t0).TotalMilliseconds);
        Thread.Sleep(1500);   // let startup settle before typing

        // Type the keys one by one, like a person. A lone Esc needs a pause
        // after it or it reads as the start of an Alt sequence.
        foreach (char c in keys) {
            var b = Encoding.UTF8.GetBytes(c.ToString());
            try { input.Write(b, 0, b.Length); input.Flush(); } catch (IOException) { break; }
            Thread.Sleep(c == '\r' || c == '\x1b' ? 400 : 40);
        }
        log.Append("typed; ");

        uint rc = 0;
        int code = -1;
        if (WaitForSingleObject(pi.hProcess, (uint)exitMs) == 0 && GetExitCodeProcess(pi.hProcess, out rc)) code = unchecked((int)rc);
        else { TerminateProcess(pi.hProcess, 1); log.Append("did not exit, killed; "); }
        ClosePseudoConsole(pc);
        try { input.Dispose(); } catch (IOException) { }
        reader.Join(2000);
        lock (buf) screen = buf.ToString();
        steps = log.ToString();
        return code;
    }
}
'@

$exe = (Resolve-Path $Exe).Path
$screen = ''; $steps = ''
# A fresh home has no account, so the login picker opens first: Esc closes it.
$rc = [ConPty]::Run("`"$exe`"", 30000, "`e/quit`r", 20000, [ref]$screen, [ref]$steps)
$plain = [regex]::Replace($screen, "`e\[[0-9;?]*[ -/]*[@-~]|`e\][^`a]*`a|`e.", '')
Write-Host "steps: $steps"
Write-Host ("exit: {0} (0x{0:X8})" -f $rc)
Write-Host "--- screen (plain, last 1500 chars) ---"
Write-Host ($plain.Substring([Math]::Max(0, $plain.Length - 1500)))

if ($rc -ne 0) {
    $log = Join-Path $env:AGENTTY_HOME 'logs/stderr.log'
    if (Test-Path $log) { Write-Host '--- stderr.log ---'; Get-Content $log -Tail 40 }
    throw ("agentty under ConPTY did not exit cleanly after /quit (exit 0x{0:X8})" -f $rc)
}
if ($screen.Length -lt 200) { throw "agentty exited 0 but never drew a frame" }
Write-Host 'ok: drew a frame, took /quit, exited 0'
