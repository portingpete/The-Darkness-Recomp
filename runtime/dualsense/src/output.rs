//! Sony output reports with checked overlapped completion. Submit exactly the
//! protocol bytes; Windows may still report its larger HID collection size.
//! Unlike hidapi 2.6.7's immediate-success path, always retrieve the completion.
use std::{
    ffi::CStr,
    io,
    os::windows::{
        ffi::OsStrExt,
        io::{AsRawHandle, FromRawHandle, OwnedHandle},
    },
    ptr,
};
use windows_sys::Win32::{
    Foundation::{
        ERROR_IO_INCOMPLETE, ERROR_IO_PENDING, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE,
        WAIT_TIMEOUT,
    },
    Storage::FileSystem::{
        CreateFileW, WriteFile, FILE_FLAG_OVERLAPPED, FILE_SHARE_READ, FILE_SHARE_WRITE,
        OPEN_EXISTING,
    },
    System::{
        Threading::{CreateEventW, ResetEvent},
        IO::{CancelIoEx, GetOverlappedResultEx, OVERLAPPED},
    },
};

struct Operation {
    file: OwnedHandle,
    event: OwnedHandle,
    overlapped: OVERLAPPED,
    bytes: Vec<u8>,
    pending: bool,
}
impl Operation {
    fn handle(&self) -> HANDLE {
        self.file.as_raw_handle()
    }
    fn finish(&mut self, timeout: u32) -> io::Result<usize> {
        let mut written = 0;
        if unsafe {
            GetOverlappedResultEx(self.handle(), &self.overlapped, &mut written, timeout, 0)
        } != 0
        {
            self.pending = false;
            return Ok(written as usize);
        }
        let error = io::Error::last_os_error();
        self.pending = matches!(
            error.raw_os_error().map(|n| n as u32),
            Some(ERROR_IO_INCOMPLETE | ERROR_IO_PENDING | WAIT_TIMEOUT)
        );
        Err(error)
    }
}
pub struct Output {
    operation: Option<Box<Operation>>,
}
impl Output {
    pub fn open(path: &CStr) -> io::Result<Self> {
        let text = path.to_str().map_err(|_| {
            io::Error::new(io::ErrorKind::InvalidInput, "Invalid HID path encoding")
        })?;
        let wide: Vec<u16> = std::ffi::OsStr::new(text)
            .encode_wide()
            .chain(Some(0))
            .collect();
        let raw = unsafe {
            CreateFileW(
                wide.as_ptr(),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                ptr::null(),
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                ptr::null_mut(),
            )
        };
        if raw == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        Self::from_handle(unsafe { OwnedHandle::from_raw_handle(raw) })
    }
    fn from_handle(file: OwnedHandle) -> io::Result<Self> {
        let raw = unsafe { CreateEventW(ptr::null(), 1, 0, ptr::null()) };
        if raw.is_null() {
            return Err(io::Error::last_os_error());
        }
        let event = unsafe { OwnedHandle::from_raw_handle(raw) };
        let overlapped = OVERLAPPED {
            hEvent: raw,
            ..unsafe { std::mem::zeroed() }
        };
        Ok(Self {
            operation: Some(Box::new(Operation {
                file,
                event,
                overlapped,
                bytes: Vec::new(),
                pending: false,
            })),
        })
    }
    pub fn write(&mut self, report: &[u8]) -> io::Result<usize> {
        let op = self.operation.as_mut().unwrap();
        if op.pending {
            let result = op.finish(0);
            if op.pending {
                return result;
            }
            // An older cancelled write is now terminal. A fresh stop or retry
            // must be allowed even if that older completion was an error.
        }
        // Neither the buffer nor OVERLAPPED moves or is reused while Windows
        // owns a pending operation, including after timeout/cancellation.
        op.bytes.clear();
        op.bytes.extend_from_slice(report);
        unsafe {
            ResetEvent(op.event.as_raw_handle());
        }
        op.overlapped = OVERLAPPED {
            hEvent: op.event.as_raw_handle(),
            ..unsafe { std::mem::zeroed() }
        };
        op.pending = true;
        let started = unsafe {
            WriteFile(
                op.handle(),
                op.bytes.as_ptr(),
                op.bytes.len() as u32,
                ptr::null_mut(),
                &mut op.overlapped,
            )
        };
        if started == 0 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() != Some(ERROR_IO_PENDING as i32) {
                op.pending = false;
                return Err(error);
            }
        }
        let result = op.finish(100);
        if op.pending {
            unsafe {
                CancelIoEx(op.handle(), &op.overlapped);
            }
            let _ = op.finish(0);
        }
        result
    }
}
impl Drop for Output {
    fn drop(&mut self) {
        if let Some(mut op) = self.operation.take() {
            if op.pending {
                unsafe {
                    CancelIoEx(op.handle(), &op.overlapped);
                }
                let _ = op.finish(1000);
                if op.pending {
                    // A broken driver may refuse cancellation. Keep its handles
                    // and buffers alive until process exit rather than free live I/O.
                    std::mem::forget(op);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        fs::{self, OpenOptions},
        os::windows::fs::OpenOptionsExt,
        sync::atomic::{AtomicU32, Ordering},
    };
    static NEXT: AtomicU32 = AtomicU32::new(0);
    fn unique() -> String {
        format!(
            "DarkDualSenseOutput-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        )
    }
    #[test]
    fn writes_exact_report_and_counts_immediate_completion() {
        let path = std::env::temp_dir().join(unique());
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .custom_flags(FILE_FLAG_OVERLAPPED)
            .open(&path)
            .unwrap();
        let report =
            crate::protocol::rumble(crate::protocol::Transport::Bluetooth, 24000, 12000, false);
        let mut output = Output::from_handle(file.into()).unwrap();
        assert_eq!(output.write(&report).unwrap(), 78);
        drop(output);
        assert_eq!(fs::read(&path).unwrap(), report);
        fs::remove_file(path).unwrap();
    }
    #[test]
    fn pending_timeout_is_cancelled_before_buffer_reuse_and_peer_disconnect() {
        use windows_sys::Win32::{
            Foundation::ERROR_PIPE_CONNECTED,
            Storage::FileSystem::PIPE_ACCESS_OUTBOUND,
            System::Pipes::{ConnectNamedPipe, CreateNamedPipeW, PIPE_TYPE_BYTE, PIPE_WAIT},
        };
        let name = format!(r"\\.\pipe\{}", unique());
        let wide: Vec<u16> = std::ffi::OsStr::new(&name)
            .encode_wide()
            .chain(Some(0))
            .collect();
        let raw = unsafe {
            CreateNamedPipeW(
                wide.as_ptr(),
                PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_WAIT,
                1,
                64,
                64,
                0,
                ptr::null(),
            )
        };
        assert_ne!(raw, INVALID_HANDLE_VALUE);
        let file = unsafe { OwnedHandle::from_raw_handle(raw) };
        let reader = OpenOptions::new().read(true).open(&name).unwrap();
        let mut output = Output::from_handle(file).unwrap();
        let op = output.operation.as_mut().unwrap();
        let connected = unsafe { ConnectNamedPipe(op.handle(), &mut op.overlapped) };
        assert!(
            connected != 0
                || io::Error::last_os_error().raw_os_error() == Some(ERROR_PIPE_CONNECTED as i32)
        );
        assert!(output.write(&vec![7; 1024 * 1024]).is_err());
        let op = output.operation.as_mut().unwrap();
        if op.pending {
            let _ = op.finish(1000);
        }
        assert!(!op.pending, "Cancelled write still owns its buffer");
        drop(reader);
        assert!(
            output.write(&[1; 78]).is_err(),
            "Disconnected peer accepted output"
        );
    }
}
