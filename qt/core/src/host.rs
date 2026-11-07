//! Native host services. No Tauri, browser or webview runtime is linked.
use std::{marker::PhantomData, path::{Path, PathBuf}, sync::{Arc, Mutex}};
#[cfg(not(windows))]
use std::io::Write;
use serde::Serialize;
pub use tokio::task as async_runtime;
pub struct NativeRuntime;
pub struct AppHandle<R = NativeRuntime> { profile: Arc<PathBuf>, marker: PhantomData<fn() -> R> }
impl<R> Clone for AppHandle<R> { fn clone(&self) -> Self { Self { profile: self.profile.clone(), marker: PhantomData } } }
impl<R> AppHandle<R> {
    pub fn new(profile: PathBuf) -> Self { Self { profile: Arc::new(profile), marker: PhantomData } }
    pub fn path(&self) -> PathResolver { PathResolver(self.profile.clone()) }
}
pub trait Manager {}
impl<R> Manager for AppHandle<R> {}
pub mod path { pub enum BaseDirectory { AppConfig } }
pub struct PathResolver(Arc<PathBuf>);
impl PathResolver { pub fn resolve(&self, file: impl AsRef<Path>, _: path::BaseDirectory) -> Result<PathBuf, String> { Ok(self.0.join(file)) } }
pub struct State<'a,T>(&'a T);
impl<'a,T> State<'a,T> { pub fn new(value: &'a T) -> Self { Self(value) } pub fn inner(&self) -> &'a T { self.0 } }
impl<T> std::ops::Deref for State<'_,T> { type Target=T; fn deref(&self) -> &T { self.0 } }
static OUTPUT: Mutex<()> = Mutex::new(());
pub fn write_message(value: &serde_json::Value) -> Result<(), String> {
    let _lock = OUTPUT.lock().map_err(|e| e.to_string())?;
    let mut line = serde_json::to_vec(value).map_err(|e| e.to_string())?;
    line.push(b'\n');
    #[cfg(windows)]
    {
        pipe::write_all(&line).map_err(|e| e.to_string())
    }
    #[cfg(not(windows))]
    {
        let stdout = std::io::stdout();
        let mut out = stdout.lock();
        out.write_all(&line).and_then(|_| out.flush()).map_err(|e| e.to_string())
    }
}
/// Lines from the parent process.
pub fn request_lines() -> impl Iterator<Item = std::io::Result<String>> {
    use std::io::BufRead;
    #[cfg(windows)]
    {
        std::io::BufReader::new(pipe::Stdin).lines()
    }
    #[cfg(not(windows))]
    {
        std::io::stdin().lock().lines()
    }
}
/// Windows stdio that works on synchronous and overlapped pipes alike. The Rust runtime aborts the
/// process when a standard handle answers an operation asynchronously, and which end of a pipe is
/// overlapped is the parent's choice.
#[cfg(windows)]
mod pipe {
    use std::{ffi::c_void, io};
    #[repr(C)]
    struct Overlapped { internal: usize, internal_high: usize, offset: u32, offset_high: u32, event: *mut c_void }
    extern "system" {
        fn GetStdHandle(id: u32) -> *mut c_void;
        fn ReadFile(handle: *mut c_void, buffer: *mut u8, length: u32, done: *mut u32, overlapped: *mut Overlapped) -> i32;
        fn WriteFile(handle: *mut c_void, buffer: *const u8, length: u32, done: *mut u32, overlapped: *mut Overlapped) -> i32;
        fn GetOverlappedResult(handle: *mut c_void, overlapped: *mut Overlapped, done: *mut u32, wait: i32) -> i32;
        fn CreateEventW(attributes: *mut c_void, manual: i32, initial: i32, name: *const u16) -> *mut c_void;
        fn CloseHandle(handle: *mut c_void) -> i32;
        fn GetLastError() -> u32;
    }
    const STD_INPUT: u32 = -10i32 as u32;
    const STD_OUTPUT: u32 = -11i32 as u32;
    const IO_PENDING: u32 = 997;
    const BROKEN_PIPE: u32 = 109;
    const HANDLE_EOF: u32 = 38;
    fn transfer(id: u32, run: impl Fn(*mut c_void, *mut u32, *mut Overlapped) -> i32) -> io::Result<usize> {
        unsafe {
            let handle = GetStdHandle(id);
            let event = CreateEventW(std::ptr::null_mut(), 1, 0, std::ptr::null());
            let mut overlapped = Overlapped { internal: 0, internal_high: 0, offset: 0, offset_high: 0, event };
            let mut done = 0u32;
            let mut ok = run(handle, &mut done, &mut overlapped);
            let mut error = if ok == 0 { GetLastError() } else { 0 };
            if ok == 0 && error == IO_PENDING {
                ok = GetOverlappedResult(handle, &mut overlapped, &mut done, 1);
                error = if ok == 0 { GetLastError() } else { 0 };
            }
            CloseHandle(event);
            if ok != 0 || error == BROKEN_PIPE || error == HANDLE_EOF {
                Ok(if ok != 0 { done as usize } else { 0 })
            } else {
                Err(io::Error::from_raw_os_error(error as i32))
            }
        }
    }
    pub struct Stdin;
    impl io::Read for Stdin {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            transfer(STD_INPUT, |handle, done, overlapped| unsafe { ReadFile(handle, buffer.as_ptr() as *mut u8, buffer.len() as u32, done, overlapped) })
        }
    }
    pub fn write_all(mut bytes: &[u8]) -> io::Result<()> {
        while !bytes.is_empty() {
            let written = transfer(STD_OUTPUT, |handle, done, overlapped| unsafe { WriteFile(handle, bytes.as_ptr(), bytes.len() as u32, done, overlapped) })?;
            if written == 0 {
                return Err(io::ErrorKind::BrokenPipe.into());
            }
            bytes = &bytes[written..];
        }
        Ok(())
    }
}
pub trait Emitter { fn emit<T: Serialize>(&self, name: &str, payload:T) -> Result<(),String>; }
impl<R> Emitter for AppHandle<R> { fn emit<T: Serialize>(&self,name:&str,payload:T)->Result<(),String>{ write_message(&serde_json::json!({"event":name,"payload":payload})) } }
