use std::{
    collections::HashSet,
    fs,
    io::Read,
    path::{Path, PathBuf},
    sync::{Condvar, Mutex},
    time::{Duration, Instant},
};

use crate::{
    constants::{IMAGE_EXTENSIONS, VIDEO_EXTENSIONS},
    metadata::{read_file_entry, source_revision, write_thumb_state, FileMetaEntry},
    settings::{read_settings, AdvancedSettings},
    util::{apply_ffmpeg_tuning_with_settings, wait_ffmpeg_with_timeout, FileLock},
};
use base64::Engine;
use once_cell::sync::Lazy;
use ffmpeg_sidecar::command::FfmpegCommand;
use image::GenericImageView;
use sha2::{Digest, Sha256};

// This prefix identifies temporary files protected by the directory lease.
// Older binaries don't take that lease, so maintenance preserves their temps.
pub(crate) const THUMBNAIL_TEMP_PREFIX: &str = ".room237-thumbnail-v2-";

pub(crate) fn thumbnail_maintenance_lock_path(thumb_dir: &Path) -> PathBuf {
    thumb_dir.join(".room237-maintenance.lock")
}

pub fn thumb_path(original: &Path, thumb_dir: &Path) -> Result<PathBuf, String> {
    let name = original
        .file_name()
        .ok_or("missing filename")?
        .to_string_lossy();
    let legacy = thumb_dir.join(format!("{name}.webp"));
    // Leave existing previews usable on filesystems with a larger name limit.
    // The name must also fit a 255-byte component on other filesystems.
    if name.len() <= 250 || legacy.is_file() {
        return Ok(legacy);
    }
    Ok(thumb_dir.join(hashed_thumb_name(&name)))
}

fn hashed_thumb_name(name: &str) -> String {
    format!(
        "room237-preview-v1-{}.webp",
        hex::encode(Sha256::digest(name.as_bytes()))
    )
}

pub(crate) fn deleted_thumbnail_paths(name: &str, thumb_dir: &Path) -> Vec<PathBuf> {
    let mut paths = vec![thumb_dir.join(format!("{name}.webp"))];
    if name.len() > 250 {
        paths.push(thumb_dir.join(hashed_thumb_name(name)));
    }
    paths
}

pub(crate) fn detached_thumbnail_name(original: &Path) -> Option<String> {
    let original_name = original.file_name()?.to_string_lossy();
    if original_name.len() <= 250 {
        return None;
    }
    let dir = original.parent()?.join(".room237-thumb");
    let preview = thumb_path(original, &dir).ok()?;
    let name = preview.file_name()?.to_string_lossy();
    (name != format!("{original_name}.webp")).then(|| name.into_owned())
}

/// Previews this process is generating. A worker and a foreground request for the same preview wait for each other
/// instead of decoding twice. Other processes are kept out by the directory lease where it matters (maintenance);
/// concurrent generators elsewhere only publish complete files by rename.
static GENERATING: Lazy<(Mutex<HashSet<PathBuf>>, Condvar)> =
    Lazy::new(|| (Mutex::new(HashSet::new()), Condvar::new()));

struct GenerationGuard(PathBuf);

impl GenerationGuard {
    fn acquire(thumb: &Path, timeout: Duration) -> Result<Self, String> {
        let (active, released) = &*GENERATING;
        let deadline = Instant::now() + timeout;
        let mut active = active.lock().unwrap();
        while active.contains(thumb) {
            let left = deadline
                .checked_duration_since(Instant::now())
                .ok_or_else(|| format!("Thumbnail is still being generated: {}", thumb.display()))?;
            active = released.wait_timeout(active, left).unwrap().0;
        }
        active.insert(thumb.to_path_buf());
        Ok(Self(thumb.to_path_buf()))
    }
}

impl Drop for GenerationGuard {
    fn drop(&mut self) {
        let (active, released) = &*GENERATING;
        active.lock().unwrap().remove(&self.0);
        released.notify_all();
    }
}

/// Versions up to 7.0 left an empty `<preview>.lock` beside every preview. They are removed once per album and run,
/// under the exclusive directory lease so that no generator of an older version is still using one.
pub(crate) fn remove_legacy_preview_locks(thumb_dir: &Path) {
    static CLEANED: Lazy<Mutex<HashSet<PathBuf>>> = Lazy::new(|| Mutex::new(HashSet::new()));
    if CLEANED.lock().unwrap().contains(thumb_dir) {
        return;
    }
    let Ok(Some(_lease)) = FileLock::try_acquire(&thumbnail_maintenance_lock_path(thumb_dir)) else {
        return;
    };
    let Ok(entries) = fs::read_dir(thumb_dir) else { return };
    for entry in entries.flatten() {
        if is_legacy_preview_lock(&entry.file_name().to_string_lossy()) {
            let _ = fs::remove_file(entry.path());
        }
    }
    CLEANED.lock().unwrap().insert(thumb_dir.to_path_buf());
}

/// `photo.jpg.lock` or `room237-preview-v1-<hash>.lock`; the directory's own leases start with ".room237-".
pub(crate) fn is_legacy_preview_lock(name: &str) -> bool {
    name.ends_with(".lock") && !name.starts_with(".room237-")
}

fn generate_image_thumbnail(
    input: &Path,
    output: &Path,
    blur_output: &Path,
    settings: &AdvancedSettings,
) -> Result<bool, String> {
    let _permit = crate::util::DecodePermit::acquire();
    #[cfg(target_os = "macos")]
    if crate::native_image::thumbnails_enabled() {
        if let Some((main, blur)) = crate::native_image::thumbnail(
            input,
            settings.thumbnails.max_dim,
            settings.thumbnails.image_webp_quality as f32,
            settings.thumbnails.image_webp_compression_level,
        ) {
            fs::write(output, main).map_err(|e| e.to_string())?;
            fs::write(blur_output, blur).map_err(|e| e.to_string())?;
            return Ok(true);
        }
    }
    let mut cmd = FfmpegCommand::new();
    apply_ffmpeg_tuning_with_settings(&mut cmd, false, &settings.ffmpeg);
    cmd.input(input)
        .arg("-y")
        .arg("-frames:v")
        .arg("1")
        .arg("-vf")
        .arg(format!(
            "scale=min(iw\\,{d}):min(ih\\,{d}):force_original_aspect_ratio=decrease",
            d = settings.thumbnails.max_dim
        ))
        .arg("-c:v")
        .arg("libwebp")
        .arg("-q:v")
        .arg(settings.thumbnails.image_webp_quality.to_string())
        .arg("-compression_level")
        .arg(settings.thumbnails.image_webp_compression_level.to_string())
        .output(output);
    add_blur_output(&mut cmd, blur_output);
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    match wait_ffmpeg_with_timeout(
        &mut child,
        Duration::from_secs(settings.ffmpeg.timeout_secs),
    ) {
        Ok(status) => {
            if !status.success() {
                log::error!(
                    "thumb image failed {}→{}",
                    input.display(),
                    output.display()
                );
            }
            Ok(status.success())
        }
        Err(e) => {
            log::error!(
                "thumb image timeout {}→{}: {}",
                input.display(),
                output.display(),
                e
            );
            Err(e)
        }
    }
}

fn generate_video_thumbnail(
    input: &Path,
    output: &Path,
    blur_output: &Path,
    settings: &AdvancedSettings,
) -> Result<bool, String> {
    let seek = settings.thumbnails.video_seek_seconds.max(0.0);
    if generate_video_thumbnail_at(input, output, blur_output, settings, seek)? {
        return Ok(true);
    }
    // A valid clip can end before the configured seek position.
    // Retry only once, and never restart a timed-out process.
    if seek > 0.0 {
        return generate_video_thumbnail_at(input, output, blur_output, settings, 0.0);
    }
    Ok(false)
}

fn generate_video_thumbnail_at(
    input: &Path,
    output: &Path,
    blur_output: &Path,
    settings: &AdvancedSettings,
    seek: f32,
) -> Result<bool, String> {
    let _permit = crate::util::DecodePermit::acquire();
    let mut cmd = FfmpegCommand::new();
    apply_ffmpeg_tuning_with_settings(&mut cmd, true, &settings.ffmpeg);
    cmd.arg("-ss")
        .arg(format!("{seek:.3}"))
        .input(input)
        .arg("-y")
        .arg("-frames:v")
        .arg("1")
        .arg("-vf")
        .arg(format!(
            "scale=min(iw\\,{d}):min(ih\\,{d}):force_original_aspect_ratio=decrease",
            d = settings.thumbnails.max_dim
        ))
        .output(output);
    add_blur_output(&mut cmd, blur_output);
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    match wait_ffmpeg_with_timeout(
        &mut child,
        Duration::from_secs(settings.ffmpeg.timeout_secs),
    ) {
        Ok(status) => {
            if !status.success() {
                log::error!(
                    "thumb video failed {}→{}",
                    input.display(),
                    output.display()
                );
            }
            Ok(status.success() && fs::metadata(output).is_ok_and(|file| file.len() > 0))
        }
        Err(e) => {
            log::error!(
                "thumb video timeout {}→{}: {}",
                input.display(),
                output.display(),
                e
            );
            Err(e)
        }
    }
}

// This output shares the original decode with the regular preview.
fn add_blur_output(cmd: &mut FfmpegCommand, output: &Path) {
    cmd.arg("-map")
        .arg("0:v:0")
        .arg("-vf")
        .arg("scale=16:16,format=yuv420p")
        .arg("-frames:v")
        .arg("1")
        .arg("-an")
        .arg("-c:v")
        .arg("libwebp")
        .arg("-q:v")
        .arg("20")
        .arg("-compression_level")
        .arg("0")
        .output(output);
}

fn read_blur_preview(path: &Path) -> Result<String, String> {
    let mut bytes = Vec::new();
    fs::File::open(path)
        .map_err(|e| e.to_string())?
        .take(4097)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 4096 {
        return Err("Blur preview exceeds 4 KiB".into());
    }
    let image = image::load_from_memory_with_format(&bytes, image::ImageFormat::WebP)
        .map_err(|e| e.to_string())?;
    if image.width() != 16 || image.height() != 16 {
        return Err("Blur preview must be 16 by 16 pixels".into());
    }
    Ok(base64::engine::general_purpose::STANDARD.encode(bytes))
}

// Upgrade existing libraries from their small cached preview, without reading
// or decoding the original again. Only the indexing worker calls this.
fn backfill_blur_preview(thumb: &Path, settings: &AdvancedSettings) -> Result<String, String> {
    let temporary = tempfile::Builder::new()
        .prefix(THUMBNAIL_TEMP_PREFIX)
        .suffix(".webp")
        .tempfile_in(thumb.parent().ok_or("Missing preview directory")?)
        .map_err(|e| e.to_string())?;
    let _permit = crate::util::DecodePermit::acquire();
    let mut cmd = FfmpegCommand::new();
    apply_ffmpeg_tuning_with_settings(&mut cmd, false, &settings.ffmpeg);
    cmd.arg("-y").input(thumb);
    add_blur_output(&mut cmd, temporary.path());
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    if !wait_ffmpeg_with_timeout(
        &mut child,
        Duration::from_secs(settings.ffmpeg.timeout_secs),
    )?
    .success()
    {
        return Err("Blur preview generation failed".into());
    }
    read_blur_preview(temporary.path())
}

pub(crate) fn cached_previews_current(
    path: &Path,
    thumb_dir: &Path,
    entry: &FileMetaEntry,
    version: &str,
) -> bool {
    entry.blur_preview.is_some() && cached_thumb_current(path, thumb_dir, entry, version)
}

// ctime detects same-size edits with a restored mtime on Unix. The hash is
// streamed only after this identity changes, never because time has elapsed.
fn content_revision(path: &Path) -> Option<String> {
    let info = path.metadata().ok()?;
    let revision = crate::metadata::source_revision_from_info(&info)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        return Some(format!("{revision}-{}-{}", info.ctime(), info.ctime_nsec()));
    }
    #[cfg(not(unix))]
    Some(revision)
}

// Keep sequential source reads bounded even when many workers are configured.
struct SourceHashPermit;
static SOURCE_READERS: once_cell::sync::Lazy<(std::sync::Mutex<usize>, std::sync::Condvar)> =
    once_cell::sync::Lazy::new(|| (std::sync::Mutex::new(0), std::sync::Condvar::new()));
impl SourceHashPermit {
    fn acquire() -> Self {
        let mut active = SOURCE_READERS.0.lock().unwrap();
        while *active >= 2 {
            active = SOURCE_READERS.1.wait(active).unwrap();
        }
        *active += 1;
        Self
    }
}
impl Drop for SourceHashPermit {
    fn drop(&mut self) {
        *SOURCE_READERS.0.lock().unwrap() -= 1;
        SOURCE_READERS.1.notify_one();
    }
}
fn source_hash(path: &Path) -> Result<String, String> {
    let _permit = SourceHashPermit::acquire();
    let mut file = fs::File::open(path).map_err(|e| e.to_string())?;
    let mut hasher = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let count = file.read(&mut buffer).map_err(|e| e.to_string())?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(hex::encode(hasher.finalize()))
}

pub(crate) fn cached_thumb_current(
    path: &Path,
    thumb_dir: &Path,
    entry: &FileMetaEntry,
    version: &str,
) -> bool {
    entry.thumb_version.as_deref() == Some(version)
        && entry.thumb_source_hash.is_some()
        && entry.thumb_hash_revision.is_some()
        && entry.thumb_hash_revision == content_revision(path)
        && thumb_path(path, thumb_dir).is_ok_and(|thumb| thumb.is_file())
}

pub fn ensure_thumb(path: &Path, thumb_dir: &Path) -> Result<PathBuf, String> {
    let settings = read_settings();
    ensure_thumb_with_settings(path, thumb_dir, &settings)
}

pub fn ensure_thumb_with_settings(
    path: &Path,
    thumb_dir: &Path,
    settings: &AdvancedSettings,
) -> Result<PathBuf, String> {
    let album = path.parent().unwrap_or(Path::new("."));
    let tree = crate::album_tree::read(&[album])?;
    let thumb = thumb_path(path, thumb_dir)?;
    let thumb_version = settings.thumb_version();
    if read_file_entry(path)?
        .is_some_and(|entry| cached_previews_current(path, thumb_dir, &entry, &thumb_version))
    {
        return Ok(thumb);
    }

    log::info!("generating thumb {}→{}", path.display(), thumb.display());
    crate::util::create_derived_dir(thumb_dir)?;

    let lock_timeout = Duration::from_secs(settings.ffmpeg.timeout_secs * 2 + 5);
    // Generators can run concurrently; maintenance needs exclusive ownership
    // before removing abandoned temporary files. Take this before the generation guard.
    let _directory_lease = FileLock::acquire_shared(
        &thumbnail_maintenance_lock_path(thumb_dir),
        lock_timeout,
        Duration::from_millis(settings.thumbnails.lock_poll_ms),
    )?;
    tree.ensure_current(&[album])?;
    let _generation = GenerationGuard::acquire(&thumb, lock_timeout)?;
    let cached = read_file_entry(path)?;
    if let Some(entry) = cached
        .as_ref()
        .filter(|entry| cached_thumb_current(path, thumb_dir, entry, &thumb_version))
    {
        if entry.blur_preview.is_some() {
            return Ok(thumb);
        }
        let source = source_revision(path).ok_or("Source media is unavailable")?;
        let blur = backfill_blur_preview(&thumb, settings)?;
        write_thumb_state(
            path,
            &thumb_version,
            &source,
            entry.thumb_source_hash.clone(),
            entry.thumb_hash_revision.clone(),
            Some(blur),
        )?;
        return Ok(thumb);
    }
    let source = source_revision(path).ok_or("Source media is unavailable")?;
    let hash_revision = content_revision(path).ok_or("Source media is unavailable")?;
    let hash = source_hash(path)?;
    if content_revision(path).as_deref() != Some(&hash_revision) {
        return Err("Media changed while hashing thumbnail source".into());
    }
    // A timestamp, inode or external attribute change alone needs no decoding.
    if thumb.is_file()
        && cached.as_ref().is_some_and(|entry| {
            entry.thumb_version.as_deref() == Some(&thumb_version)
                && entry.thumb_source_hash.as_deref() == Some(&hash)
        })
    {
        write_thumb_state(
            path,
            &thumb_version,
            &source,
            Some(hash),
            Some(hash_revision),
            Some(
                match cached.as_ref().and_then(|entry| entry.blur_preview.clone()) {
                    Some(blur) => blur,
                    None => backfill_blur_preview(&thumb, settings)?,
                },
            ),
        )?;
        return Ok(thumb);
    }
    let temporary = tempfile::Builder::new()
        .prefix(THUMBNAIL_TEMP_PREFIX)
        .suffix(".webp")
        .tempfile_in(thumb_dir)
        .map_err(|e| e.to_string())?;
    let blur_temp = tempfile::Builder::new()
        .prefix(THUMBNAIL_TEMP_PREFIX)
        .suffix(".webp")
        .tempfile_in(thumb_dir)
        .map_err(|e| e.to_string())?;
    let res = match path
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_ascii_lowercase()
        .as_str()
    {
        ext if IMAGE_EXTENSIONS.contains(&ext) => {
            generate_image_thumbnail(path, temporary.path(), blur_temp.path(), settings)
        }
        ext if VIDEO_EXTENSIONS.contains(&ext) => {
            generate_video_thumbnail(path, temporary.path(), blur_temp.path(), settings)
        }
        _ => Ok(false),
    };

    if !res?
        || temporary
            .as_file()
            .metadata()
            .map_err(|e| e.to_string())?
            .len()
            == 0
    {
        return Err(format!("Thumbnail generation failed: {}", path.display()));
    }
    if content_revision(path).as_deref() != Some(&hash_revision) {
        return Err("Media changed during thumbnail generation".into());
    }
    let blur = read_blur_preview(blur_temp.path())?;
    temporary.persist(&thumb).map_err(|e| e.to_string())?;
    write_thumb_state(
        path,
        &thumb_version,
        &source,
        Some(hash),
        Some(hash_revision),
        Some(blur),
    )?;
    Ok(thumb)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_test_image(path: &Path, color: [u8; 3]) {
        image::RgbImage::from_pixel(128, 64, image::Rgb(color))
            .save(path)
            .unwrap();
    }
    fn decoded_blur(path: &Path) -> (String, image::DynamicImage) {
        let entry = read_file_entry(path).unwrap().unwrap();
        let encoded = entry.blur_preview.unwrap();
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(&encoded)
            .unwrap();
        assert!(bytes.len() < 1024);
        let decoded =
            image::load_from_memory_with_format(&bytes, image::ImageFormat::WebP).unwrap();
        assert_eq!((decoded.width(), decoded.height()), (16, 16));
        (encoded, decoded)
    }
    #[test]
    #[ignore = "requires FFmpeg with libwebp"]
    fn creates_tiny_webp_with_regular_preview_and_refreshes_it_when_content_changes() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("photo.png");
        let thumbs = dir.path().join(".room237-thumb");
        let settings = AdvancedSettings::default();
        write_test_image(&path, [255, 0, 0]);
        let main = ensure_thumb_with_settings(&path, &thumbs, &settings).unwrap();
        let (before, _) = decoded_blur(&path);
        let regular = image::open(&main).unwrap();
        assert_eq!((regular.width(), regular.height()), (128, 64));
        write_test_image(&path, [0, 0, 255]);
        ensure_thumb_with_settings(&path, &thumbs, &settings).unwrap();
        let (after, _) = decoded_blur(&path);
        assert_ne!(before, after);
    }
    #[test]
    #[ignore = "requires FFmpeg with libwebp"]
    fn backfills_old_preview_without_decoding_original_or_rewriting_large_preview() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("photo.png");
        let thumbs = dir.path().join(".room237-thumb");
        let settings = AdvancedSettings::default();
        write_test_image(&source, [20, 180, 30]);
        let main = ensure_thumb_with_settings(&source, &thumbs, &settings).unwrap();
        let published = fs::read(&main).unwrap();
        fs::write(&source, b"intentionally not a decodable original").unwrap();
        write_thumb_state(
            &source,
            &settings.thumb_version(),
            &source_revision(&source).unwrap(),
            Some(source_hash(&source).unwrap()),
            content_revision(&source),
            None,
        )
        .unwrap();
        let cached = read_file_entry(&source).unwrap().unwrap();
        assert!(cached_thumb_current(
            &source,
            &thumbs,
            &cached,
            &settings.thumb_version()
        ));
        assert!(!cached_previews_current(
            &source,
            &thumbs,
            &cached,
            &settings.thumb_version()
        ));
        ensure_thumb_with_settings(&source, &thumbs, &settings).unwrap();
        decoded_blur(&source);
        assert_eq!(fs::read(&main).unwrap(), published);
        let cached = read_file_entry(&source).unwrap().unwrap();
        assert!(cached_previews_current(
            &source,
            &thumbs,
            &cached,
            &settings.thumb_version()
        ));
    }

    #[test]
    #[ignore = "requires FFmpeg with libwebp and libx264"]
    fn tiny_preview_supports_alpha_and_short_video_seek_fallback() {
        let dir = tempfile::tempdir().unwrap();
        let thumbs = dir.path().join(".room237-thumb");
        let png = dir.path().join("alpha.png");
        image::RgbaImage::from_pixel(64, 32, image::Rgba([10, 20, 220, 128]))
            .save(&png)
            .unwrap();
        let settings = AdvancedSettings::default();
        ensure_thumb_with_settings(&png, &thumbs, &settings).unwrap();
        decoded_blur(&png);
        let video = dir.path().join("short.mp4");
        let output = std::process::Command::new(ffmpeg_sidecar::paths::ffmpeg_path())
            .args(["-y", "-loop", "1", "-i"])
            .arg(&png)
            .args(["-t", "0.2", "-c:v", "libx264", "-pix_fmt", "yuv420p"])
            .arg(&video)
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        // The default seek is past this clip's end, so both outputs must retry at zero.
        ensure_thumb_with_settings(&video, &thumbs, &settings).unwrap();
        decoded_blur(&video);
    }

    #[test]
    fn old_thumbnail_and_timestamp_only_source_changes_reuse_published_bytes() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("photo.jpg");
        let thumbs = dir.path().join(".room237-thumb");
        fs::create_dir(&thumbs).unwrap();
        // Deliberately not decodable: reuse must succeed without invoking FFmpeg.
        fs::write(&source, b"unchanged original").unwrap();
        let preview = thumb_path(&source, &thumbs).unwrap();
        fs::write(&preview, b"published preview").unwrap();
        let old = std::time::UNIX_EPOCH + Duration::from_secs(1);
        fs::File::options()
            .write(true)
            .open(&preview)
            .unwrap()
            .set_times(fs::FileTimes::new().set_modified(old))
            .unwrap();
        let settings = AdvancedSettings::default();
        write_thumb_state(
            &source,
            &settings.thumb_version(),
            &source_revision(&source).unwrap(),
            Some(source_hash(&source).unwrap()),
            content_revision(&source),
            Some("cached-tiny-webp".into()),
        )
        .unwrap();
        let before = preview.metadata().unwrap().modified().unwrap();
        assert_eq!(
            ensure_thumb_with_settings(&source, &thumbs, &settings).unwrap(),
            preview
        );
        fs::File::options()
            .write(true)
            .open(&source)
            .unwrap()
            .set_times(fs::FileTimes::new().set_modified(old + Duration::from_secs(1)))
            .unwrap();
        assert_eq!(
            ensure_thumb_with_settings(&source, &thumbs, &settings).unwrap(),
            preview
        );
        assert_eq!(preview.metadata().unwrap().modified().unwrap(), before);
        assert_eq!(fs::read(&preview).unwrap(), b"published preview");
        let cached = read_file_entry(&source).unwrap().unwrap();
        assert!(cached_thumb_current(
            &source,
            &thumbs,
            &cached,
            &settings.thumb_version()
        ));
        assert!(!cached_thumb_current(
            &source,
            &thumbs,
            &cached,
            "changed settings"
        ));
        fs::remove_file(&preview).unwrap();
        assert!(!cached_thumb_current(
            &source,
            &thumbs,
            &cached,
            &settings.thumb_version()
        ));
    }

    #[cfg(unix)]
    #[test]
    fn same_size_edit_with_restored_mtime_invalidates_the_source_hash() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("photo.jpg");
        fs::write(&source, b"original").unwrap();
        let before = source.metadata().unwrap();
        let revision = content_revision(&source);
        let hash = source_hash(&source).unwrap();
        fs::write(&source, b"modified").unwrap();
        fs::File::options()
            .write(true)
            .open(&source)
            .unwrap()
            .set_times(fs::FileTimes::new().set_modified(before.modified().unwrap()))
            .unwrap();
        assert_ne!(content_revision(&source), revision);
        assert_ne!(source_hash(&source).unwrap(), hash);
    }

    #[test]
    fn published_previews_are_readable_as_native_files() {
        let root = tempfile::tempdir().unwrap();
        for name in ["photo.png", ".photo.png", ".private/.photo.png"] {
            let original = root.path().join(name);
            let thumbs = original.parent().unwrap().join(".room237-thumb");
            fs::create_dir_all(&thumbs).unwrap();
            fs::write(&original, b"source").unwrap();
            let preview = thumb_path(&original, &thumbs).unwrap();
            fs::write(&preview, b"preview").unwrap();
            assert_eq!(fs::read(&preview).unwrap(), b"preview");
        }
    }

    #[test]
    fn preview_components_fit_without_changing_ordinary_names() {
        let root = tempfile::tempdir().unwrap();
        let thumbs = root.path().join(".room237-thumb");
        fs::create_dir(&thumbs).unwrap();
        for length in [249, 250, 251, 255] {
            let name = format!("{}.png", "x".repeat(length - 4));
            let original = root.path().join(&name);
            fs::write(&original, b"source").unwrap();
            let preview = thumb_path(&original, &thumbs).unwrap();
            assert!(preview.file_name().unwrap().len() <= 255);
            fs::write(&preview, b"published preview").unwrap();
            assert_eq!(thumb_path(&original, &thumbs).unwrap(), preview);
            if length <= 250 {
                assert_eq!(preview, thumbs.join(format!("{name}.webp")));
                assert!(detached_thumbnail_name(&original).is_none());
            } else {
                let detached = detached_thumbnail_name(&original).unwrap();
                assert_eq!(preview, thumbs.join(detached));
            }
        }
    }

    #[test]
    fn a_second_generator_of_the_same_preview_waits_for_the_first() {
        let thumb = PathBuf::from("/nonexistent/.room237-thumb/a.jpg.webp");
        let first = GenerationGuard::acquire(&thumb, Duration::ZERO).unwrap();
        assert!(GenerationGuard::acquire(&thumb, Duration::from_millis(20)).is_err());
        let other = GenerationGuard::acquire(&thumb.with_file_name("b.jpg.webp"), Duration::ZERO);
        assert!(other.is_ok());
        let waiter = std::thread::spawn({
            let thumb = thumb.clone();
            move || GenerationGuard::acquire(&thumb, Duration::from_secs(5)).is_ok()
        });
        std::thread::sleep(Duration::from_millis(30));
        drop(first);
        assert!(waiter.join().unwrap());
    }

    #[test]
    fn legacy_preview_locks_are_removed_and_leases_kept() {
        let root = tempfile::tempdir().unwrap();
        let thumbs = root.path().join(".room237-thumb");
        fs::create_dir(&thumbs).unwrap();
        for name in ["a.jpg.webp", "a.jpg.lock", "room237-preview-v1-00.lock", ".room237-heic-00.lock"] {
            fs::write(thumbs.join(name), b"").unwrap();
        }
        let lease = thumbnail_maintenance_lock_path(&thumbs);
        let held = FileLock::acquire_shared(&lease, Duration::ZERO, Duration::ZERO).unwrap();
        remove_legacy_preview_locks(&thumbs);
        assert!(thumbs.join("a.jpg.lock").exists(), "a generator still holds the lease");
        drop(held);
        remove_legacy_preview_locks(&thumbs);
        let mut left: Vec<_> = fs::read_dir(&thumbs).unwrap().map(|e| e.unwrap().file_name().into_string().unwrap()).collect();
        left.sort();
        assert_eq!(left, [".room237-heic-00.lock", ".room237-maintenance.lock", "a.jpg.webp"]);
    }

    #[test]
    #[cfg(target_os = "macos")]
    fn existing_long_utf8_previews_remain_usable_on_macos() {
        let root = tempfile::tempdir().unwrap();
        let thumbs = root.path().join(".room237-thumb");
        fs::create_dir(&thumbs).unwrap();
        let name = format!("{}.png", "é".repeat(126));
        let original = root.path().join(&name);
        fs::write(&original, b"source").unwrap();
        let legacy = thumbs.join(format!("{name}.webp"));
        fs::write(&legacy, b"existing preview").unwrap();
        assert_eq!(thumb_path(&original, &thumbs).unwrap(), legacy);
        assert!(detached_thumbnail_name(&original).is_none());
        let with_preview = crate::metadata::media_revision(&original);
        fs::remove_file(&legacy).unwrap();
        assert_ne!(crate::metadata::media_revision(&original), with_preview);
        assert!(detached_thumbnail_name(&original).is_some());
    }
}
