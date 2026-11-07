use std::{
    io::{self, Read, Write},
    path::Path,
    process::{Child, Command, ExitStatus, Output, Stdio},
    sync::Mutex,
    thread,
    time::{Duration, Instant},
};

#[cfg(target_os = "windows")]
use std::os::windows::process::CommandExt;

use ffmpeg_sidecar::{child::FfmpegChild, command::FfmpegCommand};
#[cfg(target_family = "unix")]
use libc;
use once_cell::sync::Lazy;

use crate::settings::{read_settings, FfmpegSettings};

pub static MEDIA_MOVE_LOCK: Lazy<Mutex<()>> = Lazy::new(|| Mutex::new(()));

/// A QA run of qt/tests. Only debug builds and builds with the `qa` feature honour ROOM237_QA_PORT.
pub(crate) fn qa_run() -> bool {
    cfg!(any(debug_assertions, feature = "qa")) && std::env::var_os("ROOM237_QA_PORT").is_some()
}

static QA_FENCE: Lazy<Option<std::path::PathBuf>> = Lazy::new(|| {
    if !qa_run() {
        return None;
    }
    std::fs::canonicalize(std::env::var_os("ROOM237_QA_FENCE")?).ok()
});

/// QA runs only. The client's fence cannot see a root swapped under a background job that is already queued.
pub(crate) fn qa_path_allowed(path: &Path) -> bool {
    let Some(fence) = QA_FENCE.as_ref() else { return true };
    match std::fs::canonicalize(path) {
        Ok(real) => real.starts_with(fence),
        Err(_) => true,
    }
}

pub static STORE_WRITE_LOCK: Lazy<Mutex<()>> = Lazy::new(|| Mutex::new(()));

/// Shared budget across imports, background workers and foreground requests.
/// Raising a worker setting cannot launch dozens of original-image decoders.
pub(crate) struct DecodePermit;
static DECODERS: Lazy<(Mutex<usize>, std::sync::Condvar)> =
    Lazy::new(|| (Mutex::new(0), std::sync::Condvar::new()));
impl DecodePermit {
    pub(crate) fn acquire() -> Self {
        let mut active = DECODERS.0.lock().unwrap();
        while *active >= 4 {
            active = DECODERS.1.wait(active).unwrap();
        }
        *active += 1;
        Self
    }
}
impl Drop for DecodePermit {
    fn drop(&mut self) {
        *DECODERS.0.lock().unwrap() -= 1;
        DECODERS.1.notify_one();
    }
}

// Keep the lock inode in place: unlinking it lets another process lock a
// different inode while a waiter still holds the original file open.
// Closing the handle (including process death) releases the OS lock.
pub(crate) struct FileLock {
    _file: std::fs::File,
}

impl Drop for FileLock {
    fn drop(&mut self) {
        // Closing just this descriptor may leave the lease held by a duplicate
        // or an inherited descriptor. Release ownership at the guard boundary.
        loop {
            match self._file.unlock() {
                Ok(()) => break,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => {
                    log::warn!("Cannot release file lease: {error}");
                    break;
                }
            }
        }
    }
}

impl FileLock {
    pub(crate) fn owns_path(&self, path: &Path) -> bool {
        let Ok(current) = std::fs::symlink_metadata(path) else {
            return false;
        };
        let Ok(owned) = self._file.metadata() else {
            return false;
        };
        if !current.is_file() {
            return false;
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::MetadataExt;
            current.dev() == owned.dev() && current.ino() == owned.ino()
        }
        #[cfg(not(unix))]
        {
            current.created().is_ok() && current.created().ok() == owned.created().ok()
        }
    }
    pub(crate) fn try_acquire(path: &Path) -> Result<Option<Self>, String> {
        Self::try_acquire_with_mode(path, false)
    }

    pub(crate) fn try_acquire_shared(path: &Path) -> Result<Option<Self>, String> {
        Self::try_acquire_with_mode(path, true)
    }

    fn try_acquire_with_mode(path: &Path, shared: bool) -> Result<Option<Self>, String> {
        let file = Self::open(path)?;
        loop {
            let result = if shared {
                file.try_lock_shared()
            } else {
                file.try_lock()
            };
            match result {
                Ok(()) => return Ok(Some(Self { _file: file })),
                Err(std::fs::TryLockError::WouldBlock) => return Ok(None),
                Err(std::fs::TryLockError::Error(error))
                    if error.kind() == io::ErrorKind::Interrupted => {}
                Err(std::fs::TryLockError::Error(error)) => return Err(error.to_string()),
            }
        }
    }

    pub(crate) fn acquire(path: &Path, timeout: Duration, poll: Duration) -> Result<Self, String> {
        Self::acquire_with_mode(path, timeout, poll, false)
    }

    pub(crate) fn acquire_shared(
        path: &Path,
        timeout: Duration,
        poll: Duration,
    ) -> Result<Self, String> {
        Self::acquire_with_mode(path, timeout, poll, true)
    }

    fn acquire_with_mode(
        path: &Path,
        timeout: Duration,
        poll: Duration,
        shared: bool,
    ) -> Result<Self, String> {
        let file = Self::open(path)?;
        let started = Instant::now();
        loop {
            let acquired = if shared {
                file.try_lock_shared()
            } else {
                file.try_lock()
            };
            match acquired {
                Ok(()) => return Ok(Self { _file: file }),
                Err(std::fs::TryLockError::WouldBlock) => {
                    if started.elapsed() >= timeout {
                        return Err(format!("File lock timed out: {}", path.display()));
                    }
                    thread::sleep(poll.max(Duration::from_millis(1)));
                }
                Err(std::fs::TryLockError::Error(error))
                    if error.kind() == io::ErrorKind::Interrupted => {}
                Err(std::fs::TryLockError::Error(error)) => return Err(error.to_string()),
            }
        }
    }

    fn open(path: &Path) -> Result<std::fs::File, String> {
        let mut options = std::fs::OpenOptions::new();
        options.read(true).write(true).create(true).truncate(false);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.custom_flags(libc::O_NOFOLLOW).mode(0o600);
        }
        if fs_is_symlink(path) {
            return Err("File lease cannot follow a symlink".into());
        }
        let file = options
            .open(path)
            .map_err(|error| format!("Cannot open lock {}: {error}", path.display()))?;
        if !file
            .metadata()
            .map_err(|error| error.to_string())?
            .is_file()
        {
            return Err(format!("Lock is not a regular file: {}", path.display()));
        }
        Ok(file)
    }
}

fn fs_is_symlink(path: &Path) -> bool {
    std::fs::symlink_metadata(path).is_ok_and(|info| info.file_type().is_symlink())
}

pub fn validate_file_name(name: &str) -> Result<(), String> {
    if name.is_empty() || name == "." || name == ".." || name.contains(['/', '\\', ':', '\0']) {
        return Err("Expected a single file name".to_string());
    }
    Ok(())
}

// Rename without replacing a concurrently created destination.
pub fn rename_noclobber(source: &Path, target: &Path) -> io::Result<()> {
    #[cfg(any(target_os = "macos", target_os = "linux"))]
    {
        use std::{ffi::CString, os::unix::ffi::OsStrExt};
        let source = CString::new(source.as_os_str().as_bytes())?;
        let target = CString::new(target.as_os_str().as_bytes())?;
        #[cfg(target_os = "macos")]
        let result =
            unsafe { libc::renamex_np(source.as_ptr(), target.as_ptr(), libc::RENAME_EXCL) };
        #[cfg(target_os = "linux")]
        let result = unsafe {
            libc::renameat2(
                libc::AT_FDCWD,
                source.as_ptr(),
                libc::AT_FDCWD,
                target.as_ptr(),
                libc::RENAME_NOREPLACE,
            )
        };
        if result == 0 {
            Ok(())
        } else {
            Err(io::Error::last_os_error())
        }
    }
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::ffi::OsStrExt;
        #[link(name = "kernel32")]
        extern "system" {
            fn MoveFileW(existing: *const u16, new: *const u16) -> i32;
        }
        let source: Vec<u16> = source.as_os_str().encode_wide().chain(Some(0)).collect();
        let target: Vec<u16> = target.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe { MoveFileW(source.as_ptr(), target.as_ptr()) } != 0 {
            Ok(())
        } else {
            Err(io::Error::last_os_error())
        }
    }
    #[cfg(not(any(target_os = "macos", target_os = "linux", target_os = "windows")))]
    {
        std::fs::hard_link(source, target)?;
        if let Err(error) = std::fs::remove_file(source) {
            let _ = std::fs::remove_file(target);
            return Err(error);
        }
        Ok(())
    }
}

pub fn atomic_write(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let parent = path.parent().ok_or("Missing parent directory")?;
    std::fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    atomic_write_owned(path, bytes).map(|_| ())
}

// The parent must already exist: metadata writes must not recreate a deleted album.
// Keep the published file open and retain its pre-publication identity for cache validation.
pub(crate) fn atomic_write_owned(
    path: &Path,
    bytes: &[u8],
) -> Result<(std::fs::File, std::fs::Metadata), String> {
    let parent = path.parent().ok_or("Missing parent directory")?;
    let mut file = tempfile::NamedTempFile::new_in(parent).map_err(|e| e.to_string())?;
    file.write_all(bytes).map_err(|e| e.to_string())?;
    file.as_file().sync_all().map_err(|e| e.to_string())?;
    let metadata = file.as_file().metadata().map_err(|e| e.to_string())?;
    file.persist(path)
        .map(|file| (file, metadata))
        .map_err(|e| e.to_string())
}

// Drain while the process runs: waiting first can deadlock on a full pipe.
fn drain_pipe<R: Read + Send + 'static>(mut pipe: R) -> thread::JoinHandle<io::Result<Vec<u8>>> {
    thread::spawn(move || {
        const LIMIT: usize = 4 * 1024 * 1024;
        let mut retained = Vec::new();
        let mut chunk = [0u8; 8192];
        loop {
            let size = pipe.read(&mut chunk)?;
            if size == 0 {
                break;
            }
            let keep = size.min(LIMIT.saturating_sub(retained.len()));
            retained.extend_from_slice(&chunk[..keep]);
        }
        Ok(retained)
    })
}

pub fn wait_ffmpeg_with_timeout(
    child: &mut FfmpegChild,
    timeout: Duration,
) -> Result<ExitStatus, String> {
    let stdout = child.take_stdout().map(drain_pipe);
    let stderr = child.take_stderr().map(drain_pipe);
    let result = wait_with_timeout(child, timeout);
    for reader in [stdout, stderr].into_iter().flatten() {
        let _ = reader.join();
    }
    result
}

pub fn has_extension(path: &Path, exts: &[&str]) -> bool {
    path.extension()
        .and_then(|s| s.to_str())
        .map(|e| {
            let e = e.to_ascii_lowercase();
            exts.iter().any(|&x| x == e)
        })
        .unwrap_or(false)
}

pub fn apply_ffmpeg_tuning(cmd: &mut FfmpegCommand, is_video: bool) {
    apply_ffmpeg_tuning_with_settings(cmd, is_video, &read_settings().ffmpeg);
}

pub(crate) fn apply_ffmpeg_tuning_with_settings(
    cmd: &mut FfmpegCommand,
    is_video: bool,
    settings: &FfmpegSettings,
) {
    let threads = settings.threads.resolved();
    let mut hwaccel = settings.hwaccel.to_ascii_lowercase();
    if !hardware_decoding_usable() {
        hwaccel = "none".to_string();
    }
    if is_video {
        match hwaccel.as_str() {
            "none" => {}
            other => {
                cmd.arg("-hwaccel").arg(other);
            }
        }
    }
    cmd.arg("-threads").arg(threads.to_string());
}

/// Static Linux FFmpeg builds load VAAPI lazily and abort the process when libva is missing.
#[cfg(target_os = "linux")]
fn hardware_decoding_usable() -> bool {
    static USABLE: once_cell::sync::Lazy<bool> = once_cell::sync::Lazy::new(|| unsafe {
        ["libva.so.2\0", "libva-drm.so.2\0"].iter().all(|name| {
            let handle = libc::dlopen(name.as_ptr().cast(), libc::RTLD_LAZY);
            if !handle.is_null() {
                libc::dlclose(handle);
            }
            !handle.is_null()
        })
    });
    *USABLE
}
#[cfg(not(target_os = "linux"))]
fn hardware_decoding_usable() -> bool {
    true
}

pub(crate) fn apply_command_tuning_with_settings(
    cmd: &mut Command,
    is_video: bool,
    settings: &FfmpegSettings,
) {
    let threads = settings.threads.resolved();
    let mut hwaccel = settings.hwaccel.to_ascii_lowercase();
    if !hardware_decoding_usable() {
        hwaccel = "none".to_string();
    }
    if is_video {
        match hwaccel.as_str() {
            "none" => {}
            other => {
                cmd.arg("-hwaccel").arg(other);
            }
        }
    }
    cmd.arg("-threads").arg(threads.to_string());
}

pub fn ffmpeg_timeout() -> Duration {
    Duration::from_secs(read_settings().ffmpeg.timeout_secs)
}

pub fn set_low_priority_current_thread() {
    #[cfg(target_family = "unix")]
    unsafe {
        let _ = libc::setpriority(libc::PRIO_PROCESS, 0, 19);
    }
    #[cfg(not(target_family = "unix"))]
    {
        // ? Not supported cross-platform; best-effort no-op.
    }
}

pub trait WaitableChild {
    fn try_wait(&mut self) -> io::Result<Option<ExitStatus>>;
    fn kill(&mut self) -> io::Result<()>;
    fn wait(&mut self) -> io::Result<ExitStatus>;
}

impl WaitableChild for Child {
    fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        Child::try_wait(self)
    }
    fn kill(&mut self) -> io::Result<()> {
        Child::kill(self)
    }
    fn wait(&mut self) -> io::Result<ExitStatus> {
        Child::wait(self)
    }
}

impl WaitableChild for FfmpegChild {
    fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        self.as_inner_mut().try_wait()
    }
    fn kill(&mut self) -> io::Result<()> {
        FfmpegChild::kill(self)
    }
    fn wait(&mut self) -> io::Result<ExitStatus> {
        FfmpegChild::wait(self)
    }
}

pub fn wait_with_timeout<T: WaitableChild>(
    child: &mut T,
    timeout: Duration,
) -> Result<ExitStatus, String> {
    let poll_ms = read_settings().ffmpeg.process_wait_poll_ms;
    let start = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(status)) => return Ok(status),
            Ok(None) => {}
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(error.to_string());
            }
        }
        if start.elapsed() >= timeout {
            let _ = child.kill();
            let _ = child.wait();
            return Err("process timed out".to_string());
        }
        thread::sleep(Duration::from_millis(poll_ms));
    }
}

#[cfg(test)]
pub fn run_command_with_timeout(
    mut cmd: Command,
    timeout: Duration,
    is_video: bool,
) -> Result<Output, String> {
    apply_command_tuning_with_settings(&mut cmd, is_video, &read_settings().ffmpeg);
    run_prepared_command_with_timeout(cmd, timeout)
}

pub(crate) fn run_prepared_command_with_timeout(
    mut cmd: Command,
    timeout: Duration,
) -> Result<Output, String> {
    #[cfg(target_os = "windows")]
    {
        const CREATE_NO_WINDOW: u32 = 0x08000000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    cmd.stdout(Stdio::piped());
    cmd.stderr(Stdio::piped());
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    let stdout = drain_pipe(child.stdout.take().ok_or("Missing stdout")?);
    let stderr = drain_pipe(child.stderr.take().ok_or("Missing stderr")?);
    let status = wait_with_timeout(&mut child, timeout);
    let stdout = stdout
        .join()
        .map_err(|_| "stdout reader panicked")?
        .map_err(|e| e.to_string())?;
    let stderr = stderr
        .join()
        .map_err(|_| "stderr reader panicked")?
        .map_err(|e| e.to_string())?;
    let status = status?;
    if !status.success() {
        return Err(String::from_utf8_lossy(&stderr).to_string());
    }
    Ok(Output {
        status,
        stdout,
        stderr,
    })
}

#[cfg(target_os = "linux")]
fn detect_linux_file_manager() -> String {
    let env_hint = std::env::var("XDG_CURRENT_DESKTOP")
        .or_else(|_| std::env::var("DESKTOP_SESSION"))
        .unwrap_or_default()
        .to_lowercase();
    let hints = [
        ("kde", "Dolphin"),
        ("plasma", "Dolphin"),
        ("gnome", "GNOME Files"),
        ("xfce", "Thunar"),
        ("lxqt", "PCManFM-Qt"),
        ("lxde", "PCManFM"),
        ("cinnamon", "Nemo"),
        ("pantheon", "Pantheon Files"),
        ("mate", "Caja"),
    ];
    for (needle, name) in hints {
        if env_hint.contains(needle) {
            return name.to_string();
        }
    }

    let common_processes = [
        ("dolphin", "Dolphin"),
        ("nautilus", "GNOME Files"),
        ("nemo", "Nemo"),
        ("thunar", "Thunar"),
        ("pcmanfm-qt", "PCManFM-Qt"),
        ("pcmanfm", "PCManFM"),
        ("konqueror", "Konqueror"),
        ("caja", "Caja"),
    ];
    if let Ok(output) = Command::new("ps").args(["-e", "-o", "comm"]).output() {
        let stdout = String::from_utf8_lossy(&output.stdout).to_lowercase();
        for (proc, name) in common_processes {
            if stdout.contains(proc) {
                return name.to_string();
            }
        }
    }

    "File Manager".to_string()
}

pub fn get_file_manager_name() -> Result<String, String> {
    #[cfg(target_os = "macos")]
    {
        return Ok("Finder".to_string());
    }
    #[cfg(target_os = "windows")]
    {
        return Ok("File Explorer".to_string());
    }
    #[cfg(target_os = "linux")]
    {
        return Ok(detect_linux_file_manager());
    }
}

/// Creates a derived directory inside an existing album and never recreates the album.
pub(crate) fn create_derived_dir(path: &Path) -> Result<(), String> {
    match std::fs::create_dir(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => Ok(()),
        Err(error) => Err(format!("{}: {}", path.display(), error)),
    }
}

#[allow(dead_code)]
pub(crate) fn generate_heic_jpeg(
    src: &Path,
    output_dir: &Path,
) -> Result<tempfile::NamedTempFile, String> {
    let _permit = DecodePermit::acquire();
    let temporary = tempfile::Builder::new()
        .prefix(".room237-heic-")
        .suffix(".jpeg")
        .tempfile_in(output_dir)
        .map_err(|e| e.to_string())?;
    log::info!("heic→jpeg {}→{}", src.display(), temporary.path().display());
    let mut cmd = FfmpegCommand::new();
    apply_ffmpeg_tuning(&mut cmd, false);
    cmd.arg("-hide_banner").arg("-loglevel").arg("error");
    let mut child = cmd
        .input(src)
        .arg("-y")
        .arg("-map_metadata")
        .arg("0")
        .output(temporary.path())
        .spawn()
        .map_err(|e| e.to_string())?;
    match wait_ffmpeg_with_timeout(&mut child, ffmpeg_timeout()) {
        Ok(status) => {
            if !status.success() {
                log::error!("heic→jpeg failed {}", src.display());
            }
            if !status.success()
                || temporary
                    .as_file()
                    .metadata()
                    .map_err(|e| e.to_string())?
                    .len()
                    == 0
            {
                return Err("HEIC conversion failed".to_string());
            }
            Ok(temporary)
        }
        Err(e) => {
            log::error!(
                "heic→jpeg timeout {}→{}: {}",
                src.display(),
                temporary.path().display(),
                e
            );
            Err(e)
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    #[cfg(unix)]
    fn file_lease_refuses_symlinks_and_preserves_external_lock_bytes() {
        let dir = tempfile::tempdir().unwrap();
        let foreign = dir.path().join("foreign");
        std::fs::write(&foreign, b"existing data").unwrap();
        let alias = dir.path().join("alias.lock");
        std::os::unix::fs::symlink(&foreign, &alias).unwrap();
        assert!(FileLock::try_acquire(&alias).is_err());
        assert!(FileLock::acquire_shared(&alias, Duration::ZERO, Duration::ZERO).is_err());
        assert!(FileLock::try_acquire(&foreign).unwrap().is_some());
        assert_eq!(std::fs::read(&foreign).unwrap(), b"existing data");
    }
    #[test]
    #[cfg(unix)]
    fn file_lock_releases_its_lease_even_while_a_duplicate_descriptor_remains_open() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("lease.lock");
        for shared in [false, true] {
            let owner =
                FileLock::acquire_with_mode(&path, Duration::ZERO, Duration::ZERO, shared).unwrap();
            let duplicate = owner._file.try_clone().unwrap();
            assert!(FileLock::try_acquire(&path).unwrap().is_none());
            drop(owner);
            let next = FileLock::try_acquire(&path).unwrap();
            assert!(next.is_some(), "a dropped guard must release its lease without waiting for duplicate descriptors to close");
            drop(duplicate);
            assert!(FileLock::try_acquire(&path).unwrap().is_none());
            drop(next);
        }
    }
    use super::*;
    #[test]
    fn file_lock_excludes_other_processes_and_recovers_after_process_death() {
        use crate::test_support::{spawn_test, wait_for_file};
        if let Ok(dir) = std::env::var("ROOM237_TEST_DIR") {
            let dir = std::path::PathBuf::from(dir);
            let lock = dir.join("lock");
            assert!(
                FileLock::acquire(&lock, Duration::from_millis(100), Duration::from_millis(5))
                    .is_err()
            );
            std::fs::write(dir.join("blocked"), b"ready").unwrap();
            let _lock = FileLock::acquire(&lock, Duration::from_secs(10), Duration::from_millis(5))
                .unwrap();
            std::fs::write(dir.join("held"), b"ready").unwrap();
            loop {
                thread::sleep(Duration::from_millis(50));
            }
        }
        let dir = tempfile::tempdir().unwrap();
        let lock = dir.path().join("lock");
        std::fs::write(&lock, b"existing service file").unwrap();
        let guard = FileLock::acquire(&lock, Duration::ZERO, Duration::from_millis(1)).unwrap();
        let mut child = spawn_test(
            "util::tests::file_lock_excludes_other_processes_and_recovers_after_process_death",
            dir.path(),
            "holder",
        );
        wait_for_file(&dir.path().join("blocked"));
        drop(guard);
        wait_for_file(&dir.path().join("held"));
        assert!(
            FileLock::acquire(&lock, Duration::from_millis(100), Duration::from_millis(5)).is_err()
        );
        child.0.kill().unwrap();
        child.0.wait().unwrap();
        let _recovered =
            FileLock::acquire(&lock, Duration::from_secs(1), Duration::from_millis(5)).unwrap();
        assert_eq!(std::fs::read(&lock).unwrap(), b"existing service file");
    }
    #[test]
    fn rejects_path_traversal_names() {
        for name in [
            "", ".", "..", "../x.jpg", "/x.jpg", "a\\b.jpg", "C:x.jpg", "x\0.jpg",
        ] {
            assert!(validate_file_name(name).is_err(), "accepted {name:?}");
        }
        assert!(validate_file_name("holiday 2026_1.jpg").is_ok());
    }
    #[test]
    fn atomic_write_replaces_whole_file_and_keeps_previous_on_failure() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("album.json");
        atomic_write(&file, b"previous").unwrap();
        atomic_write(&file, b"next").unwrap();
        assert_eq!(std::fs::read(&file).unwrap(), b"next");
        assert!(atomic_write(dir.path(), b"bad").is_err());
        assert_eq!(std::fs::read(&file).unwrap(), b"next");
    }
    #[test]
    fn move_never_clobbers_existing_files() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("source");
        let target = dir.path().join("target");
        std::fs::write(&source, b"source").unwrap();
        std::fs::write(&target, b"target").unwrap();
        assert!(rename_noclobber(&source, &target).is_err());
        assert_eq!(std::fs::read(&source).unwrap(), b"source");
        assert_eq!(std::fs::read(&target).unwrap(), b"target");
        let next = dir.path().join("next");
        rename_noclobber(&source, &next).unwrap();
        assert!(!source.exists());
        assert_eq!(std::fs::read(&next).unwrap(), b"source");
    }
    #[cfg(unix)]
    #[test]
    fn drains_large_stdout_and_stderr_without_deadlocking() {
        let mut cmd = Command::new("sh");
        cmd.args([
            "-c",
            "head -c 200000 /dev/zero; head -c 200000 /dev/zero >&2",
        ]);
        let output = run_command_with_timeout(cmd, Duration::from_secs(3), false).unwrap();
        assert_eq!(output.stdout.len(), 200000);
        assert_eq!(output.stderr.len(), 200000);
    }
    #[cfg(unix)]
    #[test]
    fn timed_out_child_is_killed_and_reaped() {
        let mut cmd = Command::new("sh");
        cmd.args(["-c", "exec sleep 5"]);
        let started = Instant::now();
        assert!(
            run_command_with_timeout(cmd, Duration::from_millis(100), false)
                .unwrap_err()
                .contains("timed out")
        );
        assert!(started.elapsed() < Duration::from_secs(2));
    }
}
pub fn canonicalize_directory(directory: String) -> Result<String, String> {
    let canonical = std::fs::canonicalize(directory).map_err(|e| e.to_string())?;
    if !canonical.is_dir() {
        return Err("Expected a directory".into());
    }
    Ok(canonical.to_string_lossy().into_owned())
}
