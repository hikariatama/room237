#[cfg(target_os = "windows")]
use std::ffi::OsString;
use std::{
    cell::Cell,
    collections::HashMap,
    fs::{self, File},
    io::BufReader,
    path::{Path, PathBuf},
    process::Command,
    sync::{mpsc, Arc, Condvar, Mutex, MutexGuard},
    thread,
    time::{Duration, Instant},
};

#[cfg(test)]
use crate::util::STORE_WRITE_LOCK;
use crate::{
    settings::read_settings,
    util::{
        apply_command_tuning_with_settings, run_prepared_command_with_timeout, validate_file_name,
        FileLock,
    },
};
use chrono::{DateTime, NaiveDateTime, TimeZone, Utc};
use exif::{Field, In, Reader, Tag, Value};
#[cfg(test)]
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
#[cfg(test)]
use std::collections::VecDeque;

type MetadataStamp = (std::time::SystemTime, u64, u64);
#[cfg(test)]
struct MetadataSnapshot {
    dir: PathBuf,
    stamp: MetadataStamp,
    data: Arc<AlbumMeta>,
}
#[cfg(test)]
static META_CACHE: once_cell::sync::Lazy<Mutex<VecDeque<MetadataSnapshot>>> =
    once_cell::sync::Lazy::new(|| Mutex::new(VecDeque::new()));

thread_local! { static BACKGROUND_WRITES: Cell<bool> = const { Cell::new(false) }; }
pub(crate) struct BackgroundMetadataWrites(bool, std::marker::PhantomData<std::rc::Rc<()>>);
impl BackgroundMetadataWrites {
    pub(crate) fn enter() -> Self {
        Self(
            BACKGROUND_WRITES.with(|flag| flag.replace(true)),
            std::marker::PhantomData,
        )
    }
}
impl Drop for BackgroundMetadataWrites {
    fn drop(&mut self) {
        BACKGROUND_WRITES.with(|flag| flag.set(self.0));
    }
}

struct DerivedWrite {
    path: PathBuf,
    name: String,
    expected_revision: Option<String>,
    expected_source: Option<String>,
    update: Box<dyn FnOnce(&mut FileMetaEntry) + Send>,
    response: mpsc::SyncSender<Result<FileMetaEntry, String>>,
    permit: Option<DerivedPermit>,
}
const DERIVED_BATCH_SIZE: usize = 32;
const DERIVED_BATCH_WAIT: Duration = Duration::from_millis(8);
// 64 queued + 32 in-flight requests.
const DERIVED_MAX_OUTSTANDING: usize = 96;
const METADATA_LEASE_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Default)]
struct DerivedBudget {
    used: Mutex<usize>,
    available: Condvar,
}
struct DerivedPermit(Arc<DerivedBudget>);
impl DerivedBudget {
    fn acquire(self: &Arc<Self>) -> DerivedPermit {
        let mut used = self.used.lock().unwrap();
        while *used >= DERIVED_MAX_OUTSTANDING {
            used = self.available.wait(used).unwrap();
        }
        *used += 1;
        DerivedPermit(self.clone())
    }
}
impl Drop for DerivedPermit {
    fn drop(&mut self) {
        let mut used = self.0.used.lock().unwrap();
        *used -= 1;
        self.0.available.notify_one();
    }
}

#[derive(Clone)]
struct DerivedWriter {
    sender: mpsc::SyncSender<DerivedWrite>,
    budget: Arc<DerivedBudget>,
}
impl DerivedWriter {
    fn send(&self, mut write: DerivedWrite) -> Result<(), String> {
        write.permit = Some(self.budget.acquire());
        self.sender
            .send(write)
            .map_err(|_| "Metadata writer stopped".to_string())
    }
}

static DERIVED_WRITER: once_cell::sync::Lazy<DerivedWriter> =
    once_cell::sync::Lazy::new(|| spawn_derived_writer(METADATA_LEASE_TIMEOUT).0);

fn spawn_derived_writer(timeout: Duration) -> (DerivedWriter, thread::JoinHandle<()>) {
    let (sender, receiver) = mpsc::sync_channel(64);
    let worker = thread::spawn(move || {
        crate::util::set_low_priority_current_thread();
        run_derived_writer(receiver, timeout);
    });
    (
        DerivedWriter {
            sender,
            budget: Arc::default(),
        },
        worker,
    )
}

struct PendingDerivedWrite {
    write: DerivedWrite,
    waiting_since: Option<Instant>,
}

fn run_derived_writer(receiver: mpsc::Receiver<DerivedWrite>, timeout: Duration) {
    let mut pending: Vec<PendingDerivedWrite> = Vec::new();
    let mut disconnected = false;
    loop {
        let had_pending = !pending.is_empty();
        if !had_pending {
            if disconnected {
                return;
            }
            match receiver.recv() {
                Ok(write) => pending.push(PendingDerivedWrite {
                    write,
                    waiting_since: None,
                }),
                Err(_) => return,
            }
        }
        // Ready leftovers need no extra batching delay. Busy leases are polled
        // at the normal batch interval while newly arriving albums join the batch.
        let wait = if had_pending && pending.iter().any(|write| write.waiting_since.is_none()) {
            Duration::ZERO
        } else {
            DERIVED_BATCH_WAIT
        };
        let deadline = Instant::now() + wait;
        let mut received = usize::from(!had_pending);
        while !disconnected && pending.len() < DERIVED_MAX_OUTSTANDING {
            let remaining = deadline.saturating_duration_since(Instant::now());
            match receiver.recv_timeout(remaining) {
                Ok(write) => {
                    pending.push(PendingDerivedWrite {
                        write,
                        waiting_since: None,
                    });
                    received += 1;
                    if received >= DERIVED_BATCH_SIZE {
                        break;
                    }
                }
                Err(mpsc::RecvTimeoutError::Timeout) => break,
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    disconnected = true;
                    break;
                }
            }
        }
        // A closed or full channel must not turn unavailable leases into a busy loop.
        if (disconnected || pending.len() == DERIVED_MAX_OUTSTANDING) && !wait.is_zero() {
            thread::sleep(deadline.saturating_duration_since(Instant::now()));
        }
        let mut albums: HashMap<PathBuf, Vec<PendingDerivedWrite>> = HashMap::new();
        for write in pending.drain(..) {
            albums
                .entry(write.write.path.parent().unwrap().to_path_buf())
                .or_default()
                .push(write);
        }
        for (dir, mut writes) in albums {
            match try_lock_metadata_for_write(&dir) {
                Ok(Some(guard)) => {
                    let remainder = writes.split_off(writes.len().min(DERIVED_BATCH_SIZE));
                    flush_derived_album(
                        &dir,
                        writes.into_iter().map(|write| write.write).collect(),
                        &guard,
                    );
                    pending.extend(remainder.into_iter().map(|mut write| {
                        write.waiting_since = None;
                        write
                    }));
                }
                Ok(None) => {
                    let now = Instant::now();
                    for mut write in writes {
                        let started = *write.waiting_since.get_or_insert(now);
                        if now.duration_since(started) >= timeout {
                            let _ = write.write.response.send(Err(format!(
                                "File lock timed out: {}",
                                dir.join(".room237-metadata.lock").display()
                            )));
                        } else {
                            pending.push(write);
                        }
                    }
                }
                Err(error) => {
                    for write in writes {
                        let _ = write.write.response.send(Err(error.clone()));
                    }
                }
            }
        }
    }
}

fn flush_derived_writes(writes: Vec<DerivedWrite>) {
    let mut albums: HashMap<PathBuf, Vec<DerivedWrite>> = HashMap::new();
    for write in writes {
        albums
            .entry(write.path.parent().unwrap().to_path_buf())
            .or_default()
            .push(write);
    }
    for (dir, writes) in albums {
        let _guard = match lock_metadata_for_write(&[&dir]) {
            Ok(lock) => lock,
            Err(error) => {
                for write in writes {
                    let _ = write.response.send(Err(error.clone()));
                }
                continue;
            }
        };
        flush_derived_album(&dir, writes, &_guard);
    }
}

fn flush_derived_album(dir: &Path, writes: Vec<DerivedWrite>, _guard: &MetadataWriteGuard) {
    if let Err(error) = ensure_database(dir) {
        for write in writes {
            let _ = write.response.send(Err(error.clone()));
        }
        return;
    }
    let mut completed = Vec::new();
    let mut pending = Some(writes);
    let persisted = crate::album_db::update_rows(&album_meta_path(dir), |conn, rows| {
        for write in pending.take().unwrap() {
            if !write.path.is_file()
                || write
                    .expected_revision
                    .as_deref()
                    .is_some_and(|revision| media_revision(&write.path) != revision)
                || write
                    .expected_source
                    .as_deref()
                    .is_some_and(|source| source_revision(&write.path).as_deref() != Some(source))
            {
                let _ = write.response.send(Err(format!(
                    "Media changed during processing: {}",
                    write.path.display()
                )));
                continue;
            }
            let result = (|| {
                if !rows.contains_key(&write.name) {
                    rows.insert(
                        write.name.clone(),
                        crate::album_db::entry(conn, &write.name)?,
                    );
                }
                let entry = rows.get_mut(&write.name).unwrap();
                (write.update)(entry);
                Ok(entry.clone())
            })();
            completed.push((write.name, write.response, result, write.permit));
        }
        Ok(())
    });
    if let Some(writes) = pending {
        let error = persisted.clone().unwrap_err();
        for write in writes {
            let _ = write.response.send(Err(error.clone()));
        }
    }
    invalidate_album_snapshot(dir);
    // A worker receives success only after the entire transaction commits.
    for (_name, response, entry, _permit) in completed {
        let _ = response.send(persisted.clone().and(entry));
    }
}

fn submit_derived_write(
    path: &Path,
    expected_revision: Option<&str>,
    expected_source: Option<&str>,
    update: impl FnOnce(&mut FileMetaEntry) + Send + 'static,
) -> Result<FileMetaEntry, String> {
    path.parent().ok_or("Invalid path")?;
    let name = path
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or("Invalid filename")?;
    validate_file_name(name)?;
    let (response, receiver) = mpsc::sync_channel(1);
    let write = DerivedWrite {
        path: path.to_path_buf(),
        name: name.to_string(),
        expected_revision: expected_revision.map(str::to_string),
        expected_source: expected_source.map(str::to_string),
        update: Box::new(update),
        response,
        permit: None,
    };
    if BACKGROUND_WRITES.with(Cell::get) {
        DERIVED_WRITER.send(write)?;
    } else {
        flush_derived_writes(vec![write]);
    }
    receiver
        .recv()
        .map_err(|_| "Metadata writer stopped before persistence".to_string())?
}
#[cfg(test)]
fn metadata_stamp(path: &Path) -> Option<MetadataStamp> {
    metadata_stamp_from_info(&path.metadata().ok()?)
}
fn metadata_stamp_from_info(meta: &fs::Metadata) -> Option<MetadataStamp> {
    #[cfg(unix)]
    let identity = {
        use std::os::unix::fs::MetadataExt;
        meta.ino()
    };
    #[cfg(not(unix))]
    let identity = 0;
    Some((meta.modified().ok()?, meta.len(), identity))
}

pub(crate) fn source_revision(path: &Path) -> Option<String> {
    source_revision_from_info(&path.metadata().ok()?)
}
pub(crate) fn source_revision_from_info(info: &fs::Metadata) -> Option<String> {
    metadata_stamp_from_info(info).map(|(modified, size, identity)| {
        let nanos = modified
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos();
        format!("{nanos:x}-{size:x}-{identity:x}")
    })
}

/// Source and preview stamps, joined by `media_revision`. Callers that only care
/// whether the original changed compare the first part.
pub(crate) fn media_revision_parts(path: &Path) -> (String, String) {
    let thumbnail = crate::thumb::thumb_path(
        path,
        &path
            .parent()
            .unwrap_or(Path::new("."))
            .join(".room237-thumb"),
    )
    .ok();
    (
        source_revision(path).unwrap_or_else(|| "missing".into()),
        thumbnail
            .as_deref()
            .and_then(source_revision)
            .unwrap_or_else(|| "missing".into()),
    )
}

// Stable between reads; changes when either the source or generated preview changes.
pub fn media_revision(path: &Path) -> String {
    let (source, thumbnail) = media_revision_parts(path);
    format!("{source}-{thumbnail}")
}

#[derive(Deserialize)]
pub struct ObservedMedia {
    pub path: String,
    pub revision: String,
}

pub fn validate_media_revisions(observed: Vec<ObservedMedia>) -> Result<(), String> {
    if observed.is_empty()
        || observed.iter().any(|item| {
            item.revision.is_empty()
                || !Path::new(&item.path).is_file()
                || media_revision(Path::new(&item.path)) != item.revision
        })
    {
        return Err("Media changed; refresh duplicates before deleting.".into());
    }
    Ok(())
}

pub fn delete_media_if_unchanged(
    path: String,
    revision: String,
    observed: Vec<ObservedMedia>,
) -> Result<(), String> {
    crate::media_delete::delete_if_unchanged(Path::new(&path), &revision, &observed)
}

const META_DIR: &str = ".room237-metadata";
#[cfg(test)]
const META_FILE_EXT: &str = ".meta";
const ALBUM_META_FILE: &str = "album.sqlite";

fn supported_timestamp(timestamp: i64) -> bool {
    (-(1i64 << 40)..(1i64 << 40)).contains(&timestamp)
}

pub struct DetachedFileMeta {
    pub a: Option<u64>,
    pub s: Option<i64>,
    pub i: bool,
    pub v: bool,
    pub w: Option<u32>,
    pub h: Option<u32>,
}

impl DetachedFileMeta {
    pub fn pack(&self) -> String {
        let mut packed: u128 = 0;
        let a = self.a.unwrap_or(0) as u128 & ((1 << 40) - 1);
        let s = self.s.unwrap_or(0) as u128 & ((1 << 40) - 1);
        let w = self.w.unwrap_or(0) as u128 & ((1 << 20) - 1);
        let h = self.h.unwrap_or(0) as u128 & ((1 << 20) - 1);
        packed |= a;
        packed |= s << 40;
        packed |= w << 80;
        packed |= h << 100;
        if self.i {
            packed |= 1u128 << 120;
        }
        if self.v {
            packed |= 1u128 << 121;
        }
        if self.a.is_some() {
            packed |= 1u128 << 122;
        }
        if self.s.is_some() {
            packed |= 1u128 << 123;
        }
        if self.w.is_some() {
            packed |= 1u128 << 124;
        }
        if self.h.is_some() {
            packed |= 1u128 << 125;
        }
        // Bit 126 was unused in the legacy format. Retain the complete
        // unsigned positive 40-bit range and mark negative two's-complement dates.
        if self.s.is_some_and(|timestamp| timestamp < 0) {
            packed |= 1u128 << 126;
        }
        packed.to_string()
    }
}

pub(crate) fn unpack_file_meta(packed: &str) -> Result<DetachedFileMeta, String> {
    let p = packed
        .parse::<u128>()
        .map_err(|e| format!("Invalid packed meta: {e}"))?;
    let added = (p & ((1u128 << 40) - 1)) as u64;
    let mut shoot = ((p >> 40) & ((1u128 << 40) - 1)) as i64;
    if p & (1u128 << 126) != 0 {
        shoot -= 1i64 << 40;
    }
    let width = ((p >> 80) & ((1u128 << 20) - 1)) as u32;
    let height = ((p >> 100) & ((1u128 << 20) - 1)) as u32;

    let is_image = (p & (1u128 << 120)) != 0;
    let is_video = (p & (1u128 << 121)) != 0;
    let has_a = (p & (1u128 << 122)) != 0;
    let has_s = (p & (1u128 << 123)) != 0;
    let has_w = (p & (1u128 << 124)) != 0;
    let has_h = (p & (1u128 << 125)) != 0;

    Ok(DetachedFileMeta {
        a: if has_a { Some(added) } else { None },
        s: if has_s { Some(shoot) } else { None },
        i: is_image,
        v: is_video,
        w: if has_w { Some(width) } else { None },
        h: if has_h { Some(height) } else { None },
    })
}

#[derive(Serialize)]
pub struct DetachedMediaEntry {
    #[serde(rename = "blurPreview", skip_serializing_if = "Option::is_none")]
    pub blur_preview: Option<String>,
    pub revision: String,
    pub meta: String,
    pub name: String,
    #[serde(rename = "thumbnailName", skip_serializing_if = "Option::is_none")]
    pub thumbnail_name: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub favorite: Option<bool>,
}

#[derive(Serialize)]
pub struct DetachedAlbum {
    pub path: String,
    pub name: String,
    pub size: usize,
    pub thumb_path: Option<String>,
    pub relative_path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub parent: Option<String>,
    // Survives a rename or a move made outside the application.
    pub directory_id: Option<String>,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct StoredMetadata {
    pub meta: String,
    #[serde(default)]
    pub favorite: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct CachedHash {
    #[serde(default)]
    pub hash: String,
    #[serde(default)]
    pub version: Option<String>,
    #[serde(default)]
    pub bits: Option<u32>,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
pub struct FileMetaEntry {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub blur_preview: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub heic_conversion: Option<HeicConversion>,
    #[serde(default)]
    pub meta: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub meta_source_revision: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub meta_embedded_timestamp: Option<i64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub meta_embedded_revision: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub meta_settings_version: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thumb_failed_processing_version: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub meta_failed_processing_version: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub hash_failed_processing_version: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub meta_failed_source_revision: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thumb_failed_source_revision: Option<String>,
    #[serde(default)]
    pub favorite: bool,
    #[serde(default)]
    pub hash: Option<String>,
    #[serde(default)]
    pub hash_version: Option<String>,
    #[serde(default)]
    pub hash_bits: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub hash_source_revision: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub hash_failed_source_revision: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thumb_source_revision: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thumb_source_hash: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thumb_hash_revision: Option<String>,
    #[serde(default)]
    pub thumb_version: Option<String>,
    #[serde(default)]
    pub hash_failed: bool,
    #[serde(default)]
    pub thumb_failed: bool,
    #[serde(default)]
    pub meta_failed: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct HeicConversion {
    pub output_name: String,
    pub source_revision: String,
    pub output_revision: String,
}

/// HEIC is decoded directly. A HEIC stays hidden only when an older build recorded a JPEG
/// companion that still exists, so a converted library keeps showing one photo per pair.
pub(crate) fn heic_hidden_by_entry(album: &Path, entry: &FileMetaEntry) -> bool {
    entry.heic_conversion.as_ref().is_some_and(|binding| {
        validate_file_name(&binding.output_name).is_ok()
            && fs::symlink_metadata(album.join(&binding.output_name)).is_ok_and(|info| info.is_file())
    })
}

pub(crate) fn hidden_heic_names(album: &Path) -> Result<std::collections::HashSet<String>, String> {
    let database = album.join(".room237-metadata/album.sqlite");
    if !database.is_file() {
        return Ok(Default::default());
    }
    let mut reader = crate::album_db::FileReader::new(&database)?;
    let mut hidden = std::collections::HashSet::new();
    for file in fs::read_dir(album).map_err(|e| e.to_string())? {
        let file = file.map_err(|e| e.to_string())?;
        let path = file.path();
        if !crate::util::has_extension(&path, &["heic"]) {
            continue;
        }
        let name = file.file_name().to_string_lossy().into_owned();
        if reader.get(&name)?.is_some_and(|entry| heic_hidden_by_entry(album, &entry)) {
            hidden.insert(name);
        }
    }
    Ok(hidden)
}

// Only a current, recorded relationship can make two files one logical photo.
pub(crate) fn heic_binding_current(
    dir: &Path,
    name: &str,
    binding: &HeicConversion,
) -> Result<bool, String> {
    validate_file_name(name)?;
    validate_file_name(&binding.output_name)?;
    let raw = dir.join(name);
    let jpeg = dir.join(&binding.output_name);
    Ok(crate::util::has_extension(&raw, &["heic"])
        && crate::util::has_extension(&jpeg, &["jpg", "jpeg"])
        && fs::symlink_metadata(&raw).is_ok_and(|info| info.is_file())
        && fs::symlink_metadata(&jpeg).is_ok_and(|info| info.is_file())
        && source_revision(&raw).as_deref() == Some(&binding.source_revision)
        && source_revision(&jpeg).as_deref() == Some(&binding.output_revision))
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct AlbumMeta {
    #[serde(default)]
    pub files: HashMap<String, FileMetaEntry>,
    #[serde(default)]
    pub duplicates_ignore: Vec<[String; 2]>,
}

#[cfg(target_family = "unix")]
const EMBEDDED_TS_XATTR: &str = "user.room237.shoot_ts";

#[cfg(unix)]
fn embedded_timestamp_revision(path: &Path) -> Option<String> {
    use std::os::unix::fs::MetadataExt;
    // Xattrs follow the directory entry's object, including symlinks. Their
    // changes update ctime without invalidating visual content/hash revisions.
    let info = fs::symlink_metadata(path).ok()?;
    Some(format!(
        "{:x}-{:x}-{:x}-{:x}",
        info.dev(),
        info.ino(),
        info.ctime(),
        info.ctime_nsec()
    ))
}

#[cfg(not(unix))]
fn embedded_timestamp_revision(_path: &Path) -> Option<String> {
    None
}

fn embedded_timestamp_stamp_if_current(path: &Path, expected: Option<i64>) -> Option<String> {
    let before = embedded_timestamp_revision(path);
    let actual = read_embedded_timestamp(path);
    let after = embedded_timestamp_revision(path);
    if before == after && actual == expected {
        after
    } else {
        None
    }
}

#[cfg(target_family = "unix")]
pub(crate) fn read_embedded_timestamp(path: &Path) -> Option<i64> {
    xattr::get(path, EMBEDDED_TS_XATTR)
        .ok()
        .flatten()
        .and_then(|v| std::str::from_utf8(&v).ok()?.parse::<i64>().ok())
        .filter(|timestamp| supported_timestamp(*timestamp))
}

#[cfg(all(unix, test))]
fn write_embedded_timestamp(path: &Path, timestamp: i64) -> Result<(), String> {
    xattr::set(path, EMBEDDED_TS_XATTR, timestamp.to_string().as_bytes())
        .map_err(|error| error.to_string())
}

#[cfg(target_os = "windows")]
const EMBEDDED_TS_STREAM: &str = "room237_ts";

#[cfg(target_os = "windows")]
fn ads_path(path: &Path) -> PathBuf {
    let mut s = OsString::from(path);
    s.push(format!(":{}", EMBEDDED_TS_STREAM));
    PathBuf::from(s)
}

#[cfg(target_os = "windows")]
pub(crate) fn read_embedded_timestamp(path: &Path) -> Option<i64> {
    let ads = ads_path(path);
    fs::read(&ads)
        .ok()
        .and_then(|v| String::from_utf8(v).ok())
        .and_then(|s| s.trim().parse::<i64>().ok())
        .filter(|timestamp| supported_timestamp(*timestamp))
}

#[cfg(not(any(target_family = "unix", target_os = "windows")))]
pub(crate) fn read_embedded_timestamp(_path: &Path) -> Option<i64> {
    None
}

#[cfg(target_os = "windows")]
fn write_embedded_timestamp(path: &Path, timestamp: i64) -> Result<(), String> {
    let ads = ads_path(path);
    fs::write(ads, timestamp.to_string()).map_err(|e| e.to_string())
}

#[cfg(not(any(target_family = "unix", target_os = "windows")))]
fn write_embedded_timestamp(_path: &Path, _timestamp: i64) -> Result<(), String> {
    Err("Embedded timestamp storage unsupported on this platform".to_string())
}

fn meta_dir(dir: &Path) -> PathBuf {
    dir.join(META_DIR)
}

fn album_meta_path(dir: &Path) -> PathBuf {
    meta_dir(dir).join(ALBUM_META_FILE)
}

fn ensure_meta_dir(dir: &Path) -> std::io::Result<()> {
    // Metadata operations may race an album deletion. Only create the service
    // directory, never resurrect a missing album or one of its ancestors.
    let path = meta_dir(dir);
    match fs::create_dir(&path) {
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists && path.is_dir() => Ok(()),
        result => result,
    }
}

pub(crate) fn lock_album_metadata(dir: &Path) -> Result<FileLock, String> {
    let dir = dir.canonicalize().map_err(|error| error.to_string())?;
    let identity = crate::media_move::directory_id(&dir)?;
    // Outside the data directory so clearing derived/user metadata cannot
    // unlink a lock held by a writer or a waiting process.
    let lease = FileLock::acquire(
        &dir.join(".room237-metadata.lock"),
        METADATA_LEASE_TIMEOUT,
        Duration::from_millis(5),
    )?;
    if crate::media_move::directory_id(&dir)? != identity {
        return Err("Album changed while waiting for metadata lease".into());
    }
    Ok(lease)
}

pub(crate) struct MetadataWriteGuard {
    _leases: Vec<FileLock>,
    _tree: Option<crate::album_tree::TreeLease>,
    directories: Vec<PathBuf>,
}

pub(crate) fn read_selected_metadata(
    dir: &Path,
    names: &[String],
    related: bool,
) -> Result<AlbumMeta, String> {
    if !meta_dir(dir).try_exists().map_err(|e| e.to_string())? {
        return Ok(AlbumMeta::default());
    }
    let _guard = lock_metadata_for_write(&[dir])?;
    ensure_database(dir)?;
    crate::album_db::subset(&album_meta_path(dir), names, related)
}

pub(crate) fn read_selected_with_guard(
    dir: &Path,
    names: &[String],
    related: bool,
    guard: &MetadataWriteGuard,
) -> Result<Arc<AlbumMeta>, String> {
    let canonical = dir.canonicalize().map_err(|e| e.to_string())?;
    if !guard.directories.contains(&canonical) {
        return Err("Album metadata lease is required".into());
    }
    if !meta_dir(dir).try_exists().map_err(|e| e.to_string())? {
        return Ok(Arc::new(AlbumMeta::default()));
    }
    ensure_database(dir)?;
    crate::album_db::subset(&album_meta_path(dir), names, related).map(Arc::new)
}

fn write_selected(dir: &Path, names: &[String], album: &AlbumMeta) -> Result<(), String> {
    ensure_database(dir)?;
    crate::album_db::patch(&album_meta_path(dir), names, album)?;
    invalidate_album_snapshot(dir);
    Ok(())
}

fn write_timestamp_rows(dir: &Path, album: Arc<AlbumMeta>) -> Result<(), String> {
    write_selected(
        dir,
        &album.files.keys().cloned().collect::<Vec<_>>(),
        &album,
    )
}

pub(crate) fn remove_deleted_metadata_with_guard(
    dir: &Path,
    names: &[String],
    guard: &MetadataWriteGuard,
) -> Result<(), String> {
    let canonical = dir.canonicalize().map_err(|e| e.to_string())?;
    if !guard.directories.contains(&canonical) {
        return Err("Album metadata lease is required".into());
    }
    ensure_database(dir)?;
    let mut removed = Vec::new();
    for name in names {
        validate_file_name(name)?;
        match fs::symlink_metadata(dir.join(name)) {
            Ok(_) => continue,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                removed.push(name.clone())
            }
            Err(error) => return Err(error.to_string()),
        }
    }
    crate::album_db::remove(&album_meta_path(dir), &removed)?;
    if let Err(error) = crate::catalog::forget(dir, &removed) {
        log::warn!("Catalog cleanup failed: {error}");
    }
    invalidate_album_snapshot(dir);
    Ok(())
}

fn try_lock_metadata_for_write(dir: &Path) -> Result<Option<MetadataWriteGuard>, String> {
    let canonical = dir.canonicalize().map_err(|error| error.to_string())?;
    let Some(tree) = crate::album_tree::try_read(&[&canonical])? else {
        return Ok(None);
    };
    let identity = crate::media_move::directory_id(&canonical)?;
    let Some(lease) = FileLock::try_acquire(&canonical.join(".room237-metadata.lock"))? else {
        return Ok(None);
    };
    if crate::media_move::directory_id(&canonical)? != identity {
        return Err("Album changed during metadata admission".into());
    }
    tree.ensure_current(&[&canonical])?;
    Ok(Some(MetadataWriteGuard {
        _leases: vec![lease],
        _tree: Some(tree),
        directories: vec![canonical],
    }))
}

fn acquire_metadata_leases(dirs: &[&Path]) -> Result<(Vec<PathBuf>, Vec<FileLock>), String> {
    let mut directories = dirs
        .iter()
        .map(|dir| dir.canonicalize().map_err(|error| error.to_string()))
        .collect::<Result<Vec<_>, _>>()?;
    directories.sort();
    directories.dedup();
    // Never wait for an external process while holding the global store mutex.
    // Multi-album operations use the same canonical order in every process.
    let leases = directories
        .iter()
        .map(|dir| lock_album_metadata(dir))
        .collect::<Result<Vec<_>, _>>()?;
    Ok((directories, leases))
}

pub(crate) fn lock_metadata_for_write(dirs: &[&Path]) -> Result<MetadataWriteGuard, String> {
    // A caller may hold a leaf lease from before a root marker existed. Do not
    // wait behind the tree writer now draining that leaf.
    let tree = crate::album_tree::try_read(dirs)?
        .ok_or("Album tree is being renamed; retry metadata write")?;
    let (directories, leases) = acquire_metadata_leases(dirs)?;
    tree.ensure_current(dirs)?;
    Ok(MetadataWriteGuard {
        _leases: leases,
        _tree: Some(tree),
        directories,
    })
}

#[cfg(test)]
pub(crate) fn lock_metadata_for_move(
    dirs: &[&Path],
) -> Result<(MutexGuard<'static, ()>, MetadataWriteGuard), String> {
    let tree = crate::album_tree::read(dirs)?;
    let (moving, mut guard) = lock_metadata_for_move_with_tree(dirs, &tree)?;
    guard._tree = Some(tree);
    Ok((moving, guard))
}

pub(crate) fn lock_metadata_for_move_with_tree(
    dirs: &[&Path],
    tree: &crate::album_tree::TreeLease,
) -> Result<(MutexGuard<'static, ()>, MetadataWriteGuard), String> {
    let (directories, leases) = acquire_metadata_leases(dirs)?;
    tree.ensure_current(dirs)?;
    let moving = crate::util::MEDIA_MOVE_LOCK.lock().unwrap();
    let guard = MetadataWriteGuard {
        _leases: leases,
        _tree: None,
        directories,
    };
    Ok((moving, guard))
}

pub(crate) fn update_file_meta(
    dir: &Path,
    name: &str,
    update: impl FnOnce(&mut FileMetaEntry),
) -> Result<FileMetaEntry, String> {
    validate_file_name(name)?;
    let _guard = lock_metadata_for_write(&[dir])?;
    ensure_database(dir)?;
    let result = crate::album_db::update_rows(&album_meta_path(dir), |conn, rows| {
        let mut entry = crate::album_db::entry(conn, name)?;
        update(&mut entry);
        rows.insert(name.to_string(), entry.clone());
        Ok(entry)
    })?;
    invalidate_album_snapshot(dir);
    Ok(result)
}

fn update_file_meta_if_current(
    path: &Path,
    expected_revision: &str,
    expected_source: Option<&str>,
    update: impl FnOnce(&mut FileMetaEntry) + Send + 'static,
) -> Result<FileMetaEntry, String> {
    submit_derived_write(path, Some(expected_revision), expected_source, update)
}

pub(crate) fn hash_cache_current(path: &Path, entry: &FileMetaEntry) -> bool {
    if entry.hash_failed {
        entry.hash_failed_processing_version.as_deref()
            == Some(&read_settings().processing_version())
            && source_revision(path).as_deref() == entry.hash_failed_source_revision.as_deref()
            && entry.hash_failed_source_revision.is_some()
            && (!read_settings().preload.thumb_hash_retry_on_thumb_change
                || entry.hash_source_revision.as_deref() == Some(&media_revision(path)))
    } else {
        path.is_file() && entry.hash_source_revision.as_deref() == Some(&media_revision(path))
    }
}

#[cfg(test)]
pub(crate) fn update_album_meta(
    dir: &Path,
    update: impl FnOnce(&mut AlbumMeta),
) -> Result<(), String> {
    let _guard = lock_metadata_for_write(&[dir])?;
    let mut album = (*read_album_snapshot_without_lock(dir, true)?).clone();
    update(&mut album);
    write_album_meta_owned_without_lock(dir, album)
}

pub(crate) fn update_file_meta_if_source_current(
    path: &Path,
    expected_source: &str,
    update: impl FnOnce(&mut FileMetaEntry) + Send + 'static,
) -> Result<FileMetaEntry, String> {
    submit_derived_write(path, None, Some(expected_source), update)
}

#[cfg(test)]
pub(crate) fn metadata_cache_current(path: &Path, entry: &FileMetaEntry) -> bool {
    metadata_cache_current_with_settings(path, entry, &read_settings())
}

pub(crate) fn metadata_cache_current_with_settings(
    path: &Path,
    entry: &FileMetaEntry,
    settings: &crate::settings::AdvancedSettings,
) -> bool {
    metadata_content_cache_current_with_settings(path, entry, settings)
        && (entry
            .meta_embedded_revision
            .as_deref()
            .is_some_and(|revision| embedded_timestamp_revision(path).as_deref() == Some(revision))
            || entry.meta_embedded_timestamp == read_embedded_timestamp(path))
}

fn metadata_content_cache_current_with_settings(
    path: &Path,
    entry: &FileMetaEntry,
    settings: &crate::settings::AdvancedSettings,
) -> bool {
    entry.meta_settings_version.as_deref() == Some(&settings.metadata_version())
        && entry.meta_source_revision.is_some()
        && entry.meta_source_revision.as_deref() == source_revision(path).as_deref()
        && entry
            .meta
            .as_deref()
            .is_some_and(|meta| unpack_file_meta(meta).is_ok())
}

pub(crate) fn mark_thumb_failed(
    path: &Path,
    expected_source: &str,
    processing_version: &str,
) -> Result<(), String> {
    let source = expected_source.to_string();
    let processing_version = thumb_failure_processing_version(path, processing_version);
    update_file_meta_if_source_current(path, expected_source, move |entry| {
        entry.thumb_failed_processing_version = Some(processing_version);
        entry.thumb_failed = true;
        entry.thumb_failed_source_revision = Some(source);
    })
    .map(|_| ())
}

fn thumb_failure_processing_version(path: &Path, processing_version: &str) -> String {
    match crate::thumb::detached_thumbnail_name(path) {
        Some(name) => format!("{processing_version}|preview-name:{name}"),
        None => processing_version.to_string(),
    }
}

pub(crate) fn mark_meta_failed(
    path: &Path,
    expected_source: &str,
    processing_version: &str,
) -> Result<(), String> {
    let source = expected_source.to_string();
    let processing_version = processing_version.to_string();
    update_file_meta_if_source_current(path, expected_source, move |entry| {
        entry.meta_failed_processing_version = Some(processing_version);
        entry.meta_failed = true;
        entry.meta_failed_source_revision = Some(source);
    })
    .map(|_| ())
}

pub(crate) fn read_file_entry(path: &Path) -> Result<Option<FileMetaEntry>, String> {
    let dir = path.parent().ok_or("Invalid path")?;
    let name = path
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or("Invalid filename")?;
    if album_meta_path(dir)
        .try_exists()
        .map_err(|e| e.to_string())?
    {
        crate::album_db::read_file(&album_meta_path(dir), name)
    } else if meta_dir(dir).try_exists().map_err(|e| e.to_string())? {
        prepare_database(dir)?;
        crate::album_db::read_file(&album_meta_path(dir), name)
    } else {
        Ok(None)
    }
}

pub(crate) fn is_thumb_failed(path: &Path) -> bool {
    read_file_entry(path).ok().flatten().is_some_and(|entry| {
        entry.thumb_failed
            && entry.thumb_failed_processing_version.as_deref()
                == Some(
                    thumb_failure_processing_version(path, &read_settings().processing_version())
                        .as_str(),
                )
            && entry.thumb_failed_source_revision.is_some()
            && entry.thumb_failed_source_revision.as_deref() == source_revision(path).as_deref()
    })
}

pub(crate) fn is_meta_failed(path: &Path) -> bool {
    read_file_entry(path).ok().flatten().is_some_and(|entry| {
        entry.meta_failed
            && entry.meta_failed_processing_version.as_deref()
                == Some(&read_settings().processing_version())
            && entry.meta_failed_source_revision.is_some()
            && entry.meta_failed_source_revision.as_deref() == source_revision(path).as_deref()
    })
}

pub(crate) fn mark_hash_failed_if_current(
    path: &Path,
    expected_revision: &str,
    cfg: &crate::settings::HashConfig,
    processing_version: &str,
) -> Result<(), String> {
    let path_owned = path.to_path_buf();
    let revision = expected_revision.to_string();
    let bits = cfg.bits;
    let version = cfg.hash_version.clone();
    let processing_version = processing_version.to_string();
    update_file_meta_if_current(path, expected_revision, None, move |entry| {
        entry.hash_failed_processing_version = Some(processing_version);
        entry.hash = None;
        entry.hash_bits = Some(bits);
        entry.hash_version = Some(version);
        entry.hash_source_revision = Some(revision);
        entry.hash_failed_source_revision = source_revision(&path_owned);
        entry.hash_failed = true;
    })
    .map(|_| ())
}

#[cfg(test)]
fn read_album_meta_without_lock(dir: &Path, migrate: bool) -> Result<AlbumMeta, String> {
    if album_meta_path(dir)
        .try_exists()
        .map_err(|e| e.to_string())?
    {
        return crate::album_db::read(&album_meta_path(dir));
    }
    let album = read_legacy_album(dir)?;
    if migrate {
        ensure_database(dir)?;
    }
    Ok(album)
}

pub(crate) fn prepare_database(dir: &Path) -> Result<(), String> {
    let _guard = lock_metadata_for_write(&[dir])?;
    ensure_database(dir)?;
    crate::album_db::FileReader::new(&album_meta_path(dir)).map(|_| ())
}

fn ensure_database(dir: &Path) -> Result<(), String> {
    if !album_meta_path(dir)
        .try_exists()
        .map_err(|e| e.to_string())?
    {
        // Parse before creating anything. Corrupt legacy metadata must survive.
        ensure_meta_dir(dir).map_err(|e| e.to_string())?;
        crate::album_db::initialize_legacy(&album_meta_path(dir))?;
    }
    Ok(())
}

#[cfg(test)]
fn read_legacy_album(dir: &Path) -> Result<AlbumMeta, String> {
    let album_path = meta_dir(dir).join("album.json");

    match fs::read_to_string(&album_path) {
        Ok(txt) => {
            return serde_json::from_str::<AlbumMeta>(&txt)
                .map_err(|error| format!("Invalid metadata in {}: {error}", album_path.display()))
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => return Err(format!("Cannot read {}: {error}", album_path.display())),
    }

    let mut album = AlbumMeta::default();
    let meta_paths: Vec<(String, PathBuf)> = if let Ok(entries) = fs::read_dir(meta_dir(dir)) {
        entries
            .flatten()
            .filter_map(|entry| {
                let path = entry.path();
                if !path.is_file() {
                    return None;
                }
                let name = path.file_name()?.to_str()?;
                let stem = name.strip_suffix(META_FILE_EXT)?;
                Some((stem.to_string(), path))
            })
            .collect()
    } else {
        Vec::new()
    };

    let file_entries: Result<Vec<(String, FileMetaEntry)>, String> = meta_paths
        .par_iter()
        .map(|(stem, path)| {
            let txt = fs::read_to_string(path).map_err(|error| error.to_string())?;
            let parsed = serde_json::from_str::<FileMetaEntry>(&txt)
                .map_err(|error| format!("Invalid metadata in {}: {error}", path.display()))?;
            Ok((stem.clone(), parsed))
        })
        .collect();

    for (stem, entry) in file_entries? {
        album.files.insert(stem, entry);
    }

    // Keep legacy files as a migration backup; SQLite is authoritative once
    // published. Subsequent writes never touch these files.

    Ok(album)
}

#[cfg(test)]
pub(crate) fn read_album_meta(dir: &Path) -> Arc<AlbumMeta> {
    match read_album_meta_checked(dir) {
        Ok(data) => data,
        Err(error) => {
            log::error!("{error}");
            META_CACHE
                .lock()
                .unwrap()
                .iter()
                .find(|entry| entry.dir == dir)
                .map(|entry| entry.data.clone())
                .unwrap_or_else(|| Arc::new(AlbumMeta::default()))
        }
    }
}

#[cfg(test)]
pub(crate) fn read_album_meta_checked(dir: &Path) -> Result<Arc<AlbumMeta>, String> {
    let guard = STORE_WRITE_LOCK.lock().unwrap();
    // Migration is the only read that needs a writer lease. Ordinary SQLite
    // snapshots also work in read-only albums.
    let legacy_exists = !album_meta_path(dir).exists()
        && (meta_dir(dir).join("album.json").exists()
            || fs::read_dir(meta_dir(dir)).is_ok_and(|entries| {
                entries.flatten().any(|entry| {
                    entry
                        .path()
                        .extension()
                        .is_some_and(|extension| extension == "meta")
                })
            }));
    if !legacy_exists {
        // A legacy file may appear after the check (e.g. an older process).
        // This read may load it, but only a leased reader may publish migration.
        return read_album_snapshot_without_lock(dir, false);
    }
    drop(guard);
    let _guard = lock_metadata_for_write(&[dir])?;
    read_album_snapshot_without_lock(dir, true)
}

#[cfg(test)]
fn read_album_snapshot_without_lock(dir: &Path, migrate: bool) -> Result<Arc<AlbumMeta>, String> {
    let stamp = metadata_stamp(&album_meta_path(dir));
    let mut cache = META_CACHE.lock().unwrap();
    if let Some(position) = cache
        .iter()
        .position(|entry| entry.dir == dir && Some(entry.stamp) == stamp)
    {
        let entry = cache.remove(position).unwrap();
        let data = entry.data.clone();
        cache.push_back(entry);
        return Ok(data);
    }
    drop(cache);
    let data = Arc::new(read_album_meta_without_lock(dir, migrate)?);
    cache_album_snapshot_if_unchanged(dir, stamp, &data);
    Ok(data)
}

#[cfg(test)]
fn cache_album_snapshot_if_unchanged(
    dir: &Path,
    observed_stamp: Option<MetadataStamp>,
    data: &Arc<AlbumMeta>,
) {
    // An external writer may change the database during this read.
    // Never label an older snapshot with the replacement's stamp.
    if let Some(stamp) =
        metadata_stamp(&album_meta_path(dir)).filter(|stamp| Some(*stamp) == observed_stamp)
    {
        let mut cache = META_CACHE.lock().unwrap();
        cache.retain(|entry| entry.dir != dir);
        if cache.len() >= 2 {
            cache.pop_front();
        }
        cache.push_back(MetadataSnapshot {
            dir: dir.to_path_buf(),
            stamp,
            data: data.clone(),
        });
    }
}

#[cfg(test)]
fn write_album_meta_owned_without_lock(dir: &Path, data: AlbumMeta) -> Result<(), String> {
    write_album_meta_arc_without_lock(dir, Arc::new(data))
}
#[cfg(test)]
fn write_album_meta_arc_without_lock(dir: &Path, data: Arc<AlbumMeta>) -> Result<(), String> {
    ensure_database(dir)?;
    crate::album_db::replace(&album_meta_path(dir), &data)?;
    invalidate_album_snapshot(dir);
    Ok(())
}

fn invalidate_album_snapshot(_dir: &Path) {
    #[cfg(test)]
    META_CACHE.lock().unwrap().retain(|entry| entry.dir != _dir);
}

#[cfg(test)]
pub(crate) fn transfer_media_metadata_batch_caller_holds_lock(
    source_dir: &Path,
    target_dir: &Path,
    moves: &[(String, String)],
    guard: &MetadataWriteGuard,
) -> Result<(), String> {
    transfer_media_state(source_dir, target_dir, moves, guard, true)
}

#[cfg(test)]
fn transfer_media_state(
    source_dir: &Path,
    target_dir: &Path,
    moves: &[(String, String)],
    guard: &MetadataWriteGuard,
    transfer_fields: bool,
) -> Result<(), String> {
    transfer_media_state_with_hooks(
        source_dir,
        target_dir,
        moves,
        guard,
        transfer_fields,
        |_| Ok(()),
        |_| Ok(()),
    )
}

#[derive(Serialize, Deserialize)]
struct MoveMetadataRow {
    name: String,
    before: Option<FileMetaEntry>,
    after: Option<FileMetaEntry>,
}

#[derive(Serialize, Deserialize)]
pub(crate) struct MoveMetadataUndo {
    source: Vec<MoveMetadataRow>,
    target: Vec<MoveMetadataRow>,
}

impl MoveMetadataUndo {
    pub(crate) fn validate(&self) -> Result<(), String> {
        for rows in [&self.source, &self.target] {
            let mut seen = std::collections::HashSet::new();
            for row in rows {
                validate_file_name(&row.name)?;
                if !seen.insert(&row.name) {
                    return Err("Duplicate move metadata row".into());
                }
                for entry in [&row.before, &row.after].into_iter().flatten() {
                    if let Some(binding) = &entry.heic_conversion {
                        validate_file_name(&binding.output_name)?;
                    }
                }
            }
        }
        Ok(())
    }

    fn validate_current(&self, source: &AlbumMeta, target: &AlbumMeta) -> Result<(), String> {
        self.validate()?;
        for (album, rows) in [(source, &self.source), (target, &self.target)] {
            for row in rows {
                let current = album.files.get(&row.name);
                if current != row.before.as_ref() && current != row.after.as_ref() {
                    return Err(format!("Move metadata changed; preserving {}", row.name));
                }
            }
        }
        Ok(())
    }
}

pub(crate) fn validate_move_metadata_undo_with_guard(
    source: &Path,
    target: &Path,
    undo: &MoveMetadataUndo,
    guard: &MetadataWriteGuard,
) -> Result<(), String> {
    let source_album = read_selected_with_guard(
        source,
        &undo
            .source
            .iter()
            .map(|row| row.name.clone())
            .collect::<Vec<_>>(),
        false,
        guard,
    )?;
    let target_album = read_selected_with_guard(
        target,
        &undo
            .target
            .iter()
            .map(|row| row.name.clone())
            .collect::<Vec<_>>(),
        false,
        guard,
    )?;
    undo.validate_current(&source_album, &target_album)
}

pub(crate) fn restore_move_metadata_with_guard(
    source: &Path,
    target: &Path,
    undo: &MoveMetadataUndo,
    guard: &MetadataWriteGuard,
) -> Result<(), String> {
    restore_move_metadata_with_hooks(source, target, undo, guard, |_| Ok(()))
}

pub(crate) fn restore_move_metadata_with_hooks(
    source: &Path,
    target: &Path,
    undo: &MoveMetadataUndo,
    guard: &MetadataWriteGuard,
    mut checkpoint: impl FnMut(&str) -> Result<(), String>,
) -> Result<(), String> {
    undo.validate()?;
    let mut source_album = (*read_selected_with_guard(
        source,
        &undo
            .source
            .iter()
            .map(|row| row.name.clone())
            .collect::<Vec<_>>(),
        false,
        guard,
    )?)
    .clone();
    let mut target_album = (*read_selected_with_guard(
        target,
        &undo
            .target
            .iter()
            .map(|row| row.name.clone())
            .collect::<Vec<_>>(),
        false,
        guard,
    )?)
    .clone();
    // Check both albums before publishing either rollback. Rows unrelated to
    // this move, and writers that changed a touched row, remain untouched.
    undo.validate_current(&source_album, &target_album)?;
    for (dir, album, rows) in [
        (target, &mut target_album, &undo.target),
        (source, &mut source_album, &undo.source),
    ] {
        let mut changed = false;
        for row in rows {
            if album.files.get(&row.name) != row.before.as_ref() {
                match &row.before {
                    Some(entry) => {
                        album.files.insert(row.name.clone(), entry.clone());
                    }
                    None => {
                        album.files.remove(&row.name);
                    }
                }
                changed = true;
            }
        }
        if changed {
            write_selected(
                dir,
                &rows.iter().map(|row| row.name.clone()).collect::<Vec<_>>(),
                album,
            )?;
            checkpoint(if dir == target {
                "move_recovery_metadata_target"
            } else {
                "move_recovery_metadata_source"
            })?;
        }
    }
    Ok(())
}

pub(crate) fn transfer_media_state_journaled(
    source: &Path,
    target: &Path,
    moves: &[(String, String)],
    guard: &MetadataWriteGuard,
    transfer_fields: bool,
    journal: &mut crate::media_move::MoveJournal,
    checkpoint: impl FnMut(&str) -> Result<(), String>,
) -> Result<(), String> {
    transfer_media_state_with_hooks(
        source,
        target,
        moves,
        guard,
        transfer_fields,
        |undo| journal.record_metadata(undo),
        checkpoint,
    )
}

fn transfer_media_state_with_hooks(
    source_dir: &Path,
    target_dir: &Path,
    moves: &[(String, String)],
    guard: &MetadataWriteGuard,
    transfer_fields: bool,
    prepare: impl FnOnce(MoveMetadataUndo) -> Result<(), String>,
    mut checkpoint: impl FnMut(&str) -> Result<(), String>,
) -> Result<(), String> {
    if moves.is_empty() || source_dir == target_dir {
        return Ok(());
    }
    for dir in [source_dir, target_dir] {
        let canonical = dir.canonicalize().map_err(|error| error.to_string())?;
        if !guard.directories.contains(&canonical) {
            return Err("Metadata transfer requires leases for both albums".into());
        }
    }
    if transfer_fields || meta_dir(source_dir).exists() {
        ensure_database(source_dir)?;
    }
    ensure_database(target_dir)?;
    let source_names: Vec<String> = moves.iter().map(|(name, _)| name.clone()).collect();
    let target_names: Vec<String> = moves.iter().map(|(_, name)| name.clone()).collect();
    let mut source_album = if album_meta_path(source_dir).exists() {
        crate::album_db::subset(&album_meta_path(source_dir), &source_names, false)?
    } else {
        AlbumMeta::default()
    };
    let previous_source: HashMap<String, FileMetaEntry> = moves
        .iter()
        .filter_map(|(name, _)| {
            source_album
                .files
                .get(name)
                .map(|entry| (name.clone(), entry.clone()))
        })
        .collect();
    let previous_target =
        crate::album_db::subset(&album_meta_path(target_dir), &target_names, false)?;
    let mut target_album = previous_target.clone();
    let mut bindings_changed = false;
    let names: std::collections::HashMap<&str, &str> = moves
        .iter()
        .map(|(source, target)| (source.as_str(), target.as_str()))
        .collect();
    for (source_name, target_name) in moves {
        target_album.files.remove(target_name);
        let mut entry = if transfer_fields {
            source_album.files.remove(source_name)
        } else {
            source_album
                .files
                .get_mut(source_name)
                .and_then(|entry| {
                    let binding = entry.heic_conversion.take();
                    bindings_changed |= binding.is_some();
                    binding
                })
                .map(|binding| FileMetaEntry {
                    heic_conversion: Some(binding),
                    ..Default::default()
                })
        };
        if let Some(entry) = entry.as_mut() {
            if transfer_fields {
                let preview = crate::thumb::thumb_path(
                    &target_dir.join(target_name),
                    &target_dir.join(".room237-thumb"),
                )?;
                // Artifact publication preserves an existing destination. Its
                // bytes cannot inherit the moved source's freshness marker.
                if fs::symlink_metadata(preview).is_ok() {
                    entry.thumb_version = None;
                    entry.thumb_source_revision = None;
                }
            }
            entry.heic_conversion = entry.heic_conversion.take().and_then(|binding| {
                let output_name = names.get(binding.output_name.as_str())?;
                let raw = target_dir.join(target_name);
                let jpeg = target_dir.join(output_name);
                (source_revision(&raw).as_deref() == Some(&binding.source_revision)
                    && source_revision(&jpeg).as_deref() == Some(&binding.output_revision))
                .then(|| HeicConversion {
                    output_name: (*output_name).into(),
                    ..binding
                })
            });
        }
        if let Some(entry) = entry {
            target_album.files.insert(target_name.clone(), entry);
        }
    }
    let changes = |before: &HashMap<String, FileMetaEntry>, after: &AlbumMeta, source: bool| {
        let mut seen = std::collections::HashSet::new();
        moves
            .iter()
            .filter_map(|(old, new)| {
                let name = if source { old } else { new };
                if !seen.insert(name) || before.get(name) == after.files.get(name) {
                    return None;
                }
                Some(MoveMetadataRow {
                    name: name.clone(),
                    before: before.get(name).cloned(),
                    after: after.files.get(name).cloned(),
                })
            })
            .collect()
    };
    prepare(MoveMetadataUndo {
        source: changes(&previous_source, &source_album, true),
        target: changes(&previous_target.files, &target_album, false),
    })?;
    // Never discard source metadata until the destination is persisted.
    write_selected(target_dir, &target_names, &target_album)?;
    checkpoint("after_target_metadata")?;
    if !transfer_fields && !bindings_changed {
        return Ok(());
    }
    if let Err(error) = write_selected(source_dir, &source_names, &source_album) {
        return Err(
            match write_selected(target_dir, &target_names, &previous_target) {
                Ok(()) => error,
                Err(rollback) => format!("{error}; destination metadata rollback: {rollback}"),
            },
        );
    }
    checkpoint("after_source_metadata")?;
    Ok(())
}

pub(crate) fn load_album_file_hash(path: &Path) -> Option<CachedHash> {
    let entry = read_file_entry(path).ok().flatten()?;
    if !hash_cache_current(path, &entry) {
        return None;
    }
    let hash = entry.hash.as_ref()?;
    Some(CachedHash {
        hash: hash.clone(),
        version: entry.hash_version.clone(),
        bits: entry.hash_bits,
    })
}

pub(crate) fn write_album_file_hash(
    path: &Path,
    hash_b64: String,
    hash_version: String,
    hash_bits: u32,
    expected_revision: &str,
) -> Result<(), String> {
    let revision = expected_revision.to_string();
    update_file_meta_if_current(path, expected_revision, None, move |entry| {
        entry.hash = Some(hash_b64);
        entry.hash_version = Some(hash_version);
        entry.hash_bits = Some(hash_bits);
        entry.hash_source_revision = Some(revision);
        entry.hash_failed_source_revision = None;
        entry.hash_failed = false;
    })
    .map(|_| ())
}

#[cfg(test)]
pub(crate) fn load_thumb_version(path: &Path) -> Option<String> {
    read_file_entry(path)
        .ok()
        .flatten()
        .filter(|entry| entry.thumb_source_revision.as_deref() == source_revision(path).as_deref())
        .and_then(|entry| entry.thumb_version)
}

#[cfg(test)]
pub(crate) fn write_thumb_version(path: &Path, version: &str, source: &str) -> Result<(), String> {
    write_thumb_state(path, version, source, None, None, None)
}

pub(crate) fn write_thumb_state(
    path: &Path,
    thumb_version: &str,
    expected_source: &str,
    source_hash: Option<String>,
    hash_revision: Option<String>,
    blur_preview: Option<String>,
) -> Result<(), String> {
    if source_revision(path).as_deref() != Some(expected_source) {
        return Err("Media changed during thumbnail generation".into());
    }
    let source = expected_source.to_string();
    let version = thumb_version.to_string();
    update_file_meta_if_current(
        path,
        &media_revision(path),
        Some(expected_source),
        move |entry| {
            entry.blur_preview = blur_preview;
            entry.thumb_source_revision = Some(source);
            entry.thumb_source_hash = source_hash;
            entry.thumb_hash_revision = hash_revision;
            entry.thumb_version = Some(version);
            entry.thumb_failed = false;
            entry.thumb_failed_source_revision = None;
        },
    )
    .map(|_| ())
}

pub fn get_metadata_with_favorite(path: &Path) -> Result<StoredMetadata, String> {
    get_metadata_with_favorite_with_settings(path, &read_settings())
}

fn get_metadata_with_favorite_with_settings(
    path: &Path,
    settings: &crate::settings::AdvancedSettings,
) -> Result<StoredMetadata, String> {
    if let Some(entry) = read_file_entry(path)?
        .filter(|entry| metadata_cache_current_with_settings(path, entry, settings))
    {
        return Ok(StoredMetadata {
            meta: entry.meta.clone().unwrap(),
            favorite: entry.favorite,
        });
    }
    get_file_metadata_with_settings(&path.to_string_lossy(), settings)?;
    let entry = read_file_entry(path)?
        .filter(|entry| metadata_cache_current_with_settings(path, entry, settings))
        .ok_or("Media changed during metadata extraction")?;
    Ok(StoredMetadata {
        meta: entry.meta.clone().unwrap(),
        favorite: entry.favorite,
    })
}

pub fn datetime_original(p: &Path) -> Option<i64> {
    let mut buf = BufReader::new(File::open(p).ok()?);
    let exif = Reader::new().read_from_container(&mut buf).ok()?;
    fn ascii(f: &Field) -> Option<String> {
        if let Value::Ascii(ref v) = f.value {
            v.get(0)
                .and_then(|bytes| std::str::from_utf8(bytes).ok())
                .map(|s| s.trim_matches(char::from(0)).trim().to_string())
        } else {
            None
        }
    }
    let txt = [Tag::DateTimeOriginal, Tag::DateTime]
        .iter()
        .find_map(|tag| {
            exif.get_field(*tag, In::PRIMARY)
                .or_else(|| exif.fields().find(|f| f.tag == *tag))
                .and_then(ascii)
        })?;
    NaiveDateTime::parse_from_str(&txt, "%Y:%m:%d %H:%M:%S")
        .ok()
        .map(|ndt| Utc.from_utc_datetime(&ndt).timestamp())
}

fn probe_with_settings(
    path: &str,
    is_video: bool,
    settings: &crate::settings::AdvancedSettings,
) -> Result<(Option<i64>, Option<u32>, Option<u32>), String> {
    // Dimensions of a still image are in its header; only containers need a decoder probe.
    #[cfg(target_os = "macos")]
    if !is_video && crate::native_image::metadata_enabled() {
        if let Some((width, height)) = crate::native_image::dimensions(Path::new(path)) {
            return Ok((None, Some(width), Some(height)));
        }
    }
    let _permit = crate::util::DecodePermit::acquire();
    let mut cmd = Command::new(ffmpeg_sidecar::paths::ffmpeg_path());
    apply_command_tuning_with_settings(&mut cmd, is_video, &settings.ffmpeg);
    // One frame is enough for dimensions and stream tags; do not decode an entire video.
    cmd.args([
        "-hide_banner",
        "-i",
        path,
        "-frames:v",
        "1",
        "-an",
        "-f",
        "null",
        "-",
    ]);
    let output = run_prepared_command_with_timeout(
        cmd,
        std::time::Duration::from_secs(
            settings
                .metadata
                .ffmpeg_probe_timeout_secs
                .unwrap_or(settings.ffmpeg.timeout_secs),
        ),
    )?;
    if !output.status.success() {
        return Err(format!(
            "Cannot read metadata: {}",
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    let turned = !is_video && exif_turns_picture(Path::new(path));
    let (shoot, width, height) = parse_probe_output(
        &String::from_utf8_lossy(&output.stderr),
        settings.metadata.parse_creation_time,
    );
    Ok(if turned { (shoot, height, width) } else { (shoot, width, height) })
}

/// JPEG and TIFF carry the turn in EXIF; FFmpeg reports the stored size for them.
fn exif_turns_picture(path: &Path) -> bool {
    if !crate::util::has_extension(path, &["jpg", "jpeg", "tif", "tiff"]) {
        return false;
    }
    let Ok(file) = File::open(path) else { return false };
    let Ok(exif) = Reader::new().read_from_container(&mut BufReader::new(file)) else { return false };
    exif.get_field(Tag::Orientation, In::PRIMARY)
        .and_then(|field| field.value.get_uint(0))
        .is_some_and(|orientation| orientation >= 5)
}

fn parse_probe_output(
    stderr: &str,
    parse_creation_time: bool,
) -> (Option<i64>, Option<u32>, Option<u32>) {
    let (mut shoot, mut width, mut height) = (None, None, None);
    for line in stderr.lines() {
        if width.is_none() && line.contains("Stream") && line.contains("Video:") {
            for token in line.split(',').skip(1) {
                let token = token.trim().split_whitespace().next().unwrap_or("");
                if let Some((w, h)) = token.split_once('x') {
                    if let (Ok(w), Ok(h)) = (w.parse::<u32>(), h.parse::<u32>()) {
                        if w > 0 && h > 0 {
                            width = Some(w);
                            height = Some(h);
                            break;
                        }
                    }
                }
            }
        }
        if parse_creation_time {
            if let Some((_, time)) = line.split_once("creation_time") {
                if let Ok(dt) =
                    DateTime::parse_from_rfc3339(time.trim().trim_start_matches(':').trim())
                {
                    shoot =
                        Some(dt.timestamp()).filter(|timestamp| supported_timestamp(*timestamp));
                }
            }
        }
    }
    (shoot, width, height)
}

pub fn get_file_metadata(path: &str) -> Result<String, String> {
    get_file_metadata_with_settings(path, &read_settings())
}

fn get_file_metadata_with_settings(
    path: &str,
    settings: &crate::settings::AdvancedSettings,
) -> Result<String, String> {
    let p = Path::new(path);
    let source = source_revision(p).ok_or("Media file does not exist")?;
    let added = p
        .metadata()
        .ok()
        .and_then(|m| m.created().ok())
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_secs());
    let ext_lower = p
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    let is_image = crate::constants::IMAGE_EXTENSIONS.contains(&ext_lower.as_str());
    let is_video = crate::constants::VIDEO_EXTENSIONS.contains(&ext_lower.as_str());
    let (probe_shoot, width, height) = if is_image || is_video {
        probe_with_settings(path, is_video, settings)?
    } else {
        (None, None, None)
    };
    // FFmpeg treats QuickTime dates before 1970 as Unix timestamps, shifting
    // standards-compliant dates by 66 years. Correct only that exact shift;
    // distinct explicit/stream tags retain their existing precedence.
    let container_shoot = (settings.metadata.parse_creation_time
        && matches!(ext_lower.as_str(), "mov" | "mp4" | "m4v"))
    .then(|| crate::quicktime::creation_time(p))
    .flatten()
    .filter(|timestamp| {
        *timestamp < 0
            && supported_timestamp(*timestamp)
            && timestamp.checked_add(crate::quicktime::EPOCH_OFFSET) == probe_shoot
    });
    let fallback_shoot = container_shoot
        .or(probe_shoot)
        .or_else(|| is_image.then(|| datetime_original(p)).flatten());
    let mut meta = DetachedFileMeta {
        a: added,
        s: fallback_shoot,
        i: is_image,
        v: is_video,
        w: width,
        h: height,
    };
    let path_owned = p.to_path_buf();
    let source_owned = source.clone();
    let version = settings.metadata_version();
    let written = update_file_meta_if_source_current(p, &source, move |entry| {
        // A user may have changed the date while FFmpeg was probing this file.
        let before = embedded_timestamp_revision(&path_owned);
        let embedded = read_embedded_timestamp(&path_owned);
        let after = embedded_timestamp_revision(&path_owned);
        // Read the manual override at publication, including its removal. A
        // hardlink or external xattr writer does not change content revisions.
        meta.s = embedded.or(fallback_shoot);
        entry.meta_embedded_timestamp = embedded;
        entry.meta_embedded_revision = if before == after { after } else { None };
        entry.meta = Some(meta.pack());
        entry.meta_source_revision = Some(source_owned);
        entry.meta_settings_version = Some(version);
        entry.meta_failed = false;
        entry.meta_failed_source_revision = None;
    })?;
    written
        .meta
        .ok_or_else(|| "Metadata result was not persisted".into())
}

/// A filesystem event may replace a file without changing its name. Keep user
/// metadata, but discard artifacts derived from the previous content.
pub(crate) fn invalidate_media_content(path: &Path) -> Result<(), String> {
    let dir = path.parent().ok_or("Invalid path")?;
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or("Invalid filename")?;
    update_file_meta(dir, name, |entry| {
        entry.meta = None;
        entry.meta_source_revision = None;
        entry.meta_embedded_timestamp = None;
        entry.meta_embedded_revision = None;
        entry.meta_settings_version = None;
        entry.meta_failed_source_revision = None;
        entry.thumb_failed_source_revision = None;
        entry.hash = None;
        entry.hash_version = None;
        entry.hash_bits = None;
        entry.hash_source_revision = None;
        entry.hash_failed_source_revision = None;
        entry.thumb_hash_revision = None;
        entry.hash_failed = false;
        entry.thumb_failed = false;
        entry.meta_failed = false;
    })
    .map(|_| ())
}

pub fn get_file_metadata_cached(path: &Path) -> Result<String, String> {
    get_metadata_with_favorite(path).map(|entry| entry.meta)
}

pub(crate) fn get_file_metadata_cached_with_settings(
    path: &Path,
    settings: &crate::settings::AdvancedSettings,
) -> Result<String, String> {
    get_metadata_with_favorite_with_settings(path, settings).map(|entry| entry.meta)
}

#[cfg(unix)]
// Keep descriptors bounded, reserving room for the rest of the application.
const TIMESTAMP_BATCH_SIZE: usize = 128;
#[cfg(unix)]
fn timestamp_batch_size() -> usize {
    let mut limit = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    if unsafe { libc::getrlimit(libc::RLIMIT_NOFILE, &mut limit) } != 0 {
        return 32;
    }
    if limit.rlim_cur >= 1024 {
        TIMESTAMP_BATCH_SIZE
    } else {
        ((limit.rlim_cur / 2) as usize).clamp(1, 32)
    }
}
#[cfg(unix)]
type TimestampNamespace = (std::time::SystemTime, u64, u64, u64, bool);

#[cfg(unix)]
fn timestamp_namespace(meta: &fs::Metadata) -> Option<TimestampNamespace> {
    use std::os::unix::fs::MetadataExt;
    let (device, identity) = (meta.dev(), meta.ino());
    Some((
        meta.modified().ok()?,
        meta.len(),
        device,
        identity,
        meta.is_symlink(),
    ))
}
#[cfg(unix)]
fn timestamp_path_namespace(path: &Path) -> Option<TimestampNamespace> {
    timestamp_namespace(&fs::symlink_metadata(path).ok()?)
}

#[cfg(unix)]
struct TimestampTarget {
    path: PathBuf,
    namespace: TimestampNamespace,
    file: File,
}
#[cfg(unix)]
impl TimestampTarget {
    fn open(path: &Path, namespace: &TimestampNamespace) -> Result<Self, String> {
        use std::os::unix::fs::OpenOptionsExt;
        #[cfg(target_os = "macos")]
        let flags = libc::O_SYMLINK;
        #[cfg(not(target_os = "macos"))]
        let flags = libc::O_NOFOLLOW;
        let opened = fs::OpenOptions::new()
            .read(true)
            .custom_flags(flags)
            .open(path);
        let file = match opened {
            Err(error) if error.kind() == std::io::ErrorKind::PermissionDenied => {
                fs::OpenOptions::new()
                    .write(true)
                    .custom_flags(flags)
                    .open(path)
            }
            other => other,
        }
        .map_err(|error| error.to_string())?;
        if timestamp_namespace(&file.metadata().map_err(|error| error.to_string())?).as_ref()
            != Some(namespace)
            || timestamp_path_namespace(path).as_ref() != Some(namespace)
        {
            return Err(format!(
                "Media changed while updating its date: {}",
                path.display()
            ));
        }
        Ok(Self {
            path: path.to_owned(),
            namespace: *namespace,
            file,
        })
    }
    fn current(&self, source: &str) -> bool {
        timestamp_path_namespace(&self.path).as_ref() == Some(&self.namespace)
            && source_revision(&self.path).as_deref() == Some(source)
    }
    fn raw(&self) -> std::io::Result<Option<Vec<u8>>> {
        use xattr::FileExt;
        self.file.get_xattr(EMBEDDED_TS_XATTR)
    }
    fn write_raw(&self, raw: Option<&[u8]>) -> Result<(), String> {
        use xattr::FileExt;
        if raw.is_none() && self.raw().map_err(|error| error.to_string())?.is_none() {
            return Ok(());
        }
        match raw {
            Some(raw) => self.file.set_xattr(EMBEDDED_TS_XATTR, raw),
            None => self.file.remove_xattr(EMBEDDED_TS_XATTR),
        }
        .map_err(|error| error.to_string())
    }
    fn restore(&self, previous: Option<&[u8]>, written: &[u8]) -> Result<(), String> {
        // Address the opened object even if its original path was replaced.
        let current = self.raw().map_err(|error| error.to_string())?;
        if current.as_deref() == previous {
            return Ok(());
        }
        if current.as_deref() != Some(written) {
            return Err(format!(
                "Cannot restore date: another writer changed {}",
                self.path.display()
            ));
        }
        self.write_raw(previous)
    }
}

#[cfg(unix)]
struct PreparedTimestamp {
    name: String,
    path: PathBuf,
    source: String,
    namespace: TimestampNamespace,
    meta: String,
}
#[cfg(unix)]
fn prepare_timestamp_updates(
    dir: &Path,
    names: Vec<String>,
) -> (Vec<PreparedTimestamp>, Option<String>) {
    let mut prepared = Vec::with_capacity(names.len());
    let settings = read_settings();
    let album = match read_selected_metadata(dir, &names, false) {
        Ok(album) => album,
        Err(error) => return (prepared, Some(error)),
    };
    for name in names {
        let path = dir.join(&name);
        let result = (|| {
            let source = source_revision(&path)
                .ok_or_else(|| format!("{} does not exist", path.display()))?;
            let namespace = timestamp_path_namespace(&path).ok_or("Cannot stat media namespace")?;
            // The requested date replaces any manual override. Reuse current
            // content fields even if its xattr changed or is unreadable (e.g.
            // a write-only original); probing is necessary only for new content.
            let meta = match album.files.get(&name).filter(|entry| {
                metadata_content_cache_current_with_settings(&path, entry, &settings)
            }) {
                Some(entry) => entry.meta.clone().unwrap(),
                None => get_file_metadata_cached_with_settings(&path, &settings)?,
            };
            unpack_file_meta(&meta)?;
            Ok::<_, String>(PreparedTimestamp {
                name,
                path,
                source,
                namespace,
                meta,
            })
        })();
        match result {
            Ok(record) => prepared.push(record),
            Err(error) => return (prepared, Some(error)),
        }
    }
    (prepared, None)
}

#[cfg(unix)]
fn apply_timestamp_metadata(
    album: &mut AlbumMeta,
    record: &PreparedTimestamp,
    timestamp: i64,
    version: &str,
) -> DetachedMediaEntry {
    let entry = album.files.entry(record.name.clone()).or_default();
    let mut meta = entry
        .meta
        .as_deref()
        .filter(|_| entry.meta_source_revision.as_deref() == Some(&record.source))
        .and_then(|packed| unpack_file_meta(packed).ok())
        .unwrap_or_else(|| {
            unpack_file_meta(&record.meta).expect("prepared metadata was validated")
        });
    meta.s = Some(timestamp);
    let packed = meta.pack();
    entry.meta = Some(packed.clone());
    entry.meta_source_revision = Some(record.source.clone());
    entry.meta_embedded_timestamp = Some(timestamp);
    entry.meta_embedded_revision =
        embedded_timestamp_stamp_if_current(&record.path, Some(timestamp));
    entry.meta_settings_version = Some(version.to_owned());
    entry.meta_failed = false;
    entry.meta_failed_source_revision = None;
    DetachedMediaEntry {
        blur_preview: entry.blur_preview.clone(),
        revision: media_revision(&record.path),
        thumbnail_name: crate::thumb::detached_thumbnail_name(&record.path),
        meta: packed,
        name: record.name.clone(),
        favorite: entry.favorite.then_some(true),
    }
}

#[cfg(unix)]
struct TimestampChunk {
    album: AlbumMeta,
    originals: Vec<(TimestampTarget, Option<Vec<u8>>)>,
    updated: Vec<DetachedMediaEntry>,
}
#[cfg(unix)]
fn persist_timestamp_chunk(
    dir: &Path,
    chunk: TimestampChunk,
    written: &[u8],
    persist: &mut impl FnMut(&Path, Arc<AlbumMeta>) -> Result<(), String>,
) -> Result<(Arc<AlbumMeta>, Vec<DetachedMediaEntry>), String> {
    let next = Arc::new(chunk.album);
    if let Err(error) = persist(dir, next.clone()) {
        let errors = chunk
            .originals
            .iter()
            .rev()
            .filter_map(|(target, previous)| target.restore(previous.as_deref(), written).err())
            .collect::<Vec<_>>();
        return Err(if errors.is_empty() {
            error
        } else {
            format!("{error}; date restore failed: {}", errors.join("; "))
        });
    }
    Ok((next, chunk.updated))
}

#[cfg(unix)]
fn commit_timestamp_updates(
    dir: &Path,
    prepared: &[PreparedTimestamp],
    timestamp: i64,
    mut failure: Option<String>,
    mut persist: impl FnMut(&Path, Arc<AlbumMeta>) -> Result<(), String>,
) -> Result<Vec<DetachedMediaEntry>, String> {
    if prepared.is_empty() {
        return failure.map_or_else(|| Ok(Vec::new()), Err);
    }
    let _guard = lock_metadata_for_write(&[dir])?;
    ensure_database(dir)?;
    let selected_names: Vec<String> = prepared.iter().map(|record| record.name.clone()).collect();
    let mut album = Arc::new(crate::album_db::subset(
        &album_meta_path(dir),
        &selected_names,
        false,
    )?);
    let version = read_settings().metadata_version();
    let written = timestamp.to_string().into_bytes();
    let mut updated = Vec::with_capacity(prepared.len());
    let mut pending: Option<TimestampChunk> = None;
    let batch_size = timestamp_batch_size();
    for record in prepared {
        let target = match TimestampTarget::open(&record.path, &record.namespace) {
            Ok(target) if target.current(&record.source) => target,
            Ok(_) => {
                failure.get_or_insert_with(|| {
                    format!(
                        "Media changed while updating its date: {}",
                        record.path.display()
                    )
                });
                continue;
            }
            Err(error) => {
                failure.get_or_insert(error);
                break;
            }
        };
        let previous = match target.raw() {
            Ok(raw) => raw,
            Err(error) if error.kind() == std::io::ErrorKind::PermissionDenied => {
                // Write-only files can allow setting xattrs but forbid reading
                // the old bytes. Commit their JSON first; a failed atomic xattr
                // set leaves the old attribute intact, so restore only the JSON.
                if let Some(chunk) = pending.take() {
                    let (next, entries) =
                        persist_timestamp_chunk(dir, chunk, &written, &mut persist)?;
                    album = next;
                    updated.extend(entries);
                }
                let mut next = (*album).clone();
                let entry = apply_timestamp_metadata(&mut next, record, timestamp, &version);
                let next = Arc::new(next);
                persist(dir, next.clone())?;
                let changed = if target.current(&record.source) {
                    target.write_raw(Some(&written))
                } else {
                    Err(format!(
                        "Media changed while updating its date: {}",
                        record.path.display()
                    ))
                };
                if let Err(error) = changed {
                    return Err(match persist(dir, album.clone()) {
                        Ok(()) => error,
                        Err(restore_error) => {
                            format!("{error}; metadata restore failed: {restore_error}")
                        }
                    });
                }
                album = next;
                updated.push(entry);
                continue;
            }
            Err(error) => {
                failure.get_or_insert(error.to_string());
                break;
            }
        };
        if let Err(error) = target.write_raw(Some(&written)) {
            let restored = target.restore(previous.as_deref(), &written);
            failure.get_or_insert_with(|| match restored {
                Ok(()) => error,
                Err(restore_error) => format!("{error}; date restore failed: {restore_error}"),
            });
            break;
        }
        if !target.current(&record.source) {
            let error = format!(
                "Media changed while updating its date: {}",
                record.path.display()
            );
            match target.restore(previous.as_deref(), &written) {
                Ok(()) => {
                    failure.get_or_insert(error);
                    continue;
                }
                Err(restore_error) => {
                    failure.get_or_insert(format!("{error}; date restore failed: {restore_error}"));
                    break;
                }
            }
        }
        let chunk = pending.get_or_insert_with(|| TimestampChunk {
            album: (*album).clone(),
            originals: Vec::with_capacity(batch_size),
            updated: Vec::with_capacity(batch_size),
        });
        let entry = apply_timestamp_metadata(&mut chunk.album, record, timestamp, &version);
        chunk.updated.push(entry);
        chunk.originals.push((target, previous));
        if chunk.originals.len() == batch_size {
            let chunk = pending.take().unwrap();
            let (next, entries) = persist_timestamp_chunk(dir, chunk, &written, &mut persist)?;
            album = next;
            updated.extend(entries);
        }
    }
    if let Some(chunk) = pending {
        let (_, entries) = persist_timestamp_chunk(dir, chunk, &written, &mut persist)?;
        updated.extend(entries);
    }
    failure.map_or(Ok(updated), Err)
}

#[cfg(unix)]
fn set_media_timestamp_leased(
    album_path: String,
    names: Vec<String>,
    timestamp: i64,
) -> Result<Vec<DetachedMediaEntry>, String> {
    let dir = PathBuf::from(&album_path);
    if !dir.is_dir() {
        return Err(format!("{} is not a directory", album_path));
    }
    for name in &names {
        validate_file_name(name)?;
    }
    let (prepared, failure) = prepare_timestamp_updates(&dir, names);
    commit_timestamp_updates(&dir, &prepared, timestamp, failure, write_timestamp_rows)
}

pub fn set_media_timestamp(
    album_path: String,
    names: Vec<String>,
    timestamp: i64,
) -> Result<Vec<DetachedMediaEntry>, String> {
    if !supported_timestamp(timestamp) {
        return Err("Date exceeds the supported timestamp range".into());
    }
    #[cfg(unix)]
    {
        set_media_timestamp_leased(album_path, names, timestamp)
    }
    #[cfg(not(unix))]
    {
        set_media_timestamp_legacy(album_path, names, timestamp)
    }
}

// Windows ADS changes file times and needs its own pinned-stream transaction.
#[cfg(any(not(unix), test))]
fn set_media_timestamp_legacy(
    album_path: String,
    names: Vec<String>,
    timestamp: i64,
) -> Result<Vec<DetachedMediaEntry>, String> {
    let dir = PathBuf::from(&album_path);
    if !dir.is_dir() {
        return Err(format!("{} is not a directory", album_path));
    }
    if names.is_empty() {
        return Ok(Vec::new());
    }

    for name in &names {
        validate_file_name(name)?;
    }
    let mut prepared = Vec::new();
    let mut failure = None;
    for name in names {
        let file_path = dir.join(&name);
        let result = (|| -> Result<(String, String), String> {
            let source = source_revision(&file_path)
                .ok_or_else(|| format!("{} does not exist", file_path.display()))?;
            let meta = get_file_metadata_cached(&file_path)?;
            unpack_file_meta(&meta)?;
            write_embedded_timestamp(&file_path, timestamp)?;
            Ok((source, meta))
        })();
        match result {
            Ok((source, meta)) => prepared.push((name, file_path, source, meta)),
            Err(error) => {
                failure = Some(error);
                break;
            }
        }
    }

    let mut updated = Vec::new();
    for (name, file_path, source, prepared_meta) in prepared {
        if source_revision(&file_path).as_deref() != Some(&source) {
            failure.get_or_insert("Media changed during date update".into());
            continue;
        }
        let saved = update_file_meta(&dir, &name, |entry| {
            let mut meta = entry
                .meta
                .as_deref()
                .and_then(|m| unpack_file_meta(m).ok())
                .unwrap_or_else(|| unpack_file_meta(&prepared_meta).unwrap());
            let embedded = read_embedded_timestamp(&file_path);
            meta.s = embedded.or(Some(timestamp));
            entry.meta_embedded_timestamp = embedded;
            entry.meta_embedded_revision =
                embedded_timestamp_stamp_if_current(&file_path, embedded);
            entry.meta = Some(meta.pack());
            entry.meta_source_revision = Some(source.clone());
            entry.meta_settings_version = Some(read_settings().metadata_version());
            entry.meta_failed = false;
            entry.meta_failed_source_revision = None;
        })?;
        updated.push(DetachedMediaEntry {
            blur_preview: saved.blur_preview.clone(),
            revision: media_revision(&file_path),
            thumbnail_name: crate::thumb::detached_thumbnail_name(&file_path),
            meta: saved.meta.unwrap(),
            name,
            favorite: Some(saved.favorite),
        });
    }
    if let Some(error) = failure {
        return Err(error);
    }
    Ok(updated)
}

pub fn set_media_favorite(path: String, favorite: bool) -> Result<DetachedMediaEntry, String> {
    let p = Path::new(&path);
    if !p.exists() {
        return Err(format!("{} does not exist", path));
    }

    let meta = get_file_metadata_cached(p)?;
    let dir = p
        .parent()
        .ok_or("Invalid path")
        .map_err(|e| e.to_string())?;
    let name = p
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or("Invalid filename")
        .map_err(|e| e.to_string())?
        .to_string();
    update_file_meta(dir, &name, |entry| {
        entry.favorite = favorite;
        if entry
            .meta
            .as_deref()
            .is_none_or(|meta| unpack_file_meta(meta).is_err())
        {
            entry.meta = Some(meta.clone());
        }
    })?;

    Ok(DetachedMediaEntry {
        blur_preview: read_file_entry(p)?.and_then(|entry| entry.blur_preview),
        revision: media_revision(p),
        thumbnail_name: crate::thumb::detached_thumbnail_name(p),
        meta,
        name,
        favorite: if favorite { Some(true) } else { None },
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn new_preview_mapping_retries_legacy_failures_but_keeps_current_failures_suppressed() {
        let dir = tempfile::tempdir().unwrap();
        let name = format!("{}.png", "x".repeat(251));
        let path = dir.path().join(&name);
        fs::write(&path, b"unchanged media source").unwrap();
        let source = source_revision(&path).unwrap();
        let processing_version = read_settings().processing_version();
        update_file_meta(dir.path(), &name, |entry| {
            entry.favorite = true;
            entry.thumb_failed = true;
            entry.thumb_failed_source_revision = Some(source.clone());
            entry.thumb_failed_processing_version = Some(processing_version.clone());
        })
        .unwrap();
        assert!(
            !is_thumb_failed(&path),
            "a failure from the previous filename mapping must allow one new attempt"
        );
        mark_thumb_failed(&path, &source, &processing_version).unwrap();
        assert!(
            is_thumb_failed(&path),
            "a repeated failure with the current mapping must still be suppressed"
        );
        assert!(read_album_meta(dir.path()).files[&name].favorite);
    }

    #[test]
    fn stale_reader_cannot_cache_old_metadata_with_a_replacement_stamp() {
        let dir = tempfile::tempdir().unwrap();
        update_file_meta(dir.path(), "a.jpg", |entry| entry.favorite = false).unwrap();
        let observed = metadata_stamp(&album_meta_path(dir.path()));
        let old = read_album_meta_checked(dir.path()).unwrap();
        let mut replacement = (*old).clone();
        replacement.files.get_mut("a.jpg").unwrap().favorite = true;
        crate::album_db::replace(&album_meta_path(dir.path()), &replacement).unwrap();
        assert_ne!(observed, metadata_stamp(&album_meta_path(dir.path())));
        cache_album_snapshot_if_unchanged(dir.path(), observed, &old);
        assert!(read_album_meta_checked(dir.path()).unwrap().files["a.jpg"].favorite);
        assert!(!old.files["a.jpg"].favorite);
    }

    #[test]
    fn album_waiting_for_external_writer_does_not_delay_another_album() {
        external_writer_contention_scenario(false);
    }

    #[test]
    fn legacy_migration_wait_does_not_delay_another_album() {
        external_writer_contention_scenario(true);
    }

    fn external_writer_contention_scenario(migrate: bool) {
        use crate::test_support::{spawn_test, wait_for_file};
        if let Ok(dir) = std::env::var("ROOM237_TEST_DIR") {
            let dir = PathBuf::from(dir);
            let _lease = lock_album_metadata(&dir.join("blocked")).unwrap();
            fs::write(dir.join("lease-held"), b"ready").unwrap();
            wait_for_file(&dir.join("release"));
            return;
        }
        let root = tempfile::tempdir().unwrap();
        let blocked = root.path().join("blocked");
        let free = root.path().join("free");
        fs::create_dir(&blocked).unwrap();
        fs::create_dir(&free).unwrap();
        if migrate {
            ensure_meta_dir(&blocked).unwrap();
            fs::write(
                meta_dir(&blocked).join("a.jpg.meta"),
                serde_json::to_vec(&FileMetaEntry {
                    favorite: true,
                    ..Default::default()
                })
                .unwrap(),
            )
            .unwrap();
        } else {
            update_file_meta(&blocked, "a.jpg", |_| {}).unwrap();
        }
        update_file_meta(&free, "b.jpg", |_| {}).unwrap();
        let mut holder = spawn_test(
            if migrate {
                "metadata::tests::legacy_migration_wait_does_not_delay_another_album"
            } else {
                "metadata::tests::album_waiting_for_external_writer_does_not_delay_another_album"
            },
            root.path(),
            "holder",
        );
        wait_for_file(&root.path().join("lease-held"));
        let (started, receiver) = mpsc::channel();
        let writer = thread::spawn(move || {
            started.send(()).unwrap();
            if migrate {
                read_album_meta_checked(&blocked)?;
                assert!(album_meta_path(&blocked).is_file());
                assert!(meta_dir(&blocked).join("a.jpg.meta").exists());
            } else {
                update_file_meta(&blocked, "a.jpg", |entry| entry.favorite = true)?;
            }
            Ok::<_, String>(())
        });
        receiver.recv().unwrap();
        thread::sleep(Duration::from_millis(100));
        let (done, receiver) = mpsc::channel();
        let independent_started = Instant::now();
        let free_reader_writer = thread::spawn(move || {
            let before = read_album_meta_checked(&free)?;
            if before.files["b.jpg"].favorite {
                return Err("unexpected initial favorite".to_string());
            }
            update_file_meta(&free, "b.jpg", |entry| entry.favorite = true)?;
            let after = read_album_meta_checked(&free)?;
            done.send(after.files["b.jpg"].favorite).unwrap();
            Ok::<_, String>(())
        });
        let independent_result = receiver.recv_timeout(Duration::from_secs(1));
        let independent_elapsed = independent_started.elapsed();
        // Always release the external lease before asserting or joining.
        fs::write(root.path().join("release"), b"release").unwrap();
        assert!(holder.0.wait().unwrap().success());
        writer.join().unwrap().unwrap();
        free_reader_writer.join().unwrap().unwrap();
        assert!(independent_result.unwrap());
        eprintln!("external metadata lease: migration={migrate}, independent read/write/read={independent_elapsed:?}");
    }
    #[test]
    fn concurrent_processes_preserve_all_acknowledged_metadata_edits() {
        use crate::test_support::{spawn_test, wait_for_file};
        if let Ok(dir) = std::env::var("ROOM237_TEST_DIR") {
            let dir = PathBuf::from(dir);
            let role = std::env::var("ROOM237_TEST_ROLE").unwrap();
            fs::write(dir.join(format!("ready-{role}")), b"ready").unwrap();
            wait_for_file(&dir.join("go"));
            for index in 0..40 {
                update_file_meta(&dir, &format!("{role}-{index}.jpg"), |entry| {
                    entry.favorite = true;
                    thread::sleep(Duration::from_millis(2));
                })
                .unwrap();
            }
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        update_album_meta(dir.path(), |_| {}).unwrap();
        let test = "metadata::tests::concurrent_processes_preserve_all_acknowledged_metadata_edits";
        let mut a = spawn_test(test, dir.path(), "a");
        let mut b = spawn_test(test, dir.path(), "b");
        wait_for_file(&dir.path().join("ready-a"));
        wait_for_file(&dir.path().join("ready-b"));
        fs::write(dir.path().join("go"), b"go").unwrap();
        assert!(a.0.wait().unwrap().success());
        assert!(b.0.wait().unwrap().success());
        let disk: AlbumMeta = crate::album_db::read(&album_meta_path(dir.path())).unwrap();
        assert_eq!(disk.files.len(), 80);
        assert!(disk.files.values().all(|entry| entry.favorite));
    }

    #[test]
    fn opposite_metadata_transfers_from_two_processes_keep_both_user_entries() {
        use crate::test_support::{spawn_test, wait_for_file};
        if let Ok(dir) = std::env::var("ROOM237_TEST_DIR") {
            let dir = PathBuf::from(dir);
            let role = std::env::var("ROOM237_TEST_ROLE").unwrap();
            let other = if role == "a" { "b" } else { "a" };
            fs::write(dir.join(format!("ready-{role}")), b"ready").unwrap();
            wait_for_file(&dir.join("go"));
            let guard = lock_metadata_for_write(&[&dir.join(&role), &dir.join(other)]).unwrap();
            transfer_media_metadata_batch_caller_holds_lock(
                &dir.join(&role),
                &dir.join(other),
                &[(format!("{role}.jpg"), format!("{role}.jpg"))],
                &guard,
            )
            .unwrap();
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        for role in ["a", "b"] {
            let album = dir.path().join(role);
            fs::create_dir(&album).unwrap();
            update_file_meta(&album, &format!("{role}.jpg"), |entry| {
                entry.favorite = true
            })
            .unwrap();
        }
        let test = "metadata::tests::opposite_metadata_transfers_from_two_processes_keep_both_user_entries";
        let mut a = spawn_test(test, dir.path(), "a");
        let mut b = spawn_test(test, dir.path(), "b");
        wait_for_file(&dir.path().join("ready-a"));
        wait_for_file(&dir.path().join("ready-b"));
        fs::write(dir.path().join("go"), b"go").unwrap();
        assert!(a.0.wait().unwrap().success());
        assert!(b.0.wait().unwrap().success());
        for (album, name) in [("a", "b.jpg"), ("b", "a.jpg")] {
            let disk: AlbumMeta =
                crate::album_db::read(&album_meta_path(&dir.path().join(album))).unwrap();
            assert_eq!(disk.files.len(), 1);
            assert!(disk.files[name].favorite);
        }
    }

    fn pending_write(
        path: &Path,
        update: impl FnOnce(&mut FileMetaEntry) + Send + 'static,
    ) -> (DerivedWrite, mpsc::Receiver<Result<FileMetaEntry, String>>) {
        let (response, receiver) = mpsc::sync_channel(1);
        (
            DerivedWrite {
                path: path.to_path_buf(),
                name: path.file_name().unwrap().to_str().unwrap().into(),
                expected_revision: None,
                expected_source: source_revision(path),
                update: Box::new(update),
                response,
                permit: None,
            },
            receiver,
        )
    }

    #[test]
    fn background_writer_services_another_album_while_one_lease_is_busy() {
        use crate::test_support::{spawn_test, wait_for_file};
        if let Ok(dir) = std::env::var("ROOM237_TEST_DIR") {
            let dir = PathBuf::from(dir);
            let _lease = lock_album_metadata(&dir.join("blocked")).unwrap();
            fs::write(dir.join("lease-held"), b"ready").unwrap();
            wait_for_file(&dir.join("release"));
            return;
        }
        let root = tempfile::tempdir().unwrap();
        let blocked = root.path().join("blocked");
        let free = root.path().join("free");
        for dir in [&blocked, &free] {
            fs::create_dir(dir).unwrap();
            fs::write(dir.join("photo.jpg"), b"unchanged photo").unwrap();
            update_file_meta(dir, "photo.jpg", |entry| entry.favorite = true).unwrap();
        }
        let mut holder = spawn_test(
            "metadata::tests::background_writer_services_another_album_while_one_lease_is_busy",
            root.path(),
            "holder",
        );
        wait_for_file(&root.path().join("lease-held"));
        let (a, result_a) = pending_write(&blocked.join("photo.jpg"), |entry| {
            entry.hash = Some("derived a".into());
        });
        DERIVED_WRITER.send(a).unwrap();
        thread::sleep(Duration::from_millis(100));
        let (b, result_b) = pending_write(&free.join("photo.jpg"), |entry| {
            entry.hash = Some("derived b".into());
        });
        let started = Instant::now();
        DERIVED_WRITER.send(b).unwrap();
        let independent = result_b.recv_timeout(Duration::from_secs(1));
        let elapsed = started.elapsed();
        let a_waited = matches!(result_a.try_recv(), Err(mpsc::TryRecvError::Empty));
        fs::write(root.path().join("release"), b"release").unwrap();
        assert!(holder.0.wait().unwrap().success());
        assert!(
            result_a
                .recv_timeout(Duration::from_secs(5))
                .unwrap()
                .unwrap()
                .favorite
        );
        if independent.is_err() {
            result_b
                .recv_timeout(Duration::from_secs(5))
                .unwrap()
                .unwrap();
        }
        for (dir, hash) in [(&blocked, "derived a"), (&free, "derived b")] {
            let disk: AlbumMeta = crate::album_db::read(&album_meta_path(dir)).unwrap();
            assert!(disk.files["photo.jpg"].favorite);
            assert_eq!(disk.files["photo.jpg"].hash.as_deref(), Some(hash));
        }
        assert!(
            a_waited,
            "busy album must not acknowledge before its lease is released"
        );
        assert!(independent.unwrap().unwrap().favorite);
        eprintln!("background writer: independent persisted result={elapsed:?} while external album lease remains held");
    }

    #[test]
    fn background_writer_bounds_admission_until_persisted_results_release_slots() {
        let dir = tempfile::tempdir().unwrap();
        let photo = dir.path().join("photo.jpg");
        fs::write(&photo, b"unchanged original").unwrap();
        update_file_meta(dir.path(), "photo.jpg", |entry| entry.favorite = true).unwrap();
        let lease = lock_album_metadata(dir.path()).unwrap();
        let (writer, worker) = spawn_derived_writer(METADATA_LEASE_TIMEOUT);
        let mut responses = Vec::new();
        for i in 0..DERIVED_MAX_OUTSTANDING {
            let (write, result) =
                pending_write(&photo, move |entry| entry.hash = Some(i.to_string()));
            writer.send(write).unwrap();
            responses.push(result);
        }
        let (admitted, admission) = mpsc::channel();
        let extra_writer = writer.clone();
        let (extra, extra_result) = pending_write(&photo, |entry| entry.hash = Some("last".into()));
        let producer = thread::spawn(move || {
            extra_writer.send(extra).unwrap();
            admitted.send(()).unwrap();
        });
        let blocked = admission.recv_timeout(Duration::from_millis(100)).is_err();
        let unacknowledged = responses
            .iter()
            .all(|result| matches!(result.try_recv(), Err(mpsc::TryRecvError::Empty)));
        drop(lease);
        producer.join().unwrap();
        admission.recv_timeout(Duration::from_secs(5)).unwrap();
        for result in responses {
            assert!(
                result
                    .recv_timeout(Duration::from_secs(5))
                    .unwrap()
                    .unwrap()
                    .favorite
            );
        }
        assert!(
            extra_result
                .recv_timeout(Duration::from_secs(5))
                .unwrap()
                .unwrap()
                .favorite
        );
        drop(writer);
        worker.join().unwrap();
        let disk: AlbumMeta = crate::album_db::read(&album_meta_path(dir.path())).unwrap();
        assert!(disk.files["photo.jpg"].favorite);
        assert_eq!(disk.files["photo.jpg"].hash.as_deref(), Some("last"));
        assert!(
            blocked,
            "request 97 must wait until an admitted request finishes"
        );
        assert!(
            unacknowledged,
            "no success acknowledgement before persistence"
        );
    }

    #[test]
    fn background_writer_expires_busy_requests_without_changes_and_accepts_later_work() {
        let dir = tempfile::tempdir().unwrap();
        let photo = dir.path().join("photo.jpg");
        fs::write(&photo, b"unchanged original").unwrap();
        update_file_meta(dir.path(), "photo.jpg", |entry| entry.favorite = true).unwrap();
        let before = fs::read(album_meta_path(dir.path())).unwrap();
        let lease = lock_album_metadata(dir.path()).unwrap();
        let (writer, worker) = spawn_derived_writer(Duration::from_millis(40));
        let (write, result) = pending_write(&photo, |entry| entry.hash = Some("expired".into()));
        writer.send(write).unwrap();
        let expired = result.recv_timeout(Duration::from_secs(2));
        let unchanged = fs::read(album_meta_path(dir.path())).unwrap() == before;
        drop(lease);
        let (write, result) = pending_write(&photo, |entry| entry.hash = Some("current".into()));
        writer.send(write).unwrap();
        assert!(
            result
                .recv_timeout(Duration::from_secs(2))
                .unwrap()
                .unwrap()
                .favorite
        );
        drop(writer);
        worker.join().unwrap();
        assert!(expired
            .unwrap()
            .unwrap_err()
            .contains("File lock timed out"));
        assert!(unchanged);
        let disk: AlbumMeta = crate::album_db::read(&album_meta_path(dir.path())).unwrap();
        assert_eq!(disk.files["photo.jpg"].hash.as_deref(), Some("current"));
    }

    #[test]
    fn background_writer_rechecks_sources_after_a_busy_lease_is_released() {
        let dir = tempfile::tempdir().unwrap();
        let photo = dir.path().join("photo.jpg");
        fs::write(&photo, b"old original").unwrap();
        update_file_meta(dir.path(), "photo.jpg", |entry| entry.favorite = true).unwrap();
        let before = fs::read(album_meta_path(dir.path())).unwrap();
        let lease = lock_album_metadata(dir.path()).unwrap();
        let (writer, worker) = spawn_derived_writer(METADATA_LEASE_TIMEOUT);
        let (write, result) = pending_write(&photo, |entry| entry.hash = Some("stale hash".into()));
        writer.send(write).unwrap();
        thread::sleep(Duration::from_millis(40));
        let waited = matches!(result.try_recv(), Err(mpsc::TryRecvError::Empty));
        fs::write(
            &photo,
            b"replacement original which must not get the old result",
        )
        .unwrap();
        drop(lease);
        let stale = result.recv_timeout(Duration::from_secs(2));
        drop(writer);
        worker.join().unwrap();
        assert!(waited);
        assert!(stale
            .unwrap()
            .unwrap_err()
            .contains("Media changed during processing"));
        assert_eq!(fs::read(album_meta_path(dir.path())).unwrap(), before);
    }

    #[test]
    fn derived_batch_merges_fields_preserves_user_edits_and_rejects_only_stale_results() {
        let dir = tempfile::tempdir().unwrap();
        let photo = dir.path().join("photo.jpg");
        let stale = dir.path().join("stale.jpg");
        fs::write(&photo, b"source photo").unwrap();
        fs::write(&stale, b"old source").unwrap();
        let (hash, hash_result) =
            pending_write(&photo, |entry| entry.hash = Some("new hash".into()));
        let (thumb, thumb_result) = pending_write(&photo, |entry| {
            entry.thumb_version = Some("new thumb".into())
        });
        let (old, old_result) =
            pending_write(&stale, |entry| entry.hash = Some("stale hash".into()));
        // A foreground edit lands after processing began but before its flush.
        update_file_meta(dir.path(), "photo.jpg", |entry| {
            entry.favorite = true;
            entry.meta = Some("manually edited metadata".into());
        })
        .unwrap();
        fs::write(
            &stale,
            b"a replacement which must not receive old derived results",
        )
        .unwrap();
        assert!(matches!(
            hash_result.try_recv(),
            Err(mpsc::TryRecvError::Empty)
        ));
        flush_derived_writes(vec![hash, thumb, old]);
        assert!(hash_result.recv().unwrap().is_ok());
        assert!(thumb_result.recv().unwrap().is_ok());
        assert!(old_result.recv().unwrap().is_err());
        // Inspect the persisted file directly; a cached snapshot alone is insufficient.
        let persisted: AlbumMeta = crate::album_db::read(&album_meta_path(dir.path())).unwrap();
        let entry = &persisted.files["photo.jpg"];
        assert!(entry.favorite);
        assert_eq!(entry.meta.as_deref(), Some("manually edited metadata"));
        assert_eq!(entry.hash.as_deref(), Some("new hash"));
        assert_eq!(entry.thumb_version.as_deref(), Some("new thumb"));
        assert!(!persisted.files.contains_key("stale.jpg"));
    }

    #[test]
    fn derived_batch_reports_corruption_and_write_failure_without_success_acknowledgement() {
        let dir = tempfile::tempdir().unwrap();
        let photo = dir.path().join("photo.jpg");
        fs::write(&photo, b"source photo").unwrap();
        update_file_meta(dir.path(), "photo.jpg", |entry| entry.favorite = true).unwrap();
        let target = album_meta_path(dir.path());
        fs::write(&target, b"{ damaged json").unwrap();
        let (write, result) = pending_write(&photo, |entry| entry.hash = Some("hash".into()));
        flush_derived_writes(vec![write]);
        assert!(result.recv().unwrap().is_err());
        assert_eq!(fs::read(&target).unwrap(), b"{ damaged json");

        fs::remove_file(&target).unwrap();
        update_file_meta(dir.path(), "photo.jpg", |entry| entry.favorite = true).unwrap();
        let obstructed = target.clone();
        let (write1, result1) = pending_write(&photo, move |entry| {
            entry.hash = Some("hash".into());
            // Deterministically model an external filesystem change after the read.
            fs::remove_file(&obstructed).unwrap();
            fs::create_dir(&obstructed).unwrap();
        });
        let (write2, result2) =
            pending_write(&photo, |entry| entry.thumb_version = Some("thumb".into()));
        flush_derived_writes(vec![write1, write2]);
        assert!(result1.recv().unwrap().is_err());
        assert!(result2.recv().unwrap().is_err());
        assert!(target.is_dir());
        assert_eq!(fs::read(&photo).unwrap(), b"source photo");
    }

    #[test]
    fn background_writer_persists_concurrent_results_and_keeps_foreground_fields() {
        let dir = tempfile::tempdir().unwrap();
        let photo = dir.path().join("photo.jpg");
        fs::write(&photo, b"source photo").unwrap();
        update_file_meta(dir.path(), "photo.jpg", |entry| {
            entry.favorite = true;
            entry.meta = Some("manual metadata".into());
        })
        .unwrap();
        let barrier = Arc::new(std::sync::Barrier::new(16));
        thread::scope(|scope| {
            for n in 0..16 {
                let photo = &photo;
                let barrier = barrier.clone();
                scope.spawn(move || {
                    let _background = BackgroundMetadataWrites::enter();
                    barrier.wait();
                    for i in 0..4 {
                        let result = submit_derived_write(photo, None, None, move |entry| {
                            entry.thumb_version = Some(format!("{n}-{i}"));
                        })
                        .unwrap();
                        assert!(result.favorite);
                    }
                });
            }
        });
        let persisted: AlbumMeta = crate::album_db::read(&album_meta_path(dir.path())).unwrap();
        assert!(persisted.files["photo.jpg"].favorite);
        assert_eq!(
            persisted.files["photo.jpg"].meta.as_deref(),
            Some("manual metadata")
        );
        assert!(persisted.files["photo.jpg"].thumb_version.is_some());
    }

    #[test]
    #[ignore = "paired derived metadata persistence benchmark"]
    fn benchmark_large_album_derived_batches() {
        for sample in 0..3 {
            for background in [false, true] {
                let dir = tempfile::tempdir().unwrap();
                for n in 0..100 {
                    fs::write(dir.path().join(format!("{n:05}.jpg")), b"unchanged source").unwrap();
                }
                update_album_meta(dir.path(), |album| {
                    for n in 0..10000 {
                        album.files.insert(
                            format!("{n:05}.jpg"),
                            FileMetaEntry {
                                favorite: true,
                                meta: Some("unchanged user metadata".into()),
                                ..Default::default()
                            },
                        );
                    }
                })
                .unwrap();
                let barrier = Arc::new(std::sync::Barrier::new(4));
                let started = Instant::now();
                thread::scope(|scope| {
                    for worker in 0..4 {
                        let root = dir.path();
                        let barrier = barrier.clone();
                        scope.spawn(move || {
                            let _background = background.then(BackgroundMetadataWrites::enter);
                            barrier.wait();
                            for n in (worker..100).step_by(4) {
                                let path = root.join(format!("{n:05}.jpg"));
                                let source = source_revision(&path).unwrap();
                                submit_derived_write(&path, None, Some(&source), move |entry| {
                                    entry.hash = Some(format!("derived hash {n}"));
                                })
                                .unwrap();
                            }
                        });
                    }
                });
                let elapsed = started.elapsed();
                let persisted: AlbumMeta =
                    crate::album_db::read(&album_meta_path(dir.path())).unwrap();
                assert_eq!(
                    persisted
                        .files
                        .values()
                        .filter(|entry| entry.hash.is_some())
                        .count(),
                    100
                );
                assert!(persisted.files.values().all(|entry| entry.favorite
                    && entry.meta.as_deref() == Some("unchanged user metadata")));
                eprintln!("derived persistence paired benchmark sample={sample}, background={background}, entries=10000, updates=100, workers=4, elapsed={elapsed:?}");
            }
        }
    }

    #[test]
    #[ignore = "paired old/new background writer benchmark"]
    fn benchmark_background_writer_before_and_after_fair_lease_scheduling() {
        // The batching loop before fair lease scheduling, with the same storage implementation.
        // Each pair alternates order; neither worker is alive during the other run.
        for sample in 0..3 {
            let order = if sample % 2 == 0 {
                [false, true]
            } else {
                [true, false]
            };
            for fair in order {
                let dir = tempfile::tempdir().unwrap();
                for n in 0..100 {
                    fs::write(dir.path().join(format!("{n:05}.jpg")), b"unchanged source").unwrap();
                }
                update_album_meta(dir.path(), |album| {
                    for n in 0..10000 {
                        album.files.insert(
                            format!("{n:05}.jpg"),
                            FileMetaEntry {
                                favorite: true,
                                meta: Some("unchanged user metadata".into()),
                                ..Default::default()
                            },
                        );
                    }
                })
                .unwrap();
                let (legacy_sender, legacy_receiver) = mpsc::sync_channel::<DerivedWrite>(64);
                let (candidate, worker) = if fair {
                    let (candidate, worker) = spawn_derived_writer(METADATA_LEASE_TIMEOUT);
                    (Some(candidate), worker)
                } else {
                    (
                        None,
                        thread::spawn(move || {
                            crate::util::set_low_priority_current_thread();
                            while let Ok(first) = legacy_receiver.recv() {
                                let mut batch = vec![first];
                                let deadline = Instant::now() + DERIVED_BATCH_WAIT;
                                while batch.len() < DERIVED_BATCH_SIZE {
                                    let Some(remaining) =
                                        deadline.checked_duration_since(Instant::now())
                                    else {
                                        break;
                                    };
                                    match legacy_receiver.recv_timeout(remaining) {
                                        Ok(next) => batch.push(next),
                                        Err(_) => break,
                                    }
                                }
                                flush_derived_writes(batch);
                            }
                        }),
                    )
                };
                let barrier = Arc::new(std::sync::Barrier::new(4));
                let started = Instant::now();
                thread::scope(|scope| {
                    for worker_id in 0..4 {
                        let root = dir.path();
                        let barrier = barrier.clone();
                        let candidate = candidate.clone();
                        let legacy_sender = legacy_sender.clone();
                        scope.spawn(move || {
                            barrier.wait();
                            for n in (worker_id..100).step_by(4) {
                                let path = root.join(format!("{n:05}.jpg"));
                                let (write, result) = pending_write(&path, move |entry| {
                                    entry.hash = Some(format!("derived hash {n}"))
                                });
                                if let Some(candidate) = &candidate {
                                    candidate.send(write).unwrap();
                                } else {
                                    legacy_sender.send(write).unwrap();
                                }
                                assert!(result.recv().unwrap().unwrap().favorite);
                            }
                        });
                    }
                });
                let elapsed = started.elapsed();
                drop(candidate);
                drop(legacy_sender);
                worker.join().unwrap();
                let disk: AlbumMeta = crate::album_db::read(&album_meta_path(dir.path())).unwrap();
                assert_eq!(
                    disk.files
                        .values()
                        .filter(|entry| entry.hash.is_some())
                        .count(),
                    100
                );
                assert!(disk.files.values().all(|entry| entry.favorite
                    && entry.meta.as_deref() == Some("unchanged user metadata")));
                for n in 0..100 {
                    assert_eq!(
                        disk.files[&format!("{n:05}.jpg")].hash.as_deref(),
                        Some(format!("derived hash {n}").as_str())
                    );
                }
                eprintln!("background writer paired sample={sample}, fair={fair}, entries=10000, updates=100, workers=4, elapsed={elapsed:?}");
            }
        }
    }
    #[test]
    fn failures_keep_separate_processing_versions_and_metadata_respects_parse_policy() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("photo.jpg");
        fs::write(&path, b"cached photo").unwrap();
        let source = source_revision(&path).unwrap();
        let settings = read_settings();
        let mut changed = settings.clone();
        changed.ffmpeg.timeout_secs += 1;
        assert_ne!(settings.processing_version(), changed.processing_version());
        mark_thumb_failed(&path, &source, &changed.processing_version()).unwrap();
        assert!(!is_thumb_failed(&path));
        mark_meta_failed(&path, &source, &settings.processing_version()).unwrap();
        assert!(is_meta_failed(&path));
        assert!(
            !is_thumb_failed(&path),
            "new metadata failure must not revive an old thumbnail failure"
        );
        update_file_meta(dir.path(), "photo.jpg", |entry| {
            entry.meta = Some("0".into());
            entry.meta_source_revision = Some(source.clone());
            entry.meta_settings_version = Some(settings.metadata_version());
        })
        .unwrap();
        let entry = &read_album_meta(dir.path()).files["photo.jpg"];
        assert!(metadata_cache_current_with_settings(
            &path, entry, &settings
        ));
        assert!(metadata_cache_current_with_settings(&path, entry, &changed));
        changed.metadata.parse_creation_time = !settings.metadata.parse_creation_time;
        assert!(!metadata_cache_current_with_settings(
            &path, entry, &changed
        ));
    }

    #[test]
    fn failed_probes_are_bound_to_the_source_and_late_failure_is_rejected() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("replaced.jpg");
        fs::write(&path, b"broken old file").unwrap();
        let old = source_revision(&path).unwrap();
        mark_meta_failed(&path, &old, &read_settings().processing_version()).unwrap();
        mark_thumb_failed(&path, &old, &read_settings().processing_version()).unwrap();
        assert!(is_meta_failed(&path));
        assert!(is_thumb_failed(&path));
        fs::write(&path, b"a replacement with different contents").unwrap();
        assert!(!is_meta_failed(&path));
        assert!(!is_thumb_failed(&path));
        assert!(mark_meta_failed(&path, &old, &read_settings().processing_version()).is_err());
        assert!(mark_thumb_failed(&path, &old, &read_settings().processing_version()).is_err());
        assert!(
            update_file_meta_if_source_current(&path, &old, |entry| entry.meta = Some("0".into()))
                .is_err()
        );
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_batch_persists_successful_files_before_a_later_failure() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("first.jpg");
        fs::write(&source, b"file with cached metadata").unwrap();
        update_file_meta(dir.path(), "first.jpg", |entry| {
            entry.meta = Some("0".into());
            entry.meta_source_revision = source_revision(&source);
            entry.meta_settings_version = Some(read_settings().metadata_version());
            entry.favorite = true;
        })
        .unwrap();
        assert!(set_media_timestamp(
            dir.path().to_string_lossy().into(),
            vec!["first.jpg".into(), "missing.jpg".into()],
            123
        )
        .is_err());
        let entry = &read_album_meta(dir.path()).files["first.jpg"];
        assert!(entry.favorite);
        assert_eq!(
            unpack_file_meta(entry.meta.as_deref().unwrap()).unwrap().s,
            Some(123)
        );
        assert_eq!(read_embedded_timestamp(&source), Some(123));
    }

    #[test]
    #[cfg(unix)]
    fn failed_timestamp_persistence_does_not_change_the_embedded_date() {
        for previous in [
            None,
            Some(b"100".as_slice()),
            Some(b"invalid old date".as_slice()),
        ] {
            let dir = tempfile::tempdir().unwrap();
            let source = dir.path().join("photo.jpg");
            fs::write(&source, b"original with cached metadata").unwrap();
            if let Some(previous) = previous {
                xattr::set(&source, EMBEDDED_TS_XATTR, previous).unwrap();
            }
            update_file_meta(dir.path(), "photo.jpg", |entry| {
                entry.meta = Some(
                    DetachedFileMeta {
                        a: None,
                        s: Some(100),
                        i: true,
                        v: false,
                        w: Some(64),
                        h: Some(32),
                    }
                    .pack(),
                );
                entry.meta_source_revision = source_revision(&source);
                entry.meta_settings_version = Some(read_settings().metadata_version());
                entry.favorite = true;
            })
            .unwrap();
            fs::write(album_meta_path(dir.path()), b"{broken-json").unwrap();
            assert!(set_media_timestamp(
                dir.path().to_string_lossy().into(),
                vec!["photo.jpg".into()],
                123
            )
            .is_err());
            assert_eq!(
                xattr::get(&source, EMBEDDED_TS_XATTR).unwrap().as_deref(),
                previous
            );
            assert_eq!(
                fs::read(album_meta_path(dir.path())).unwrap(),
                b"{broken-json"
            );
            assert_eq!(fs::read(&source).unwrap(), b"original with cached metadata");
        }
    }

    #[cfg(unix)]
    fn timestamp_fixture(dir: &Path, names: &[&str]) {
        for name in names {
            fs::write(dir.join(name), b"unchanged cached original").unwrap();
        }
        update_album_meta(dir, |album| {
            for name in names {
                album.files.insert(
                    (*name).into(),
                    FileMetaEntry {
                        meta: Some(
                            DetachedFileMeta {
                                a: None,
                                s: Some(100),
                                i: true,
                                v: false,
                                w: Some(64),
                                h: Some(32),
                            }
                            .pack(),
                        ),
                        meta_source_revision: source_revision(&dir.join(name)),
                        meta_settings_version: Some(read_settings().metadata_version()),
                        favorite: true,
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
    }
    #[cfg(unix)]
    fn reject_timestamp_database_write(dir: &Path, album: Arc<AlbumMeta>) -> Result<(), String> {
        let path = album_meta_path(dir);
        let backup = meta_dir(dir).join("album.sqlite.backup");
        fs::rename(&path, &backup).unwrap();
        fs::create_dir(&path).unwrap();
        let result = write_album_meta_arc_without_lock(dir, album);
        fs::remove_dir(&path).unwrap();
        fs::rename(&backup, &path).unwrap();
        assert!(result.is_err(), "the actual database write must fail");
        result
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_database_write_failure_restores_raw_attributes_and_metadata() {
        let dir = tempfile::tempdir().unwrap();
        let names = ["absent.jpg", "valid.jpg", "invalid.jpg"];
        timestamp_fixture(dir.path(), &names);
        let old: [Option<&[u8]>; 3] = [None, Some(b"100"), Some(b"invalid\xff\0date")];
        for (name, raw) in names.iter().zip(old) {
            if let Some(raw) = raw {
                xattr::set(dir.path().join(name), EMBEDDED_TS_XATTR, raw).unwrap();
            }
        }
        let before = fs::read(album_meta_path(dir.path())).unwrap();
        let (prepared, failure) = prepare_timestamp_updates(
            dir.path(),
            names.iter().map(|name| name.to_string()).collect(),
        );
        assert!(failure.is_none());
        assert!(commit_timestamp_updates(
            dir.path(),
            &prepared,
            123,
            failure,
            reject_timestamp_database_write
        )
        .is_err());
        for (name, raw) in names.iter().zip(old) {
            assert_eq!(
                xattr::get(dir.path().join(name), EMBEDDED_TS_XATTR)
                    .unwrap()
                    .as_deref(),
                raw
            );
            assert_eq!(
                fs::read(dir.path().join(name)).unwrap(),
                b"unchanged cached original"
            );
        }
        assert_eq!(fs::read(album_meta_path(dir.path())).unwrap(), before);
        assert!(read_album_meta(dir.path())
            .files
            .values()
            .all(|entry| entry.favorite));
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_rollback_owns_the_original_after_its_path_is_replaced() {
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["photo.jpg"]);
        let source = dir.path().join("photo.jpg");
        xattr::set(&source, EMBEDDED_TS_XATTR, b"100").unwrap();
        let before = fs::read(album_meta_path(dir.path())).unwrap();
        let (prepared, failure) = prepare_timestamp_updates(dir.path(), vec!["photo.jpg".into()]);
        let parked = dir.path().join("parked-original");
        let result = commit_timestamp_updates(dir.path(), &prepared, 123, failure, |dir, album| {
            assert_eq!(read_embedded_timestamp(&source), Some(123));
            fs::rename(&source, &parked).unwrap();
            fs::write(&source, b"replacement must remain untouched").unwrap();
            xattr::set(&source, EMBEDDED_TS_XATTR, b"456").unwrap();
            reject_timestamp_database_write(dir, album)
        });
        assert!(result.is_err());
        assert_eq!(read_embedded_timestamp(&parked), Some(100));
        assert_eq!(read_embedded_timestamp(&source), Some(456));
        assert_eq!(
            fs::read(&source).unwrap(),
            b"replacement must remain untouched"
        );
        assert_eq!(fs::read(album_meta_path(dir.path())).unwrap(), before);
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_rejects_a_replacement_after_preparation_without_touching_it() {
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["photo.jpg"]);
        let source = dir.path().join("photo.jpg");
        let before = fs::read(album_meta_path(dir.path())).unwrap();
        let (prepared, failure) = prepare_timestamp_updates(dir.path(), vec!["photo.jpg".into()]);
        fs::rename(&source, dir.path().join("previous-original")).unwrap();
        fs::write(&source, b"replacement").unwrap();
        assert!(commit_timestamp_updates(
            dir.path(),
            &prepared,
            123,
            failure,
            write_album_meta_arc_without_lock
        )
        .is_err());
        assert!(read_embedded_timestamp(&source).is_none());
        assert_eq!(fs::read(&source).unwrap(), b"replacement");
        assert_eq!(fs::read(album_meta_path(dir.path())).unwrap(), before);
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_does_not_modify_attributes_while_waiting_for_an_album_lease() {
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["photo.jpg"]);
        let source = dir.path().join("photo.jpg");
        let lease = lock_album_metadata(dir.path()).unwrap();
        let path = dir.path().to_string_lossy().into_owned();
        let (entered, entry) = mpsc::channel();
        let worker = thread::spawn(move || {
            entered.send(()).unwrap();
            set_media_timestamp(path, vec!["photo.jpg".into()], 123)
        });
        entry.recv_timeout(Duration::from_secs(5)).unwrap();
        thread::sleep(Duration::from_millis(100));
        let before_release = read_embedded_timestamp(&source);
        drop(lease);
        assert_eq!(worker.join().unwrap().unwrap().len(), 1);
        assert_eq!(before_release, None);
        assert_eq!(read_embedded_timestamp(&source), Some(123));
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_later_database_failure_keeps_earlier_persisted_files() {
        let dir = tempfile::tempdir().unwrap();
        let names = (0..129).map(|n| format!("{n}.jpg")).collect::<Vec<_>>();
        timestamp_fixture(
            dir.path(),
            &names.iter().map(String::as_str).collect::<Vec<_>>(),
        );
        let (prepared, failure) = prepare_timestamp_updates(dir.path(), names.clone());
        let mut writes = 0;
        assert!(
            commit_timestamp_updates(dir.path(), &prepared, 123, failure, |dir, album| {
                writes += 1;
                if writes == 2 {
                    reject_timestamp_database_write(dir, album)
                } else {
                    write_album_meta_arc_without_lock(dir, album)
                }
            })
            .is_err()
        );
        let album = read_album_meta(dir.path());
        let persisted = names
            .iter()
            .filter(|name| read_embedded_timestamp(&dir.path().join(name)) == Some(123))
            .count();
        assert!(persisted > 0 && persisted < names.len());
        for name in &names {
            let embedded = read_embedded_timestamp(&dir.path().join(name));
            let cached = unpack_file_meta(album.files[name].meta.as_deref().unwrap())
                .unwrap()
                .s;
            assert_eq!(cached, embedded.or(Some(100)));
            assert!(album.files[name].favorite);
        }
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_overlapping_writers_leave_database_and_attributes_in_agreement() {
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["photo.jpg"]);
        let barrier = Arc::new(std::sync::Barrier::new(3));
        let workers = [123, 456]
            .into_iter()
            .map(|timestamp| {
                let barrier = barrier.clone();
                let path = dir.path().to_string_lossy().into_owned();
                thread::spawn(move || {
                    barrier.wait();
                    set_media_timestamp(path, vec!["photo.jpg".into()], timestamp)
                })
            })
            .collect::<Vec<_>>();
        barrier.wait();
        for worker in workers {
            assert_eq!(worker.join().unwrap().unwrap().len(), 1);
        }
        let embedded = read_embedded_timestamp(&dir.path().join("photo.jpg")).unwrap();
        assert!([123, 456].contains(&embedded));
        let entry = &read_album_meta(dir.path()).files["photo.jpg"];
        assert_eq!(
            unpack_file_meta(entry.meta.as_deref().unwrap()).unwrap().s,
            Some(embedded)
        );
        assert!(entry.favorite);
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_range_validation_does_not_truncate_or_modify_files_on_failure() {
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["photo.jpg"]);
        let before = fs::read(album_meta_path(dir.path())).unwrap();
        let path = dir.path().to_string_lossy().into_owned();
        assert!(set_media_timestamp(path.clone(), vec!["photo.jpg".into()], 1 << 40).is_err());
        assert!(read_embedded_timestamp(&dir.path().join("photo.jpg")).is_none());
        assert_eq!(fs::read(album_meta_path(dir.path())).unwrap(), before);
        let upper = (1 << 40) - 1;
        let result = set_media_timestamp(path, vec!["photo.jpg".into()], upper).unwrap();
        assert_eq!(unpack_file_meta(&result[0].meta).unwrap().s, Some(upper));
        assert_eq!(
            read_embedded_timestamp(&dir.path().join("photo.jpg")),
            Some(upper)
        );
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn timestamp_preserves_symlink_attribute_semantics_and_detects_same_target_replacement() {
        use std::os::unix::fs::symlink;
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["target.jpg"]);
        let target = dir.path().join("target.jpg");
        let link = dir.path().join("link.jpg");
        symlink(&target, &link).unwrap();
        update_file_meta(dir.path(), "link.jpg", |entry| {
            entry.meta = Some("0".into());
            entry.meta_source_revision = source_revision(&link);
            entry.meta_settings_version = Some(read_settings().metadata_version());
        })
        .unwrap();
        assert_eq!(
            set_media_timestamp(
                dir.path().to_string_lossy().into(),
                vec!["link.jpg".into()],
                123
            )
            .unwrap()
            .len(),
            1
        );
        assert_eq!(read_embedded_timestamp(&link), Some(123));
        assert!(read_embedded_timestamp(&target).is_none());
        let (prepared, failure) = prepare_timestamp_updates(dir.path(), vec!["link.jpg".into()]);
        let old_source = source_revision(&link);
        let old_link = dir.path().join("previous-link");
        fs::rename(&link, &old_link).unwrap();
        symlink(&target, &link).unwrap();
        assert_eq!(source_revision(&link), old_source);
        assert!(commit_timestamp_updates(
            dir.path(),
            &prepared,
            456,
            failure,
            write_album_meta_arc_without_lock
        )
        .is_err());
        assert!(read_embedded_timestamp(&link).is_none());
        assert!(read_embedded_timestamp(&target).is_none());
        assert_eq!(read_embedded_timestamp(&old_link), Some(123));
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn timestamp_write_only_original_keeps_its_permissions_and_survives_database_failure() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["photo.jpg"]);
        let source = dir.path().join("photo.jpg");
        xattr::set(&source, EMBEDDED_TS_XATTR, b"100").unwrap();
        fs::set_permissions(&source, fs::Permissions::from_mode(0o200)).unwrap();
        xattr::set(&source, EMBEDDED_TS_XATTR, b"100").unwrap();
        let result = set_media_timestamp(
            dir.path().to_string_lossy().into(),
            vec!["photo.jpg".into()],
            123,
        );
        assert_eq!(
            source.metadata().unwrap().permissions().mode() & 0o777,
            0o200
        );
        fs::set_permissions(&source, fs::Permissions::from_mode(0o600)).unwrap();
        assert_eq!(result.unwrap().len(), 1);
        assert_eq!(read_embedded_timestamp(&source), Some(123));
        let (prepared, failure) = prepare_timestamp_updates(dir.path(), vec!["photo.jpg".into()]);
        let before = fs::read(album_meta_path(dir.path())).unwrap();
        fs::set_permissions(&source, fs::Permissions::from_mode(0o200)).unwrap();
        let result = commit_timestamp_updates(
            dir.path(),
            &prepared,
            456,
            failure,
            reject_timestamp_database_write,
        );
        fs::set_permissions(&source, fs::Permissions::from_mode(0o600)).unwrap();
        assert!(result.is_err());
        assert_eq!(read_embedded_timestamp(&source), Some(123));
        assert_eq!(fs::read(album_meta_path(dir.path())).unwrap(), before);
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_large_selection_succeeds_with_a_low_descriptor_limit() {
        use crate::test_support::spawn_test;
        if std::env::var("ROOM237_TEST_ROLE").as_deref() == Ok("timestamp-fd-limit") {
            let root = PathBuf::from(std::env::var_os("ROOM237_TEST_DIR").unwrap());
            let names = (0..512).map(|n| format!("{n}.jpg")).collect::<Vec<_>>();
            timestamp_fixture(&root, &names.iter().map(String::as_str).collect::<Vec<_>>());
            let mut limit = libc::rlimit {
                rlim_cur: 0,
                rlim_max: 0,
            };
            assert_eq!(
                unsafe { libc::getrlimit(libc::RLIMIT_NOFILE, &mut limit) },
                0
            );
            limit.rlim_cur = 64;
            assert_eq!(unsafe { libc::setrlimit(libc::RLIMIT_NOFILE, &limit) }, 0);
            assert_eq!(
                set_media_timestamp(root.to_string_lossy().into(), names.clone(), 123)
                    .unwrap()
                    .len(),
                names.len()
            );
            for name in names {
                assert_eq!(read_embedded_timestamp(&root.join(name)), Some(123));
            }
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut child = spawn_test(
            "metadata::tests::timestamp_large_selection_succeeds_with_a_low_descriptor_limit",
            dir.path(),
            "timestamp-fd-limit",
        );
        assert!(child.0.wait().unwrap().success());
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_duplicate_names_rollback_to_the_exact_original_attribute() {
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["photo.jpg"]);
        let source = dir.path().join("photo.jpg");
        xattr::set(&source, EMBEDDED_TS_XATTR, b"invalid old bytes\xff").unwrap();
        let (prepared, failure) =
            prepare_timestamp_updates(dir.path(), vec!["photo.jpg".into(), "photo.jpg".into()]);
        assert!(commit_timestamp_updates(
            dir.path(),
            &prepared,
            123,
            failure,
            reject_timestamp_database_write
        )
        .is_err());
        assert_eq!(
            xattr::get(&source, EMBEDDED_TS_XATTR).unwrap().unwrap(),
            b"invalid old bytes\xff"
        );
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_rollback_preserves_a_visible_external_attribute_edit() {
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["photo.jpg"]);
        let source = dir.path().join("photo.jpg");
        xattr::set(&source, EMBEDDED_TS_XATTR, b"100").unwrap();
        let (prepared, failure) = prepare_timestamp_updates(dir.path(), vec!["photo.jpg".into()]);
        let result = commit_timestamp_updates(dir.path(), &prepared, 123, failure, |dir, album| {
            xattr::set(&source, EMBEDDED_TS_XATTR, b"456").unwrap();
            reject_timestamp_database_write(dir, album)
        });
        assert!(result.err().unwrap().contains("another writer changed"));
        assert_eq!(read_embedded_timestamp(&source), Some(456));
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn timestamp_write_only_attribute_failure_restores_the_saved_database() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().unwrap();
        timestamp_fixture(dir.path(), &["photo.jpg"]);
        let source = dir.path().join("photo.jpg");
        xattr::set(&source, EMBEDDED_TS_XATTR, b"100").unwrap();
        let (prepared, failure) = prepare_timestamp_updates(dir.path(), vec!["photo.jpg".into()]);
        let before = crate::album_db::read(&album_meta_path(dir.path())).unwrap();
        fs::set_permissions(&source, fs::Permissions::from_mode(0o200)).unwrap();
        let mut commits = 0;
        let result = commit_timestamp_updates(dir.path(), &prepared, 456, failure, |dir, album| {
            commits += 1;
            write_album_meta_arc_without_lock(dir, album)?;
            if commits == 1 {
                fs::set_permissions(&source, fs::Permissions::from_mode(0o000)).unwrap();
            }
            Ok(())
        });
        fs::set_permissions(&source, fs::Permissions::from_mode(0o600)).unwrap();
        assert!(result.is_err());
        assert_eq!(commits, 2);
        assert_eq!(read_embedded_timestamp(&source), Some(100));
        let restored = crate::album_db::read(&album_meta_path(dir.path())).unwrap();
        assert_eq!(restored.files, before.files);
        assert_eq!(restored.duplicates_ignore, before.duplicates_ignore);
    }

    #[test]
    fn album_cache_rejects_an_external_sqlite_transaction() {
        let dir = tempfile::tempdir().unwrap();
        update_file_meta(dir.path(), "photo.jpg", |entry| entry.favorite = true).unwrap();
        let original = read_album_meta_checked(dir.path()).unwrap();
        let mut replacement = (*original).clone();
        replacement.files.get_mut("photo.jpg").unwrap().favorite = false;
        crate::album_db::replace(&album_meta_path(dir.path()), &replacement).unwrap();
        assert!(!read_album_meta_checked(dir.path()).unwrap().files["photo.jpg"].favorite);
        assert!(original.files["photo.jpg"].favorite);
    }

    #[test]
    #[cfg(unix)]
    fn timestamp_publication_after_album_deletion_does_not_recreate_it() {
        let root = tempfile::tempdir().unwrap();
        let album = root.path().join("album");
        fs::create_dir(&album).unwrap();
        timestamp_fixture(&album, &["photo.jpg"]);
        let source = album.join("photo.jpg");
        xattr::set(&source, EMBEDDED_TS_XATTR, b"100").unwrap();
        let (prepared, failure) = prepare_timestamp_updates(&album, vec!["photo.jpg".into()]);
        let parked = root.path().join("parked-original");
        let result = commit_timestamp_updates(&album, &prepared, 123, failure, |dir, snapshot| {
            fs::rename(&source, &parked).unwrap();
            fs::remove_dir_all(dir).unwrap();
            write_album_meta_arc_without_lock(dir, snapshot)
        });
        assert!(result.is_err());
        assert!(!album.exists());
        assert_eq!(read_embedded_timestamp(&parked), Some(100));
        assert_eq!(fs::read(&parked).unwrap(), b"unchanged cached original");
    }

    #[test]
    fn metadata_read_does_not_create_a_missing_album() {
        let root = tempfile::tempdir().unwrap();
        let absent = root.path().join("removed-album");
        assert!(read_album_meta(&absent).files.is_empty());
        assert!(!absent.exists());
    }

    #[test]
    #[ignore = "manual date transaction paired benchmark"]
    #[cfg(unix)]
    fn benchmark_timestamp_transaction_before_and_after() {
        let dir = tempfile::tempdir().unwrap();
        let names = (0..100).map(|n| format!("{n:05}.jpg")).collect::<Vec<_>>();
        for name in &names {
            fs::write(dir.path().join(name), b"cached image").unwrap();
        }
        update_album_meta(dir.path(), |album| {
            for n in 0..10000 {
                let name = format!("{n:05}.jpg");
                album.files.insert(
                    name.clone(),
                    FileMetaEntry {
                        meta: Some("0".into()),
                        meta_settings_version: Some(read_settings().metadata_version()),
                        meta_source_revision: source_revision(&dir.path().join(name)),
                        favorite: true,
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
        for sample in 0..3 {
            let order = if sample % 2 == 0 {
                [false, true]
            } else {
                [true, false]
            };
            for leased in order {
                let started = Instant::now();
                let result = if leased {
                    set_media_timestamp(dir.path().to_string_lossy().into(), names.clone(), 123)
                } else {
                    set_media_timestamp_legacy(
                        dir.path().to_string_lossy().into(),
                        names.clone(),
                        123,
                    )
                }
                .unwrap();
                let elapsed = started.elapsed();
                assert_eq!(result.len(), names.len());
                assert!(result.iter().all(|entry| entry.favorite == Some(true)
                    && unpack_file_meta(&entry.meta).unwrap().s == Some(123)));
                let album = read_album_meta(dir.path());
                for name in &names {
                    assert_eq!(read_embedded_timestamp(&dir.path().join(name)), Some(123));
                    assert_eq!(
                        unpack_file_meta(album.files[name].meta.as_deref().unwrap())
                            .unwrap()
                            .s,
                        Some(123)
                    );
                    assert!(album.files[name].favorite);
                }
                eprintln!("manual date transaction paired sample={sample}, leased={leased}, entries=10000, files=100, fd_group={}, elapsed={elapsed:?}", timestamp_batch_size());
            }
        }
    }

    #[test]
    #[ignore = "metadata date batch microbenchmark"]
    #[cfg(unix)]
    fn benchmark_large_album_date_batch() {
        let dir = tempfile::tempdir().unwrap();
        let names = (0..100).map(|n| format!("{n:05}.jpg")).collect::<Vec<_>>();
        for name in &names {
            fs::write(dir.path().join(name), b"cached image").unwrap();
        }
        update_album_meta(dir.path(), |album| {
            for n in 0..10000 {
                album.files.insert(
                    format!("{n:05}.jpg"),
                    FileMetaEntry {
                        meta: Some("0".into()),
                        meta_settings_version: Some(read_settings().metadata_version()),
                        meta_source_revision: source_revision(
                            &dir.path().join(format!("{n:05}.jpg")),
                        ),
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
        let start = std::time::Instant::now();
        let result = set_media_timestamp(dir.path().to_string_lossy().into(), names, 123).unwrap();
        eprintln!(
            "metadata date batch benchmark: 10000 entries, 100 files={:?}",
            start.elapsed()
        );
        assert_eq!(result.len(), 100);
        assert!(result
            .iter()
            .all(|entry| unpack_file_meta(&entry.meta).unwrap().s == Some(123)));
    }

    #[test]
    #[ignore = "metadata write microbenchmark"]
    fn benchmark_large_album_metadata_updates() {
        let dir = tempfile::tempdir().unwrap();
        update_album_meta(dir.path(), |album| {
            for n in 0..10000 {
                album.files.insert(
                    format!("{n:05}.jpg"),
                    FileMetaEntry {
                        meta: Some(format!("{n:x}")),
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
        let bytes = fs::metadata(album_meta_path(dir.path())).unwrap().len();
        let start = std::time::Instant::now();
        for n in 0..100 {
            update_file_meta(dir.path(), &format!("{n:05}.jpg"), |entry| {
                entry.favorite = true
            })
            .unwrap();
        }
        eprintln!(
            "metadata write benchmark: 10000 entries, bytes={bytes}, 100 atomic updates={:?}",
            start.elapsed()
        );
        assert_eq!(
            read_album_meta(dir.path())
                .files
                .values()
                .filter(|entry| entry.favorite)
                .count(),
            100
        );
    }

    #[test]
    fn hashes_without_input_revisions_are_not_trusted_and_replacements_invalidate_the_cache() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("photo.jpg");
        fs::write(&path, b"first").unwrap();
        let cfg = read_settings().hash_config();
        update_file_meta(dir.path(), "photo.jpg", |entry| {
            entry.favorite = true;
            entry.hash = Some("legacy".into());
            entry.hash_bits = Some(cfg.bits);
            entry.hash_version = Some(cfg.hash_version.clone());
        })
        .unwrap();
        assert!(load_album_file_hash(&path).is_none());
        write_album_file_hash(
            &path,
            "fresh".into(),
            cfg.hash_version.clone(),
            cfg.bits,
            &media_revision(&path),
        )
        .unwrap();
        assert_eq!(load_album_file_hash(&path).unwrap().hash, "fresh");
        let old_revision = media_revision(&path);
        fs::write(&path, b"different file contents").unwrap();
        assert!(load_album_file_hash(&path).is_none());
        assert!(write_album_file_hash(
            &path,
            "late stale result".into(),
            cfg.hash_version.clone(),
            cfg.bits,
            &old_revision
        )
        .is_err());
        let meta = read_album_meta(dir.path());
        assert!(meta.files["photo.jpg"].favorite);
        assert_eq!(meta.files["photo.jpg"].hash.as_deref(), Some("fresh"));
    }

    #[test]
    fn thumbnail_changes_invalidate_hashes_and_late_failures_cannot_poison_a_new_source() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("photo.jpg");
        fs::write(&path, b"source").unwrap();
        let cfg = read_settings().hash_config();
        write_album_file_hash(
            &path,
            "original hash".into(),
            cfg.hash_version.clone(),
            cfg.bits,
            &media_revision(&path),
        )
        .unwrap();
        fs::create_dir(dir.path().join(".room237-thumb")).unwrap();
        fs::write(dir.path().join(".room237-thumb/photo.jpg.webp"), b"preview").unwrap();
        assert!(load_album_file_hash(&path).is_none());
        let before = media_revision(&path);
        mark_hash_failed_if_current(&path, &before, &cfg, &read_settings().processing_version())
            .unwrap();
        assert!(hash_cache_current(
            &path,
            &read_album_meta(dir.path()).files["photo.jpg"]
        ));
        fs::write(&path, b"fixed source").unwrap();
        assert!(!hash_cache_current(
            &path,
            &read_album_meta(dir.path()).files["photo.jpg"]
        ));
        assert!(mark_hash_failed_if_current(
            &path,
            &before,
            &cfg,
            &read_settings().processing_version()
        )
        .is_err());
    }

    #[test]
    fn thumbnail_versions_are_bound_to_their_source_even_without_a_watcher() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("photo.jpg");
        fs::write(&path, b"source").unwrap();
        let source = source_revision(&path).unwrap();
        write_thumb_version(&path, "version", &source).unwrap();
        assert_eq!(load_thumb_version(&path).as_deref(), Some("version"));
        fs::write(&path, b"replaced source").unwrap();
        assert!(load_thumb_version(&path).is_none());
        assert!(write_thumb_version(&path, "stale", &source).is_err());
    }

    #[test]
    fn shared_snapshot_is_reused_and_invalidated_by_internal_and_external_writes() {
        let dir = tempfile::tempdir().unwrap();
        update_file_meta(dir.path(), "a.jpg", |entry| entry.favorite = true).unwrap();
        let first = {
            let _guard = STORE_WRITE_LOCK.lock().unwrap();
            let first = read_album_snapshot_without_lock(dir.path(), false).unwrap();
            assert!(Arc::ptr_eq(
                &first,
                &read_album_snapshot_without_lock(dir.path(), false).unwrap()
            ));
            first
        };
        update_file_meta(dir.path(), "a.jpg", |entry| entry.favorite = false).unwrap();
        let second = read_album_meta(dir.path());
        assert!(!Arc::ptr_eq(&first, &second));
        assert!(!second.files["a.jpg"].favorite);
        crate::album_db::replace(&album_meta_path(dir.path()), &AlbumMeta::default()).unwrap();
        assert!(read_album_meta(dir.path()).files.is_empty());
        assert!(first.files["a.jpg"].favorite);
    }
    #[test]
    #[ignore = "CPU benchmark, run separately with --nocapture"]
    fn benchmark_shared_metadata_reads() {
        let dir = tempfile::tempdir().unwrap();
        update_album_meta(dir.path(), |album| {
            for n in 0..10000 {
                album.files.insert(
                    format!("{n}.jpg"),
                    FileMetaEntry {
                        meta: Some("0".into()),
                        favorite: n % 4 == 0,
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
        let bytes =
            serde_json::to_vec(&crate::album_db::read(&album_meta_path(dir.path())).unwrap())
                .unwrap();
        let start = std::time::Instant::now();
        for _ in 0..100 {
            std::hint::black_box(serde_json::from_slice::<AlbumMeta>(&bytes).unwrap());
        }
        let parsed = start.elapsed();
        read_album_meta(dir.path());
        let start = std::time::Instant::now();
        for _ in 0..1000 {
            std::hint::black_box(read_album_meta(dir.path()));
        }
        println!("metadata benchmark: 10000 entries, JSON bytes={}, old parse 100 reads={:?}, shared 1000 reads={:?}", bytes.len(), parsed, start.elapsed());
    }
    #[test]
    #[cfg(unix)]
    #[ignore = "filesystem benchmark, run separately with --nocapture"]
    fn benchmark_embedded_date_cache_validation() {
        let root = tempfile::tempdir().unwrap();
        let settings = read_settings();
        let rows = (0..10000)
            .map(|n| {
                let path = root.path().join(format!("{n:05}.jpg"));
                fs::write(&path, b"cached original").unwrap();
                let manual = (n % 2 == 0).then_some(-315619200);
                if let Some(timestamp) = manual {
                    write_embedded_timestamp(&path, timestamp).unwrap();
                }
                let entry = FileMetaEntry {
                    meta: Some(
                        DetachedFileMeta {
                            a: None,
                            s: manual,
                            i: true,
                            v: false,
                            w: Some(64),
                            h: Some(32),
                        }
                        .pack(),
                    ),
                    meta_source_revision: source_revision(&path),
                    meta_settings_version: Some(settings.metadata_version()),
                    meta_embedded_timestamp: manual,
                    meta_embedded_revision: embedded_timestamp_revision(&path),
                    favorite: true,
                    ..Default::default()
                };
                (path, entry)
            })
            .collect::<Vec<_>>();
        // Warm both predicates before alternating equal-size samples. No FFmpeg,
        // JSON parsing or writes are part of this cache-validation benchmark.
        for (path, entry) in &rows {
            assert!(metadata_content_cache_current_with_settings(
                path, entry, &settings
            ));
            assert!(metadata_cache_current_with_settings(path, entry, &settings));
        }
        for sample in 0..3 {
            let order = if sample % 2 == 0 {
                [false, true]
            } else {
                [true, false]
            };
            for check_embedded in order {
                let started = Instant::now();
                let actual = rows
                    .iter()
                    .map(|(path, entry)| {
                        if check_embedded {
                            metadata_cache_current_with_settings(path, entry, &settings)
                        } else {
                            metadata_content_cache_current_with_settings(path, entry, &settings)
                        }
                    })
                    .collect::<Vec<_>>();
                let elapsed = started.elapsed();
                assert!(actual.into_iter().all(|current| current));
                eprintln!("embedded date cache benchmark sample={sample}, check_embedded={check_embedded}, files=10000, manual=5000, elapsed={elapsed:?}");
            }
        }
        let (path, entry) = &rows[0];
        write_embedded_timestamp(path, -42).unwrap();
        assert!(metadata_content_cache_current_with_settings(
            path, entry, &settings
        ));
        assert!(!metadata_cache_current_with_settings(
            path, entry, &settings
        ));
    }

    #[test]
    fn signed_date_exif_preserves_pre_epoch_timestamp() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("old.tiff");
        // Minimal little-endian TIFF with DateTime in the primary IFD.
        let mut bytes = b"II\x2a\x00\x08\x00\x00\x00".to_vec();
        bytes.extend_from_slice(&1u16.to_le_bytes());
        bytes.extend_from_slice(&0x0132u16.to_le_bytes());
        bytes.extend_from_slice(&2u16.to_le_bytes());
        bytes.extend_from_slice(&20u32.to_le_bytes());
        bytes.extend_from_slice(&26u32.to_le_bytes());
        bytes.extend_from_slice(&0u32.to_le_bytes());
        bytes.extend_from_slice(b"1960:01:01 00:00:00\0");
        fs::write(&path, bytes).unwrap();
        assert_eq!(datetime_original(&path), Some(-315619200));
    }

    #[test]
    #[cfg(unix)]
    fn signed_date_manual_round_trip_and_limits_preserve_other_fields() {
        let root = tempfile::tempdir().unwrap();
        timestamp_fixture(root.path(), &["photo.jpg"]);
        let path = root.path().join("photo.jpg");
        let original = fs::read(&path).unwrap();
        for timestamp in [-(1i64 << 40), -315619200, -1, 0, (1i64 << 40) - 1] {
            let entries = set_media_timestamp(
                root.path().to_string_lossy().into(),
                vec!["photo.jpg".into()],
                timestamp,
            )
            .unwrap();
            let decoded = unpack_file_meta(&entries[0].meta).unwrap();
            assert_eq!(decoded.s, Some(timestamp));
            assert_eq!((decoded.w, decoded.h), (Some(64), Some(32)));
            assert_eq!(read_embedded_timestamp(&path), Some(timestamp));
            assert_eq!(fs::read(&path).unwrap(), original);
            assert_eq!(entries[0].favorite, Some(true));
            assert!(metadata_cache_current(
                &path,
                &read_album_meta(root.path()).files["photo.jpg"]
            ));
        }
        let json = fs::read(album_meta_path(root.path())).unwrap();
        for timestamp in [-(1i64 << 40) - 1, 1i64 << 40, i64::MIN, i64::MAX] {
            assert!(set_media_timestamp(
                root.path().to_string_lossy().into(),
                vec!["photo.jpg".into()],
                timestamp
            )
            .is_err());
            assert_eq!(fs::read(album_meta_path(root.path())).unwrap(), json);
            assert_eq!(read_embedded_timestamp(&path), Some((1i64 << 40) - 1));
        }
    }

    #[test]
    #[cfg(unix)]
    fn signed_date_failed_database_write_restores_negative_attribute_and_cache() {
        let root = tempfile::tempdir().unwrap();
        timestamp_fixture(root.path(), &["photo.jpg"]);
        set_media_timestamp(
            root.path().to_string_lossy().into(),
            vec!["photo.jpg".into()],
            -315619200,
        )
        .unwrap();
        let path = root.path().join("photo.jpg");
        let json = fs::read(album_meta_path(root.path())).unwrap();
        let (prepared, failure) = prepare_timestamp_updates(root.path(), vec!["photo.jpg".into()]);
        assert!(commit_timestamp_updates(
            root.path(),
            &prepared,
            -1,
            failure,
            reject_timestamp_database_write
        )
        .is_err());
        assert_eq!(read_embedded_timestamp(&path), Some(-315619200));
        assert_eq!(fs::read(album_meta_path(root.path())).unwrap(), json);
        assert!(metadata_cache_current(
            &path,
            &read_album_meta(root.path()).files["photo.jpg"]
        ));
    }

    #[test]
    fn signed_date_probe_preserves_pre_epoch_creation_time() {
        let log = "creation_time : 1960-01-01T00:00:00Z";
        assert_eq!(
            parse_probe_output(log, true).0.map(i128::from),
            Some(-315619200)
        );
        assert_eq!(parse_probe_output(log, false).0, None);
    }

    #[test]
    fn signed_date_unpack_preserves_legacy_positive_and_new_negative_values() {
        for (value, negative, expected) in [
            (0, false, 0),
            ((1u128 << 40) - 1, false, (1i128 << 40) - 1),
            ((1u128 << 40) - 315619200, true, -315619200),
        ] {
            let packed = (value << 40) | (1u128 << 123) | if negative { 1u128 << 126 } else { 0 };
            assert_eq!(
                unpack_file_meta(&packed.to_string())
                    .unwrap()
                    .s
                    .map(i128::from),
                Some(expected)
            );
        }
    }

    #[test]
    #[cfg(unix)]
    fn signed_date_embedded_attribute_accepts_negative_timestamp() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("photo.jpg");
        fs::write(&path, b"original bytes").unwrap();
        xattr::set(&path, EMBEDDED_TS_XATTR, b"-315619200").unwrap();
        assert_eq!(
            read_embedded_timestamp(&path).map(i128::from),
            Some(-315619200)
        );
    }

    #[test]
    #[cfg(unix)]
    fn signed_date_external_hardlink_change_invalidates_only_metadata_cache() {
        let root = tempfile::tempdir().unwrap();
        timestamp_fixture(root.path(), &["photo.jpg"]);
        let path = root.path().join("photo.jpg");
        let alias = root.path().join("alias.jpg");
        fs::hard_link(&path, &alias).unwrap();
        set_media_timestamp(
            root.path().to_string_lossy().into(),
            vec!["photo.jpg".into()],
            123,
        )
        .unwrap();
        let album = read_album_meta(root.path());
        let entry = &album.files["photo.jpg"];
        assert!(metadata_cache_current(&path, entry));
        let revision = source_revision(&path);
        let media_revision_before = media_revision(&path);
        xattr::set(&alias, EMBEDDED_TS_XATTR, b"456").unwrap();
        assert_eq!(source_revision(&path), revision);
        assert_eq!(media_revision(&path), media_revision_before);
        assert!(!metadata_cache_current(&path, entry));
        assert_eq!(fs::read(&path).unwrap(), fs::read(&alias).unwrap());
    }

    #[test]
    fn probe_parses_dimensions_after_pixel_format_and_utc_tags() {
        let log = "    creation_time   : 2025-04-01T12:34:56.000000Z\n  Stream #0:0: Video: h264, yuv420p(tv, bt709), 1920x1080 [SAR 1:1 DAR 16:9], 30 fps";
        assert_eq!(
            parse_probe_output(log, true),
            (Some(1743510896), Some(1920), Some(1080))
        );
        assert_eq!(
            parse_probe_output(log, false),
            (None, Some(1920), Some(1080))
        );
    }
    #[test]
    fn concurrent_updates_preserve_other_fields_and_produce_valid_database() {
        let dir = tempfile::tempdir().unwrap();
        let mut handles = Vec::new();
        for kind in 0..3 {
            let path = dir.path().to_path_buf();
            handles.push(std::thread::spawn(move || {
                for _ in 0..20 {
                    update_file_meta(&path, "a.jpg", |entry| match kind {
                        0 => entry.favorite = true,
                        1 => entry.thumb_version = Some("thumb-v1".into()),
                        _ => entry.hash = Some("hash-v1".into()),
                    })
                    .unwrap();
                    crate::album_db::read(&album_meta_path(&path)).unwrap();
                }
            }));
        }
        for handle in handles {
            handle.join().unwrap();
        }
        let album = read_album_meta(dir.path());
        let entry = album.files.get("a.jpg").unwrap();
        assert!(entry.favorite);
        assert_eq!(entry.thumb_version.as_deref(), Some("thumb-v1"));
        assert_eq!(entry.hash.as_deref(), Some("hash-v1"));
    }
    #[test]
    fn failed_destination_write_does_not_remove_source_metadata() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("source");
        let target = root.path().join("target");
        fs::create_dir_all(&source).unwrap();
        fs::create_dir_all(&target).unwrap();
        update_file_meta(&source, "a.jpg", |entry| entry.favorite = true).unwrap();
        fs::write(target.join(META_DIR), b"blocked directory").unwrap();
        {
            let guard = lock_metadata_for_write(&[&source, &target]).unwrap();
            assert!(transfer_media_metadata_batch_caller_holds_lock(
                &source,
                &target,
                &[("a.jpg".into(), "a.jpg".into())],
                &guard,
            )
            .is_err());
        }
        assert!(
            read_album_meta(&source)
                .files
                .get("a.jpg")
                .unwrap()
                .favorite
        );
    }
    #[test]
    fn ignore_updates_preserve_file_metadata() {
        let dir = tempfile::tempdir().unwrap();
        update_file_meta(dir.path(), "a.jpg", |entry| entry.favorite = true).unwrap();
        update_album_meta(dir.path(), |album| {
            album
                .duplicates_ignore
                .push(["a.jpg".into(), "b.jpg".into()])
        })
        .unwrap();
        let album = read_album_meta(dir.path());
        assert!(album.files.get("a.jpg").unwrap().favorite);
        assert_eq!(album.duplicates_ignore.len(), 1);
    }
    #[test]
    fn content_invalidation_keeps_favorites_and_ignores_but_discards_computed_data() {
        let dir = tempfile::tempdir().unwrap();
        update_file_meta(dir.path(), "a.jpg", |entry| {
            entry.favorite = true;
            entry.meta = Some("123".into());
            entry.hash = Some("old content".into());
            entry.thumb_version = Some("current settings".into());
            entry.hash_failed = true;
        })
        .unwrap();
        invalidate_media_content(&dir.path().join("a.jpg")).unwrap();
        let album = read_album_meta(dir.path());
        let entry = &album.files["a.jpg"];
        assert!(entry.favorite);
        assert!(entry.meta.is_none());
        assert!(entry.hash.is_none());
        assert_eq!(entry.thumb_version.as_deref(), Some("current settings"));
        assert!(entry.thumb_hash_revision.is_none());
        assert!(!entry.hash_failed);
    }

    #[test]
    fn corrupt_metadata_is_never_overwritten_by_an_update_or_migration() {
        let dir = tempfile::tempdir().unwrap();
        update_file_meta(dir.path(), "a.jpg", |entry| entry.favorite = true).unwrap();
        let _snapshot = read_album_meta(dir.path());
        let path = album_meta_path(dir.path());
        fs::write(&path, b"{broken-json").unwrap();
        fs::write(meta_dir(dir.path()).join("a.jpg.meta"), b"{}").unwrap();
        assert!(read_album_meta_checked(dir.path()).is_err());
        assert!(update_file_meta(dir.path(), "a.jpg", |entry| entry.favorite = false).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"{broken-json");
        assert!(meta_dir(dir.path()).join("a.jpg.meta").exists());
    }
}
