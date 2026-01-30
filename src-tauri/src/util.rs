use std::{
    io,
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

use crate::settings::read_settings;

pub static STORE_WRITE_LOCK: Lazy<Mutex<()>> = Lazy::new(|| Mutex::new(()));

pub fn newer_than(a: &Path, b: &Path) -> io::Result<bool> {
    Ok(a.metadata()?.modified()? >= b.metadata()?.modified()?)
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
    let settings = read_settings();
    let threads = settings.ffmpeg.threads.resolved();
    let hwaccel = settings.ffmpeg.hwaccel.to_ascii_lowercase();
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

pub fn apply_command_tuning(cmd: &mut Command, is_video: bool) {
    let settings = read_settings();
    let threads = settings.ffmpeg.threads.resolved();
    let hwaccel = settings.ffmpeg.hwaccel.to_ascii_lowercase();
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

pub fn metadata_probe_timeout() -> Duration {
    let settings = read_settings();
    Duration::from_secs(
        settings
            .metadata
            .ffmpeg_probe_timeout_secs
            .unwrap_or(settings.ffmpeg.timeout_secs),
    )
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
        if let Some(status) = child.try_wait().map_err(|e| e.to_string())? {
            return Ok(status);
        }
        if start.elapsed() >= timeout {
            let _ = child.kill();
            let _ = child.wait();
            return Err("process timed out".to_string());
        }
        thread::sleep(Duration::from_millis(poll_ms));
    }
}

pub fn run_command_with_timeout(
    mut cmd: Command,
    timeout: Duration,
    is_video: bool,
) -> Result<Output, String> {
    apply_command_tuning(&mut cmd, is_video);

    #[cfg(target_os = "windows")]
    {
        const CREATE_NO_WINDOW: u32 = 0x08000000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    cmd.stdout(Stdio::piped());
    cmd.stderr(Stdio::piped());
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    let status = wait_with_timeout(&mut child, timeout)?;
    let output = child.wait_with_output().map_err(|e| e.to_string())?;
    if !status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).to_string());
    }
    Ok(output)
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

#[tauri::command]
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

pub fn heic_to_jpeg(src: &Path, dst: &Path) -> Result<(), String> {
    if dst.exists() && newer_than(dst, src).unwrap_or(false) {
        return Ok(());
    }
    log::info!("heic→jpeg {}→{}", src.display(), dst.display());
    let mut cmd = FfmpegCommand::new();
    apply_ffmpeg_tuning(&mut cmd, false);
    cmd.arg("-hide_banner").arg("-loglevel").arg("error");
    let mut child = cmd
        .input(src.to_string_lossy())
        .arg("-y")
        .arg("-map_metadata")
        .arg("0")
        .output(dst.to_string_lossy())
        .spawn()
        .map_err(|e| e.to_string())?;
    match wait_with_timeout(&mut child, ffmpeg_timeout()) {
        Ok(status) => {
            if !status.success() {
                log::error!("heic→jpeg failed {}→{}", src.display(), dst.display());
            }
            Ok(())
        }
        Err(e) => {
            log::error!(
                "heic→jpeg timeout {}→{}: {}",
                src.display(),
                dst.display(),
                e
            );
            Err(e)
        }
    }
}
