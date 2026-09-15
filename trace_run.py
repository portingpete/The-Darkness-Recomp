import ctypes, sys, struct
from ctypes import wintypes

DEBUG_PROCESS = 0x00000001

class STARTUPINFO(ctypes.Structure):
    _fields_ = [
        ('cb', wintypes.DWORD), ('lpReserved', ctypes.c_char_p), ('lpDesktop', ctypes.c_char_p),
        ('lpTitle', ctypes.c_char_p), ('dwX', wintypes.DWORD), ('dwY', wintypes.DWORD),
        ('dwXSize', wintypes.DWORD), ('dwYSize', wintypes.DWORD), ('dwXCountChars', wintypes.DWORD),
        ('dwYCountChars', wintypes.DWORD), ('dwFillAttribute', wintypes.DWORD), ('dwFlags', wintypes.DWORD),
        ('wShowWindow', wintypes.WORD), ('cbReserved2', wintypes.WORD), ('lpReserved2', ctypes.c_void_p),
        ('hStdInput', wintypes.HANDLE), ('hStdOutput', wintypes.HANDLE), ('hStdError', wintypes.HANDLE)
    ]

class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ('hProcess', wintypes.HANDLE), ('hThread', wintypes.HANDLE),
        ('dwProcessId', wintypes.DWORD), ('dwThreadId', wintypes.DWORD)
    ]

class EXCEPTION_RECORD(ctypes.Structure):
    pass
EXCEPTION_RECORD._fields_ = [
    ('ExceptionCode', wintypes.DWORD),
    ('ExceptionFlags', wintypes.DWORD),
    ('ExceptionRecord', ctypes.POINTER(EXCEPTION_RECORD)),
    ('ExceptionAddress', ctypes.c_void_p),
    ('NumberParameters', wintypes.DWORD),
    ('ExceptionInformation', ctypes.c_ulonglong * 15)
]

class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [
        ('ExceptionRecord', EXCEPTION_RECORD),
        ('dwFirstChance', wintypes.DWORD)
    ]

class CREATE_THREAD_DEBUG_INFO(ctypes.Structure):
    _fields_ = [
        ('hThread', wintypes.HANDLE),
        ('lpThreadLocalBase', ctypes.c_void_p),
        ('lpStartAddress', ctypes.c_void_p)
    ]

class CREATE_PROCESS_DEBUG_INFO(ctypes.Structure):
    _fields_ = [
        ('hFile', wintypes.HANDLE),
        ('hProcess', wintypes.HANDLE),
        ('hThread', wintypes.HANDLE),
        ('lpBaseOfImage', ctypes.c_void_p),
        ('dwDebugInfoFileOffset', wintypes.DWORD),
        ('nDebugInfoSize', wintypes.DWORD),
        ('lpThreadLocalBase', ctypes.c_void_p),
        ('lpStartAddress', ctypes.c_void_p),
        ('lpImageName', ctypes.c_void_p),
        ('fUnicode', wintypes.WORD)
    ]

class EXIT_THREAD_DEBUG_INFO(ctypes.Structure):
    _fields_ = [('dwExitCode', wintypes.DWORD)]

class EXIT_PROCESS_DEBUG_INFO(ctypes.Structure):
    _fields_ = [('dwExitCode', wintypes.DWORD)]

class LOAD_DLL_DEBUG_INFO(ctypes.Structure):
    _fields_ = [
        ('hFile', wintypes.HANDLE),
        ('lpBaseOfDll', ctypes.c_void_p),
        ('dwDebugInfoFileOffset', wintypes.DWORD),
        ('nDebugInfoSize', wintypes.DWORD),
        ('lpImageName', ctypes.c_void_p),
        ('fUnicode', wintypes.WORD)
    ]

class OUTPUT_DEBUG_STRING_INFO(ctypes.Structure):
    _fields_ = [
        ('lpDebugStringData', ctypes.c_void_p),
        ('fUnicode', wintypes.WORD),
        ('nDebugStringLength', wintypes.WORD)
    ]

class DEBUG_EVENT_UNION(ctypes.Union):
    _fields_ = [
        ('Exception', EXCEPTION_DEBUG_INFO),
        ('CreateThread', CREATE_THREAD_DEBUG_INFO),
        ('CreateProcessInfo', CREATE_PROCESS_DEBUG_INFO),
        ('ExitThread', EXIT_THREAD_DEBUG_INFO),
        ('ExitProcess', EXIT_PROCESS_DEBUG_INFO),
        ('LoadDll', LOAD_DLL_DEBUG_INFO),
        ('DebugString', OUTPUT_DEBUG_STRING_INFO)
    ]

class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [
        ('dwDebugEventCode', wintypes.DWORD),
        ('dwProcessId', wintypes.DWORD),
        ('dwThreadId', wintypes.DWORD),
        ('u', DEBUG_EVENT_UNION)
    ]

k32 = ctypes.windll.kernel32
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wintypes.BOOL
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.OpenProcess.restype = wintypes.HANDLE

si = STARTUPINFO()
si.cb = ctypes.sizeof(si)
pi = PROCESS_INFORMATION()

cmd = r'K:\DarkRecomp\build_clang\Release\DarkRecomp.exe'
ok = k32.CreateProcessA(None, cmd.encode(), None, None, False, DEBUG_PROCESS, None, b'K:\\DarkRecomp', ctypes.byref(si), ctypes.byref(pi))
if not ok:
    print('Failed to start process:', k32.GetLastError())
    sys.exit(1)

print('Debugger attached to PID:', pi.dwProcessId)

evt = DEBUG_EVENT()
while True:
    if not k32.WaitForDebugEvent(ctypes.byref(evt), 20000):
        print('Timeout waiting for debug event.')
        break
    
    code = evt.dwDebugEventCode
    continue_status = 0x00010002 # DBG_CONTINUE
    
    if code == 1: # EXCEPTION
        exc = evt.u.Exception.ExceptionRecord
        first = evt.u.Exception.dwFirstChance
        if exc.ExceptionCode not in (0x80000003, 0x40010006):
            print(f'Exception: 0x{exc.ExceptionCode:08X} at 0x{exc.ExceptionAddress:016X} (first_chance={first})')
        if exc.ExceptionCode == 0x80000003:
            continue_status = 0x00010002
        else:
            continue_status = 0x80010001
    elif code == 2: # CREATE_THREAD
        print(f'Thread created: TID {evt.dwThreadId} StartAddress: 0x{evt.u.CreateThread.lpStartAddress:016X}')
    elif code == 3: # CREATE_PROCESS
        print(f'Process created: Base: 0x{evt.u.CreateProcessInfo.lpBaseOfImage:016X}')
    elif code == 4: # EXIT_THREAD
        print(f'Thread exit: TID {evt.dwThreadId} ExitCode: {evt.u.ExitThread.dwExitCode}')
    elif code == 5: # EXIT_PROCESS
        print(f'Process exit: PID {evt.dwProcessId} ExitCode: 0x{evt.u.ExitProcess.dwExitCode:08X} ({evt.u.ExitProcess.dwExitCode})')
        k32.ContinueDebugEvent(evt.dwProcessId, evt.dwThreadId, continue_status)
        break
    elif code == 8: # OUTPUT_DEBUG_STRING
        info = evt.u.DebugString
        hProc = k32.OpenProcess(0x10, False, evt.dwProcessId)
        if hProc:
            length = info.nDebugStringLength
            buf = ctypes.create_string_buffer(length + 2)
            read = ctypes.c_size_t()
            if k32.ReadProcessMemory(hProc, ctypes.c_void_p(info.lpDebugStringData), buf, length, ctypes.byref(read)):
                try:
                    s = buf.raw[:read.value].decode('utf-8', errors='replace')
                    print(f'[ODS] {s.strip()}')
                except:
                    pass
            k32.CloseHandle(hProc)
            
    k32.ContinueDebugEvent(evt.dwProcessId, evt.dwThreadId, continue_status)

k32.CloseHandle(pi.hProcess)
k32.CloseHandle(pi.hThread)
