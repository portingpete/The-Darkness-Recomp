//! Bounded external controller for the game's own diagnostic input file.
//! It reads a logged player pose; it never writes process memory or OS input.
use std::{env, fs::{self, OpenOptions}, io::{self, Write, BufWriter}, path::{Path, PathBuf}, thread, time::{Duration, Instant}};
use std::ffi::c_void;

#[link(name="kernel32")]
extern "system" {
    fn QueryPerformanceCounter(value: *mut i64) -> i32;
    fn QueryPerformanceFrequency(value: *mut i64) -> i32;
    fn GetTickCount64() -> u64;
    fn OpenProcess(access: u32, inherit: i32, pid: u32) -> *mut c_void;
    fn CloseHandle(handle: *mut c_void) -> i32;
    fn WaitForSingleObject(handle: *mut c_void, millis: u32) -> u32;
    fn ReadProcessMemory(handle: *mut c_void, address: *const c_void, output: *mut c_void, size: usize, read: *mut usize) -> i32;
    fn QueryFullProcessImageNameW(handle: *mut c_void, flags: u32, name: *mut u16, length: *mut u32) -> i32;
}
#[link(name="user32")]
extern "system" {
    fn EnumWindows(callback: Option<unsafe extern "system" fn(isize, isize) -> i32>, parameter: isize) -> i32;
    fn GetWindowThreadProcessId(window: isize, pid: *mut u32) -> u32;
    fn IsWindowVisible(window: isize) -> i32;
    fn PostMessageW(window: isize, message: u32, w: usize, l: isize) -> i32;
}
fn qpc() -> i64 { let mut value=0; unsafe { QueryPerformanceCounter(&mut value); } value }
fn frequency() -> i64 { let mut value=0; unsafe { QueryPerformanceFrequency(&mut value); } value }
fn json_string(text: &str) -> String {
    let mut result=String::from("\"");
    for c in text.chars() { match c { '\\'=>result.push_str("\\\\"), '"'=>result.push_str("\\\""), '\n'=>result.push_str("\\n"), '\r'=>result.push_str("\\r"), c if c<' '=>result.push_str(&format!("\\u{:04x}",c as u32)), c=>result.push(c) } }
    result.push('"'); result
}
fn field<'a>(line: &'a str, name: &str) -> Option<&'a str> {
    line.split_whitespace().find_map(|word|word.strip_prefix(name).and_then(|rest|rest.strip_prefix('=')))
}
fn latest_state(log: &str) -> Option<(bool,bool)> {
    log.lines().rev().filter(|line|line.starts_with("[DeveloperTest]")).find_map(|line|Some((field(line,"canLoadMission")?=="1",field(line,"hasActivePlayer")?=="1")))
}
fn latest_actor(log: &str) -> Option<(u64,u32)> {
    log.lines().rev().filter(|line|line.starts_with("[DeveloperPlayer]")).find_map(|line| {
        let base=u64::from_str_radix(field(line,"base")?.trim_start_matches("0x"),16).ok()?;
        let actor=u32::from_str_radix(field(line,"actor")?.trim_start_matches("0x"),16).ok()?;
        (base!=0 && actor!=0).then_some((base,actor))
    })
}
fn text(path: &Path) -> String { String::from_utf8_lossy(&fs::read(path).unwrap_or_default()).into_owned() }
fn normalized(path: &Path) -> io::Result<String> {
    Ok(fs::canonicalize(path)?.to_string_lossy().trim_start_matches(r"\\?\").replace('/',r"\").to_lowercase())
}
struct Process(*mut c_void);
impl Drop for Process { fn drop(&mut self) { unsafe { CloseHandle(self.0); } } }
impl Process {
    fn open(pid:u32, expected:&Path)->io::Result<Self> {
        let process=Self(unsafe { OpenProcess(0x100000|0x1000|0x10,0,pid) });
        if process.0.is_null() { return Err(io::Error::last_os_error()); }
        let mut buffer=vec![0u16;32768]; let mut size=buffer.len() as u32;
        if unsafe { QueryFullProcessImageNameW(process.0,0,buffer.as_mut_ptr(),&mut size) }==0 { return Err(io::Error::last_os_error()); }
        let actual=PathBuf::from(String::from_utf16_lossy(&buffer[..size as usize]));
        if normalized(&actual)?!=normalized(expected)? { return Err(io::Error::other("PID does not own expected frozen executable")); }
        Ok(process)
    }
    fn alive(&self)->bool { unsafe { WaitForSingleObject(self.0,0)==258 } }
    fn pose(&self, base:u64, actor:u32)->Option<[f32;3]> {
        let address=base.checked_add(u64::from(actor))?.checked_add(128)?;
        let mut bytes=[0u8;12]; let mut read=0;
        if unsafe { ReadProcessMemory(self.0,address as *const c_void,bytes.as_mut_ptr().cast(),12,&mut read) }==0 || read!=12 { return None; }
        let pose=std::array::from_fn(|index|f32::from_bits(u32::from_be_bytes(bytes[index*4..index*4+4].try_into().unwrap())));
        pose.iter().all(|value|value.is_finite() && value.abs()<100_000_000.0).then_some(pose)
    }
}
struct Controller { case:PathBuf, events:BufWriter<fs::File>, process:Process, pid:u32, begin:Instant }
impl Controller {
    fn event(&mut self,name:&str,detail:&str)->io::Result<()> {
        writeln!(self.events,"{{\"qpc\":{},\"boot_ms\":{},\"event\":{},\"detail\":{}}}",qpc(),unsafe { GetTickCount64() },json_string(name),json_string(detail))
    }
    fn check(&self)->io::Result<()> {
        if !self.process.alive() { return Err(io::Error::other("owned process exited")); }
        if self.begin.elapsed()>Duration::from_secs(230) { return Err(io::Error::other("bounded controller deadline")); }
        Ok(())
    }
    fn sleep(&self,millis:u64)->io::Result<()> {
        let end=Instant::now()+Duration::from_millis(millis);
        while Instant::now()<end { self.check()?; thread::sleep(Duration::from_millis(100)); }
        Ok(())
    }
    fn append(&mut self,commands:&[&str])->io::Result<()> {
        self.check()?;
        let path=self.case.join("input.txt");
        if text(&path).lines().count()+commands.len()>60 { return Err(io::Error::other("input line budget exceeded")); }
        let mut file=OpenOptions::new().create(true).append(true).open(path)?;
        for command in commands { writeln!(file,"{command}")?; }
        file.flush()?; self.event("append",&commands.join(" | "))?; self.events.flush()
    }
    fn run(&mut self,bootstrap:u64)->io::Result<()> {
        self.event("start",&format!("pid={} qpc_frequency={} bootstrap_ms={bootstrap}; ensure DARK_DEVELOPER_PROBE=1; setup is ineligible",self.pid,frequency()))?;
        self.events.flush()?;
        self.sleep(bootstrap)?;
        self.append(&["dev open"])?;
        let mut ready=false;
        for _ in 0..12 {
            self.sleep(1800)?; self.append(&["dev status"])?; self.sleep(1200)?;
            if latest_state(&text(&self.case.join("stdout.log"))).is_some_and(|state|state.0) { ready=true; break; }
        }
        if !ready { return Err(io::Error::other("mission menu readiness not established")); }
        let before=text(&self.case.join("stdout.log")).len();
        self.append(&["dev mission NY1_Chinatown:ny1+layer1","dev open"])?;
        let mut inactive=false; ready=false;
        for _ in 0..22 {
            self.sleep(1500)?; self.append(&["dev status"])?; self.sleep(1000)?;
            let log=text(&self.case.join("stdout.log"));
            let tail=log.get(before..).unwrap_or("");
            inactive|=tail.lines().any(|line|line.starts_with("[DeveloperTest]") && field(line,"hasActivePlayer")==Some("0"));
            if inactive && latest_state(tail).is_some_and(|state|state.1) { ready=true; break; }
        }
        if !ready { return Err(io::Error::other("new active player transition not established")); }
        self.event("new_player_ready","after inactive transition; no setup times accepted")?;
        self.append(&["dev invincible on","dev close"])?;
        self.sleep(2000)?;
        let (base,actor)=latest_actor(&text(&self.case.join("stderr.log"))).ok_or_else(||io::Error::other("missing logged actor identity; enable DARK_DEVELOPER_PROBE=1"))?;
        let start=self.process.pose(base,actor).ok_or_else(||io::Error::other("initial read-only actor pose unavailable"))?;
        self.event("phase_start",&format!("base={base:x} actor={actor:x} position={start:?}; input delivery is asynchronous"))?;
        self.append(&["87 4000","capture","87 3000","capture","68 1600","capture","87 3000","capture"])?;
        let phase=Instant::now(); let mut valid=0; let mut last=start;
        while phase.elapsed()<Duration::from_secs(25) {
            self.check()?;
            match self.process.pose(base,actor) {
                Some(pose)=>{ last=pose; valid+=1; self.event("pose",&format!("position={pose:?}"))?; }
                None=>{ self.event("pose_unavailable","no zero substituted")?; }
            }
            if valid%20==0 { self.events.flush()?; }
            thread::sleep(Duration::from_millis(100));
        }
        let displacement=start.iter().zip(last.iter()).map(|(a,b)|f64::from(b-a).powi(2)).sum::<f64>().sqrt();
        self.event("phase_end",&format!("position={last:?} valid_pose_samples={valid} displacement={displacement}; controller completion does not certify route or fix"))?;
        self.events.flush()?;
        if valid<100 || displacement<200.0 { return Err(io::Error::other("insufficient observed player progress")); }
        let mut close=CloseWindow { pid:self.pid, window:0 };
        unsafe { EnumWindows(Some(find_window),&mut close as *mut CloseWindow as isize); }
        let sent=close.window!=0 && unsafe { PostMessageW(close.window,0x10,0,0) }!=0;
        self.event("close_request",if sent {"WM_CLOSE sent to own visible window"} else {"no close sent; owner must finish bounded process"})?;
        self.events.flush()?; Ok(())
    }
}
struct CloseWindow { pid:u32,window:isize }
unsafe extern "system" fn find_window(window:isize,parameter:isize)->i32 {
    let state=unsafe { &mut *(parameter as *mut CloseWindow) }; let mut pid=0;
    unsafe { GetWindowThreadProcessId(window,&mut pid); }
    if pid==state.pid && unsafe { IsWindowVisible(window) }!=0 { state.window=window; 0 } else { 1 }
}
fn main()->io::Result<()> {
    let args:Vec<_>=env::args_os().collect();
    if args.len()!=5 { eprintln!("Usage: replay-controller CASE_DIR OWNED_PID FROZEN_EXE BOOTSTRAP_MS\nCase must have stdout.log, stderr.log and an initially empty input.txt; launch game with DARK_DEVELOPER_PROBE=1. No process memory writes or OS input."); std::process::exit(2); }
    let case=PathBuf::from(&args[1]); let pid=args[2].to_string_lossy().parse::<u32>().map_err(|_|io::Error::other("invalid PID"))?;
    let bootstrap=args[4].to_string_lossy().parse::<u64>().map_err(|_|io::Error::other("invalid bootstrap"))?;
    if bootstrap>90_000 { return Err(io::Error::other("bootstrap exceeds bound")); }
    if !text(&case.join("input.txt")).is_empty() { return Err(io::Error::other("new empty input required")); }
    let events=OpenOptions::new().write(true).create_new(true).open(case.join("controller-events.jsonl"))?;
    let process=Process::open(pid,Path::new(&args[3]))?;
    let mut controller=Controller { case,events:BufWriter::new(events),process,pid,begin:Instant::now() };
    let result=controller.run(bootstrap);
    if let Err(ref error)=result { controller.event("failed",&error.to_string())?; controller.events.flush()?; }
    result
}
