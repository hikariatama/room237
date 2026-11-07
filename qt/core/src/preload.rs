use std::{
    collections::{HashMap, HashSet, VecDeque},
    fs,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, AtomicU8, AtomicUsize, Ordering},
        Arc, Condvar, Mutex,
    },
    thread,
    time::Duration,
};

use once_cell::sync::Lazy;
use serde::Serialize;
use serde_json::json;
use crate::host::{AppHandle, Emitter, NativeRuntime};

use crate::{
    constants::{IMAGE_EXTENSIONS, VIDEO_EXTENSIONS},
    duplicates::compute_hash_for_file,
    metadata::{
        get_file_metadata_cached_with_settings, hash_cache_current, is_meta_failed,
        is_thumb_failed, load_album_file_hash, mark_meta_failed, mark_thumb_failed,
        source_revision, BackgroundMetadataWrites, FileMetaEntry,
    },
    preload_queue::PendingTasks,
    settings::{read_settings, HashConfig},
    thumb::{cached_previews_current, ensure_thumb_with_settings},
    util::{has_extension, set_low_priority_current_thread},
};

const MAX_PENDING_MEDIA: usize = 512;

/// Media of the albums the preloader has been given: a quick count when an album is queued, made exact once its pass
/// has looked at every file. `scanned` files are settled or queued, so progress is `scanned - queued` of `planned`
/// and the totals are known before the passes over the albums finish.
#[derive(Default, Clone, Copy)]
struct AlbumPlan {
    planned: usize,
    scanned: usize,
}

static PLAN: Lazy<Mutex<HashMap<PathBuf, AlbumPlan>>> = Lazy::new(|| Mutex::new(HashMap::new()));

fn quick_media_count(dir: &Path) -> usize {
    fs::read_dir(dir)
        .map(|entries| {
            entries
                .flatten()
                .filter(|entry| {
                    let path = entry.path();
                    entry.file_type().is_ok_and(|kind| kind.is_file())
                        && (has_extension(&path, IMAGE_EXTENSIONS) || has_extension(&path, VIDEO_EXTENSIONS))
                })
                .count()
        })
        .unwrap_or(0)
}

/// Counts the queued albums that have no plan yet.
fn plan_queued() {
    let queued: Vec<PathBuf> = PRELOAD_QUEUE.lock().unwrap().iter().cloned().collect();
    let missing: Vec<PathBuf> = {
        let plan = PLAN.lock().unwrap();
        queued.into_iter().filter(|dir| !plan.contains_key(dir)).collect()
    };
    if missing.is_empty() {
        return;
    }
    let counted: Vec<(PathBuf, usize)> = missing.into_iter().map(|dir| { let n = quick_media_count(&dir); (dir, n) }).collect();
    let mut plan = PLAN.lock().unwrap();
    for (dir, planned) in counted {
        plan.entry(dir).or_insert(AlbumPlan { planned, scanned: 0 });
    }
}

fn plan_scanned(dir: &Path, files: usize) {
    if let Some(entry) = PLAN.lock().unwrap().get_mut(dir) {
        entry.scanned += files;
    }
}

/// The pass over an album is over: what it saw is the album's size.
fn plan_settle(dir: &Path) {
    if let Some(entry) = PLAN.lock().unwrap().get_mut(dir) {
        entry.planned = entry.scanned;
    }
}

/// (planned, scanned) over all albums, and whether a pass has files left to look at.
fn plan_totals() -> (usize, usize, bool) {
    let plan = PLAN.lock().unwrap();
    let mut planned = 0;
    let mut scanned = 0;
    for entry in plan.values() {
        planned += entry.planned.max(entry.scanned);
        scanned += entry.scanned;
    }
    (planned, scanned, scanned < planned)
}

#[derive(Clone, Copy, Serialize)]
#[serde(rename_all = "snake_case")]
enum PreloadStage {
    Conversion,
    Thumbnails,
    Metadata,
    Idle,
}

impl PreloadStage {
    fn as_u8(self) -> u8 {
        match self {
            PreloadStage::Conversion => 0,
            PreloadStage::Thumbnails => 1,
            PreloadStage::Metadata => 2,
            PreloadStage::Idle => 3,
        }
    }

    fn from_u8(value: u8) -> Self {
        match value {
            0 => PreloadStage::Conversion,
            1 => PreloadStage::Thumbnails,
            2 => PreloadStage::Metadata,
            _ => PreloadStage::Idle,
        }
    }
}

#[derive(Clone, Copy, Default, Serialize)]
struct StageProgress {
    completed: usize,
    total: usize,
}

pub(crate) static PRELOADED: Lazy<Mutex<HashMap<PathBuf, String>>> =
    Lazy::new(|| Mutex::new(HashMap::new()));
pub(crate) static PRELOAD_QUEUE: Lazy<Mutex<VecDeque<PathBuf>>> =
    Lazy::new(|| Mutex::new(VecDeque::new()));
pub(crate) static PRELOADER_RUNNING: Lazy<AtomicBool> = Lazy::new(|| AtomicBool::new(false));
pub(crate) static CURRENT_PRELOAD_CANCEL: Lazy<Mutex<Option<(PathBuf, Arc<AtomicBool>)>>> =
    Lazy::new(|| Mutex::new(None));
static PRELOAD_DONE_THUMBS: Lazy<AtomicUsize> = Lazy::new(|| AtomicUsize::new(0));
static PRELOAD_DONE_META: Lazy<AtomicUsize> = Lazy::new(|| AtomicUsize::new(0));
static PRELOAD_STAGE: Lazy<AtomicU8> = Lazy::new(|| AtomicU8::new(PreloadStage::Idle.as_u8()));
static PRELOAD_CONVERSIONS_TOTAL: Lazy<AtomicUsize> = Lazy::new(|| AtomicUsize::new(0));
static PRELOAD_CONVERSIONS_DONE: Lazy<AtomicUsize> = Lazy::new(|| AtomicUsize::new(0));

static THUMB_HASH_ALBUMS: Lazy<Mutex<HashSet<PathBuf>>> = Lazy::new(|| Mutex::new(HashSet::new()));
static THUMB_HASH_DONE_ONCE: Lazy<AtomicBool> = Lazy::new(|| AtomicBool::new(false));
static PRELOAD_APP: Lazy<Mutex<Option<AppHandle<NativeRuntime>>>> = Lazy::new(|| Mutex::new(None));

static LAST_PROGRESS_EMIT: Lazy<Mutex<Option<std::time::Instant>>> = Lazy::new(|| Mutex::new(None));
static ALLOW_OPEN: Lazy<AtomicBool> = Lazy::new(|| AtomicBool::new(false));
static THUMB_WORKER_COUNTER: Lazy<AtomicUsize> = Lazy::new(|| AtomicUsize::new(0));
static META_WORKER_COUNTER: Lazy<AtomicUsize> = Lazy::new(|| AtomicUsize::new(0));
static HASH_WORKER_COUNTER: Lazy<AtomicUsize> = Lazy::new(|| AtomicUsize::new(0));
static ACTIVE_ROOT: Lazy<Mutex<Option<PathBuf>>> = Lazy::new(|| Mutex::new(None));

#[derive(Clone, Copy, Debug)]
pub(crate) enum TaskPriority {
    High,
    Low,
}

impl Default for TaskPriority {
    fn default() -> Self {
        TaskPriority::Low
    }
}

struct WorkQueueState<T> {
    queue: PendingTasks<T>,
    in_progress: HashSet<PathBuf>,
    started_workers: usize,
}

impl<T> Default for WorkQueueState<T> {
    fn default() -> Self {
        Self {
            queue: PendingTasks::default(),
            in_progress: HashSet::new(),
            started_workers: 0,
        }
    }
}

struct WorkQueue<T> {
    state: Mutex<WorkQueueState<T>>,
    cv: Condvar,
}

impl<T: Send + 'static> WorkQueue<T> {
    fn enqueue(&self, path: PathBuf, data: T, priority: TaskPriority) -> bool {
        let mut state = self.state.lock().unwrap();
        let capacity = if matches!(priority, TaskPriority::High) {
            MAX_PENDING_MEDIA + crate::catalog::PAGE_SIZE
        } else {
            MAX_PENDING_MEDIA
        };
        while state.queue.len() >= capacity && !state.queue.contains(&path) {
            if matches!(priority, TaskPriority::High) {
                return false;
            }
            state = self
                .cv
                .wait_timeout(state, Duration::from_millis(100))
                .unwrap()
                .0;
        }
        if state.in_progress.contains(&path) {
            return false;
        }
        if !state.queue.enqueue(path, data, priority) {
            return false;
        }
        self.cv.notify_all();
        true
    }

    #[cfg(test)]
    fn next_task_blocking(&self) -> (PathBuf, T) {
        self.next_task_blocking_with_limit(|| usize::MAX)
    }

    fn next_task_blocking_with_limit(&self, limit: impl Fn() -> usize) -> (PathBuf, T) {
        let mut guard = self.state.lock().unwrap();
        loop {
            if !guard.queue.is_empty() && guard.in_progress.len() < limit().max(1) {
                let (path, data) = guard.queue.pop_front().unwrap();
                guard.in_progress.insert(path.clone());
                self.cv.notify_all();
                return (path, data);
            }
            guard = self.cv.wait(guard).unwrap();
        }
    }

    fn mark_done(&self, path: &Path) {
        self.mark_done_with_retry(path, None);
    }

    fn mark_done_with_retry(&self, path: &Path, retry: Option<T>) {
        let mut state = self.state.lock().unwrap();
        state.in_progress.remove(path);
        if let Some(data) = retry {
            state
                .queue
                .enqueue(path.to_path_buf(), data, TaskPriority::Low);
        }
        let idle = state.queue.is_empty() && state.in_progress.is_empty();
        drop(state);
        self.cv.notify_all();
        if idle {
            try_emit_end();
        }
    }

    fn has_work(&self) -> bool {
        let state = self.state.lock().unwrap();
        !state.queue.is_empty() || !state.in_progress.is_empty()
    }

    fn outstanding(&self) -> usize {
        let state = self.state.lock().unwrap();
        state.queue.len() + state.in_progress.len()
    }

    fn trim_prefix(&self, prefix: &Path) -> usize {
        let mut state = self.state.lock().unwrap();
        let removed = state.queue.trim_prefix(prefix);
        if removed > 0 {
            self.cv.notify_all();
        }
        removed
    }

    fn clear(&self) {
        let mut state = self.state.lock().unwrap();
        state.queue.clear();
        self.cv.notify_all();
    }

    #[cfg(test)]
    fn is_tracked(&self, path: &Path) -> bool {
        let state = self.state.lock().unwrap();
        state.queue.contains(path) || state.in_progress.contains(path)
    }
}

#[derive(Clone)]
struct ThumbTask {
    thumb_dir: PathBuf,
}

static THUMB_WORK: Lazy<WorkQueue<ThumbTask>> = Lazy::new(|| WorkQueue {
    state: Mutex::new(WorkQueueState::default()),
    cv: Condvar::new(),
});

static META_WORK: Lazy<WorkQueue<()>> = Lazy::new(|| WorkQueue {
    state: Mutex::new(WorkQueueState::default()),
    cv: Condvar::new(),
});

#[derive(Default)]
struct HashQueueState {
    queue: PendingTasks<()>,
    in_progress: HashSet<PathBuf>,
    in_progress_started: HashMap<PathBuf, std::time::Instant>,
    in_progress_timeouts: HashMap<PathBuf, Duration>,
    // Timed-out work may still be decoding on a native worker. Keep its physical
    // slot until that exact task returns, independently of logical progress.
    running_tasks: HashSet<(PathBuf, std::time::Instant)>,
    done: usize,
    started_workers: usize,
}

struct HashQueue {
    state: Mutex<HashQueueState>,
    cv: Condvar,
}

static HASH_QUEUE: Lazy<HashQueue> = Lazy::new(|| HashQueue {
    state: Mutex::new(HashQueueState::default()),
    cv: Condvar::new(),
});

#[derive(Clone, Serialize)]
struct PreloadProgressPayload {
    stage: PreloadStage,
    stage_progress: StageProgress,
    overall_completed: usize,
    overall_total: usize,
    progress: usize,
    conversions: StageProgress,
    thumbnails: StageProgress,
    metadata: StageProgress,
    active_actions: usize,
}

fn combined_progress_totals() -> (usize, usize) {
    let conv_total = PRELOAD_CONVERSIONS_TOTAL.load(Ordering::SeqCst);
    let conv_done = PRELOAD_CONVERSIONS_DONE.load(Ordering::SeqCst);

    let thumb_done = PRELOAD_DONE_THUMBS.load(Ordering::SeqCst);
    let meta_done = PRELOAD_DONE_META.load(Ordering::SeqCst);
    let thumb_outstanding = THUMB_WORK.outstanding();
    let meta_outstanding = META_WORK.outstanding();

    let completed = conv_done + thumb_done + meta_done;
    let total = conv_total + thumb_done + thumb_outstanding + meta_done + meta_outstanding;
    (completed, total)
}

fn active_actions() -> usize {
    let conv_total = PRELOAD_CONVERSIONS_TOTAL.load(Ordering::SeqCst);
    let conv_done = PRELOAD_CONVERSIONS_DONE.load(Ordering::SeqCst);
    let conv_outstanding = conv_total.saturating_sub(conv_done);
    conv_outstanding + THUMB_WORK.outstanding() + META_WORK.outstanding()
}

fn stage_counts(stage: PreloadStage) -> StageProgress {
    match stage {
        PreloadStage::Conversion => {
            let total = PRELOAD_CONVERSIONS_TOTAL.load(Ordering::SeqCst);
            let completed = PRELOAD_CONVERSIONS_DONE.load(Ordering::SeqCst);
            StageProgress { completed, total }
        }
        PreloadStage::Thumbnails => planned_progress(PRELOAD_DONE_THUMBS.load(Ordering::SeqCst), THUMB_WORK.outstanding()),
        PreloadStage::Metadata => planned_progress(PRELOAD_DONE_META.load(Ordering::SeqCst), META_WORK.outstanding()),
        PreloadStage::Idle => {
            let (completed, total) = combined_progress_totals();
            StageProgress { completed, total }
        }
    }
}

fn planned_progress(done: usize, outstanding: usize) -> StageProgress {
    let (planned, scanned, _) = plan_totals();
    if planned == 0 {
        // Work outside a preload pass, such as a preview asked for by the gallery.
        return StageProgress { completed: done, total: done + outstanding };
    }
    let completed = scanned.saturating_sub(outstanding).min(planned);
    StageProgress { completed, total: planned.max(completed + outstanding) }
}

fn build_progress_payload(stage: PreloadStage) -> PreloadProgressPayload {
    let (overall_completed, overall_total) = combined_progress_totals();
    let progress = if overall_total == 0 {
        0
    } else {
        ((overall_completed * 100) / overall_total).min(100)
    };

    PreloadProgressPayload {
        stage,
        stage_progress: stage_counts(stage),
        overall_completed,
        overall_total,
        progress,
        conversions: stage_counts(PreloadStage::Conversion),
        thumbnails: stage_counts(PreloadStage::Thumbnails),
        metadata: stage_counts(PreloadStage::Metadata),
        active_actions: active_actions(),
    }
}

fn current_stage() -> PreloadStage {
    let conv_total = PRELOAD_CONVERSIONS_TOTAL.load(Ordering::SeqCst);
    let conv_done = PRELOAD_CONVERSIONS_DONE.load(Ordering::SeqCst);
    if conv_total.saturating_sub(conv_done) > 0 {
        PreloadStage::Conversion
    } else if THUMB_WORK.outstanding() > 0 || (PRELOADER_RUNNING.load(Ordering::SeqCst) && plan_totals().2) {
        PreloadStage::Thumbnails
    } else if META_WORK.outstanding() > 0 {
        PreloadStage::Metadata
    } else {
        PreloadStage::Idle
    }
}

fn emit_progress(stage: Option<PreloadStage>) {
    let prev_stage = PreloadStage::from_u8(PRELOAD_STAGE.load(Ordering::SeqCst));
    let stage = match stage {
        Some(s) => {
            PRELOAD_STAGE.store(s.as_u8(), Ordering::SeqCst);
            s
        }
        None => {
            let current = current_stage();
            PRELOAD_STAGE.store(current.as_u8(), Ordering::SeqCst);
            current
        }
    };
    let stage_changed = stage.as_u8() != prev_stage.as_u8();

    let settings = read_settings();
    let min_interval = std::time::Duration::from_millis(settings.preload.progress_emit_ms);
    let now = std::time::Instant::now();
    let mut last_emit = LAST_PROGRESS_EMIT.lock().unwrap();
    let should_emit = stage_changed
        || last_emit
            .map(|t| now.duration_since(t) >= min_interval)
            .unwrap_or(true);
    if !should_emit {
        return;
    }
    *last_emit = Some(now);

    if let Some(app) = PRELOAD_APP.lock().unwrap().as_ref() {
        let payload = build_progress_payload(stage);
        let _ = app.emit("preload-progress", payload);
    }
}

pub(crate) fn artifacts_missing(dir: &Path) -> bool {
    let thumb_dir = dir.join(".room237-thumb");
    let meta_dir = dir.join(".room237-metadata");
    !thumb_dir.exists() || !meta_dir.exists()
}

fn reset_preload_state() {
    if let Some((_, cancel)) = CURRENT_PRELOAD_CANCEL.lock().unwrap().as_ref() {
        cancel.store(true, Ordering::Relaxed);
    }
    {
        let mut q = PRELOAD_QUEUE.lock().unwrap();
        q.clear();
    }
    PRELOADED.lock().unwrap().clear();
    THUMB_WORK.clear();
    META_WORK.clear();
    {
        let mut hashes = HASH_QUEUE.state.lock().unwrap();
        hashes.queue.clear();
        hashes.done = 0;
    }
    PRELOAD_DONE_THUMBS.store(0, Ordering::SeqCst);
    PRELOAD_DONE_META.store(0, Ordering::SeqCst);
    PRELOAD_CONVERSIONS_TOTAL.store(0, Ordering::SeqCst);
    PRELOAD_CONVERSIONS_DONE.store(0, Ordering::SeqCst);
    PLAN.lock().unwrap().clear();
    *LAST_PROGRESS_EMIT.lock().unwrap() = None;
    THUMB_HASH_ALBUMS.lock().unwrap().clear();
    PRELOAD_STAGE.store(PreloadStage::Idle.as_u8(), Ordering::SeqCst);
    emit_progress(Some(PreloadStage::Idle));
}

pub(crate) fn set_active_root(root: &Path) -> bool {
    let mut active = ACTIVE_ROOT.lock().unwrap();
    if active.as_ref().map(|r| r == root).unwrap_or(false) {
        return false;
    }
    *active = Some(root.to_path_buf());
    reset_preload_state();
    true
}

fn emit_hash_event(kind: &str, completed: usize, total: usize) {
    if let Some(app) = PRELOAD_APP.lock().unwrap().as_ref() {
        let _ = app.emit(
            kind,
            json!({
                "completed": completed,
                "total": total
            }),
        );
    }
}

fn emit_processed_media(path: &Path, kind: &str) {
    if let Some(app) = PRELOAD_APP.lock().unwrap().as_ref() {
        let _ = app.emit(
            "media-processed",
            json!({ "path": path.to_string_lossy(), "kind": kind }),
        );
    }
}

fn hash_task_timeout(settings: &crate::settings::AdvancedSettings) -> Duration {
    // Allow for a thumbnail lock holder, this task's FFmpeg attempt, and image
    // decoding. A configured 60s FFmpeg attempt must not be expired after 30s.
    Duration::from_secs(
        settings
            .ffmpeg
            .timeout_secs
            .saturating_mul(3)
            .saturating_add(35)
            .max(30),
    )
}

impl HashQueue {
    fn enqueue_many(&self, paths: &[PathBuf], priority: TaskPriority) -> (bool, usize, usize) {
        let mut state = self.state.lock().unwrap();
        let mut added = false;
        if state.queue.is_empty() && state.in_progress.is_empty() && state.done > 0 {
            state.done = 0;
        }
        for path in paths {
            while state.queue.len() >= MAX_PENDING_MEDIA && !state.queue.contains(path) {
                if matches!(priority, TaskPriority::High) {
                    break;
                }
                state = self
                    .cv
                    .wait_timeout(state, Duration::from_millis(100))
                    .unwrap()
                    .0;
            }
            if state.queue.len() >= MAX_PENDING_MEDIA && !state.queue.contains(path) {
                continue;
            }

            if state.in_progress.contains(path) {
                continue;
            }
            added |= state.queue.enqueue(path.clone(), (), priority);
        }
        let total = state.done + state.queue.len() + state.in_progress.len();
        let done = state.done.min(total);
        if added {
            self.cv.notify_all();
        }
        (added, done, total)
    }

    fn next_task_blocking(
        &self,
    ) -> (
        PathBuf,
        std::time::Instant,
        crate::settings::AdvancedSettings,
    ) {
        self.next_task_blocking_with_settings(read_settings)
    }

    fn next_task_blocking_with_settings(
        &self,
        get_settings: impl Fn() -> crate::settings::AdvancedSettings,
    ) -> (
        PathBuf,
        std::time::Instant,
        crate::settings::AdvancedSettings,
    ) {
        let mut guard = self.state.lock().unwrap();
        loop {
            let settings = get_settings();
            if !guard.queue.is_empty()
                && guard.running_tasks.len() < settings.preload.hash_workers.max(1) as usize
            {
                let (path, ()) = guard.queue.pop_front().unwrap();
                guard.in_progress.insert(path.clone());
                self.cv.notify_all();
                let started = std::time::Instant::now();
                guard.running_tasks.insert((path.clone(), started));
                guard.in_progress_started.insert(path.clone(), started);
                guard
                    .in_progress_timeouts
                    .insert(path.clone(), hash_task_timeout(&settings));
                return (path, started, settings);
            }
            guard = self.cv.wait(guard).unwrap();
        }
    }

    fn mark_done(&self, path: &Path, started: std::time::Instant) -> (usize, usize) {
        let mut state = self.state.lock().unwrap();
        state.running_tasks.remove(&(path.to_path_buf(), started));
        self.cv.notify_all();
        if state.in_progress_started.get(path) != Some(&started) {
            let total = state.done + state.queue.len() + state.in_progress.len();
            return (state.done.min(total), total);
        }
        state.in_progress.remove(path);
        state.in_progress_started.remove(path);
        state.in_progress_timeouts.remove(path);
        state.done = state.done.saturating_add(1);
        let queue_empty = state.queue.is_empty() && state.in_progress.is_empty();
        let total = state.done + state.queue.len() + state.in_progress.len();
        let done = state.done.min(total);
        self.cv.notify_all();
        drop(state);
        if queue_empty {
            THUMB_HASH_DONE_ONCE.store(true, Ordering::SeqCst);
            try_emit_end();
        }
        (done, total)
    }

    fn has_work(&self) -> bool {
        let state = self.state.lock().unwrap();
        !state.queue.is_empty() || !state.in_progress.is_empty()
    }

    fn trim_prefix(&self, prefix: &Path) -> Option<(usize, usize)> {
        let mut state = self.state.lock().unwrap();
        let removed = state.queue.trim_prefix(prefix);
        if removed > 0 {
            if state.queue.is_empty() && state.in_progress.is_empty() {
                state.done = 0;
            }
            let total = state.done + state.queue.len() + state.in_progress.len();
            let done = state.done.min(total);
            self.cv.notify_all();
            return Some((done, total));
        }
        None
    }

    fn totals(&self) -> (usize, usize) {
        let state = self.state.lock().unwrap();
        let total = state.done + state.queue.len() + state.in_progress.len();
        (state.done.min(total), total)
    }

    #[cfg(test)]
    fn expire_in_progress(&self) -> Option<(usize, usize, Vec<PathBuf>)> {
        let mut state = self.state.lock().unwrap();
        if state.in_progress.is_empty() {
            return None;
        }
        let now = std::time::Instant::now();
        let mut stuck: Vec<PathBuf> = Vec::new();
        for p in state.in_progress.iter() {
            if let Some(started) = state.in_progress_started.get(p) {
                if now.duration_since(*started)
                    >= state
                        .in_progress_timeouts
                        .get(p)
                        .copied()
                        .unwrap_or(Duration::from_secs(30))
                {
                    stuck.push(p.clone());
                }
            }
        }
        if stuck.is_empty() {
            return None;
        }
        for p in &stuck {
            state.in_progress.remove(p);
            state.in_progress_started.remove(p);
            state.in_progress_timeouts.remove(p);
            state.done = state.done.saturating_add(1);
        }
        let total = state.done + state.queue.len() + state.in_progress.len();
        let done = state.done.min(total);
        self.cv.notify_all();
        drop(state);
        Some((done, total, stuck))
    }
}

fn hash_entry_ready(path: &Path, entry: Option<&FileMetaEntry>, cfg: &HashConfig) -> bool {
    entry.is_some_and(|entry| {
        entry.hash_version.as_deref() == Some(&cfg.hash_version)
            && entry.hash_bits.unwrap_or(cfg.bits) == cfg.bits
            && hash_cache_current(path, entry)
            && (entry.hash_failed || entry.hash.is_some())
    })
}

fn album_entry_needs_hash(path: &Path, entry: Option<&FileMetaEntry>, cfg: &HashConfig) -> bool {
    !hash_entry_ready(path, entry, cfg)
}

fn enqueue_hashes(
    paths: &[PathBuf],
    cfg: &HashConfig,
    priority: TaskPriority,
    emit_event: bool,
) -> bool {
    enqueue_hashes_internal(paths, Some(cfg), priority, emit_event, false)
}

fn enqueue_hashes_internal(
    paths: &[PathBuf],
    cfg: Option<&HashConfig>,
    priority: TaskPriority,
    emit_event: bool,
    prescreened: bool,
) -> bool {
    if paths.is_empty() {
        return false;
    }

    let start = std::time::Instant::now();
    let mut needed: Vec<PathBuf> = Vec::new();

    if prescreened {
        needed.extend_from_slice(paths);
    } else {
        for p in paths {
            let entry = crate::metadata::read_file_entry(p).ok().flatten();
            if let Some(cfg) = cfg {
                let ext = p
                    .extension()
                    .and_then(|s| s.to_str())
                    .unwrap_or("")
                    .to_ascii_lowercase();
                if !IMAGE_EXTENSIONS.contains(&ext.as_str()) {
                    continue;
                }
                if !album_entry_needs_hash(p, entry.as_ref(), cfg) {
                    continue;
                }
            }

            needed.push(p.clone());
        }
    }

    if needed.is_empty() {
        log::info!(
            "hash-queue: nothing to enqueue (filtered) input={} prescreened={} priority={:?}",
            paths.len(),
            prescreened,
            priority
        );
        return false;
    }

    let (added, done, total) = HASH_QUEUE.enqueue_many(&needed, priority);
    if added {
        log::info!(
            "hash-queue: enqueued {} paths priority={:?} done={} total={} elapsed={:?}",
            needed.len(),
            priority,
            done,
            total,
            start.elapsed()
        );
        if emit_event {
            emit_hash_event("hash-progress", done, total);
        }
        start_thumb_hash_worker();
    }
    added
}

pub(crate) fn enqueue_thumb_task(
    path: PathBuf,
    thumb_dir: PathBuf,
    totals_accounted: bool,
    priority: TaskPriority,
) -> bool {
    if THUMB_WORK.enqueue(path.clone(), ThumbTask { thumb_dir }, priority) {
        let _ = totals_accounted;
        emit_progress(None);
        start_thumb_worker();
        return true;
    }
    false
}

pub(crate) fn enqueue_meta_task(
    path: PathBuf,
    totals_accounted: bool,
    priority: TaskPriority,
) -> bool {
    if META_WORK.enqueue(path.clone(), (), priority) {
        let _ = totals_accounted;
        emit_progress(None);
        start_meta_worker();
        return true;
    }
    false
}

fn try_emit_end() {
    let thumb_running = THUMB_WORK.has_work();
    let meta_running = META_WORK.has_work();
    let hash_running = HASH_QUEUE.has_work();
    let thumb_pending = THUMB_WORK.outstanding() > 0;
    let meta_pending = META_WORK.outstanding() > 0;
    if !PRELOADER_RUNNING.load(Ordering::SeqCst)
        && !thumb_running
        && !meta_running
        && !thumb_pending
        && !meta_pending
    {
        PRELOAD_DONE_THUMBS.store(0, Ordering::SeqCst);
        PRELOAD_DONE_META.store(0, Ordering::SeqCst);
        PLAN.lock().unwrap().clear();
        PRELOAD_STAGE.store(PreloadStage::Idle.as_u8(), Ordering::SeqCst);
        emit_progress(Some(PreloadStage::Idle));
        if !hash_running {
            schedule_thumb_hash_scan();
        }
    }
}

fn queue_album_for_hashing(dir: &Path) {
    if let Some(root) = ACTIVE_ROOT.lock().unwrap().clone() {
        if !dir.starts_with(&root) {
            return;
        }
    }
    THUMB_HASH_ALBUMS.lock().unwrap().insert(dir.to_path_buf());
}

fn schedule_thumb_hash_scan() {
    static SCANNING: AtomicBool = AtomicBool::new(false);
    if SCANNING.swap(true, Ordering::SeqCst) {
        return;
    }
    thread::spawn(|| {
        set_low_priority_current_thread();
        loop {
            let album = THUMB_HASH_ALBUMS.lock().unwrap().iter().next().cloned();
            let Some(album) = album else {
                break;
            };
            THUMB_HASH_ALBUMS.lock().unwrap().remove(&album);
            let cfg = read_settings().hash_config();
            let mut batch = Vec::with_capacity(32);
            let hidden_heic = crate::metadata::hidden_heic_names(&album).unwrap_or_default();
            if let Ok(entries) = fs::read_dir(&album) {
                for file in entries.flatten() {
                    if !ACTIVE_ROOT
                        .lock()
                        .unwrap()
                        .as_ref()
                        .is_some_and(|root| album.starts_with(root))
                    {
                        break;
                    }
                    let path = file.path();
                    if file.file_type().is_ok_and(|t| t.is_file())
                        && has_extension(&path, IMAGE_EXTENSIONS)
                        && !hidden_heic.contains(&*file.file_name().to_string_lossy())
                    {
                        batch.push(path);
                        if batch.len() == 32 {
                            enqueue_hashes(&batch, &cfg, TaskPriority::Low, true);
                            batch.clear();
                        }
                    }
                }
            }
            enqueue_hashes(&batch, &cfg, TaskPriority::Low, true);
        }
        SCANNING.store(false, Ordering::SeqCst);
    });
}

pub(crate) fn start_preloader_worker(app: AppHandle<NativeRuntime>) {
    *PRELOAD_APP.lock().unwrap() = Some(app.clone());
    if PRELOADER_RUNNING.swap(true, Ordering::SeqCst) {
        return;
    }
    log::info!("preloader thread spawn");
    *LAST_PROGRESS_EMIT.lock().unwrap() = None;
    PRELOAD_DONE_THUMBS.store(0, Ordering::SeqCst);
    PRELOAD_DONE_META.store(0, Ordering::SeqCst);
    let initial_stage = current_stage();
    PRELOAD_STAGE.store(initial_stage.as_u8(), Ordering::SeqCst);
    emit_progress(Some(initial_stage));

    thread::spawn(move || {
        set_low_priority_current_thread();
        loop {
            plan_queued();
            let dir_opt = {
                let mut q = PRELOAD_QUEUE.lock().unwrap();
                let next = q.pop_front();
                if next.is_none() {
                    PRELOADER_RUNNING.store(false, Ordering::SeqCst);
                }
                next
            };

            match dir_opt {
                Some(dir) => {
                    let settings_version = read_settings().processing_version();
                    let cancel = {
                        let active = ACTIVE_ROOT.lock().unwrap();
                        if active.as_ref().is_some_and(|root| !dir.starts_with(root))
                            || PRELOADED.lock().unwrap().get(&dir) == Some(&settings_version)
                        {
                            PLAN.lock().unwrap().remove(&dir);
                            continue;
                        }
                        let cancel = Arc::new(AtomicBool::new(false));
                        *CURRENT_PRELOAD_CANCEL.lock().unwrap() = Some((dir.clone(), cancel.clone()));
                        cancel
                    };
                    queue_album_for_hashing(&dir);
                    log::info!("preload begin {}", dir.display());

                    let result = preload_dir(&dir, cancel.clone(), true);
                    plan_settle(&dir);
                    *CURRENT_PRELOAD_CANCEL.lock().unwrap() = None;

                    // Checked under the lock that drop_preload_for_path takes after cancelling.
                    let mut preloaded = PRELOADED.lock().unwrap();
                    if cancel.load(Ordering::Relaxed) {
                        log::info!("preload cancelled for {}", dir.display());
                        continue;
                    }
                    if result.is_ok() {
                        preloaded.insert(dir.clone(), settings_version);
                    } else if let Err(error) = result {
                        log::error!("Failed to preload {}: {error}", dir.display());
                    }
                }
                None => {
                    log::info!("preloader idle");
                    try_emit_end();
                    break;
                }
            }
        }
    });
}

pub fn enqueue_preload(dir: &Path) {
    if !crate::util::qa_path_allowed(dir) {
        return;
    }
    if let Some(root) = ACTIVE_ROOT.lock().unwrap().clone() {
        if !dir.starts_with(&root) {
            return;
        }
    }
    let missing_artifacts = artifacts_missing(dir);
    let settings_version = read_settings().processing_version();
    {
        let mut preloaded = PRELOADED.lock().unwrap();
        if preloaded.get(dir) == Some(&settings_version) && !missing_artifacts {
            return;
        }
        preloaded.remove(dir);
    }
    {
        let mut q = PRELOAD_QUEUE.lock().unwrap();
        if q.iter().any(|d| d == dir) {
            return;
        }
        q.push_back(dir.to_path_buf());
        log::info!("queued {}", dir.display());
    }

    if PRELOADER_RUNNING.load(Ordering::SeqCst) {
        emit_progress(None);
    }
}

pub fn preload_dir(
    dir: &Path,
    cancel: Arc<AtomicBool>,
    totals_accounted: bool,
) -> Result<(), String> {
    let _ = totals_accounted;
    if !dir.is_dir() {
        log::warn!("skip preload for missing album {}", dir.display());
        return Ok(());
    }
    crate::media_delete::recover_album(dir)?;
    if let Some(root) = ACTIVE_ROOT.lock().unwrap().clone() {
        if !dir.starts_with(&root) {
            return Ok(());
        }
    }
    let thumb_dir = dir.join(".room237-thumb");
    // One level only: an album deleted from outside must stay deleted.
    crate::util::create_derived_dir(&thumb_dir).map_err(|e| e.to_string())?;
    crate::thumb::remove_legacy_preview_locks(&thumb_dir);

    crate::metadata::prepare_database(dir)?;
    let mut metadata =
        crate::album_db::FileReader::new(&dir.join(".room237-metadata/album.sqlite"))?;
    let settings = read_settings();
    let thumb_version = settings.thumb_version();
    let hidden_heic = crate::metadata::hidden_heic_names(dir)?;
    // Files are reported to the plan after their tasks are queued, so progress never runs ahead of the queues.
    let mut unreported = 0;
    for file in fs::read_dir(dir).map_err(|e| e.to_string())? {
        if cancel.load(Ordering::Relaxed) {
            plan_scanned(dir, unreported);
            return Ok(());
        }
        let file = file.map_err(|e| e.to_string())?;
        if !file.file_type().map_err(|e| e.to_string())?.is_file() {
            continue;
        }
        let path = file.path();
        if has_extension(&path, &["heic"])
            && hidden_heic.contains(&*file.file_name().to_string_lossy())
        {
            continue;
        }
        if !has_extension(&path, IMAGE_EXTENSIONS) && !has_extension(&path, VIDEO_EXTENSIONS) {
            continue;
        }
        let name = path
            .file_name()
            .and_then(|n| n.to_str())
            .ok_or("Invalid media name")?;
        let cached = metadata.get(name)?;
        if !cached
            .as_ref()
            .is_some_and(|entry| cached_previews_current(&path, &thumb_dir, entry, &thumb_version))
        {
            enqueue_thumb_task(
                path.clone(),
                thumb_dir.clone(),
                totals_accounted,
                TaskPriority::Low,
            );
        }
        if !cached.as_ref().is_some_and(|entry| {
            crate::metadata::metadata_cache_current_with_settings(&path, entry, &settings)
        }) {
            enqueue_meta_task(path, totals_accounted, TaskPriority::Low);
        }
        unreported += 1;
        if unreported == 64 {
            plan_scanned(dir, unreported);
            unreported = 0;
            emit_progress(None);
        }
    }
    plan_scanned(dir, unreported);
    Ok(())
}

pub fn drop_preload_for_path(prefix: &Path) {
    // An in-flight pass must not mark the album done: its queued work is trimmed below.
    if let Some((dir, cancel)) = CURRENT_PRELOAD_CANCEL.lock().unwrap().as_ref() {
        if dir.starts_with(prefix) {
            cancel.store(true, Ordering::Relaxed);
        }
    }
    {
        let mut q = PRELOAD_QUEUE.lock().unwrap();
        q.retain(|p| !p.starts_with(prefix));
    }
    {
        let mut preloaded = PRELOADED.lock().unwrap();
        preloaded.retain(|p, _| !p.starts_with(prefix));
    }
    PLAN.lock().unwrap().retain(|p, _| !p.starts_with(prefix));
    {
        let removed = THUMB_WORK.trim_prefix(prefix);
        if removed > 0 {
            emit_progress(None);
        }
    }
    {
        let removed = META_WORK.trim_prefix(prefix);
        if removed > 0 {
            emit_progress(None);
        }
    }
    {
        if let Some((done, total)) = HASH_QUEUE.trim_prefix(prefix) {
            emit_hash_event("hash-progress", done, total);
        }
    }
    {
        let mut ha = THUMB_HASH_ALBUMS.lock().unwrap();
        ha.retain(|p| !p.starts_with(prefix));
    }
}

fn start_thumb_worker() {
    let settings = read_settings();
    let desired = settings.preload.thumb_workers.max(1) as usize;
    loop {
        let worker_id = {
            let mut state = THUMB_WORK.state.lock().unwrap();
            if state.started_workers >= desired {
                return;
            }
            state.started_workers += 1;
            THUMB_WORKER_COUNTER.fetch_add(1, Ordering::SeqCst) + 1
        };
        thread::spawn(move || {
            set_low_priority_current_thread();
            let _background_writes = BackgroundMetadataWrites::enter();
            log::info!("thumb worker #{worker_id} start");
            loop {
                let (p, task) = THUMB_WORK.next_task_blocking_with_limit(|| {
                    read_settings().preload.thumb_workers as usize
                });
                let settings_clone = read_settings();
                if let Some(root) = ACTIVE_ROOT.lock().unwrap().clone() {
                    if !p.starts_with(&root) {
                        THUMB_WORK.mark_done(&p);
                        emit_progress(None);
                        continue;
                    }
                }
                if is_thumb_failed(p.as_path()) {
                    THUMB_WORK.mark_done(&p);
                    emit_progress(None);
                    continue;
                }
                log::debug!("thumb worker #{worker_id} processing {}", p.display());
                let source = source_revision(&p);
                match ensure_thumb_with_settings(p.as_path(), &task.thumb_dir, &settings_clone) {
                    Ok(_) => {
                        PRELOAD_DONE_THUMBS.fetch_add(1, Ordering::SeqCst);
                        emit_processed_media(&p, "thumbnail");
                    }
                    Err(e) => {
                        log::error!("Failed to generate thumbnail {}: {}", p.display(), e);
                        if let Some(source) = source {
                            let _ = mark_thumb_failed(
                                p.as_path(),
                                &source,
                                &settings_clone.processing_version(),
                            );
                        }
                    }
                }
                let retry =
                    settings_clone.processing_version() != read_settings().processing_version();
                THUMB_WORK.mark_done_with_retry(&p, retry.then_some(task));
                emit_progress(Some(PreloadStage::Thumbnails));
            }
        });
    }
}

fn start_meta_worker() {
    let settings = read_settings();
    let desired = settings.preload.meta_workers.max(1) as usize;
    loop {
        let worker_id = {
            let mut state = META_WORK.state.lock().unwrap();
            if state.started_workers >= desired {
                return;
            }
            state.started_workers += 1;
            META_WORKER_COUNTER.fetch_add(1, Ordering::SeqCst) + 1
        };
        thread::spawn(move || {
            set_low_priority_current_thread();
            let _background_writes = BackgroundMetadataWrites::enter();
            log::debug!("meta worker #{worker_id} start");
            loop {
                let (p, _) = META_WORK.next_task_blocking_with_limit(|| {
                    read_settings().preload.meta_workers as usize
                });
                if let Some(root) = ACTIVE_ROOT.lock().unwrap().clone() {
                    if !p.starts_with(&root) {
                        META_WORK.mark_done(&p);
                        emit_progress(None);
                        continue;
                    }
                }
                if is_meta_failed(p.as_path()) {
                    META_WORK.mark_done(&p);
                    emit_progress(None);
                    continue;
                }
                log::debug!("meta worker #{worker_id} processing {}", p.display());
                let source = source_revision(&p);
                let task_settings = read_settings();
                let processing_version = task_settings.processing_version();
                match get_file_metadata_cached_with_settings(p.as_path(), &task_settings) {
                    Ok(_) => {
                        PRELOAD_DONE_META.fetch_add(1, Ordering::SeqCst);
                        emit_processed_media(&p, "metadata");
                    }
                    Err(e) => {
                        log::error!("Failed to extract metadata {}: {}", p.display(), e);
                        if let Some(source) = source {
                            let _ = mark_meta_failed(p.as_path(), &source, &processing_version);
                        }
                    }
                }
                let retry = processing_version != read_settings().processing_version();
                META_WORK.mark_done_with_retry(&p, retry.then_some(()));
                emit_progress(Some(PreloadStage::Metadata));
            }
        });
    }
}

fn start_thumb_hash_worker() {
    let settings = read_settings();
    let desired = settings.preload.hash_workers.max(1) as usize;
    loop {
        let worker_id = {
            let mut state = HASH_QUEUE.state.lock().unwrap();
            if state.started_workers >= desired {
                return;
            }
            state.started_workers += 1;
            HASH_WORKER_COUNTER.fetch_add(1, Ordering::SeqCst) + 1
        };
        std::thread::spawn(move || {
            set_low_priority_current_thread();
            let _background_writes = BackgroundMetadataWrites::enter();
            log::debug!("hash worker #{} start", worker_id);
            let mut processed: u64 = 0;
            loop {
                let (original, task_started, settings_clone) = HASH_QUEUE.next_task_blocking();
                if ACTIVE_ROOT
                    .lock()
                    .unwrap()
                    .as_ref()
                    .is_some_and(|root| !original.starts_with(root))
                {
                    HASH_QUEUE.mark_done(&original, task_started);
                    continue;
                }
                let hash_cfg_clone = settings_clone.hash_config();
                let delay_ms = settings_clone.preload.thumb_hash_queue_delay_ms;
                processed = processed.saturating_add(1);
                let log_this = processed <= 20 || processed % 100 == 0;
                if log_this {
                    let (queued, in_progress, done, total) = {
                        let state = HASH_QUEUE.state.lock().unwrap();
                        (
                            state.queue.len(),
                            state.in_progress.len(),
                            state
                                .done
                                .min(state.done + state.queue.len() + state.in_progress.len()),
                            state.done + state.queue.len() + state.in_progress.len(),
                        )
                    };
                    log::info!(
                        "hash worker #{} processing {} queued={} in_progress={} hash_queue={}/{} processed={}",
                        worker_id,
                        original.display(),
                        queued,
                        in_progress,
                        done,
                        total,
                        processed
                    );
                } else {
                    log::debug!(
                        "hash worker #{} processing {}",
                        worker_id,
                        original.display()
                    );
                }
                if let Some(cached) = load_album_file_hash(&original) {
                    if cached
                        .version
                        .as_deref()
                        .map(|v| v == hash_cfg_clone.hash_version)
                        .unwrap_or(false)
                        && cached.bits.unwrap_or(hash_cfg_clone.bits) == hash_cfg_clone.bits
                    {
                        let (done, total) = HASH_QUEUE.mark_done(&original, task_started);
                        emit_hash_event("hash-progress", done, total);
                        continue;
                    }
                }
                let hashed_bits =
                    compute_hash_for_file(&original, &hash_cfg_clone, &settings_clone);
                if log_this {
                    log::info!(
                        "hash worker #{} result={} {}",
                        worker_id,
                        hashed_bits.is_some(),
                        original.display()
                    );
                }
                let (done, total) = HASH_QUEUE.mark_done(&original, task_started);
                emit_hash_event("hash-progress", done, total);
                if delay_ms > 0 {
                    std::thread::sleep(Duration::from_millis(delay_ms));
                }
            }
        });
    }
}

pub fn is_preloading() -> bool {
    PRELOADER_RUNNING.load(Ordering::SeqCst)
        || THUMB_WORK.outstanding() > 0
        || META_WORK.outstanding() > 0
}

pub(crate) fn apply_worker_limits() {
    // Wake parked workers on a limit increase even if no new path is enqueued.
    // Synchronize with their predicate check and Condvar::wait to avoid a lost
    // notification between reading the old setting and parking.
    {
        let _guard = THUMB_WORK.state.lock().unwrap();
        THUMB_WORK.cv.notify_all();
    }
    {
        let _guard = META_WORK.state.lock().unwrap();
        META_WORK.cv.notify_all();
    }
    {
        let _guard = HASH_QUEUE.state.lock().unwrap();
        HASH_QUEUE.cv.notify_all();
    }
    if THUMB_WORK.has_work() {
        start_thumb_worker();
    }
    if META_WORK.has_work() {
        start_meta_worker();
    }
    if HASH_QUEUE.has_work() {
        start_thumb_hash_worker();
    }
}

#[allow(dead_code)]
pub fn get_preload_progress() -> usize {
    let (completed, total) = combined_progress_totals();
    if total == 0 {
        return 0;
    }
    ((completed * 100) / total).min(100)
}

pub async fn lock_until_preloaded(app: crate::host::AppHandle) -> Result<bool, String> {
    if PRELOADER_RUNNING.load(Ordering::SeqCst)
        || THUMB_WORK.outstanding() > 0
        || META_WORK.outstanding() > 0
    {
        log::info!("waiting for preloader to finish");
        {
            let mut handle = PRELOAD_APP.lock().unwrap();
            if handle.is_none() {
                *handle = Some(app.clone());
            }
        }
        while PRELOADER_RUNNING.load(Ordering::SeqCst)
            || THUMB_WORK.outstanding() > 0
            || META_WORK.outstanding() > 0
        {
            emit_progress(None);
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        log::info!("preloader finished");
    }
    Ok(true)
}

pub fn set_allow_open(app: AppHandle<NativeRuntime>, allow: bool) {
    *PRELOAD_APP.lock().unwrap() = Some(app);
    ALLOW_OPEN.store(allow, Ordering::SeqCst);
    if allow {
        let (done, total) = HASH_QUEUE.totals();
        if total > 0 {
            emit_hash_event("hash-progress", done, total);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dropping_an_album_cancels_its_in_flight_preload_only() {
        let album = PathBuf::from("/tmp/room237-preload-race/album");
        let cancel = Arc::new(AtomicBool::new(false));
        *CURRENT_PRELOAD_CANCEL.lock().unwrap() = Some((album.clone(), cancel.clone()));
        drop_preload_for_path(Path::new("/tmp/room237-preload-race/other"));
        assert!(!cancel.load(Ordering::Relaxed));
        drop_preload_for_path(&album);
        assert!(cancel.load(Ordering::Relaxed));
        *CURRENT_PRELOAD_CANCEL.lock().unwrap() = None;
    }
    #[test]
    fn streaming_producer_waits_for_capacity_without_losing_media() {
        let queue = WorkQueue::<usize> {
            state: Mutex::new(WorkQueueState::default()),
            cv: Condvar::new(),
        };
        thread::scope(|scope| {
            let producer = scope.spawn(|| {
                for n in 0..MAX_PENDING_MEDIA * 4 {
                    assert!(queue.enqueue(PathBuf::from(n.to_string()), n, TaskPriority::Low));
                }
            });
            for expected in 0..MAX_PENDING_MEDIA * 4 {
                let (path, n) = queue.next_task_blocking();
                assert_eq!(n, expected);
                let mut state = queue.state.lock().unwrap();
                assert!(state.queue.len() <= MAX_PENDING_MEDIA);
                state.in_progress.remove(&path);
                drop(state);
                queue.cv.notify_all();
            }
            producer.join().unwrap();
        });
        assert_eq!(queue.outstanding(), 0);
    }
    #[test]
    fn queued_foreground_work_overtakes_background_without_duplicate_or_data_loss() {
        let queue = WorkQueue::<u32> {
            state: Mutex::new(WorkQueueState::default()),
            cv: Condvar::new(),
        };
        let paths: Vec<_> = (0..MAX_PENDING_MEDIA)
            .map(|i| PathBuf::from(format!("/background/{i}")))
            .collect();
        for (i, path) in paths.iter().enumerate() {
            assert!(queue.enqueue(path.clone(), i as u32, TaskPriority::Low));
        }
        // A second request changes priority, not the data for the admitted task.
        assert!(queue.enqueue(
            paths[MAX_PENDING_MEDIA - 10].clone(),
            u32::MAX,
            TaskPriority::High
        ));
        assert!(queue.enqueue(
            paths[MAX_PENDING_MEDIA - 1].clone(),
            u32::MAX,
            TaskPriority::High
        ));
        assert!(!queue.enqueue(
            paths[MAX_PENDING_MEDIA - 10].clone(),
            u32::MAX,
            TaskPriority::High
        ));
        assert!(!queue.enqueue(
            paths[MAX_PENDING_MEDIA - 1].clone(),
            u32::MAX,
            TaskPriority::Low
        ));
        assert_eq!(queue.outstanding(), paths.len());
        for i in [MAX_PENDING_MEDIA - 1, MAX_PENDING_MEDIA - 10] {
            let (path, data) = queue.next_task_blocking();
            assert_eq!(path, paths[i]);
            assert_eq!(data, i as u32);
            assert!(!queue.enqueue(path.clone(), u32::MAX, TaskPriority::High));
            queue.mark_done(&path);
        }
        for (i, expected) in paths
            .iter()
            .enumerate()
            .filter(|(i, _)| *i != MAX_PENDING_MEDIA - 1 && *i != MAX_PENDING_MEDIA - 10)
        {
            let (path, data) = queue.next_task_blocking();
            assert_eq!(&path, expected);
            assert_eq!(data, i as u32);
            queue.mark_done(&path);
        }
        assert_eq!(queue.outstanding(), 0);
        assert!(!queue.has_work());
    }

    #[test]
    fn queued_foreground_hashes_overtake_background_without_changing_progress_totals() {
        let queue = HashQueue {
            state: Mutex::new(HashQueueState::default()),
            cv: Condvar::new(),
        };
        let paths: Vec<_> = (0..MAX_PENDING_MEDIA)
            .map(|i| PathBuf::from(format!("/background/{i}")))
            .collect();
        assert_eq!(
            queue.enqueue_many(&paths, TaskPriority::Low),
            (true, 0, paths.len())
        );
        assert_eq!(
            queue.enqueue_many(
                &[
                    paths[MAX_PENDING_MEDIA - 10].clone(),
                    paths[MAX_PENDING_MEDIA - 1].clone()
                ],
                TaskPriority::High
            ),
            (true, 0, paths.len())
        );
        assert_eq!(
            queue.enqueue_many(&[paths[MAX_PENDING_MEDIA - 10].clone()], TaskPriority::High),
            (false, 0, paths.len())
        );
        let settings = || {
            let mut s = crate::settings::AdvancedSettings::default();
            s.preload.hash_workers = 1;
            s
        };
        for i in [MAX_PENDING_MEDIA - 1, MAX_PENDING_MEDIA - 10] {
            let (path, started, _) = queue.next_task_blocking_with_settings(settings);
            assert_eq!(path, paths[i]);
            assert!(
                !queue
                    .enqueue_many(std::slice::from_ref(&path), TaskPriority::High)
                    .0
            );
            queue.mark_done(&path, started);
        }
        assert_eq!(queue.totals(), (2, paths.len()));
        for expected in paths
            .iter()
            .enumerate()
            .filter(|(i, _)| *i != MAX_PENDING_MEDIA - 1 && *i != MAX_PENDING_MEDIA - 10)
            .map(|(_, p)| p)
        {
            let (path, started, _) = queue.next_task_blocking_with_settings(settings);
            assert_eq!(&path, expected);
            queue.mark_done(&path, started);
        }
        assert_eq!(queue.totals(), (paths.len(), paths.len()));
        assert!(!queue.has_work());
    }

    #[test]
    fn reduced_worker_limit_waits_for_running_tasks_to_release_slots() {
        let queue = Arc::new(WorkQueue::<()> {
            state: Mutex::new(WorkQueueState::default()),
            cv: Condvar::new(),
        });
        let limit = Arc::new(AtomicUsize::new(2));
        for name in ["a", "b", "c"] {
            queue.enqueue(PathBuf::from(name), (), TaskPriority::Low);
        }
        let (a, _) = queue.next_task_blocking_with_limit(|| limit.load(Ordering::SeqCst));
        let (b, _) = queue.next_task_blocking_with_limit(|| limit.load(Ordering::SeqCst));
        limit.store(1, Ordering::SeqCst);
        let (send, recv) = std::sync::mpsc::channel();
        let worker_queue = queue.clone();
        let worker_limit = limit.clone();
        let thread = thread::spawn(move || {
            let (path, _) =
                worker_queue.next_task_blocking_with_limit(|| worker_limit.load(Ordering::SeqCst));
            send.send(path).unwrap();
        });
        assert!(recv.recv_timeout(Duration::from_millis(50)).is_err());
        queue.mark_done(&a);
        assert!(recv.recv_timeout(Duration::from_millis(50)).is_err());
        queue.mark_done(&b);
        assert_eq!(
            recv.recv_timeout(Duration::from_secs(1)).unwrap(),
            PathBuf::from("c")
        );
        thread.join().unwrap();
    }

    #[test]
    fn increased_worker_limit_wakes_a_parked_worker_without_another_enqueue() {
        let queue = Arc::new(WorkQueue::<()> {
            state: Mutex::new(WorkQueueState::default()),
            cv: Condvar::new(),
        });
        let limit = Arc::new(AtomicUsize::new(1));
        for name in ["a", "b"] {
            queue.enqueue(PathBuf::from(name), (), TaskPriority::Low);
        }
        queue.next_task_blocking_with_limit(|| limit.load(Ordering::SeqCst));
        let (send, recv) = std::sync::mpsc::channel();
        let worker_queue = queue.clone();
        let worker_limit = limit.clone();
        let thread = thread::spawn(move || {
            let (path, _) =
                worker_queue.next_task_blocking_with_limit(|| worker_limit.load(Ordering::SeqCst));
            send.send(path).unwrap();
        });
        assert!(recv.recv_timeout(Duration::from_millis(50)).is_err());
        {
            let _guard = queue.state.lock().unwrap();
            limit.store(2, Ordering::SeqCst);
            queue.cv.notify_all();
        }
        assert_eq!(
            recv.recv_timeout(Duration::from_secs(1)).unwrap(),
            PathBuf::from("b")
        );
        assert_eq!(queue.state.lock().unwrap().in_progress.len(), 2);
        thread.join().unwrap();
    }

    #[test]
    fn expired_hash_retains_its_physical_slot_until_the_original_task_returns() {
        let queue = Arc::new(HashQueue {
            state: Mutex::new(HashQueueState::default()),
            cv: Condvar::new(),
        });
        let settings = || {
            let mut s = crate::settings::AdvancedSettings::default();
            s.preload.hash_workers = 1;
            s
        };
        queue.enqueue_many(&[PathBuf::from("a"), PathBuf::from("b")], TaskPriority::Low);
        let (a, started, _) = queue.next_task_blocking_with_settings(settings);
        queue
            .state
            .lock()
            .unwrap()
            .in_progress_started
            .insert(a.clone(), started - Duration::from_secs(240));
        assert!(queue.expire_in_progress().is_some());
        let (send, recv) = std::sync::mpsc::channel();
        let worker_queue = queue.clone();
        let thread = thread::spawn(move || {
            let (path, _, _) = worker_queue.next_task_blocking_with_settings(settings);
            send.send(path).unwrap();
        });
        assert!(recv.recv_timeout(Duration::from_millis(50)).is_err());
        queue.mark_done(&a, started);
        assert_eq!(
            recv.recv_timeout(Duration::from_secs(1)).unwrap(),
            PathBuf::from("b")
        );
        assert_eq!(queue.state.lock().unwrap().running_tasks.len(), 1);
        thread.join().unwrap();
    }
    #[test]
    fn processing_change_requeues_running_work_without_an_idle_gap() {
        let queue = WorkQueue::<u8> {
            state: Mutex::new(WorkQueueState::default()),
            cv: Condvar::new(),
        };
        let path = PathBuf::from("/test/file.jpg");
        assert!(queue.enqueue(path.clone(), 1, TaskPriority::Low));
        let (started, data) = queue.next_task_blocking();
        assert_eq!(data, 1);
        queue.mark_done_with_retry(&started, Some(2));
        assert_eq!(queue.outstanding(), 1);
        assert!(queue.has_work());
        let (retried, data) = queue.next_task_blocking();
        assert_eq!(retried, path);
        assert_eq!(data, 2);
    }

    #[test]
    fn preloaded_album_with_an_old_processing_version_is_queued_again() {
        let dir = tempfile::tempdir().unwrap();
        fs::create_dir(dir.path().join(".room237-thumb")).unwrap();
        fs::create_dir(dir.path().join(".room237-metadata")).unwrap();
        let previous_root = ACTIVE_ROOT
            .lock()
            .unwrap()
            .replace(dir.path().to_path_buf());
        PRELOADED.lock().unwrap().insert(
            dir.path().to_path_buf(),
            read_settings().processing_version(),
        );
        enqueue_preload(dir.path());
        assert!(!PRELOAD_QUEUE
            .lock()
            .unwrap()
            .iter()
            .any(|path| path == dir.path()));
        PRELOADED.lock().unwrap().insert(
            dir.path().to_path_buf(),
            "previous processing settings".into(),
        );
        enqueue_preload(dir.path());
        assert!(PRELOAD_QUEUE
            .lock()
            .unwrap()
            .iter()
            .any(|path| path == dir.path()));
        PRELOAD_QUEUE
            .lock()
            .unwrap()
            .retain(|path| path != dir.path());
        PRELOADED.lock().unwrap().remove(dir.path());
        *ACTIVE_ROOT.lock().unwrap() = previous_root;
    }
    #[test]
    fn queue_clear_keeps_running_tasks_tracked() {
        let queue = WorkQueue::<()> {
            state: Mutex::new(WorkQueueState::default()),
            cv: Condvar::new(),
        };
        queue.enqueue(PathBuf::from("a"), (), TaskPriority::Low);
        queue.enqueue(PathBuf::from("b"), (), TaskPriority::Low);
        let (path, _) = queue.next_task_blocking();
        queue.clear();
        assert_eq!(queue.outstanding(), 1);
        assert!(queue.is_tracked(&path));
        queue.mark_done(&path);
        assert_eq!(queue.outstanding(), 0);
    }
    #[test]
    fn hash_deadline_includes_the_configured_ffmpeg_timeout_and_lock_wait() {
        let mut settings = crate::settings::AdvancedSettings::default();
        settings.ffmpeg.timeout_secs = 60;
        assert_eq!(hash_task_timeout(&settings), Duration::from_secs(215));
        let queue = HashQueue {
            state: Mutex::new(HashQueueState::default()),
            cv: Condvar::new(),
        };
        queue.enqueue_many(&[PathBuf::from("slow.jpg")], TaskPriority::Low);
        let (path, _, _) = queue.next_task_blocking();
        {
            let mut state = queue.state.lock().unwrap();
            state.in_progress_started.insert(
                path.clone(),
                std::time::Instant::now() - Duration::from_secs(40),
            );
            state
                .in_progress_timeouts
                .insert(path.clone(), hash_task_timeout(&settings));
        }
        assert!(queue.expire_in_progress().is_none());
        assert!(queue.has_work());
    }

    #[test]
    fn late_hash_completion_cannot_finish_a_requeued_task_or_increment_progress_twice() {
        let queue = HashQueue {
            state: Mutex::new(HashQueueState::default()),
            cv: Condvar::new(),
        };
        let path = PathBuf::from("replaced.jpg");
        queue.enqueue_many(std::slice::from_ref(&path), TaskPriority::Low);
        let (_, old_started, _) = queue.next_task_blocking();
        queue
            .state
            .lock()
            .unwrap()
            .in_progress_started
            .insert(path.clone(), old_started - Duration::from_secs(240));
        assert!(queue.expire_in_progress().is_some());
        queue.enqueue_many(std::slice::from_ref(&path), TaskPriority::High);
        let (_, new_started, _) = queue.next_task_blocking();
        assert_eq!(queue.mark_done(&path, old_started), (0, 1));
        assert!(queue.has_work());
        assert_eq!(queue.mark_done(&path, new_started), (1, 1));
        assert!(!queue.has_work());
        assert_eq!(queue.mark_done(&path, old_started), (1, 1));
    }

    #[test]
    fn finishing_last_hash_does_not_reenter_a_held_queue_mutex() {
        let (send, recv) = std::sync::mpsc::channel();
        thread::spawn(move || {
            let queue = HashQueue {
                state: Mutex::new(HashQueueState::default()),
                cv: Condvar::new(),
            };
            queue.enqueue_many(&[PathBuf::from("test-complete")], TaskPriority::Low);
            let (path, started, _) = queue.next_task_blocking();
            queue.mark_done(&path, started);
            send.send(()).unwrap();
        });
        recv.recv_timeout(Duration::from_secs(2))
            .expect("last hash completion deadlocked");
    }
}
