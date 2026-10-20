"""Stacks of every thread of a running (or hung) rfg.exe, from outside the process.

python tools/stack_dump.py [--pid N] [--out file.txt]

For each thread: CPU time used over one second (to spot the threads that spin), the instruction pointer and the
return addresses found on its stack (a scan: a few may be stale), with rfg.exe addresses resolved through rfg.pdb
(research/scripts) and other modules shown as module+offset. Works on the 32-bit game from 64-bit Python through
the WOW64 thread-context API; needs no debugger and no administrator rights for a game you started.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from profile_report import Symbols  # noqa: E402

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)

PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
THREAD_SUSPEND_RESUME, THREAD_GET_CONTEXT, THREAD_QUERY_INFORMATION = 0x0002, 0x0008, 0x0040
TH32CS_SNAPTHREAD, TH32CS_SNAPPROCESS = 0x4, 0x2
WOW64_CONTEXT_CONTROL, WOW64_CONTEXT_INTEGER = 0x10001, 0x10002


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', wt.LONG), ('tpDeltaPri', wt.LONG), ('dwFlags', wt.DWORD)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD),
                ('szExeFile', ctypes.c_wchar * 260)]


class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [('ContextFlags', wt.DWORD), ('Dr', wt.DWORD * 6), ('FloatSave', ctypes.c_ubyte * 112),
                ('SegGs', wt.DWORD), ('SegFs', wt.DWORD), ('SegEs', wt.DWORD), ('SegDs', wt.DWORD),
                ('Edi', wt.DWORD), ('Esi', wt.DWORD), ('Ebx', wt.DWORD), ('Edx', wt.DWORD), ('Ecx', wt.DWORD),
                ('Eax', wt.DWORD), ('Ebp', wt.DWORD), ('Eip', wt.DWORD), ('SegCs', wt.DWORD), ('EFlags', wt.DWORD),
                ('Esp', wt.DWORD), ('SegSs', wt.DWORD), ('ExtendedRegisters', ctypes.c_ubyte * 512)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [('lpBaseOfDll', ctypes.c_void_p), ('SizeOfImage', wt.DWORD), ('EntryPoint', ctypes.c_void_p)]


k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE


def find_pid(name='rfg.exe'):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    pid = None
    while ok:
        if e.szExeFile.lower() == name:
            pid = e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def modules_of(proc):
    arr = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(proc, arr, ctypes.sizeof(arr), ctypes.byref(needed), 0x01)  # LIST_MODULES_32BIT
    mods = []
    for i in range(needed.value // ctypes.sizeof(ctypes.c_void_p)):
        if not arr[i]:
            continue
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(proc, ctypes.c_void_p(arr[i]), name, 260)
        mi = MODULEINFO()
        psapi.GetModuleInformation(proc, ctypes.c_void_p(arr[i]), ctypes.byref(mi), ctypes.sizeof(mi))
        mods.append((mi.lpBaseOfDll, mi.lpBaseOfDll + mi.SizeOfImage, name.value))
    return mods


def read(proc, addr, size):
    buf = (ctypes.c_ubyte * size)()
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(proc, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return b''
    return bytes(buf[:got.value])


def thread_cpu_ms(h):
    c, e, k, u = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
    if not k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
        return 0.0
    v = lambda f: ((f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e4
    return v(k) + v(u)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int)
    ap.add_argument('--out')
    ap.add_argument('--depth', type=int, default=40)
    a = ap.parse_args()
    pid = a.pid or find_pid()
    if not pid:
        print('rfg.exe is not running')
        return 1
    proc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not proc:
        print('cannot open process', pid, ctypes.get_last_error())
        return 1
    mods = modules_of(proc)
    rfg = next((m for m in mods if m[2].lower() == 'rfg.exe'), None)
    symbols = Symbols()

    def where(v):
        for lo, hi, name in mods:
            if lo <= v < hi:
                if name.lower() == 'rfg.exe':
                    return '%s (rfg+0x%X)' % (symbols.name(v - lo), v - lo)
                return '%s+0x%X' % (name, v - lo)
        return None

    def follows_call(v):
        b = read(proc, v - 6, 6)
        if len(b) < 6:
            return False
        return (b[1] == 0xE8 or (b[0] == 0xFF and (b[1] & 0x38) == 0x10) or (b[3] == 0xFF and (b[4] & 0x38) == 0x10)
                or (b[4] == 0xFF and (b[5] & 0x38) == 0x10))

    tids = threads_of(pid)
    handles = {}
    for t in tids:
        h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, False, t)
        if h:
            handles[t] = h
    cpu0 = {t: thread_cpu_ms(h) for t, h in handles.items()}
    time.sleep(1.0)
    cpu1 = {t: thread_cpu_ms(h) for t, h in handles.items()}
    lines = ['rfg.exe pid %d, %d threads, rfg.exe at 0x%X' % (pid, len(tids), rfg[0] if rfg else 0)]
    order = sorted(handles, key=lambda t: -(cpu1[t] - cpu0[t]))
    for t in order:
        h = handles[t]
        ctx = WOW64_CONTEXT()
        ctx.ContextFlags = WOW64_CONTEXT_CONTROL | WOW64_CONTEXT_INTEGER
        if k32.Wow64SuspendThread(h) == 0xFFFFFFFF:
            continue
        ok = k32.Wow64GetThreadContext(h, ctypes.byref(ctx))
        stack = b''
        if ok:  # page by page: the read stops at the first page that is not mapped
            addr = ctx.Esp
            while len(stack) < 32 * 1024:
                chunk = read(proc, addr, 0x1000 - (addr & 0xFFF))
                if not chunk:
                    break
                stack += chunk
                addr += len(chunk)
        k32.ResumeThread(h)
        busy = cpu1[t] - cpu0[t]
        lines.append('')
        lines.append('thread %d: %.0f ms CPU in the last second; eip %s' % (t, busy, where(ctx.Eip) or '0x%08X' % ctx.Eip))
        shown = 0
        for i in range(0, len(stack) - 3, 4):
            v = int.from_bytes(stack[i:i + 4], 'little')
            w = where(v)
            if not w or not follows_call(v):
                continue
            lines.append('    ' + w)
            shown += 1
            if shown >= a.depth:
                break
    for h in handles.values():
        k32.CloseHandle(h)
    k32.CloseHandle(proc)
    text = '\n'.join(lines)
    if a.out:
        open(a.out, 'w', encoding='utf-8').write(text + '\n')
    print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
