use crate::{
    metadata::{self, ObservedMedia},
    util::{self, FileLock},
};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
    time::Duration,
};

const PREFIX: &str = ".room237-delete-";
const JOURNAL: &str = "journal.json";
const JOURNAL_LIMIT: u64 = 1024 * 1024;
pub(crate) const DELETE_BATCH_LIMIT: usize = 128;

#[derive(Clone, Deserialize)]
pub(crate) struct DeleteTarget {
    pub name: String,
    #[serde(default)]
    pub revision: Option<String>,
}

#[derive(Serialize)]
pub(crate) struct DeleteBatchResult {
    pub deleted: Vec<String>,
    pub error: Option<String>,
}

fn validate_media_name(name: &str) -> Result<(), String> {
    util::validate_file_name(name)?;
    if !util::has_extension(Path::new(name), crate::constants::IMAGE_EXTENSIONS)
        && !util::has_extension(Path::new(name), crate::constants::VIDEO_EXTENSIONS)
    {
        return Err("Expected a supported media file".into());
    }
    Ok(())
}

#[derive(Serialize, Deserialize)]
struct Entry {
    name: String,
    revision: String,
}

#[derive(Serialize, Deserialize)]
struct Journal {
    version: u8,
    committed: bool,
    files: Vec<Entry>,
}

fn owned_revision(path: &Path) -> Option<String> {
    metadata::source_revision_from_info(&fs::symlink_metadata(path).ok()?)
}

fn sync_dir(path: &Path) -> Result<(), String> {
    #[cfg(unix)]
    fs::File::open(path)
        .and_then(|file| file.sync_all())
        .map_err(|error| error.to_string())?;
    Ok(())
}

fn write_journal(staging: &Path, journal: &Journal) -> Result<(), String> {
    let bytes = serde_json::to_vec(journal).map_err(|e| e.to_string())?;
    if bytes.len() as u64 > JOURNAL_LIMIT {
        return Err("Deletion journal is too large".into());
    }
    util::atomic_write_owned(&staging.join(JOURNAL), &bytes)?;
    sync_dir(staging)
}

fn validate_journal(journal: &Journal) -> Result<(), String> {
    if journal.version != 1 || journal.files.is_empty() {
        return Err("Unsupported deletion journal".into());
    }
    let mut names = std::collections::HashSet::new();
    for entry in &journal.files {
        validate_media_name(&entry.name)?;
        if entry.revision.is_empty() || !names.insert(&entry.name) {
            return Err("Invalid deletion journal entry".into());
        }
    }
    Ok(())
}

fn remove_optional_cache(path: &Path) -> Result<(), String> {
    match fs::symlink_metadata(path) {
        Ok(info) if info.is_file() || info.file_type().is_symlink() => {
            fs::remove_file(path).map_err(|error| error.to_string())
        }
        Ok(_) => Err(format!(
            "Derived artifact is not a file: {}",
            path.display()
        )),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        // Legacy sidecars for maximum-length names cannot exist on a filesystem
        // that rejects their longer component; the hashed preview still can.
        Err(error) => {
            #[cfg(unix)]
            let too_long = error.raw_os_error() == Some(libc::ENAMETOOLONG);
            #[cfg(windows)]
            let too_long = error.raw_os_error()
                == Some(windows::Win32::Foundation::ERROR_FILENAME_EXCED_RANGE.0 as i32);
            #[cfg(not(any(unix, windows)))]
            let too_long = false;
            if too_long {
                Ok(())
            } else {
                Err(error.to_string())
            }
        }
    }
}

fn cleanup_deleted_artifacts(
    album: &Path,
    journal: &Journal,
    guard: &metadata::MetadataWriteGuard,
) -> Result<(), String> {
    let thumbs = album.join(".room237-thumb");
    let metadata_dir = album.join(".room237-metadata");
    for directory in [&thumbs, &metadata_dir] {
        match fs::symlink_metadata(directory) {
            Ok(info) if info.is_dir() => {}
            Ok(_) => {
                return Err(format!(
                    "Derived directory is not a regular directory: {}",
                    directory.display()
                ))
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.to_string()),
        }
    }
    let mut names = Vec::new();
    for entry in &journal.files {
        match fs::symlink_metadata(album.join(&entry.name)) {
            Ok(_) => continue, // Preserve a replacement and its metadata/cache.
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.to_string()),
        }
        for preview in crate::thumb::deleted_thumbnail_paths(&entry.name, &thumbs) {
            remove_optional_cache(&preview)?;
        }
        remove_optional_cache(&metadata_dir.join(format!("{}.meta", entry.name)))?;
        names.push(entry.name.clone());
    }
    // Remove legacy rows first so a restart cannot migrate them back into JSON.
    metadata::remove_deleted_metadata_with_guard(album, &names, guard)?;
    for directory in [&thumbs, &metadata_dir] {
        if directory.is_dir() {
            sync_dir(directory)?;
        }
    }
    Ok(())
}

fn finish_or_rollback(
    album: &Path,
    staging: &Path,
    journal: &Journal,
    guard: &metadata::MetadataWriteGuard,
    checkpoint: &impl Fn(&str) -> Result<(), String>,
) -> Result<(), String> {
    validate_journal(journal)?;
    let mut errors = Vec::new();
    for entry in &journal.files {
        let staged = staging.join(&entry.name);
        let original = album.join(&entry.name);
        match fs::symlink_metadata(&staged) {
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                if !journal.committed {
                    match fs::symlink_metadata(&original) {
                        // The file was never staged or has already been restored.
                        // A later external replacement belongs to its writer.
                        Ok(_) => {}
                        Err(error) => errors.push(format!(
                            "Missing original during deletion recovery: {}: {error}",
                            entry.name
                        )),
                    }
                }
            }
            Err(error) => errors.push(error.to_string()),
            Ok(info) => {
                if !(info.is_file() || info.file_type().is_symlink())
                    || owned_revision(&staged).as_deref() != Some(&entry.revision)
                {
                    errors.push(format!("Staged media changed; preserving {}", entry.name));
                    continue;
                }
                let result = if journal.committed {
                    fs::remove_file(&staged)
                } else {
                    util::rename_noclobber(&staged, &original)
                };
                if let Err(error) = result {
                    errors.push(format!("{}: {error}", entry.name));
                }
            }
        }
    }
    if !errors.is_empty() {
        return Err(errors.join("; "));
    }
    sync_dir(album)?;
    sync_dir(staging)?;
    if journal.committed {
        checkpoint("after_media_cleanup")?;
        cleanup_deleted_artifacts(album, journal, guard)?;
        checkpoint("after_artifact_cleanup")?;
    }
    let unknown = fs::read_dir(staging)
        .map_err(|e| e.to_string())?
        .any(|entry| {
            entry
                .map(|entry| entry.file_name() != JOURNAL)
                .unwrap_or(true)
        });
    if unknown {
        let suffix = staging
            .file_name()
            .and_then(|name| name.to_str())
            .and_then(|name| name.strip_prefix(PREFIX))
            .ok_or("Invalid staging directory")?;
        let quarantine = album.join(format!(".room237-orphan-delete-{suffix}"));
        util::rename_noclobber(staging, &quarantine).map_err(|e| e.to_string())?;
        log::warn!(
            "Preserved unknown deletion staging files in {}",
            quarantine.display()
        );
        return sync_dir(album);
    }
    fs::remove_file(staging.join(JOURNAL)).map_err(|e| e.to_string())?;
    fs::remove_dir(staging).map_err(|e| e.to_string())?;
    sync_dir(album)
}

fn staging_dirs(album: &Path) -> Result<Vec<PathBuf>, String> {
    fs::read_dir(album)
        .map_err(|e| e.to_string())?
        .filter_map(|entry| {
            let entry = match entry {
                Ok(entry) => entry,
                Err(error) => return Some(Err(error.to_string())),
            };
            if !entry.file_name().to_string_lossy().starts_with(PREFIX) {
                return None;
            }
            match entry.file_type() {
                Ok(kind) if kind.is_dir() => Some(Ok(entry.path())),
                Ok(_) => None,
                Err(error) => Some(Err(error.to_string())),
            }
        })
        .collect()
}

pub(crate) fn lock_maintenance(album: &Path) -> Result<FileLock, String> {
    let thumbs = album.join(".room237-thumb");
    match fs::create_dir(&thumbs) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists && thumbs.is_dir() => {}
        Err(error) => return Err(error.to_string()),
    }
    FileLock::acquire(
        &crate::thumb::thumbnail_maintenance_lock_path(&thumbs),
        Duration::from_secs(crate::settings::read_settings().ffmpeg.timeout_secs),
        Duration::from_millis(5),
    )
}

fn recover_locked(album: &Path, guard: &metadata::MetadataWriteGuard) -> Result<(), String> {
    for staging in staging_dirs(album)? {
        if fs::read_dir(&staging)
            .map_err(|e| e.to_string())?
            .next()
            .is_none()
        {
            fs::remove_dir(&staging).map_err(|e| e.to_string())?;
            sync_dir(album)?;
            continue;
        }
        use std::io::Read;
        let journal_path = staging.join(JOURNAL);
        if !fs::symlink_metadata(&journal_path).is_ok_and(|info| info.is_file()) {
            return Err("Deletion journal is not a regular file".into());
        }
        let file = fs::File::open(&journal_path).map_err(|e| e.to_string())?;
        let mut bytes = Vec::new();
        file.take(JOURNAL_LIMIT + 1)
            .read_to_end(&mut bytes)
            .map_err(|e| e.to_string())?;
        if bytes.len() as u64 > JOURNAL_LIMIT {
            return Err("Deletion journal is too large".into());
        }
        let journal: Journal = serde_json::from_slice(&bytes)
            .map_err(|e| format!("Deletion journal {}: {e}", staging.display()))?;
        finish_or_rollback(album, &staging, &journal, guard, &|_| Ok(()))?;
    }
    Ok(())
}

pub(crate) fn recover_album(album: &Path) -> Result<(), String> {
    let tree = crate::album_tree::read(&[album])?;
    crate::media_move::recover_album(album)?;
    if staging_dirs(album)?.is_empty() {
        return Ok(());
    }
    let _maintenance = lock_maintenance(album)?;
    let (_move, guard) = metadata::lock_metadata_for_move_with_tree(&[album], &tree)?;
    recover_locked(album, &guard)
}

pub(crate) fn delete_media(path: String) -> Result<(), String> {
    delete_with(Path::new(&path), None, &[], |_| Ok(()))
}

pub(crate) fn delete_media_batch(
    album_path: String,
    targets: Vec<DeleteTarget>,
    observed: Vec<ObservedMedia>,
) -> Result<DeleteBatchResult, String> {
    delete_batch_with(Path::new(&album_path), &targets, &observed, |_| Ok(()))
}

pub(crate) fn delete_if_unchanged(
    path: &Path,
    revision: &str,
    observed: &[ObservedMedia],
) -> Result<(), String> {
    delete_with(path, Some(revision), observed, |_| Ok(()))
}

fn delete_with(
    path: &Path,
    revision: Option<&str>,
    observed: &[ObservedMedia],
    checkpoint: impl Fn(&str) -> Result<(), String>,
) -> Result<(), String> {
    let name = path
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or("Invalid media name")?;
    let result = delete_batch_with(
        path.parent().ok_or("Missing media album")?,
        &[DeleteTarget {
            name: name.into(),
            revision: revision.map(str::to_owned),
        }],
        observed,
        checkpoint,
    )?;
    result.error.map_or(Ok(()), Err)
}

fn delete_batch_with(
    album: &Path,
    targets: &[DeleteTarget],
    observed: &[ObservedMedia],
    checkpoint: impl Fn(&str) -> Result<(), String>,
) -> Result<DeleteBatchResult, String> {
    delete_batch_with_mode(album,targets,observed,true,checkpoint)
}

/// Ordinary native gallery deletion checks reviewed revisions under the same
/// leases as duplicate deletion, but may intentionally delete the whole selection.
pub(crate) fn native_delete_batch(album_path:String,targets:Vec<DeleteTarget>) -> Result<DeleteBatchResult,String> {
    if targets.iter().any(|target|target.revision.as_deref().is_none_or(str::is_empty)) {return Err("Every selected file needs its reviewed revision".into())}
    delete_batch_with_mode(Path::new(&album_path),&targets,&[],false,|_|Ok(()))
}

fn delete_batch_with_mode(
    album:&Path,targets:&[DeleteTarget],observed:&[ObservedMedia],require_retained:bool,
    checkpoint:impl Fn(&str)->Result<(),String>
)->Result<DeleteBatchResult,String> {
    if targets.is_empty() {
        return Ok(DeleteBatchResult {
            deleted: Vec::new(),
            error: None,
        });
    }
    if targets.len() > DELETE_BATCH_LIMIT {
        return Err("Too many files in one deletion batch".into());
    }
    let guarded = targets.iter().any(|target| target.revision.is_some());
    let mut seen = std::collections::HashMap::new();
    let mut unique = Vec::new();
    for target in targets {
        validate_media_name(&target.name)?;
        if guarded && target.revision.as_deref().is_none_or(str::is_empty) {
            return Err("Every duplicate deletion needs its observed revision".into());
        }
        if let Some(previous) = seen.insert(target.name.as_str(), target.revision.as_deref()) {
            if previous != target.revision.as_deref() {
                return Err("Conflicting revisions for one deletion target".into());
            }
        } else {
            unique.push(target);
        }
    }
    if (guarded && require_retained && observed.is_empty()) || (!guarded && !observed.is_empty()) {
        return Err("Duplicate deletion needs retained observations and revisions".into());
    }
    let album = album.canonicalize().map_err(|e| e.to_string())?;
    let _tree = crate::album_tree::read(&[&album])?;
    crate::media_move::recover_album(&album)?;
    let is_selected = |path: &Path| {
        path.file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| seen.contains_key(name))
            && path
                .parent()
                .and_then(|parent| parent.canonicalize().ok())
                .as_deref()
                == Some(album.as_path())
    };
    // Producers take the shared maintenance lease before their per-file locks.
    // Wait for them before reserving the move mutex or metadata leases.
    let _maintenance = lock_maintenance(&album)?;
    let (_move, guard) = metadata::lock_metadata_for_move_with_tree(&[&album], &_tree)?;
    recover_locked(&album, &guard)?;
    if guarded && require_retained {
        if !observed
            .iter()
            .any(|item| !is_selected(Path::new(&item.path)))
        {
            return Err("Duplicate deletion must retain an observed file".into());
        }
        for item in observed {
            if item.revision.is_empty()
                || !Path::new(&item.path).is_file()
                || metadata::media_revision(Path::new(&item.path)) != item.revision
            {
                return Err("Media changed; refresh duplicates before deleting.".into());
            }
        }
    }
    let snapshot = metadata::read_selected_with_guard(
        &album,
        &unique
            .iter()
            .map(|target| target.name.clone())
            .collect::<Vec<_>>(),
        true,
        &guard,
    )?;
    let mut files = Vec::new();
    let mut deleted = Vec::new();
    let mut failure = None;
    let mut selected_regular = std::collections::HashMap::new();
    for target in unique {
        let path = album.join(&target.name);
        let prepared = (|| -> Result<(bool, String), String> {
            if let Some(revision) = &target.revision {
                if metadata::media_revision(&path) != *revision {
                    return Err("Media changed; refresh duplicates before deleting.".into());
                }
            }
            let info = fs::symlink_metadata(&path).map_err(|e| e.to_string())?;
            if !(info.is_file() || info.file_type().is_symlink()) {
                return Err("Media is not a file".into());
            }
            Ok((
                info.is_file(),
                owned_revision(&path).ok_or("Media is unavailable")?,
            ))
        })();
        let (regular, revision) = match prepared {
            Ok(prepared) => prepared,
            Err(error) => {
                failure = Some(error);
                break;
            }
        };
        if regular {
            selected_regular.insert(target.name.as_str(), revision.clone());
        }
        deleted.push(target.name.clone());
        files.push(Entry {
            name: target.name.clone(),
            revision,
        });
    }
    if files.is_empty() {
        return Err(failure.unwrap_or_else(|| "No deletion targets".into()));
    }
    let mut planned: std::collections::HashSet<String> =
        files.iter().map(|entry| entry.name.clone()).collect();
    let mut companions = Vec::new();
    // Index all selected outputs once; do not scan a large album per JPEG.
    for (source_name, entry) in &snapshot.files {
        let Some(conversion) = &entry.heic_conversion else {
            continue;
        };
        let Some(selected_revision) = selected_regular.get(conversion.output_name.as_str()) else {
            continue;
        };
        util::validate_file_name(source_name)?;
        let source = album.join(source_name);
        if conversion.output_revision == *selected_revision
            && util::has_extension(&source, &["heic"])
            && fs::symlink_metadata(&source).is_ok_and(|info| info.is_file())
            && owned_revision(&source).as_deref() == Some(&conversion.source_revision)
        {
            if !planned.insert(source_name.clone()) {
                continue;
            }
            companions.push(Entry {
                name: source_name.clone(),
                revision: conversion.source_revision.clone(),
            });
        }
    }
    companions.sort_by(|a, b| a.name.cmp(&b.name));
    files.extend(companions);
    let temporary = tempfile::Builder::new()
        .prefix(PREFIX)
        .tempdir_in(&album)
        .map_err(|e| e.to_string())?;
    let mut journal = Journal {
        version: 1,
        committed: false,
        files,
    };
    validate_journal(&journal)?;
    write_journal(temporary.path(), &journal)?;
    sync_dir(&album)?;
    let staging = temporary.keep();
    let staged = (|| -> Result<(), String> {
        for entry in &journal.files {
            let original = album.join(&entry.name);
            let target = staging.join(&entry.name);
            if owned_revision(&original).as_deref() != Some(&entry.revision) {
                return Err("Media changed before deletion".into());
            }
            util::rename_noclobber(&original, &target).map_err(|e| e.to_string())?;
            if owned_revision(&target).as_deref() != Some(&entry.revision) {
                return Err("Media changed during deletion".into());
            }
            checkpoint("staged")?;
        }
        sync_dir(&album)?;
        sync_dir(&staging)?;
        for item in observed {
            let observed = Path::new(&item.path);
            let selected = is_selected(observed);
            if !selected
                && (item.revision.is_empty()
                    || !observed.is_file()
                    || metadata::media_revision(observed) != item.revision)
            {
                return Err("Retained duplicate changed during deletion".into());
            }
        }
        checkpoint("before_commit")?;
        Ok(())
    })();
    if let Err(error) = staged {
        return match finish_or_rollback(&album, &staging, &journal, &guard, &checkpoint) {
            Ok(()) => Err(error),
            Err(rollback) => Err(format!("{error}; deletion rollback: {rollback}")),
        };
    }
    journal.committed = true;
    // An ambiguous durable commit is left for recovery; never remove staged
    // originals when journal publication reports an error.
    write_journal(&staging, &journal)?;
    checkpoint("committed")?;
    if let Err(error) = finish_or_rollback(&album, &staging, &journal, &guard, &checkpoint) {
        log::error!("Committed media deletion needs cleanup: {error}");
    }
    Ok(DeleteBatchResult {
        deleted,
        error: failure,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn targets(names: &[&str]) -> Vec<DeleteTarget> {
        names
            .iter()
            .map(|name| DeleteTarget {
                name: (*name).into(),
                revision: None,
            })
            .collect()
    }
    fn guarded_targets(album: &Path, names: &[&str]) -> Vec<DeleteTarget> {
        names
            .iter()
            .map(|name| DeleteTarget {
                name: (*name).into(),
                revision: Some(metadata::media_revision(&album.join(name))),
            })
            .collect()
    }
    fn observe(path: &Path) -> ObservedMedia {
        ObservedMedia {
            path: path.to_string_lossy().into(),
            revision: metadata::media_revision(path),
        }
    }
    #[test]
    fn batch_deletes_selected_photos_and_their_verified_heic_companions() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        cache_fixture(dir.path());
        fs::write(dir.path().join("second.png"), b"second photo").unwrap();
        metadata::update_file_meta(dir.path(), "second.png", |entry| entry.favorite = true)
            .unwrap();
        let before = fs::read(dir.path().join(".room237-metadata/album.sqlite")).unwrap();
        let observed = std::cell::RefCell::new(None);
        let result = delete_batch_with(
            dir.path(),
            &targets(&["photo.jpeg", "second.png"]),
            &[],
            |phase| {
                if phase == "before_commit" {
                    let stage = staging_dirs(dir.path())?.pop().ok_or("Missing staging")?;
                    let count = fs::read_dir(stage).map_err(|e| e.to_string())?.count();
                    let json = fs::read(dir.path().join(".room237-metadata/album.sqlite"))
                        .map_err(|e| e.to_string())?;
                    *observed.borrow_mut() = Some((count, json));
                }
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(result.deleted, ["photo.jpeg", "second.png"]);
        assert!(result.error.is_none());
        let (count, json) = observed.into_inner().unwrap();
        assert_eq!(count, 4); // Three media files and one shared journal.
        assert_eq!(json, before);
        assert_cache_cleaned(dir.path());
        assert!(!dir.path().join("second.png").exists());
        assert!(!metadata::read_album_meta_checked(dir.path())
            .unwrap()
            .files
            .contains_key("second.png"));
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
    }
    #[test]
    fn batch_reports_a_successful_prefix_and_stops_at_the_first_missing_photo() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        cache_fixture(dir.path());
        let result = delete_batch_with(
            dir.path(),
            &targets(&["photo.jpeg", "missing.png", "other.jpeg"]),
            &[],
            |_| Ok(()),
        )
        .unwrap();
        assert_eq!(result.deleted, ["photo.jpeg"]);
        assert!(result.error.is_some());
        assert_cache_cleaned(dir.path());
        assert!(dir.path().join("other.jpeg").is_file());
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
    }
    #[test]
    fn a_staging_error_rolls_back_the_whole_chunk_and_keeps_its_caches() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        cache_fixture(dir.path());
        let before = fs::read(dir.path().join(".room237-metadata/album.sqlite")).unwrap();
        let result = delete_batch_with(
            dir.path(),
            &targets(&["photo.jpeg", "other.jpeg"]),
            &[],
            |phase| {
                if phase == "staged" {
                    Err("injected staging error".into())
                } else {
                    Ok(())
                }
            },
        );
        assert!(result.err().unwrap().contains("injected"));
        assert_restored(dir.path());
        assert_eq!(
            fs::read(dir.path().join(".room237-metadata/album.sqlite")).unwrap(),
            before
        );
        assert_eq!(
            fs::read(dir.path().join(".room237-thumb/photo.jpeg.webp")).unwrap(),
            b"cached preview"
        );
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
    }
    #[test]
    fn an_external_change_to_an_unstaged_file_does_not_block_rollback_recovery() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        let changed = std::cell::Cell::new(false);
        let result = delete_batch_with(
            dir.path(),
            &targets(&["photo.jpeg", "other.jpeg"]),
            &[],
            |phase| {
                if phase == "staged" && !changed.replace(true) {
                    fs::write(dir.path().join("other.jpeg"), b"foreign replacement")
                        .map_err(|e| e.to_string())?;
                }
                Ok(())
            },
        );
        assert!(result.err().unwrap().contains("changed"));
        assert_eq!(
            fs::read(dir.path().join("photo.jpeg")).unwrap(),
            b"converted JPEG"
        );
        assert_eq!(
            fs::read(dir.path().join("photo.heic")).unwrap(),
            b"original HEIC"
        );
        assert_eq!(
            fs::read(dir.path().join("other.jpeg")).unwrap(),
            b"foreign replacement"
        );
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
        recover_album(dir.path()).unwrap();
    }
    #[test]
    fn batch_deduplicates_a_selected_raw_and_its_jpeg() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        let result = delete_batch_with(
            dir.path(),
            &targets(&["photo.jpeg", "photo.heic", "photo.jpeg"]),
            &[],
            |_| Ok(()),
        )
        .unwrap();
        assert_eq!(result.deleted, ["photo.jpeg", "photo.heic"]);
        assert!(result.error.is_none());
        assert!(!dir.path().join("photo.heic").exists());
        assert!(!dir.path().join("photo.jpeg").exists());
        assert_eq!(
            fs::read(dir.path().join("other.jpeg")).unwrap(),
            b"other JPEG"
        );
    }
    #[test]
    fn guarded_batch_rechecks_all_targets_and_keeps_its_retained_file() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        cache_fixture(dir.path());
        fs::write(dir.path().join("second.png"), b"second photo").unwrap();
        let result = delete_batch_with(
            dir.path(),
            &guarded_targets(dir.path(), &["photo.jpeg", "second.png"]),
            &[observe(&dir.path().join("other.jpeg"))],
            |_| Ok(()),
        )
        .unwrap();
        assert_eq!(result.deleted, ["photo.jpeg", "second.png"]);
        assert!(result.error.is_none());
        assert_cache_cleaned(dir.path());
        assert_eq!(
            fs::read(dir.path().join("other.jpeg")).unwrap(),
            b"other JPEG"
        );
    }
    #[test]
    fn guarded_batch_preserves_a_changed_target_and_reports_its_committed_prefix() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        fs::write(dir.path().join("second.png"), b"second photo").unwrap();
        let requested = guarded_targets(dir.path(), &["photo.jpeg", "second.png"]);
        fs::write(dir.path().join("second.png"), b"changed second photo").unwrap();
        let result = delete_batch_with(
            dir.path(),
            &requested,
            &[observe(&dir.path().join("other.jpeg"))],
            |_| Ok(()),
        )
        .unwrap();
        assert_eq!(result.deleted, ["photo.jpeg"]);
        assert!(result.error.unwrap().contains("changed"));
        assert_eq!(
            fs::read(dir.path().join("second.png")).unwrap(),
            b"changed second photo"
        );
        assert!(!dir.path().join("photo.jpeg").exists());
        assert!(!dir.path().join("photo.heic").exists());
    }
    #[test]
    fn guarded_batch_rolls_back_all_targets_if_a_retained_file_changes() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        fs::write(dir.path().join("second.png"), b"second photo").unwrap();
        let result = delete_batch_with(
            dir.path(),
            &guarded_targets(dir.path(), &["photo.jpeg", "second.png"]),
            &[observe(&dir.path().join("other.jpeg"))],
            |phase| {
                if phase == "staged" {
                    fs::write(dir.path().join("other.jpeg"), b"changed anchor")
                        .map_err(|e| e.to_string())?;
                }
                Ok(())
            },
        );
        assert!(result.err().unwrap().contains("Retained"));
        assert_eq!(
            fs::read(dir.path().join("photo.jpeg")).unwrap(),
            b"converted JPEG"
        );
        assert_eq!(
            fs::read(dir.path().join("photo.heic")).unwrap(),
            b"original HEIC"
        );
        assert_eq!(
            fs::read(dir.path().join("second.png")).unwrap(),
            b"second photo"
        );
        assert_eq!(
            fs::read(dir.path().join("other.jpeg")).unwrap(),
            b"changed anchor"
        );
    }
    #[test]
    fn guarded_batch_cannot_use_a_companion_that_it_would_delete_as_a_retained_file() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        assert!(delete_batch_with(
            dir.path(),
            &guarded_targets(dir.path(), &["photo.jpeg"]),
            &[observe(&dir.path().join("photo.heic"))],
            |_| Ok(())
        )
        .is_err());
        assert_restored(dir.path());
    }
    #[test]
    fn guarded_deletion_without_an_actual_retained_file_is_rejected() {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        assert!(delete_batch_with(
            dir.path(),
            &guarded_targets(dir.path(), &["photo.jpeg"]),
            &[observe(&dir.path().join("photo.jpeg"))],
            |_| Ok(())
        )
        .is_err());
        assert_restored(dir.path());
    }
    #[test]
    fn batch_rejects_invalid_bounds_names_and_mixed_revisions_before_deleting() {
        let dir = tempfile::tempdir().unwrap();
        let photo = dir.path().join("photo.png");
        fs::write(&photo, b"photo").unwrap();
        for invalid in [
            targets(&["photo.png", "../outside.png"]),
            targets(&["photo.png", ".room237-metadata.lock"]),
            vec![
                DeleteTarget {
                    name: "photo.png".into(),
                    revision: Some("one".into()),
                },
                DeleteTarget {
                    name: "photo.png".into(),
                    revision: Some("two".into()),
                },
            ],
            vec![
                DeleteTarget {
                    name: "photo.png".into(),
                    revision: Some("one".into()),
                },
                DeleteTarget {
                    name: "other.png".into(),
                    revision: None,
                },
            ],
            vec![
                DeleteTarget {
                    name: "photo.png".into(),
                    revision: None
                };
                DELETE_BATCH_LIMIT + 1
            ],
        ] {
            assert!(delete_batch_with(dir.path(), &invalid, &[], |_| Ok(())).is_err());
            assert_eq!(fs::read(&photo).unwrap(), b"photo");
            assert!(!dir.path().join(".room237-thumb").exists());
        }
    }
    #[test]
    fn an_empty_batch_does_not_create_an_album_or_service_directory() {
        let dir = tempfile::tempdir().unwrap();
        let album = dir.path().join("absent");
        let result = delete_batch_with(&album, &[], &[], |_| Ok(())).unwrap();
        assert!(result.deleted.is_empty());
        assert!(result.error.is_none());
        assert!(!album.exists());
    }
    fn fixture(album: &Path) -> PathBuf {
        let original = album.join("photo.heic");
        let jpeg = album.join("photo.jpeg");
        fs::write(&original, b"original HEIC").unwrap();
        fs::write(&jpeg, b"converted JPEG").unwrap();
        fs::write(album.join("other.jpeg"), b"other JPEG").unwrap();
        let binding = metadata::HeicConversion {
            output_name: "photo.jpeg".into(),
            source_revision: owned_revision(&original).unwrap(),
            output_revision: owned_revision(&jpeg).unwrap(),
        };
        metadata::update_file_meta(album, "photo.heic", |entry| {
            entry.heic_conversion = Some(binding)
        })
        .unwrap();
        jpeg
    }
    fn assert_restored(album: &Path) {
        assert_eq!(
            fs::read(album.join("photo.heic")).unwrap(),
            b"original HEIC"
        );
        assert_eq!(
            fs::read(album.join("photo.jpeg")).unwrap(),
            b"converted JPEG"
        );
        assert_eq!(fs::read(album.join("other.jpeg")).unwrap(), b"other JPEG");
    }
    fn cache_fixture(album: &Path) {
        fs::create_dir_all(album.join(".room237-thumb")).unwrap();
        metadata::update_album_meta(album, |snapshot| {
            for name in ["photo.heic", "photo.jpeg", "other.jpeg"] {
                let entry = snapshot.files.entry(name.into()).or_default();
                entry.favorite = true;
                entry.meta = Some("0".into());
                entry.hash = Some("cached hash".into());
            }
            snapshot.duplicates_ignore = vec![
                ["photo.jpeg".into(), "other.jpeg".into()],
                ["other.jpeg".into(), "retained.jpeg".into()],
            ];
        })
        .unwrap();
        for name in ["photo.heic", "photo.jpeg", "other.jpeg"] {
            fs::write(
                crate::thumb::thumb_path(&album.join(name), &album.join(".room237-thumb")).unwrap(),
                b"cached preview",
            )
            .unwrap();
            fs::write(
                album.join(".room237-metadata").join(format!("{name}.meta")),
                br#"{"favorite":true,"meta":"0"}"#,
            )
            .unwrap();
        }
    }
    fn assert_cache_cleaned(album: &Path) {
        let snapshot = metadata::read_album_meta_checked(album).unwrap();
        for name in ["photo.heic", "photo.jpeg"] {
            assert!(
                !snapshot.files.contains_key(name),
                "deleted metadata remains for {name}"
            );
            assert!(
                !crate::thumb::thumb_path(&album.join(name), &album.join(".room237-thumb"))
                    .unwrap()
                    .exists()
            );
            assert!(!album
                .join(".room237-metadata")
                .join(format!("{name}.meta"))
                .exists());
        }
        assert!(snapshot.files["other.jpeg"].favorite);
        assert_eq!(
            snapshot.files["other.jpeg"].hash.as_deref(),
            Some("cached hash")
        );
        assert_eq!(
            snapshot.duplicates_ignore,
            vec![["other.jpeg".to_owned(), "retained.jpeg".to_owned()]]
        );
        assert_eq!(
            fs::read(album.join(".room237-thumb/other.jpeg.webp")).unwrap(),
            b"cached preview"
        );
        assert!(album.join(".room237-metadata/other.jpeg.meta").is_file());
    }
    #[test]
    fn committed_deletion_removes_its_metadata_and_previews_only() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        cache_fixture(dir.path());
        delete_media(jpeg.to_string_lossy().into()).unwrap();
        assert_cache_cleaned(dir.path());
        assert!(dir.path().join(".room237-metadata.lock").is_file());
        assert!(
            crate::thumb::thumbnail_maintenance_lock_path(&dir.path().join(".room237-thumb"))
                .is_file()
        );
    }
    #[test]
    fn failed_deletion_preserves_metadata_and_previews() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        cache_fixture(dir.path());
        let before = fs::read(dir.path().join(".room237-metadata/album.sqlite")).unwrap();
        assert!(delete_with(&jpeg, None, &[], |phase| {
            if phase == "before_commit" {
                Err("injected failure".into())
            } else {
                Ok(())
            }
        })
        .is_err());
        assert_restored(dir.path());
        assert_eq!(
            fs::read(dir.path().join(".room237-metadata/album.sqlite")).unwrap(),
            before
        );
        assert_eq!(
            fs::read(dir.path().join(".room237-thumb/photo.jpeg.webp")).unwrap(),
            b"cached preview"
        );
        assert!(dir
            .path()
            .join(".room237-metadata/photo.heic.meta")
            .exists());
    }
    #[test]
    fn failed_preview_cleanup_is_retained_in_the_journal_and_retried() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        cache_fixture(dir.path());
        let preview = dir.path().join(".room237-thumb/photo.jpeg.webp");
        delete_with(&jpeg, None, &[], |phase| {
            if phase == "committed" {
                fs::remove_file(&preview).map_err(|e| e.to_string())?;
                fs::create_dir(&preview).map_err(|e| e.to_string())?;
            }
            Ok(())
        })
        .unwrap();
        assert!(!jpeg.exists());
        assert_eq!(staging_dirs(dir.path()).unwrap().len(), 1);
        assert!(recover_album(dir.path())
            .unwrap_err()
            .contains("not a file"));
        assert!(preview.is_dir());
        fs::remove_dir(preview).unwrap();
        recover_album(dir.path()).unwrap();
        assert_cache_cleaned(dir.path());
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
        recover_album(dir.path()).unwrap();
    }
    #[test]
    fn failed_metadata_cleanup_retries_after_previews_are_already_removed() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        cache_fixture(dir.path());
        let json = dir.path().join(".room237-metadata/album.sqlite");
        let backup = json.with_extension("backup");
        delete_with(&jpeg, None, &[], |phase| {
            if phase == "committed" {
                fs::rename(&json, &backup).map_err(|e| e.to_string())?;
                fs::create_dir(&json).map_err(|e| e.to_string())?;
            }
            Ok(())
        })
        .unwrap();
        assert!(!jpeg.exists());
        assert_eq!(staging_dirs(dir.path()).unwrap().len(), 1);
        assert!(!dir.path().join(".room237-thumb/photo.jpeg.webp").exists());
        assert!(!dir
            .path()
            .join(".room237-metadata/photo.jpeg.meta")
            .exists());
        fs::remove_dir(&json).unwrap();
        fs::rename(&backup, &json).unwrap();
        recover_album(dir.path()).unwrap();
        assert_cache_cleaned(dir.path());
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
    }
    #[test]
    fn committed_cleanup_preserves_a_new_photo_and_its_metadata_at_the_same_path() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        cache_fixture(dir.path());
        let mut replacement = (*metadata::read_album_meta_checked(dir.path()).unwrap()).clone();
        replacement.files.get_mut("photo.jpeg").unwrap().favorite = false;
        replacement.files.get_mut("photo.jpeg").unwrap().hash = Some("replacement hash".into());
        delete_with(&jpeg, None, &[], |phase| {
            if phase == "committed" {
                fs::write(&jpeg, b"new photo").map_err(|e| e.to_string())?;
                fs::write(
                    dir.path().join(".room237-thumb/photo.jpeg.webp"),
                    b"new preview",
                )
                .map_err(|e| e.to_string())?;
                fs::write(
                    dir.path().join(".room237-metadata/photo.jpeg.meta"),
                    br#"{"favorite":false}"#,
                )
                .map_err(|e| e.to_string())?;
                crate::album_db::replace(
                    &dir.path().join(".room237-metadata/album.sqlite"),
                    &replacement,
                )?;
            }
            Ok(())
        })
        .unwrap();
        assert_eq!(fs::read(jpeg).unwrap(), b"new photo");
        assert_eq!(
            fs::read(dir.path().join(".room237-thumb/photo.jpeg.webp")).unwrap(),
            b"new preview"
        );
        assert_eq!(
            fs::read(dir.path().join(".room237-metadata/photo.jpeg.meta")).unwrap(),
            br#"{"favorite":false}"#
        );
        let snapshot = metadata::read_album_meta_checked(dir.path()).unwrap();
        assert!(!snapshot.files["photo.jpeg"].favorite);
        assert_eq!(
            snapshot.files["photo.jpeg"].hash.as_deref(),
            Some("replacement hash")
        );
        assert!(!snapshot.files.contains_key("photo.heic"));
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
    }
    #[test]
    fn deletion_cleans_hashed_previews_of_maximum_length_names() {
        let dir = tempfile::tempdir().unwrap();
        let name = format!("{}.jpeg", "a".repeat(250));
        let photo = dir.path().join(&name);
        fs::write(&photo, b"photo").unwrap();
        fs::create_dir(dir.path().join(".room237-thumb")).unwrap();
        let preview = crate::thumb::thumb_path(&photo, &dir.path().join(".room237-thumb")).unwrap();
        fs::write(&preview, b"cached preview").unwrap();
        metadata::update_file_meta(dir.path(), &name, |entry| entry.favorite = true).unwrap();
        delete_media(photo.to_string_lossy().into()).unwrap();
        assert!(!preview.exists());
        assert!(!metadata::read_album_meta_checked(dir.path())
            .unwrap()
            .files
            .contains_key(&name));
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
    }
    #[test]
    fn deletion_cleans_legacy_metadata_without_resurrecting_it_on_migration() {
        let dir = tempfile::tempdir().unwrap();
        let photo = dir.path().join("photo.png");
        fs::write(&photo, b"photo").unwrap();
        let cache = dir.path().join(".room237-metadata");
        fs::create_dir(&cache).unwrap();
        fs::write(
            cache.join("photo.png.meta"),
            br#"{"favorite":true,"meta":"0"}"#,
        )
        .unwrap();
        fs::write(cache.join("other.png.meta"), br#"{"favorite":true}"#).unwrap();
        delete_media(photo.to_string_lossy().into()).unwrap();
        assert!(!cache.join("photo.png.meta").exists());
        let snapshot = metadata::read_album_meta_checked(dir.path()).unwrap();
        assert!(!snapshot.files.contains_key("photo.png"));
        assert!(snapshot.files["other.png"].favorite);
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
    }
    #[test]
    #[cfg(unix)]
    fn cleanup_does_not_follow_replaced_service_directory_symlinks() {
        for directory in [".room237-thumb", ".room237-metadata"] {
            let dir = tempfile::tempdir().unwrap();
            let jpeg = fixture(dir.path());
            cache_fixture(dir.path());
            let outside = tempfile::tempdir().unwrap();
            let foreign = if directory == ".room237-thumb" {
                "photo.jpeg.webp"
            } else {
                "photo.jpeg.meta"
            };
            fs::write(outside.path().join(foreign), b"foreign data").unwrap();
            let service = dir.path().join(directory);
            let backup = dir.path().join(".room237-kept-cache");
            delete_with(&jpeg, None, &[], |phase| {
                if phase == "committed" {
                    fs::rename(&service, &backup).map_err(|e| e.to_string())?;
                    std::os::unix::fs::symlink(outside.path(), &service)
                        .map_err(|e| e.to_string())?;
                }
                Ok(())
            })
            .unwrap();
            assert_eq!(
                fs::read(outside.path().join(foreign)).unwrap(),
                b"foreign data"
            );
            assert_eq!(staging_dirs(dir.path()).unwrap().len(), 1);
            fs::remove_file(&service).unwrap();
            fs::rename(&backup, &service).unwrap();
            recover_album(dir.path()).unwrap();
            assert_cache_cleaned(dir.path());
        }
    }
    #[test]
    fn deletion_removes_the_verified_pair_and_preserves_other_photos() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        delete_media(jpeg.to_string_lossy().into()).unwrap();
        assert!(!jpeg.exists());
        assert!(!dir.path().join("photo.heic").exists());
        assert_eq!(
            fs::read(dir.path().join("other.jpeg")).unwrap(),
            b"other JPEG"
        );
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
        recover_album(dir.path()).unwrap();
        assert!(!jpeg.exists());
    }
    #[test]
    fn unrelated_jpeg_does_not_delete_a_same_stem_heic() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(dir.path().join("photo.heic"), b"original").unwrap();
        fs::write(dir.path().join("photo.jpeg"), b"unrelated JPEG").unwrap();
        delete_media(dir.path().join("photo.jpeg").to_string_lossy().into()).unwrap();
        assert_eq!(
            fs::read(dir.path().join("photo.heic")).unwrap(),
            b"original"
        );
    }
    #[test]
    fn changed_heic_is_preserved_when_its_old_jpeg_is_deleted() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        fs::write(dir.path().join("photo.heic"), b"foreign replacement").unwrap();
        delete_media(jpeg.to_string_lossy().into()).unwrap();
        assert_eq!(
            fs::read(dir.path().join("photo.heic")).unwrap(),
            b"foreign replacement"
        );
    }
    #[test]
    fn changed_jpeg_does_not_claim_the_old_heic_binding() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        fs::write(&jpeg, b"foreign JPEG replacement").unwrap();
        delete_media(jpeg.to_string_lossy().into()).unwrap();
        assert_eq!(
            fs::read(dir.path().join("photo.heic")).unwrap(),
            b"original HEIC"
        );
    }
    #[test]
    fn failure_before_commit_restores_both_original_objects() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        let before = owned_revision(&jpeg);
        let result = delete_with(&jpeg, None, &[], |phase| {
            if phase == "before_commit" {
                Err("injected error".into())
            } else {
                Ok(())
            }
        });
        assert!(result.unwrap_err().contains("injected"));
        assert_restored(dir.path());
        assert_eq!(owned_revision(&jpeg), before);
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
    }
    #[test]
    fn changed_retained_duplicate_aborts_and_restores_the_selected_pair() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        let anchor = dir.path().join("other.jpeg");
        let revision = metadata::media_revision(&jpeg);
        let observed = vec![
            ObservedMedia {
                path: jpeg.to_string_lossy().into(),
                revision: revision.clone(),
            },
            ObservedMedia {
                path: anchor.to_string_lossy().into(),
                revision: metadata::media_revision(&anchor),
            },
        ];
        let result = delete_with(&jpeg, Some(&revision), &observed, |phase| {
            if phase == "staged" {
                fs::write(&anchor, b"changed anchor").unwrap();
            }
            Ok(())
        });
        assert!(result.unwrap_err().contains("Retained duplicate changed"));
        assert_eq!(fs::read(jpeg).unwrap(), b"converted JPEG");
        assert_eq!(
            fs::read(dir.path().join("photo.heic")).unwrap(),
            b"original HEIC"
        );
        assert_eq!(fs::read(anchor).unwrap(), b"changed anchor");
    }
    #[test]
    fn corrupt_metadata_prevents_deleting_either_file() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        fs::write(
            dir.path().join(".room237-metadata/album.sqlite"),
            b"{broken",
        )
        .unwrap();
        assert!(delete_media(jpeg.to_string_lossy().into()).is_err());
        assert_restored(dir.path());
    }
    #[test]
    fn rollback_preserves_a_foreign_path_and_can_be_retried() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        let result = delete_with(&jpeg, None, &[], |phase| {
            if phase == "before_commit" {
                fs::write(&jpeg, b"foreign file").unwrap();
                return Err("injected error".into());
            }
            Ok(())
        });
        assert!(result.unwrap_err().contains("rollback"));
        assert_eq!(fs::read(&jpeg).unwrap(), b"foreign file");
        let staging = staging_dirs(dir.path()).unwrap().pop().unwrap();
        assert_eq!(
            fs::read(staging.join("photo.jpeg")).unwrap(),
            b"converted JPEG"
        );
        fs::remove_file(&jpeg).unwrap();
        recover_album(dir.path()).unwrap();
        assert_restored(dir.path());
    }
    #[test]
    fn deletion_crash_child() {
        let Ok(root) = std::env::var("ROOM237_DELETE_CRASH_ROOT") else {
            return;
        };
        let phase = std::env::var("ROOM237_DELETE_CRASH_PHASE").unwrap();
        if let Ok(mode) = std::env::var("ROOM237_DELETE_CRASH_BATCH") {
            let album = Path::new(&root);
            let requested = if mode == "guarded" {
                guarded_targets(album, &["photo.jpeg", "second.png"])
            } else {
                targets(&["photo.jpeg", "second.png"])
            };
            let observed = if mode == "guarded" {
                vec![observe(&album.join("other.jpeg"))]
            } else {
                Vec::new()
            };
            delete_batch_with(album, &requested, &observed, |current| {
                if current == phase {
                    std::process::exit(17);
                }
                Ok(())
            })
            .unwrap();
            panic!("batch child failed to reach checkpoint");
        }
        delete_with(&Path::new(&root).join("photo.jpeg"), None, &[], |current| {
            if current == phase {
                std::process::exit(17);
            }
            Ok(())
        })
        .unwrap();
        panic!("child failed to reach checkpoint");
    }
    #[test]
    fn waiting_for_an_album_lease_does_not_block_deletion_in_another_album() {
        let a = tempfile::tempdir().unwrap();
        let b = tempfile::tempdir().unwrap();
        let first = fixture(a.path());
        let second = fixture(b.path());
        fs::create_dir(a.path().join(".room237-thumb")).unwrap();
        let lease = metadata::lock_album_metadata(a.path()).unwrap();
        let (first_send, first_recv) = std::sync::mpsc::channel();
        let first_thread = std::thread::spawn(move || {
            first_send
                .send(delete_media(first.to_string_lossy().into()))
                .unwrap();
        });
        let maintenance =
            crate::thumb::thumbnail_maintenance_lock_path(&a.path().join(".room237-thumb"));
        let start = std::time::Instant::now();
        loop {
            match FileLock::try_acquire(&maintenance).unwrap() {
                None => break,
                Some(lock) => drop(lock),
            }
            assert!(start.elapsed() < Duration::from_secs(5));
            std::thread::sleep(Duration::from_millis(5));
        }
        let (send, recv) = std::sync::mpsc::channel();
        let thread = std::thread::spawn(move || {
            send.send(delete_media(second.to_string_lossy().into()))
                .unwrap();
        });
        let independent = recv.recv_timeout(Duration::from_secs(2));
        assert!(first_recv.try_recv().is_err());
        drop(lease);
        first_recv
            .recv_timeout(Duration::from_secs(5))
            .unwrap()
            .unwrap();
        first_thread.join().unwrap();
        thread.join().unwrap();
        independent
            .expect("unrelated deletion waited for the busy album")
            .unwrap();
    }
    #[test]
    fn recovery_rejects_path_traversal_and_preserves_outside_files() {
        let root = tempfile::tempdir().unwrap();
        let album = root.path().join("album");
        fs::create_dir(&album).unwrap();
        let outside = root.path().join("outside.jpeg");
        fs::write(&outside, b"outside").unwrap();
        let staging = album.join(format!("{PREFIX}fixture"));
        fs::create_dir(&staging).unwrap();
        write_journal(
            &staging,
            &Journal {
                version: 1,
                committed: true,
                files: vec![Entry {
                    name: "../outside.jpeg".into(),
                    revision: owned_revision(&outside).unwrap(),
                }],
            },
        )
        .unwrap();
        assert!(recover_album(&album).is_err());
        assert_eq!(fs::read(outside).unwrap(), b"outside");
    }
    #[test]
    fn recovery_bounds_journal_reads_and_preserves_oversized_journals() {
        let dir = tempfile::tempdir().unwrap();
        let staging = dir.path().join(format!("{PREFIX}fixture"));
        fs::create_dir(&staging).unwrap();
        let journal = fs::File::create(staging.join(JOURNAL)).unwrap();
        journal.set_len(JOURNAL_LIMIT + 1).unwrap();
        assert!(recover_album(dir.path()).unwrap_err().contains("too large"));
        assert_eq!(journal.metadata().unwrap().len(), JOURNAL_LIMIT + 1);
    }
    #[test]
    fn recovery_cleans_empty_directories_left_after_journal_cleanup() {
        let dir = tempfile::tempdir().unwrap();
        let staging = dir.path().join(format!("{PREFIX}fixture"));
        fs::create_dir(&staging).unwrap();
        recover_album(dir.path()).unwrap();
        assert!(!staging.exists());
    }
    #[test]
    fn maximum_length_media_name_can_be_deleted_without_truncation() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(format!("{}.jpeg", "a".repeat(250)));
        fs::write(&path, b"original").unwrap();
        delete_media(path.to_string_lossy().into()).unwrap();
        assert!(!path.exists());
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
    }
    #[test]
    fn hidden_media_with_a_service_like_prefix_can_be_deleted() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(".room237-photo.png");
        fs::write(&path, b"original photo").unwrap();
        delete_media(path.to_string_lossy().into()).unwrap();
        assert!(!path.exists());
    }
    #[test]
    fn service_files_cannot_be_deleted_as_media() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(".room237-metadata.lock");
        fs::write(&path, b"service data").unwrap();
        assert!(delete_media(path.to_string_lossy().into()).is_err());
        assert_eq!(fs::read(path).unwrap(), b"service data");
    }
    fn crash_and_recover(phase: &str, committed: bool) {
        let dir = tempfile::tempdir().unwrap();
        fixture(dir.path());
        cache_fixture(dir.path());
        let mut command = std::process::Command::new(std::env::current_exe().unwrap());
        command
            .args([
                "--exact",
                "media_delete::tests::deletion_crash_child",
                "--nocapture",
            ])
            .env("ROOM237_DELETE_CRASH_ROOT", dir.path())
            .env("ROOM237_DELETE_CRASH_PHASE", phase);
        command
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null());
        let mut child = command.spawn().unwrap();
        let status = util::wait_with_timeout(&mut child, Duration::from_secs(20)).unwrap();
        assert_eq!(status.code(), Some(17));
        assert_eq!(staging_dirs(dir.path()).unwrap().len(), 1);
        recover_album(dir.path()).unwrap();
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
        if committed {
            assert!(!dir.path().join("photo.jpeg").exists());
            assert!(!dir.path().join("photo.heic").exists());
            assert_eq!(
                fs::read(dir.path().join("other.jpeg")).unwrap(),
                b"other JPEG"
            );
            assert_cache_cleaned(dir.path());
        } else {
            assert_restored(dir.path());
            assert!(dir.path().join(".room237-thumb/photo.jpeg.webp").is_file());
            assert!(
                metadata::read_album_meta_checked(dir.path()).unwrap().files["photo.jpeg"].favorite
            );
        }
    }
    #[test]
    fn process_exit_after_first_rename_rolls_back_on_recovery() {
        crash_and_recover("staged", false);
    }
    #[test]
    fn process_exit_after_all_renames_rolls_back_on_recovery() {
        crash_and_recover("before_commit", false);
    }
    #[test]
    fn process_exit_after_commit_finishes_deletion_on_recovery() {
        crash_and_recover("committed", true);
    }
    #[test]
    fn process_exit_after_media_cleanup_finishes_artifact_cleanup_on_recovery() {
        crash_and_recover("after_media_cleanup", true);
    }
    #[test]
    fn process_exit_after_artifact_cleanup_is_idempotent_on_recovery() {
        crash_and_recover("after_artifact_cleanup", true);
    }
    fn batch_crash_and_recover(phase: &str, committed: bool) {
        for mode in ["plain", "guarded"] {
            let dir = tempfile::tempdir().unwrap();
            fixture(dir.path());
            cache_fixture(dir.path());
            fs::write(dir.path().join("second.png"), b"second photo").unwrap();
            fs::write(
                dir.path().join(".room237-thumb/second.png.webp"),
                b"second preview",
            )
            .unwrap();
            fs::write(
                dir.path().join(".room237-metadata/second.png.meta"),
                br#"{"favorite":true}"#,
            )
            .unwrap();
            metadata::update_file_meta(dir.path(), "second.png", |entry| entry.favorite = true)
                .unwrap();
            let mut command = std::process::Command::new(std::env::current_exe().unwrap());
            command
                .args([
                    "--exact",
                    "media_delete::tests::deletion_crash_child",
                    "--nocapture",
                ])
                .env("ROOM237_DELETE_CRASH_ROOT", dir.path())
                .env("ROOM237_DELETE_CRASH_PHASE", phase)
                .env("ROOM237_DELETE_CRASH_BATCH", mode)
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null());
            let mut child = command.spawn().unwrap();
            assert_eq!(
                util::wait_with_timeout(&mut child, Duration::from_secs(20))
                    .unwrap()
                    .code(),
                Some(17)
            );
            assert_eq!(staging_dirs(dir.path()).unwrap().len(), 1);
            recover_album(dir.path()).unwrap();
            assert!(staging_dirs(dir.path()).unwrap().is_empty());
            if committed {
                assert_cache_cleaned(dir.path());
                assert!(!dir.path().join("photo.jpeg").exists());
                assert!(!dir.path().join("photo.heic").exists());
                assert!(!dir.path().join("second.png").exists());
                assert!(!dir.path().join(".room237-thumb/second.png.webp").exists());
                assert!(!dir
                    .path()
                    .join(".room237-metadata/second.png.meta")
                    .exists());
                assert!(!metadata::read_album_meta_checked(dir.path())
                    .unwrap()
                    .files
                    .contains_key("second.png"));
            } else {
                assert_restored(dir.path());
                assert_eq!(
                    fs::read(dir.path().join("second.png")).unwrap(),
                    b"second photo"
                );
                assert_eq!(
                    fs::read(dir.path().join(".room237-thumb/second.png.webp")).unwrap(),
                    b"second preview"
                );
                assert!(dir
                    .path()
                    .join(".room237-metadata/second.png.meta")
                    .is_file());
                assert!(
                    metadata::read_album_meta_checked(dir.path()).unwrap().files["second.png"]
                        .favorite
                );
            }
        }
    }
    #[test]
    fn batch_process_exit_after_first_rename_restores_all_selected_files() {
        batch_crash_and_recover("staged", false);
    }
    #[test]
    fn batch_process_exit_before_commit_restores_all_selected_files() {
        batch_crash_and_recover("before_commit", false);
    }
    #[test]
    fn batch_process_exit_after_commit_finishes_all_selected_deletions() {
        batch_crash_and_recover("committed", true);
    }
    #[test]
    fn batch_process_exit_after_media_cleanup_finishes_all_cache_cleanup() {
        batch_crash_and_recover("after_media_cleanup", true);
    }
    #[test]
    fn batch_process_exit_after_cache_cleanup_recovers_idempotently() {
        batch_crash_and_recover("after_artifact_cleanup", true);
    }
    #[test]
    #[ignore = "large physical album benchmark"]
    fn benchmark_deletion_with_artifact_cleanup_in_a_large_album() {
        for sample in 0..3 {
            for batched in if sample % 2 == 0 {
                [false, true]
            } else {
                [true, false]
            } {
                let dir = tempfile::tempdir().unwrap();
                let album = dir.path().canonicalize().unwrap();
                fs::create_dir(album.join(".room237-thumb")).unwrap();
                metadata::update_album_meta(&album, |snapshot| {
                    for index in 0..10_000 {
                        let name = format!("{index:05}.jpg");
                        fs::write(album.join(&name), b"fixture photo").unwrap();
                        snapshot.files.insert(
                            name.clone(),
                            metadata::FileMetaEntry {
                                favorite: index % 2 == 0,
                                meta: Some("0".into()),
                                ..Default::default()
                            },
                        );
                        if index < 100 {
                            fs::write(
                                album.join(".room237-thumb").join(format!("{name}.webp")),
                                b"fixture preview",
                            )
                            .unwrap();
                        }
                    }
                })
                .unwrap();
                let start = std::time::Instant::now();
                if batched {
                    let requested: Vec<_> = (0..100)
                        .map(|index| DeleteTarget {
                            name: format!("{index:05}.jpg"),
                            revision: None,
                        })
                        .collect();
                    let result =
                        delete_media_batch(album.to_string_lossy().into(), requested, Vec::new())
                            .unwrap();
                    assert!(result.error.is_none());
                    assert_eq!(result.deleted.len(), 100);
                } else {
                    for index in 0..100 {
                        delete_media(
                            album
                                .join(format!("{index:05}.jpg"))
                                .to_string_lossy()
                                .into(),
                        )
                        .unwrap();
                    }
                }
                eprintln!("deletion cleanup paired sample={sample}, batched={batched}, actual_media_files=10000, metadata_rows=10000, deletions=100, elapsed={:?}", start.elapsed());
                let snapshot = metadata::read_album_meta_checked(&album).unwrap();
                assert_eq!(snapshot.files.len(), 9900);
                for index in 0..100 {
                    let name = format!("{index:05}.jpg");
                    assert!(!album.join(&name).exists());
                    assert!(!album
                        .join(".room237-thumb")
                        .join(format!("{name}.webp"))
                        .exists());
                    assert!(!snapshot.files.contains_key(&name));
                }
                for index in [100, 101, 9999] {
                    let name = format!("{index:05}.jpg");
                    assert_eq!(fs::read(album.join(&name)).unwrap(), b"fixture photo");
                    assert_eq!(snapshot.files[&name].favorite, index % 2 == 0);
                }
                assert!(staging_dirs(&album).unwrap().is_empty());
            }
        }
    }
    #[test]
    fn recovery_preserves_unknown_staging_files_without_blocking_restored_media() {
        let dir = tempfile::tempdir().unwrap();
        let jpeg = fixture(dir.path());
        let result = delete_with(&jpeg, None, &[], |phase| {
            if phase == "before_commit" {
                let staging = staging_dirs(dir.path()).unwrap().pop().unwrap();
                fs::write(staging.join("foreign.txt"), b"foreign data").unwrap();
                return Err("injected".into());
            }
            Ok(())
        });
        assert!(result.is_err());
        assert_restored(dir.path());
        assert!(staging_dirs(dir.path()).unwrap().is_empty());
        let orphan = fs::read_dir(dir.path())
            .unwrap()
            .flatten()
            .find(|entry| {
                entry
                    .file_name()
                    .to_string_lossy()
                    .starts_with(".room237-orphan-delete-")
            })
            .unwrap()
            .path();
        assert_eq!(
            fs::read(orphan.join("foreign.txt")).unwrap(),
            b"foreign data"
        );
    }
    #[test]
    #[cfg(unix)]
    fn deleting_a_relative_symlink_preserves_its_target() {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("target.png");
        fs::write(&target, b"target").unwrap();
        std::os::unix::fs::symlink("target.png", dir.path().join("link.png")).unwrap();
        delete_media(dir.path().join("link.png").to_string_lossy().into()).unwrap();
        assert!(fs::symlink_metadata(dir.path().join("link.png")).is_err());
        assert_eq!(fs::read(target).unwrap(), b"target");
    }
}

#[cfg(test)]
mod native_selection_tests {
    use super::*;
    #[test]
    fn ordinary_native_selection_can_delete_all_but_duplicate_guard_still_requires_a_keeper() {
        let dir=tempfile::tempdir().unwrap();let path=dir.path().join("generated.png");fs::write(&path,b"fixture").unwrap();
        let revision=metadata::media_revision(&path);
        assert!(delete_media_batch(dir.path().to_string_lossy().into(),vec![DeleteTarget{name:"generated.png".into(),revision:Some(revision.clone())}],Vec::new()).is_err());
        assert!(path.exists());
        let result=native_delete_batch(dir.path().to_string_lossy().into(),vec![DeleteTarget{name:"generated.png".into(),revision:Some(revision)}]).unwrap();
        assert_eq!(result.deleted,["generated.png"]);assert!(result.error.is_none());assert!(!path.exists());
    }
    #[test]
    fn ordinary_native_selection_refuses_a_file_changed_after_review() {
        let dir=tempfile::tempdir().unwrap();let path=dir.path().join("generated.png");fs::write(&path,b"fixture").unwrap();let revision=metadata::media_revision(&path);fs::write(&path,b"new content").unwrap();
        assert!(native_delete_batch(dir.path().to_string_lossy().into(),vec![DeleteTarget{name:"generated.png".into(),revision:Some(revision)}]).is_err());
        assert_eq!(fs::read(path).unwrap(),b"new content");
    }
}
