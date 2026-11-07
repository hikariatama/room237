use serde::Serialize;
use std::sync::{Arc, Mutex};
use crate::host::{AppHandle, Emitter, State};

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StartupStatus {
    phase: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    progress: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    message: Option<String>,
}
impl StartupStatus {
    fn phase(phase: &'static str) -> Self {
        Self {
            phase,
            progress: None,
            message: None,
        }
    }
}
struct Inner {
    status: StartupStatus,
    running: bool,
}
#[derive(Clone)]
pub struct StartupState(Arc<Mutex<Inner>>);
impl Default for StartupState {
    fn default() -> Self {
        Self(Arc::new(Mutex::new(Inner {
            status: StartupStatus::phase("checking"),
            running: false,
        })))
    }
}
fn publish(app: &AppHandle, state: &StartupState, status: StartupStatus) {
    if let Ok(mut inner) = state.0.lock() {
        inner.status = status.clone();
    }
    let _ = app.emit("startup-status", status);
}
pub fn begin(app: AppHandle, state: StartupState) {
    {
        let Ok(mut inner) = state.0.lock() else {
            return;
        };
        if inner.running || inner.status.phase == "ready" {
            return;
        }
        inner.running = true;
        inner.status = StartupStatus::phase("checking");
    }
    std::thread::spawn(move || {
        publish(&app, &state, StartupStatus::phase("checking"));
        let status = match bundled_ffmpeg_works() {
            Ok(()) => StartupStatus::phase("ready"),
            Err(error) => StartupStatus {
                phase: "error",
                progress: None,
                message: Some(error),
            },
        };
        if let Ok(mut inner) = state.0.lock() {
            inner.running = false;
        }
        publish(&app, &state, status);
    });
}
/// FFmpeg ships next to the core. Nothing is downloaded at run time.
fn bundled_ffmpeg_works() -> Result<(), String> {
    let path = ffmpeg_sidecar::paths::ffmpeg_path();
    let mut command = std::process::Command::new(&path);
    command.arg("-version");
    let output = crate::util::run_prepared_command_with_timeout(command, std::time::Duration::from_secs(20))
        .map_err(|error| format!("{}: {error}", path.display()))?;
    if output.status.success() {
        Ok(())
    } else {
        Err(format!("{}: exit {:?}", path.display(), output.status.code()))
    }
}
pub fn get_startup_status(state: State<'_, StartupState>) -> Result<StartupStatus, String> {
    state
        .0
        .lock()
        .map(|inner| inner.status.clone())
        .map_err(|e| e.to_string())
}
pub fn retry_startup(app: AppHandle, state: State<'_, StartupState>) {
    begin(app, state.inner().clone());
}
