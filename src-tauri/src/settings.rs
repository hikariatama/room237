use std::{
    fs,
    path::PathBuf,
    sync::{Arc, RwLock},
};

use image::imageops::FilterType;
use img_hash::HashAlg;
use once_cell::sync::OnceCell;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tauri::{path::BaseDirectory, AppHandle, Manager, Wry};

const SETTINGS_FILE: &str = "settings.json";
const HASH_SCHEMA_VERSION: &str = "1";

static SETTINGS_HANDLE: OnceCell<Arc<RwLock<AdvancedSettings>>> = OnceCell::new();

#[derive(Clone, Copy, Debug, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum HashSize {
    #[serde(rename = "8x8")]
    S8x8,
    #[serde(rename = "16x16")]
    S16x16,
    #[serde(rename = "32x32")]
    S32x32,
}

impl Default for HashSize {
    fn default() -> Self {
        HashSize::S16x16
    }
}

impl HashSize {
    pub fn dimensions(self) -> (u32, u32) {
        match self {
            HashSize::S8x8 => (8, 8),
            HashSize::S16x16 => (16, 16),
            HashSize::S32x32 => (32, 32),
        }
    }

    pub fn bits(self) -> u32 {
        let (w, h) = self.dimensions();
        w * h
    }
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum HashAlgorithm {
    Blockhash,
    Phash,
    Dhash,
}

impl Default for HashAlgorithm {
    fn default() -> Self {
        HashAlgorithm::Blockhash
    }
}

impl HashAlgorithm {
    pub fn to_img_hash_alg(self) -> HashAlg {
        match self {
            HashAlgorithm::Blockhash => HashAlg::Blockhash,
            HashAlgorithm::Phash => HashAlg::DoubleGradient,
            HashAlgorithm::Dhash => HashAlg::Gradient,
        }
    }
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ResizeFilter {
    Nearest,
    Triangle,
    Catmullrom,
    Lanczos3,
}

impl Default for ResizeFilter {
    fn default() -> Self {
        ResizeFilter::Nearest
    }
}

impl ResizeFilter {
    pub fn to_filter_type(self) -> FilterType {
        match self {
            ResizeFilter::Nearest => FilterType::Nearest,
            ResizeFilter::Triangle => FilterType::Triangle,
            ResizeFilter::Catmullrom => FilterType::CatmullRom,
            ResizeFilter::Lanczos3 => FilterType::Lanczos3,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(untagged)]
pub enum ThreadSetting {
    Auto(String),
    Count(u8),
}

impl Default for ThreadSetting {
    fn default() -> Self {
        ThreadSetting::Count(4)
    }
}

impl ThreadSetting {
    fn normalize(self) -> Self {
        match self {
            ThreadSetting::Auto(s) => {
                if s.eq_ignore_ascii_case("auto") {
                    ThreadSetting::Auto("auto".to_string())
                } else if let Ok(n) = s.parse::<u8>() {
                    ThreadSetting::Count(n)
                } else {
                    ThreadSetting::Auto("auto".to_string())
                }
            }
            ThreadSetting::Count(n) => ThreadSetting::Count(n.clamp(1, 32)),
        }
    }

    pub fn resolved(&self) -> u8 {
        match self {
            ThreadSetting::Count(n) => (*n).clamp(1, 32),
            ThreadSetting::Auto(_) => std::thread::available_parallelism()
                .map(|n| n.get().clamp(1, 32) as u8)
                .unwrap_or(4),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DuplicatesSettings {
    pub threshold: u32,
    pub hash_size: HashSize,
    pub hash_alg: HashAlgorithm,
    pub resize_filter: ResizeFilter,
    pub use_thumbnails_first: bool,
    pub max_files_per_album: u32,
}

impl Default for DuplicatesSettings {
    fn default() -> Self {
        Self {
            threshold: 32,
            hash_size: HashSize::default(),
            hash_alg: HashAlgorithm::default(),
            resize_filter: ResizeFilter::default(),
            use_thumbnails_first: true,
            max_files_per_album: 0,
        }
    }
}

impl DuplicatesSettings {
    fn clamp(self) -> Self {
        Self {
            threshold: self.threshold.min(128),
            hash_size: self.hash_size,
            hash_alg: self.hash_alg,
            resize_filter: self.resize_filter,
            use_thumbnails_first: self.use_thumbnails_first,
            max_files_per_album: self.max_files_per_album.min(20_000),
        }
    }

    pub fn effective_threshold(&self) -> u32 {
        let bits = self.hash_size.bits() as f64;
        let scaled = ((self.threshold as f64) * (bits / 256.0)).round();
        scaled.clamp(0.0, bits.max(1.0)).max(0.0).min(bits) as u32
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ThumbnailSettings {
    pub max_dim: u32,
    pub image_webp_quality: u8,
    pub image_webp_compression_level: u8,
    pub video_seek_seconds: f32,
    pub lock_poll_ms: u64,
}

impl Default for ThumbnailSettings {
    fn default() -> Self {
        Self {
            max_dim: 450,
            image_webp_quality: 75,
            image_webp_compression_level: 3,
            video_seek_seconds: 1.0,
            lock_poll_ms: 50,
        }
    }
}

impl ThumbnailSettings {
    fn clamp(self) -> Self {
        Self {
            max_dim: self.max_dim.clamp(128, 2048),
            image_webp_quality: self.image_webp_quality.clamp(30, 95),
            image_webp_compression_level: self.image_webp_compression_level.clamp(0, 9),
            video_seek_seconds: self.video_seek_seconds.clamp(0.0, 30.0),
            lock_poll_ms: self.lock_poll_ms.clamp(5, 250),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FfmpegSettings {
    pub threads: ThreadSetting,
    pub timeout_secs: u64,
    pub hwaccel: String,
    pub process_wait_poll_ms: u64,
}

impl Default for FfmpegSettings {
    fn default() -> Self {
        Self {
            threads: ThreadSetting::default(),
            timeout_secs: 5,
            hwaccel: "auto".to_string(),
            process_wait_poll_ms: 50,
        }
    }
}

impl FfmpegSettings {
    fn clamp(self) -> Self {
        Self {
            threads: self.threads.normalize(),
            timeout_secs: self.timeout_secs.clamp(1, 60),
            hwaccel: if self.hwaccel.trim().is_empty() {
                "auto".to_string()
            } else {
                self.hwaccel.clone()
            },
            process_wait_poll_ms: self.process_wait_poll_ms.clamp(5, 200),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PreloadSettings {
    pub thumb_workers: u8,
    pub meta_workers: u8,
    pub hash_workers: u8,
    pub progress_emit_ms: u64,
    pub thumb_hash_queue_delay_ms: u64,
    pub thumb_hash_only_after_idle: bool,
    pub thumb_hash_retry_on_thumb_change: bool,
}

impl Default for PreloadSettings {
    fn default() -> Self {
        Self {
            thumb_workers: 4,
            meta_workers: 4,
            hash_workers: 4,
            progress_emit_ms: 100,
            thumb_hash_queue_delay_ms: 10,
            thumb_hash_only_after_idle: true,
            thumb_hash_retry_on_thumb_change: true,
        }
    }
}

impl PreloadSettings {
    fn clamp(self) -> Self {
        Self {
            thumb_workers: self.thumb_workers.clamp(1, 32),
            meta_workers: self.meta_workers.clamp(1, 32),
            hash_workers: self.hash_workers.clamp(1, 32),
            progress_emit_ms: self.progress_emit_ms.clamp(50, 1000),
            thumb_hash_queue_delay_ms: self.thumb_hash_queue_delay_ms.clamp(0, 100),
            thumb_hash_only_after_idle: self.thumb_hash_only_after_idle,
            thumb_hash_retry_on_thumb_change: self.thumb_hash_retry_on_thumb_change,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MetadataSettings {
    pub ffmpeg_probe_timeout_secs: Option<u64>,
    pub parse_creation_time: bool,
}

impl Default for MetadataSettings {
    fn default() -> Self {
        Self {
            ffmpeg_probe_timeout_secs: None,
            parse_creation_time: true,
        }
    }
}

impl MetadataSettings {
    fn clamp(self, ffmpeg_timeout: u64) -> Self {
        let timeout = self
            .ffmpeg_probe_timeout_secs
            .map(|t| t.clamp(1, 60))
            .or(Some(ffmpeg_timeout));
        Self {
            ffmpeg_probe_timeout_secs: timeout,
            parse_creation_time: self.parse_creation_time,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AlbumSettings {
    pub rename_cleanup_delay_secs: u64,
    pub move_rename_thumbs_and_meta: bool,
}

impl Default for AlbumSettings {
    fn default() -> Self {
        Self {
            rename_cleanup_delay_secs: 1,
            move_rename_thumbs_and_meta: true,
        }
    }
}

impl AlbumSettings {
    fn clamp(self) -> Self {
        Self {
            rename_cleanup_delay_secs: self.rename_cleanup_delay_secs.clamp(0, 10),
            move_rename_thumbs_and_meta: self.move_rename_thumbs_and_meta,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PrivacySettings {
    pub enabled: bool,
    pub lockscreen_enabled: bool,
    pub confirm_open_enabled: bool,
}

impl Default for PrivacySettings {
    fn default() -> Self {
        Self {
            enabled: false,
            lockscreen_enabled: false,
            confirm_open_enabled: false,
        }
    }
}

impl PrivacySettings {
    fn clamp(self) -> Self {
        let (lockscreen_enabled, confirm_open_enabled) = if self.enabled {
            (true, true)
        } else {
            (self.lockscreen_enabled, self.confirm_open_enabled)
        };
        Self {
            enabled: self.enabled,
            lockscreen_enabled,
            confirm_open_enabled,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AdvancedSettings {
    #[serde(default)]
    pub duplicates: DuplicatesSettings,
    #[serde(default)]
    pub thumbnails: ThumbnailSettings,
    #[serde(default)]
    pub ffmpeg: FfmpegSettings,
    #[serde(default)]
    pub preload: PreloadSettings,
    #[serde(default)]
    pub metadata: MetadataSettings,
    #[serde(default)]
    pub album: AlbumSettings,
    #[serde(default)]
    pub privacy: PrivacySettings,
}

impl Default for AdvancedSettings {
    fn default() -> Self {
        Self {
            duplicates: DuplicatesSettings::default(),
            thumbnails: ThumbnailSettings::default(),
            ffmpeg: FfmpegSettings::default(),
            preload: PreloadSettings::default(),
            metadata: MetadataSettings::default(),
            album: AlbumSettings::default(),
            privacy: PrivacySettings::default(),
        }
    }
}

impl AdvancedSettings {
    pub fn clamp(self) -> Self {
        let ffmpeg = self.ffmpeg.clamp();
        Self {
            duplicates: self.duplicates.clamp(),
            thumbnails: self.thumbnails.clamp(),
            preload: self.preload.clamp(),
            metadata: self.metadata.clamp(ffmpeg.timeout_secs),
            album: self.album.clamp(),
            privacy: self.privacy.clamp(),
            ffmpeg,
        }
    }

    pub fn thumb_version(&self) -> String {
        let mut hasher = Sha256::new();
        hasher.update(format!(
            "dim:{}|q:{}|c:{}|seek:{:.3}",
            self.thumbnails.max_dim,
            self.thumbnails.image_webp_quality,
            self.thumbnails.image_webp_compression_level,
            self.thumbnails.video_seek_seconds
        ));
        let digest = hasher.finalize();
        hex::encode(digest)[0..16].to_string()
    }

    pub fn hash_config(&self) -> HashConfig {
        let size = self.duplicates.hash_size.dimensions();
        let bits = size.0 * size.1;
        let thumb_version = if self.duplicates.use_thumbnails_first {
            self.thumb_version()
        } else {
            String::new()
        };
        let mut hasher = Sha256::new();
        hasher.update(HASH_SCHEMA_VERSION.as_bytes());
        hasher.update(
            format!(
                "alg:{:?}|size:{}x{}|filter:{:?}|thumb_first:{}|thumb_version:{}",
                self.duplicates.hash_alg,
                size.0,
                size.1,
                self.duplicates.resize_filter,
                self.duplicates.use_thumbnails_first,
                thumb_version
            )
            .as_bytes(),
        );
        let hash_version = hex::encode(hasher.finalize());

        HashConfig {
            size,
            bits,
            alg: self.duplicates.hash_alg,
            resize_filter: self.duplicates.resize_filter,
            use_thumbnails_first: self.duplicates.use_thumbnails_first,
            thumb_version,
            hash_version,
            user_threshold: self.duplicates.threshold,
            effective_threshold: self.duplicates.effective_threshold(),
            thumb_settings: self.thumbnails.clone(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct HashConfig {
    pub size: (u32, u32),
    pub bits: u32,
    pub alg: HashAlgorithm,
    pub resize_filter: ResizeFilter,
    pub use_thumbnails_first: bool,
    pub thumb_version: String,
    pub hash_version: String,
    pub user_threshold: u32,
    pub effective_threshold: u32,
    pub thumb_settings: ThumbnailSettings,
}

#[derive(Clone)]
pub struct SettingsState {
    path: PathBuf,
    inner: Arc<RwLock<AdvancedSettings>>,
}

impl SettingsState {
    pub fn load(app: &AppHandle<Wry>) -> Result<Self, String> {
        let path = app
            .path()
            .resolve(SETTINGS_FILE, BaseDirectory::AppConfig)
            .map_err(|e| e.to_string())?;
        let settings = fs::read_to_string(&path)
            .ok()
            .and_then(|txt| serde_json::from_str::<AdvancedSettings>(&txt).ok())
            .unwrap_or_default()
            .clamp();
        let inner = Arc::new(RwLock::new(settings));
        let _ = SETTINGS_HANDLE.set(inner.clone());
        Ok(Self { path, inner })
    }

    fn persist(&self, settings: &AdvancedSettings) -> Result<(), String> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
        let json = serde_json::to_string_pretty(settings).map_err(|e| e.to_string())?;
        fs::write(&self.path, json).map_err(|e| e.to_string())
    }

    pub fn get(&self) -> AdvancedSettings {
        self.inner
            .read()
            .map(|g| g.clone())
            .unwrap_or_else(|_| AdvancedSettings::default())
    }

    pub fn shared(&self) -> Arc<RwLock<AdvancedSettings>> {
        self.inner.clone()
    }

    pub fn update(&self, next: AdvancedSettings) -> Result<AdvancedSettings, String> {
        let validated = next.clamp();
        {
            let mut guard = self.inner.write().map_err(|e| e.to_string())?;
            *guard = validated.clone();
        }
        self.persist(&validated)?;
        Ok(validated)
    }

    pub fn reset(&self) -> Result<AdvancedSettings, String> {
        self.update(AdvancedSettings::default())
    }
}

pub fn read_settings() -> AdvancedSettings {
    SETTINGS_HANDLE
        .get()
        .cloned()
        .and_then(|arc| arc.read().ok().map(|g| g.clone()))
        .unwrap_or_default()
}

#[tauri::command]
pub fn get_settings(state: tauri::State<SettingsState>) -> Result<AdvancedSettings, String> {
    Ok(state.get())
}

#[tauri::command]
pub fn update_settings(
    state: tauri::State<SettingsState>,
    settings: AdvancedSettings,
) -> Result<AdvancedSettings, String> {
    state.update(settings)
}

#[tauri::command]
pub fn reset_settings(state: tauri::State<SettingsState>) -> Result<AdvancedSettings, String> {
    state.reset()
}
