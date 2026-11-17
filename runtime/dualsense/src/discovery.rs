//! Device scans may take tens of milliseconds. Keep them away from live HID
//! reads; only owned path snapshots cross back to the native input worker.
use std::{
    ffi::CString,
    io,
    sync::mpsc::{self, Receiver, Sender, TrySendError},
    thread,
    time::Duration,
};

pub(crate) struct Discovery {
    updates: Receiver<Vec<CString>>,
    // Dropping the sender interrupts the worker's wait. A scan already inside
    // Windows may finish later; it owns no Context, game memory or I/O handles.
    _stop: Sender<()>,
    _worker: thread::JoinHandle<()>,
}
impl Discovery {
    pub(crate) fn spawn(
        mut refresh: impl FnMut() -> Option<Vec<CString>> + Send + 'static,
        interval: Duration,
    ) -> io::Result<Self> {
        let (send, updates) = mpsc::sync_channel(1);
        let (stop, stopped) = mpsc::channel();
        let worker = thread::Builder::new()
            .name("DualSense discovery".into())
            .spawn(move || {
                while let Err(mpsc::RecvTimeoutError::Timeout) = stopped.recv_timeout(interval) {
                    if let Some(paths) = refresh() {
                        match send.try_send(paths) {
                            Ok(()) | Err(TrySendError::Full(_)) => (),
                            Err(TrySendError::Disconnected(_)) => break,
                        }
                    }
                }
            })?;
        Ok(Self {
            updates,
            _stop: stop,
            _worker: worker,
        })
    }
    pub(crate) fn latest(&self) -> Option<Vec<CString>> {
        self.updates.try_iter().last()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn pending_scan_never_blocks_snapshot_or_context_shutdown() {
        let (entered, entry) = mpsc::channel();
        let (release, released) = mpsc::channel();
        let (finished, finish) = mpsc::channel();
        let discovery = Discovery::spawn(
            move || {
                let _ = entered.send(());
                let _ = released.recv();
                let _ = finished.send(());
                Some(vec![CString::new("test-pad").unwrap()])
            },
            Duration::from_millis(1),
        )
        .unwrap();
        entry.recv_timeout(Duration::from_secs(2)).unwrap();
        let (polled, poll) = mpsc::channel();
        let consumer = thread::spawn(move || {
            let empty = discovery.latest().is_none();
            drop(discovery);
            let _ = polled.send(empty);
        });
        let result = poll.recv_timeout(Duration::from_millis(100));
        let _ = release.send(());
        consumer.join().unwrap();
        finish.recv_timeout(Duration::from_secs(2)).unwrap();
        assert_eq!(result.unwrap(), true);
    }
    #[test]
    fn new_paths_arrive_without_a_scan_on_the_consumer() {
        let discovery = Discovery::spawn(
            || Some(vec![CString::new("new-pad").unwrap()]),
            Duration::from_millis(1),
        )
        .unwrap();
        let deadline = std::time::Instant::now() + Duration::from_secs(2);
        loop {
            if let Some(paths) = discovery.latest() {
                assert_eq!(paths, vec![CString::new("new-pad").unwrap()]);
                break;
            }
            assert!(std::time::Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }
    }
}
