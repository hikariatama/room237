//! Signed self-update. A release publishes `qt-update.json` and `qt-update.json.minisig`; the manifest
//! pins version, URL, size and SHA-256 of one payload per platform, so one signature covers everything.
//! The running app downloads; this module verifies, unpacks into a sibling directory and swaps it in
//! from a copy of itself after the app exits, rolling back if the new version never reports healthy.
use minisign_verify::{PublicKey, Signature};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::Read,
    path::{Component, Path, PathBuf},
    process::Command,
    time::{Duration, Instant},
};

pub const CURRENT_VERSION: &str = match option_env!("ROOM237_VERSION") {
    Some(version) => version,
    None => env!("CARGO_PKG_VERSION"),
};
/// Replaced at build time for a release key. The default is the existing Tauri updater key.
const PUBLIC_KEY: &str = match option_env!("ROOM237_UPDATE_PUBKEY") {
    Some(key) => key,
    None => "RWSptBXbOJ4mm7I37d8OEZ6Qs+I/q1PZQP+J6d75ZUEma6Kfmr732npU",
};
const ALLOWED_HOSTS: &[&str] = &["github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"];
const MAX_PAYLOAD: u64 = 2 << 30;
const MAX_UNPACKED: u64 = 8 << 30;

#[derive(Deserialize)]
struct Manifest {
    version: String,
    #[serde(default)]
    notes: String,
    platforms: std::collections::HashMap<String, PlatformEntry>,
}
#[derive(Deserialize, Serialize, Clone)]
struct PlatformEntry {
    url: String,
    sha256: String,
    size: u64,
    kind: String,
}
#[derive(Serialize)]
pub struct CheckResult {
    available: bool,
    version: String,
    notes: String,
    #[serde(flatten)]
    entry: Option<PlatformEntry>,
}

// Fixture overrides exist only in QA builds and are inert outside a QA run. Every updater entry point validates the complete
// configuration before opening payloads/plans, deleting siblings or starting a process.
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct QaFixture {
    feed: String,
    staging: String,
    install: String,
    executable: String,
    root: String,
    profile: String,
    legacy: String,
    home: String,
    public_key: String,
}
fn canonical_fs(path: &Path) -> Result<PathBuf, String> {
    let real = path.canonicalize().map_err(|e| e.to_string())?;
    #[cfg(windows)]
    {
        // std canonicalize uses extended-length paths; Qt/argv use ordinary drive paths.
        let text = real.to_str().ok_or("QA non-UTF8 canonical path")?;
        if let Some(drive_path) = text.strip_prefix(r"\\?\") {
            return Ok(PathBuf::from(drive_path));
        }
    }
    Ok(real)
}
fn canonical_task_path(raw: &str, fence: &Path) -> Result<PathBuf, String> {
    let path = Path::new(raw);
    if !path.is_absolute() || raw.replace('\\', "/").split('/').any(|c| c == "." || c == "..") || path.components().any(|c| matches!(c, Component::ParentDir | Component::CurDir)) {
        return Err("QA update requires canonical absolute paths".into());
    }
    let mut ancestor = path;
    let mut missing = Vec::new();
    while !ancestor.exists() {
        if ancestor.symlink_metadata().is_ok() { return Err("QA dangling symlink".into()); }
        missing.push(ancestor.file_name().ok_or("QA path has no ancestor")?);
        ancestor = ancestor.parent().ok_or("QA path has no parent")?;
    }
    if !missing.is_empty() && !ancestor.is_dir() { return Err("QA invalid ancestor".into()); }
    let mut real = canonical_fs(ancestor)?;
    for part in missing.iter().rev() { real.push(part); }
    if real.to_string_lossy().replace('\\', "/") != raw.replace('\\', "/") || !real.starts_with(fence) { return Err("QA update path outside canonical fence".into()); }
    Ok(real)
}
fn qa_fixture() -> Result<Option<QaFixture>, String> {
    if !crate::util::qa_run() { return Ok(None); }
    let f: QaFixture = serde_json::from_str(&std::env::var("ROOM237_QA_UPDATE_FIXTURE").map_err(|_| "QA updater disabled")?).map_err(|e| e.to_string())?;
    let raw = std::env::var("ROOM237_QA_FENCE").map_err(|_| "QA update fence missing")?;
    let fence = canonical_fs(Path::new(&raw))?;
    if !Path::new(&raw).is_absolute() || fence != Path::new(&raw) || fence.parent().is_none() { return Err("QA invalid update fence".into()); }
    canonical_task_path(&raw, &fence)?;
    for path in [&f.staging, &f.install, &f.executable, &f.root, &f.profile, &f.legacy, &f.home] {
        canonical_task_path(path, &fence)?;
    }
    for suffix in ["update-new", "update-old", "update-failed"] {
        canonical_task_path(sibling(Path::new(&f.install), suffix)?.to_str().ok_or("QA non-UTF8 path")?, &fence)?;
    }
    for leaf in ["updates", "update-plan.json", "update-health", "update-result.json", "room237-update-helper", "room237-update-helper.exe"] {
        canonical_task_path(Path::new(&f.profile).join(leaf).to_str().ok_or("QA non-UTF8 path")?, &fence)?;
    }
    if !Path::new(&f.executable).starts_with(&f.install) || !Path::new(&f.executable).is_file() ||
       !Path::new(&f.root).is_dir() || !Path::new(&f.profile).is_dir() || !Path::new(&f.legacy).is_dir() ||
       std::env::var("HOME").ok().as_ref() != Some(&f.home) ||
       sibling(Path::new(&f.install), "update-new")? != Path::new(&f.staging) {
        return Err("QA update fixture mismatch".into());
    }
    // A direct core test must also run from its explicit task namespace.
    canonical_task_path(std::env::current_dir().map_err(|e| e.to_string())?.to_str().ok_or("QA cwd")?, &fence)?;
    qa_url(&f.feed, &f)?;
    PublicKey::from_base64(&f.public_key).map_err(|e| e.to_string())?;
    Ok(Some(f))
}
fn qa_url(raw: &str, f: &QaFixture) -> Result<(), String> {
    let url = url::Url::parse(raw).map_err(|e| e.to_string())?;
    if !url.username().is_empty() || url.password().is_some() || url.query().is_some() || url.fragment().is_some() { return Err("QA update URL credentials/query refused".into()); }
    if url.scheme() == "file" && url.host_str().unwrap_or_default().is_empty() {
        let path = url.to_file_path().map_err(|_| "QA file URL")?;
        let fence = std::env::var("ROOM237_QA_FENCE").map_err(|_| "QA fence")?;
        canonical_task_path(path.to_str().ok_or("QA file URL path")?, Path::new(&fence))?;
        return Ok(());
    }
    let feed = url::Url::parse(&f.feed).map_err(|e| e.to_string())?;
    if url.scheme() == "http" && url.host_str() == Some("127.0.0.1") && url.port().is_some() && url.port() == feed.port() { return Ok(()); }
    Err("QA update URL is not the allowlisted local feed".into())
}
fn qa_stage(plan: &StagePlan) -> Result<(), String> {
    if let Some(f) = qa_fixture()? {
        let fence = std::env::var("ROOM237_QA_FENCE").map_err(|_| "QA fence")?;
        let payload = canonical_task_path(&plan.payload, Path::new(&fence))?;
        if plan.install != f.install || !payload.starts_with(Path::new(&f.profile).join("updates")) { return Err("QA stage outside fixture".into()); }
        let probe = Path::new(&plan.probe);
        if probe.is_absolute() || probe.components().any(|p| !matches!(p, Component::Normal(_))) { return Err("QA probe must be relative inside staging".into()); }
    }
    Ok(())
}
fn qa_helper(plan: &HelperPlan) -> Result<(), String> {
    if let Some(f) = qa_fixture()? {
        let launch = vec![f.executable.clone(), "--root".into(), f.root.clone(), "--profile".into(), f.profile.clone(), "--legacy-profile".into(), f.legacy.clone()];
        if plan.install != f.install || plan.staged != f.staging || Path::new(&plan.backup) != sibling(Path::new(&f.install), "update-old")? ||
           plan.launch != launch || Path::new(&plan.health_file) != Path::new(&f.profile).join("update-health") ||
           Path::new(&plan.result_file) != Path::new(&f.profile).join("update-result.json") || plan.wait_pid == 0 || !(1..=300).contains(&plan.health_timeout_secs) {
            return Err("QA helper plan differs from fixture".into());
        }
    }
    Ok(())
}

fn verify_signature(key: &str, data: &[u8], signature: &str) -> Result<(), String> {
    let key = PublicKey::from_base64(key).map_err(|e| format!("update key: {e}"))?;
    let signature = Signature::decode(signature).map_err(|e| format!("update signature: {e}"))?;
    key.verify(data, &signature, false).map_err(|e| format!("update signature rejected: {e}"))
}

fn version_parts(version: &str) -> Option<Vec<u64>> {
    let core = version.split(['-', '+']).next()?;
    let parts: Option<Vec<u64>> = core.split('.').map(|part| part.parse().ok()).collect();
    parts.filter(|parts| !parts.is_empty() && parts.len() <= 4)
}
fn is_newer(candidate: &str, current: &str) -> Result<bool, String> {
    let mut a = version_parts(candidate).ok_or("Invalid update version")?;
    let mut b = version_parts(current).ok_or("Invalid current version")?;
    a.resize(4, 0);
    b.resize(4, 0);
    Ok(a > b)
}

fn allowed_url(raw: &str) -> Result<(), String> {
    if let Some(f) = qa_fixture()? { return qa_url(raw, &f); }
    let url = url::Url::parse(raw).map_err(|e| e.to_string())?;
    let host = url.host_str().unwrap_or_default();
    if url.scheme() == "https" && ALLOWED_HOSTS.contains(&host) {
        return Ok(());
    }
    // Isolated update tests serve the signed feed from loopback; the signature check is the same.
    if std::env::var_os("ROOM237_UPDATE_LOOPBACK").is_some() && url.scheme() == "http" && host == "127.0.0.1" {
        return Ok(());
    }
    Err(format!("Update URL is not allowed: {raw}"))
}

fn check_with_key(key: &str, manifest: &str, signature: &str, current: &str, platform: &str) -> Result<CheckResult, String> {
    verify_signature(key, manifest.as_bytes(), signature)?;
    let parsed: Manifest = serde_json::from_str(manifest).map_err(|e| e.to_string())?;
    let unavailable = |version: String, notes: String| CheckResult { available: false, version, notes, entry: None };
    if !is_newer(&parsed.version, current)? {
        return Ok(unavailable(parsed.version, parsed.notes));
    }
    let Some(entry) = parsed.platforms.get(platform).cloned() else {
        return Ok(unavailable(parsed.version, parsed.notes));
    };
    allowed_url(&entry.url)?;
    if entry.sha256.len() != 64 || !entry.sha256.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("Invalid payload hash in update manifest".into());
    }
    if entry.size == 0 || entry.size > MAX_PAYLOAD {
        return Err("Invalid payload size in update manifest".into());
    }
    if !matches!(entry.kind.as_str(), "tar.gz" | "appimage") {
        return Err(format!("Unknown payload kind: {}", entry.kind));
    }
    Ok(CheckResult { available: true, version: parsed.version, notes: parsed.notes, entry: Some(entry) })
}

pub fn check(manifest: String, signature: String, current_version: String, platform: String) -> Result<CheckResult, String> {
    let fixture = qa_fixture()?;
    let key = fixture.as_ref().map(|f| f.public_key.as_str()).unwrap_or(PUBLIC_KEY);
    check_with_key(key, &manifest, &signature, &current_version, &platform)
}

fn sha256_file(path: &Path) -> Result<(u64, String), String> {
    let mut file = fs::File::open(path).map_err(|e| e.to_string())?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1 << 20];
    let mut total = 0u64;
    loop {
        let read = file.read(&mut buffer).map_err(|e| e.to_string())?;
        if read == 0 {
            return Ok((total, hex::encode(hasher.finalize())));
        }
        hasher.update(&buffer[..read]);
        total += read as u64;
    }
}

/// Rejects anything that could leave the staging directory or create a special file.
fn unpack(archive: &Path, destination: &Path) -> Result<(), String> {
    let decoder = flate2::read::GzDecoder::new(fs::File::open(archive).map_err(|e| e.to_string())?);
    let mut reader = tar::Archive::new(decoder);
    reader.set_preserve_permissions(true);
    let mut written = 0u64;
    for entry in reader.entries().map_err(|e| e.to_string())? {
        let mut entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path().map_err(|e| e.to_string())?.into_owned();
        if !path.components().all(|part| matches!(part, Component::Normal(_) | Component::CurDir)) {
            return Err(format!("Unsafe path in update: {}", path.display()));
        }
        let kind = entry.header().entry_type();
        if kind.is_symlink() {
            let link = entry.link_name().map_err(|e| e.to_string())?.ok_or("Symlink without target")?.into_owned();
            let mut depth = path.components().count() as i64 - 1;
            for part in link.components() {
                match part {
                    Component::Normal(_) => depth += 1,
                    Component::CurDir => {}
                    Component::ParentDir => depth -= 1,
                    _ => return Err(format!("Unsafe link in update: {}", path.display())),
                }
                if depth < 0 {
                    return Err(format!("Unsafe link in update: {}", path.display()));
                }
            }
        } else if !kind.is_file() && !kind.is_dir() {
            return Err(format!("Unsupported entry in update: {}", path.display()));
        }
        written = written.saturating_add(entry.size());
        if written > MAX_UNPACKED {
            return Err("Update unpacks past the size limit".into());
        }
        if !entry.unpack_in(destination).map_err(|e| e.to_string())? {
            return Err(format!("Unsafe path in update: {}", path.display()));
        }
    }
    Ok(())
}

#[derive(Deserialize, Serialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub struct StagePlan {
    payload: String,
    sha256: String,
    size: u64,
    kind: String,
    /// Installed bundle, directory or AppImage to replace.
    install: String,
    /// Executable to probe inside the staged copy, relative to it.
    probe: String,
    version: String,
}
#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(rename_all = "camelCase")]
pub struct Staged {
    staged: String,
    backup: String,
}

fn sibling(install: &Path, suffix: &str) -> Result<PathBuf, String> {
    let name = install.file_name().ok_or("Install path has no name")?.to_string_lossy().into_owned();
    Ok(install.with_file_name(format!(".{name}.{suffix}")))
}

pub fn stage(plan: StagePlan) -> Result<Staged, String> {
    qa_stage(&plan)?;
    let payload = Path::new(&plan.payload);
    let (size, hash) = sha256_file(payload)?;
    if size != plan.size {
        return Err(format!("Update size mismatch: {size} != {}", plan.size));
    }
    if !hash.eq_ignore_ascii_case(&plan.sha256) {
        return Err("Update hash mismatch".into());
    }
    let install = Path::new(&plan.install);
    let staged = sibling(install, "update-new")?;
    let backup = sibling(install, "update-old")?;
    remove_path(&staged)?;
    match plan.kind.as_str() {
        "tar.gz" => {
            fs::create_dir_all(&staged).map_err(|e| e.to_string())?;
            if let Err(error) = unpack(payload, &staged) {
                let _ = remove_path(&staged);
                return Err(error);
            }
        }
        "appimage" => {
            fs::copy(payload, &staged).map_err(|e| e.to_string())?;
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                fs::set_permissions(&staged, fs::Permissions::from_mode(0o755)).map_err(|e| e.to_string())?;
            }
        }
        other => return Err(format!("Unknown payload kind: {other}")),
    }
    let probe = if plan.probe.is_empty() { staged.clone() } else { staged.join(&plan.probe) };
    if crate::util::qa_run() && plan.kind == "tar.gz" &&
       !probe.canonicalize().map_err(|e| e.to_string())?.starts_with(staged.canonicalize().map_err(|e| e.to_string())?) {
        return Err("QA probe escaped staging".into());
    }
    if !probe.is_file() {
        let _ = remove_path(&staged);
        return Err(format!("Update does not contain {}", plan.probe));
    }
    if plan.kind == "tar.gz" {
        let output = crate::util::run_prepared_command_with_timeout(
            {
                let mut command = Command::new(&probe);
                command.arg("--version");
                command
            },
            Duration::from_secs(20),
        );
        let reported = output.map(|out| String::from_utf8_lossy(&out.stdout).trim().to_owned()).unwrap_or_default();
        if reported != plan.version {
            let _ = remove_path(&staged);
            return Err(format!("Staged update reports version '{reported}', expected '{}'", plan.version));
        }
    }
    Ok(Staged { staged: staged.to_string_lossy().into_owned(), backup: backup.to_string_lossy().into_owned() })
}

fn remove_path(path: &Path) -> Result<(), String> {
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.to_string()),
        Ok(info) if info.is_dir() => fs::remove_dir_all(path).map_err(|e| e.to_string()),
        Ok(_) => fs::remove_file(path).map_err(|e| e.to_string()),
    }
}

#[derive(Deserialize, Serialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub struct HelperPlan {
    wait_pid: u32,
    install: String,
    staged: String,
    backup: String,
    /// Program and arguments to start after the swap. Paths stay valid because install keeps its name.
    launch: Vec<String>,
    /// Created by the new version once its window is up and its core answers.
    health_file: String,
    result_file: String,
    health_timeout_secs: u64,
}

/// Copies this executable out of the bundle being replaced and starts it detached.
pub fn launch_helper(plan: HelperPlan) -> Result<(), String> {
    qa_helper(&plan)?;
    let directory = Path::new(&plan.result_file).parent().ok_or("Result file has no directory")?.to_path_buf();
    fs::create_dir_all(&directory).map_err(|e| e.to_string())?;
    let plan_path = directory.join("update-plan.json");
    crate::util::atomic_write(&plan_path, serde_json::to_string(&plan).map_err(|e| e.to_string())?.as_bytes())?;
    let helper = directory.join(if cfg!(windows) { "room237-update-helper.exe" } else { "room237-update-helper" });
    fs::copy(std::env::current_exe().map_err(|e| e.to_string())?, &helper).map_err(|e| e.to_string())?;
    let mut command = Command::new(&helper);
    command
        .arg("--apply-update")
        .arg(&plan_path)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000 | 0x00000008); // CREATE_NO_WINDOW | DETACHED_PROCESS
    }
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        command.process_group(0);
    }
    command.spawn().map_err(|e| e.to_string())?;
    Ok(())
}

fn pid_running(pid: u32) -> bool {
    #[cfg(unix)]
    {
        let exists = unsafe { libc::kill(pid as i32, 0) == 0 || std::io::Error::last_os_error().raw_os_error() == Some(libc::EPERM) };
        // A zombie has exited and released its files; only its parent has not collected it yet.
        exists
            && !Command::new("ps")
                .args(["-o", "stat=", "-p", &pid.to_string()])
                .output()
                .map(|out| String::from_utf8_lossy(&out.stdout).trim_start().starts_with('Z'))
                .unwrap_or(false)
    }
    #[cfg(windows)]
    {
        Command::new("tasklist")
            .args(["/FI", &format!("PID eq {pid}"), "/NH"])
            .output()
            .map(|out| String::from_utf8_lossy(&out.stdout).contains(&pid.to_string()))
            .unwrap_or(false)
    }
}

fn wait_exit(pid: u32, timeout: Duration) -> bool {
    let start = Instant::now();
    while pid_running(pid) {
        if start.elapsed() > timeout {
            return false;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    true
}

/// Renames fail briefly on Windows while a just-exited process releases its files.
fn rename_with_retry(from: &Path, to: &Path) -> Result<(), String> {
    let start = Instant::now();
    loop {
        match fs::rename(from, to) {
            Ok(()) => return Ok(()),
            Err(error) if start.elapsed() > Duration::from_secs(15) => {
                return Err(format!("{} -> {}: {error}", from.display(), to.display()));
            }
            Err(_) => std::thread::sleep(Duration::from_millis(200)),
        }
    }
}

fn start(launch: &[String]) -> Result<std::process::Child, String> {
    let (program, arguments) = launch.split_first().ok_or("Nothing to launch")?;
    Command::new(program).args(arguments).spawn().map_err(|e| format!("{program}: {e}"))
}

/// macOS opens the bundle through LaunchServices with `open`, which exits at once: health is the only signal.
fn wait_healthy(child: &mut std::process::Child, health: &Path, timeout: Duration, detached: bool) -> bool {
    let started = Instant::now();
    while started.elapsed() < timeout {
        if health.is_file() {
            return true;
        }
        if !detached && child.try_wait().ok().flatten().is_some() {
            return health.is_file();
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    false
}

fn apply(plan: &HelperPlan) -> Result<&'static str, String> {
    let install = Path::new(&plan.install);
    let staged = Path::new(&plan.staged);
    let backup = Path::new(&plan.backup);
    let health = Path::new(&plan.health_file);
    if !wait_exit(plan.wait_pid, Duration::from_secs(60)) {
        let _ = remove_path(staged);
        return Err("The app did not exit".into());
    }
    remove_path(backup)?;
    let _ = fs::remove_file(health);
    rename_with_retry(install, backup)?;
    if let Err(error) = rename_with_retry(staged, install) {
        rename_with_retry(backup, install)?;
        return Err(error);
    }
    let detached = plan.launch.first().is_some_and(|program| program == "/usr/bin/open");
    let healthy = match start(&plan.launch) {
        Ok(mut child) => {
            let healthy = wait_healthy(&mut child, health, Duration::from_secs(plan.health_timeout_secs), detached);
            if !healthy && !detached {
                let _ = child.kill();
                let _ = child.wait();
            }
            healthy
        }
        Err(_) => false,
    };
    if healthy {
        // The old copy is no longer needed; a failure here only leaves a hidden sibling for cleanup.
        let _ = remove_path(backup);
        return Ok("updated");
    }
    let failed = sibling(install, "update-failed")?;
    remove_path(&failed)?;
    rename_with_retry(install, &failed)?;
    rename_with_retry(backup, install)?;
    let _ = remove_path(&failed);
    let _ = start(&plan.launch);
    Ok("rolled-back")
}

pub fn run_helper(plan_path: &str) -> i32 {
    match qa_fixture() {
        Ok(Some(f)) if Path::new(plan_path) != Path::new(&f.profile).join("update-plan.json") => return 2,
        Err(_) => return 2,
        _ => {}
    }
    let plan: HelperPlan = match fs::read(plan_path).ok().and_then(|bytes| serde_json::from_slice(&bytes).ok()) {
        Some(plan) => plan,
        None => return 2,
    };
    if qa_helper(&plan).is_err() { return 2; }
    let result = apply(&plan);
    let record = match &result {
        Ok(status) => serde_json::json!({"status": status}),
        Err(error) => serde_json::json!({"status": "failed", "error": error}),
    };
    let _ = fs::write(&plan.result_file, record.to_string());
    if let Err(_) = result {
        let _ = start(&plan.launch);
        return 1;
    }
    0
}

/// Leftovers of an interrupted update: a pending staging copy or a backup after a killed helper.
pub fn cleanup(install: String) -> Result<(), String> {
    if let Some(f) = qa_fixture()? { if install != f.install { return Err("QA cleanup outside fixture".into()); } }
    let install = Path::new(&install);
    let backup = sibling(install, "update-old")?;
    if !install.exists() && backup.exists() {
        // The swap stopped between its two renames.
        rename_with_retry(&backup, install)?;
    }
    for suffix in ["update-new", "update-old", "update-failed"] {
        remove_path(&sibling(install, suffix)?)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    // Pair generated with the minisign algorithm for these tests only.
    const TEST_KEY: &str = include_str!("../testdata/update/key.pub");
    const MANIFEST: &str = include_str!("../testdata/update/manifest.json");
    const SIGNATURE: &str = include_str!("../testdata/update/manifest.json.minisig");

    #[test]
    fn valid_manifest_returns_the_platform_entry() {
        let result = check_with_key(TEST_KEY.trim(), MANIFEST, SIGNATURE, "1.0.0", "darwin-aarch64").unwrap();
        assert!(result.available);
        assert_eq!(result.version, "1.2.3");
        assert_eq!(result.entry.unwrap().kind, "tar.gz");
    }

    #[test]
    fn tampered_manifest_and_garbage_signature_are_rejected() {
        let tampered = MANIFEST.replace("1.2.3", "9.9.9");
        assert!(check_with_key(TEST_KEY.trim(), &tampered, SIGNATURE, "1.0.0", "darwin-aarch64").is_err());
        assert!(check_with_key(TEST_KEY.trim(), MANIFEST, "garbage", "1.0.0", "darwin-aarch64").is_err());
    }

    #[test]
    fn no_downgrade_reinstall_or_foreign_platform() {
        for current in ["1.2.3", "1.2.4", "2.0.0"] {
            assert!(!check_with_key(TEST_KEY.trim(), MANIFEST, SIGNATURE, current, "darwin-aarch64").unwrap().available);
        }
        assert!(!check_with_key(TEST_KEY.trim(), MANIFEST, SIGNATURE, "1.0.0", "plan9-mips").unwrap().available);
    }

    #[test]
    fn urls_outside_the_release_hosts_are_rejected() {
        assert!(allowed_url("https://github.com/hikariatama/room237/releases/download/x/a").is_ok());
        for bad in ["http://github.com/a", "https://evil.example/a", "https://github.com.evil.example/a", "file:///etc/passwd"] {
            assert!(allowed_url(bad).is_err(), "{bad}");
        }
    }

    fn archive_with(path: &Path, write: impl FnOnce(&mut tar::Builder<flate2::write::GzEncoder<fs::File>>)) {
        let encoder = flate2::write::GzEncoder::new(fs::File::create(path).unwrap(), flate2::Compression::fast());
        let mut builder = tar::Builder::new(encoder);
        write(&mut builder);
        builder.into_inner().unwrap().finish().unwrap();
    }
    fn raw_entry(builder: &mut tar::Builder<flate2::write::GzEncoder<fs::File>>, name: &str, kind: tar::EntryType, link: Option<&str>) {
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(kind);
        header.set_size(if kind.is_file() { 1 } else { 0 });
        header.set_mode(0o644);
        let bytes = header.as_old_mut();
        bytes.name[..name.len()].copy_from_slice(name.as_bytes());
        if let Some(link) = link {
            bytes.linkname[..link.len()].copy_from_slice(link.as_bytes());
        }
        header.set_cksum();
        if kind.is_file() {
            builder.append(&header, &b"x"[..]).unwrap();
        } else {
            builder.append(&header, std::io::empty()).unwrap();
        }
    }

    #[test]
    fn traversal_absolute_paths_escaping_links_and_devices_are_rejected() {
        for (name, kind, link) in [
            ("../evil", tar::EntryType::Regular, None),
            ("a/../../evil", tar::EntryType::Regular, None),
            ("/abs/evil", tar::EntryType::Regular, None),
            ("link", tar::EntryType::Symlink, Some("../../outside")),
            ("abslink", tar::EntryType::Symlink, Some("/etc/passwd")),
            ("dev", tar::EntryType::Char, None),
        ] {
            let dir = tempfile::tempdir().unwrap();
            let archive = dir.path().join("a.tar.gz");
            archive_with(&archive, |builder| raw_entry(builder, name, kind, link));
            let target = dir.path().join("out");
            fs::create_dir(&target).unwrap();
            assert!(unpack(&archive, &target).is_err(), "{name}");
            assert!(!dir.path().join("evil").exists());
        }
    }

    #[test]
    fn framework_style_relative_links_unpack() {
        let dir = tempfile::tempdir().unwrap();
        let archive = dir.path().join("a.tar.gz");
        archive_with(&archive, |builder| {
            raw_entry(builder, "Q.framework/Versions/A/Q", tar::EntryType::Regular, None);
            raw_entry(builder, "Q.framework/Q", tar::EntryType::Symlink, Some("Versions/A/Q"));
        });
        let target = dir.path().join("out");
        fs::create_dir(&target).unwrap();
        unpack(&archive, &target).unwrap();
        assert_eq!(fs::read(target.join("Q.framework/Q")).unwrap(), b"x");
    }

    #[test]
    fn stage_rejects_wrong_hash_and_size_and_keeps_the_install() {
        let dir = tempfile::tempdir().unwrap();
        let payload = dir.path().join("p.tar.gz");
        archive_with(&payload, |builder| raw_entry(builder, "bin", tar::EntryType::Regular, None));
        let (size, hash) = sha256_file(&payload).unwrap();
        let install = dir.path().join("App");
        fs::create_dir(&install).unwrap();
        fs::File::create(install.join("old")).unwrap().write_all(b"old").unwrap();
        let plan = |sha256: &str, size: u64| StagePlan {
            payload: payload.to_string_lossy().into(),
            sha256: sha256.into(),
            size,
            kind: "tar.gz".into(),
            install: install.to_string_lossy().into(),
            probe: "missing".into(),
            version: "1".into(),
        };
        assert!(stage(plan(&"0".repeat(64), size)).is_err());
        assert!(stage(plan(&hash, size + 1)).is_err());
        assert!(stage(plan(&hash, size)).is_err()); // No probe executable: the staged copy is removed.
        assert!(!sibling(&install, "update-new").unwrap().exists());
        assert_eq!(fs::read(install.join("old")).unwrap(), b"old");
    }

    #[test]
    #[cfg(unix)]
    fn helper_swaps_on_health_and_rolls_back_without_it() {
        use std::os::unix::fs::PermissionsExt;
        for healthy in [true, false] {
            let dir = tempfile::tempdir().unwrap();
            let install = dir.path().join("App");
            let staged = dir.path().join(".App.update-new");
            let backup = dir.path().join(".App.update-old");
            let health = dir.path().join("healthy");
            fs::create_dir(&install).unwrap();
            fs::create_dir(&staged).unwrap();
            fs::write(install.join("version"), "old").unwrap();
            fs::write(staged.join("version"), "new").unwrap();
            let launcher = dir.path().join("launch.sh");
            let body = if healthy { format!("#!/bin/sh\ntouch '{}'\nsleep 1\n", health.display()) } else { "#!/bin/sh\nexit 3\n".to_string() };
            fs::write(&launcher, body).unwrap();
            fs::set_permissions(&launcher, fs::Permissions::from_mode(0o755)).unwrap();
            let mut waited = Command::new("true").spawn().unwrap();
            let pid = waited.id();
            waited.wait().unwrap();
            let plan = HelperPlan {
                wait_pid: pid,
                install: install.to_string_lossy().into(),
                staged: staged.to_string_lossy().into(),
                backup: backup.to_string_lossy().into(),
                launch: vec![launcher.to_string_lossy().into()],
                health_file: health.to_string_lossy().into(),
                result_file: dir.path().join("result").to_string_lossy().into(),
                health_timeout_secs: 3,
            };
            let status = apply(&plan).unwrap();
            assert_eq!(status, if healthy { "updated" } else { "rolled-back" });
            assert_eq!(fs::read_to_string(install.join("version")).unwrap(), if healthy { "new" } else { "old" });
            assert!(!backup.exists());
        }
    }

    #[test]
    fn cleanup_recovers_an_install_lost_between_the_two_renames() {
        let dir = tempfile::tempdir().unwrap();
        let install = dir.path().join("App");
        let backup = dir.path().join(".App.update-old");
        fs::create_dir(&backup).unwrap();
        fs::write(backup.join("version"), "old").unwrap();
        cleanup(install.to_string_lossy().into()).unwrap();
        assert_eq!(fs::read_to_string(install.join("version")).unwrap(), "old");
    }
}
