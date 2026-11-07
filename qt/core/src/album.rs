#[cfg(test)]
use crate::{
    metadata::{
        metadata_cache_current_with_settings, read_album_meta, read_album_meta_checked,
        FileMetaEntry,
    },
    thumb::ensure_thumb,
};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
    sync::Arc,
};

use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use crate::host::{async_runtime, AppHandle, NativeRuntime};

use crate::{
    constants::{IMAGE_EXTENSIONS, VIDEO_EXTENSIONS},
    duplicates::compute_hash_for_path,
    metadata::{
        get_file_metadata_cached, invalidate_media_content, DetachedAlbum, DetachedMediaEntry,
    },
    preload::{drop_preload_for_path, enqueue_preload, set_active_root, start_preloader_worker},
    settings::read_settings,
    thumb::ensure_thumb_with_settings,
    util::{
        has_extension, rename_noclobber, validate_file_name, MEDIA_MOVE_LOCK, STORE_WRITE_LOCK,
    },
};

#[cfg(test)]
use crate::metadata::transfer_media_metadata_batch_caller_holds_lock;

#[derive(Clone)]
struct AlbumDirEntry {
    path: PathBuf,
    name: String,
    relative_path: String,
    parent: Option<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RenamedAlbumResult {
    pub old_path: String,
    pub new_path: String,
    pub old_relative_path: String,
    pub new_relative_path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub parent: Option<String>,
    pub name: String,
}

#[cfg(test)]
fn unique_filename(dir: &Path, file_name: &str) -> Result<String, String> {
    unique_filename_reserved(dir, file_name, &std::collections::HashSet::new())
}

fn unique_filename_reserved(
    dir: &Path,
    file_name: &str,
    reserved: &std::collections::HashSet<String>,
) -> Result<String, String> {
    let path = dir.join(file_name);
    let stem_raw = Path::new(file_name)
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or(file_name)
        .to_string();
    let ext = Path::new(file_name)
        .extension()
        .and_then(|s| s.to_str())
        .map(|s| format!(".{}", s))
        .unwrap_or_default();

    let (base_stem, start_counter) = if let Some(pos) = stem_raw.rfind('_') {
        if let Ok(n) = stem_raw[pos + 1..].parse::<usize>() {
            (stem_raw[..pos].to_string(), n)
        } else {
            (stem_raw.clone(), 0)
        }
    } else {
        (stem_raw.clone(), 0)
    };

    if !reserved.contains(file_name)
        && !filename_occupied(&path).map_err(|error| error.to_string())?
    {
        return Ok(file_name.to_string());
    }

    let mut counter = if start_counter == 0 {
        1
    } else {
        start_counter
            .checked_add(1)
            .ok_or("Filename counter exhausted")?
    };
    loop {
        if let Some(candidate) = collision_filename(dir, &base_stem, &format!("_{counter}"), &ext)?
        {
            if !reserved.contains(&candidate) {
                return Ok(candidate);
            }
        }
        counter = counter.checked_add(1).ok_or("Filename counter exhausted")?;
    }
}

fn filename_occupied(path: &Path) -> Result<bool, std::io::Error> {
    match fs::symlink_metadata(path) {
        Ok(_) => Ok(true),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error),
    }
}

fn collision_filename(
    dir: &Path,
    base: &str,
    suffix: &str,
    extension: &str,
) -> Result<Option<String>, String> {
    let mut candidate = format!("{base}{suffix}{extension}");
    match filename_occupied(&dir.join(&candidate)) {
        Ok(occupied) => return Ok((!occupied).then_some(candidate)),
        Err(error) => {
            #[cfg(unix)]
            let too_long = error.raw_os_error() == Some(libc::ENAMETOOLONG);
            #[cfg(windows)]
            let too_long = error.raw_os_error()
                == Some(windows::Win32::Foundation::ERROR_FILENAME_EXCED_RANGE.0 as i32);
            #[cfg(not(any(unix, windows)))]
            let too_long = false;
            if !too_long {
                return Err(error.to_string());
            }
        }
    }
    // Only shorten a name rejected by this filesystem. In particular, macOS
    // may accept a Unicode name with more than 255 UTF-8 bytes unchanged.
    let mut boundary = 255_usize
        .checked_sub(suffix.len() + extension.len())
        .ok_or("Filename suffix exceeds the component limit")?
        .min(base.len());
    while !base.is_char_boundary(boundary) {
        boundary -= 1;
    }
    if boundary == 0 {
        return Err("Filename suffix leaves no space for its stem".into());
    }
    candidate = format!("{}{suffix}{extension}", &base[..boundary]);
    filename_occupied(&dir.join(&candidate))
        .map(|occupied| (!occupied).then_some(candidate))
        .map_err(|error| error.to_string())
}

pub(crate) fn is_album_dir(path: &Path) -> bool {
    path.is_dir()
        && !fs::symlink_metadata(path)
            .map(|m| m.file_type().is_symlink())
            .unwrap_or(true)
        && path
            .file_name()
            .and_then(|s| s.to_str())
            .map(|n| !n.starts_with(".room237-"))
            .unwrap_or(false)
}

fn normalized_relative_path(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

pub(crate) fn walk_album_paths(root: &Path) -> Result<Vec<PathBuf>, String> {
    if !root.is_dir() {
        return Err(format!("{} is not a directory", root.display()));
    }

    let mut albums = Vec::new();

    let mut pending = vec![root.to_path_buf()];
    while let Some(dir) = pending.pop() {
        for entry in fs::read_dir(&dir).map_err(|e| e.to_string())? {
            let path = entry.map_err(|e| e.to_string())?.path();
            if !is_album_dir(&path) {
                continue;
            }
            albums.push(path.clone());
            pending.push(path);
        }
    }
    Ok(albums)
}

fn walk_album_entries(root: &Path) -> Result<Vec<AlbumDirEntry>, String> {
    let mut entries = Vec::new();
    let albums = walk_album_paths(root)?;

    for path in albums {
        let name = match path.file_name().and_then(|s| s.to_str()) {
            Some(name) => name.to_string(),
            None => continue,
        };

        let relative = path
            .strip_prefix(root)
            .map_err(|_| format!("{} is not under root {}", path.display(), root.display()))?;
        let relative_path = normalized_relative_path(relative);
        let parent = relative.parent().and_then(|p| {
            let normalized = normalized_relative_path(p);
            if normalized.is_empty() {
                None
            } else {
                Some(normalized)
            }
        });

        entries.push(AlbumDirEntry {
            path: path.clone(),
            name,
            relative_path: relative_path.clone(),
            parent,
        });
    }

    entries.sort_by(|a, b| a.relative_path.cmp(&b.relative_path));
    Ok(entries)
}

#[cfg(test)]
fn media_files_for_album(path: &Path) -> Result<Vec<PathBuf>, String> {
    crate::media_delete::recover_album(path)?;
    let files = fs::read_dir(path)
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| {
            if let Some(ext) = p.extension().and_then(|s| s.to_str()) {
                if ext.eq_ignore_ascii_case("heic") {
                    return false;
                }
            }
            p.is_file()
                && (has_extension(p, IMAGE_EXTENSIONS) || has_extension(p, VIDEO_EXTENSIONS))
        })
        .collect::<Vec<_>>();

    Ok(files)
}

fn next_available_name(dir: &Path, name: &str) -> Result<String, String> {
    if !filename_occupied(&dir.join(name)).map_err(|error| error.to_string())? {
        return Ok(name.to_string());
    }
    let ext = Path::new(name)
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("");
    let stem_raw = Path::new(name)
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("file");
    let (base, start_counter) = if let Some(pos) = stem_raw.rfind('_') {
        if let Ok(n) = stem_raw[pos + 1..].parse::<usize>() {
            (stem_raw[..pos].to_string(), n)
        } else {
            (stem_raw.to_string(), 0)
        }
    } else {
        (stem_raw.to_string(), 0)
    };
    let mut counter = start_counter;
    loop {
        let suffix = if counter == 0 {
            String::new()
        } else {
            format!("_{counter}")
        };
        let extension = if ext.is_empty() {
            String::new()
        } else {
            format!(".{ext}")
        };
        if let Some(candidate) = collision_filename(dir, &base, &suffix, &extension)? {
            return Ok(candidate);
        }
        counter = counter.checked_add(1).ok_or("Filename counter exhausted")?;
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FavoriteDetachedMediaEntry {
    pub revision: String,
    pub meta: String,
    pub name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub thumbnail_name: Option<String>,
    pub album_path: String,
    pub album_name: String,
    pub album_id: String,
    pub favorite: bool,
}

#[derive(Deserialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct IncomingFile {
    pub name: String,
    #[serde(default)]
    pub source_path: Option<String>,
    #[serde(default)]
    pub data: Option<Vec<u8>>,
}

#[derive(Serialize)]
pub struct ImportFailure {
    pub name: String,
    pub error: String,
}

#[derive(Serialize)]
pub struct ImportResult {
    pub added: Vec<DetachedMediaEntry>,
    pub failed: Vec<ImportFailure>,
}

#[cfg(test)]
fn album_thumbnail(media: &[PathBuf], thumb_dir: &Path) -> Option<String> {
    // Only published previews of files still in this album are candidates.
    // Orphan previews and a worker's unfinished temporary WebP are excluded.
    if let Some(thumb) = media
        .iter()
        .filter_map(|path| crate::thumb::thumb_path(path, thumb_dir).ok())
        .find(|path| path.is_file())
    {
        return Some(thumb.to_string_lossy().into_owned());
    }
    let first = media.first()?;
    match ensure_thumb(first, thumb_dir) {
        Ok(thumb) => Some(thumb.to_string_lossy().into_owned()),
        Err(error) => {
            log::warn!("Album preview unavailable for {}: {error}", first.display());
            None
        }
    }
}

pub fn get_albums_detached(
    app: AppHandle<NativeRuntime>,
    root_dir: String,
) -> Result<Vec<DetachedAlbum>, String> {
    let root = PathBuf::from(&root_dir);
    if !root.is_dir() {
        return Err(format!("{} is not a directory", root.display()));
    }
    let initial_inventory = set_active_root(&root);

    let mut albums = Vec::new();
    let entries = walk_album_entries(&root)?;
    crate::catalog::initialize(&root)?;
    let summaries: Vec<(usize, Option<String>)> = entries
        .iter()
        .map(|entry| {
            let count =
                crate::catalog::scan_album_if_needed(&root, &entry.path, initial_inventory)?;
            // Existing covers only: startup inventory never decodes originals.
            let thumbs = entry.path.join(".room237-thumb");
            let preview = if thumbs.is_dir() {
                fs::read_dir(&entry.path).ok().and_then(|entries| {
                    entries.flatten().find_map(|file| {
                        let path = file.path();
                        if !file.file_type().ok()?.is_file()
                            || !(has_extension(&path, IMAGE_EXTENSIONS)
                                || has_extension(&path, VIDEO_EXTENSIONS))
                        {
                            return None;
                        }
                        crate::thumb::thumb_path(&path, &thumbs)
                            .ok()
                            .filter(|p| p.is_file())
                            .map(|p| p.to_string_lossy().into_owned())
                    })
                })
            } else {
                None
            };
            Ok((count, preview))
        })
        .collect::<Result<_, String>>()?;

    for (idx, entry) in entries.iter().enumerate() {
        let (files, own_thumb) = &summaries[idx];
        let thumb = if *files > 0 {
            own_thumb.clone()
        } else {
            let prefix = format!("{}/", entry.relative_path);
            entries
                .iter()
                .zip(summaries.iter())
                .skip(idx + 1)
                .take_while(|(child, _)| child.relative_path.starts_with(&prefix))
                .find_map(|(_, (_, thumb))| thumb.clone())
        };

        albums.push(DetachedAlbum {
            path: entry.path.to_string_lossy().into_owned(),
            name: entry.name.clone(),
            thumb_path: thumb,
            size: *files,
            relative_path: entry.relative_path.clone(),
            parent: entry.parent.clone(),
            directory_id: crate::media_move::directory_id(&entry.path).ok(),
        });

        enqueue_preload(&entry.path);
    }

    crate::catalog::retain_albums(
        &root,
        entries.iter().map(|entry| entry.relative_path.clone()),
    )?;
    start_preloader_worker(app.clone());

    albums.sort_by(|a, b| a.relative_path.cmp(&b.relative_path));
    log::info!("listed {} albums", albums.len());
    Ok(albums)
}

pub async fn get_album_media(
    _app: AppHandle<NativeRuntime>,
    dir: String,
) -> Result<Vec<DetachedMediaEntry>, String> {
    // Compatibility endpoint is also bounded. New clients use get_media_page.
    async_runtime::spawn_blocking(move || {
        let dir = PathBuf::from(dir);
        let root = crate::catalog::library_root(&dir)
            .or_else(|| dir.parent().map(Path::to_path_buf))
            .ok_or("Missing library root")?;
        crate::catalog::initialize(&root)?;
        crate::catalog::scan_album(&root, &dir)?;
        let page = crate::catalog::query_page(crate::catalog::PageQuery {
            root_dir: root.to_string_lossy().into_owned(),
            album_id: Some(
                dir.strip_prefix(&root)
                    .map_err(|e| e.to_string())?
                    .to_string_lossy()
                    .replace('\\', "/"),
            ),
            favorites_only: false,
            sort_key: "name".into(),
            sort_dir: "asc".into(),
            random_seed: 1.0,
            page: 0,
            limit_pages: None,
        })?;
        Ok(page.items.into_iter().map(|item| item.media).collect())
    })
    .await
    .map_err(|e| e.to_string())?
}

#[cfg(test)]
fn current_album_file_meta(
    path: &Path,
    entry: &FileMetaEntry,
    settings: &crate::settings::AdvancedSettings,
) -> Option<String> {
    if metadata_cache_current_with_settings(path, entry, settings) {
        entry.meta.clone()
    } else {
        None
    }
}

fn fallback_media_metadata(path: &Path) -> String {
    let added = path
        .metadata()
        .ok()
        .and_then(|meta| meta.created().ok())
        .and_then(|time| time.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|duration| duration.as_secs());
    crate::metadata::DetachedFileMeta {
        a: added,
        s: crate::metadata::read_embedded_timestamp(path),
        i: has_extension(path, IMAGE_EXTENSIONS),
        v: has_extension(path, VIDEO_EXTENSIONS),
        w: None,
        h: None,
    }
    .pack()
}

pub fn list_favorites(
    _app: AppHandle<NativeRuntime>,
    root_dir: String,
) -> Result<Vec<FavoriteDetachedMediaEntry>, String> {
    let page = crate::catalog::query_page(crate::catalog::PageQuery {
        root_dir,
        album_id: None,
        favorites_only: true,
        sort_key: "name".into(),
        sort_dir: "asc".into(),
        random_seed: 1.0,
        page: 0,
        limit_pages: None,
    })?;
    Ok(page
        .items
        .into_iter()
        .map(|item| FavoriteDetachedMediaEntry {
            revision: item.media.revision,
            meta: item.media.meta,
            name: item.media.name,
            thumbnail_name: item.media.thumbnail_name,
            album_path: item.album_path,
            album_id: item.album_id,
            album_name: item.album_name,
            favorite: true,
        })
        .collect())
}

struct AlbumTreeRenameGuard {
    _moving: std::sync::MutexGuard<'static, ()>,
    _tree: crate::album_tree::TreeLease,
}

fn prepare_album_tree_rename(root: &Path, dir: &Path) -> Result<AlbumTreeRenameGuard, String> {
    crate::album_tree::initialize(root)?;
    for _ in 0..3 {
        let mut albums = walk_album_paths(dir)?;
        albums.push(dir.to_path_buf());
        albums.sort();
        for album in &albums {
            crate::media_delete::recover_album(album)?;
        }
        let tree = crate::album_tree::write(root)?;
        // Bootstrap a newly created root marker against operations already in
        // flight, and freeze journal recovery and metadata writers as well.
        for album in &albums {
            let _maintenance = crate::media_delete::lock_maintenance(album)?;
            let _metadata = crate::metadata::lock_album_metadata(album)?;
        }
        let moving = MEDIA_MOVE_LOCK.lock().unwrap();
        // A move can finish or leave recovery work while the mutex is awaited.
        // Recheck the current subtree before changing the journal's paths.
        let mut current = walk_album_paths(dir)?;
        current.push(dir.to_path_buf());
        current.sort();
        if current == albums
            && !current
                .iter()
                .map(|album| crate::media_move::has_journal(album))
                .collect::<Result<Vec<_>, _>>()?
                .into_iter()
                .any(|pending| pending)
        {
            return Ok(AlbumTreeRenameGuard {
                _moving: moving,
                _tree: tree,
            });
        }
        drop(moving);
    }
    Err("Album has pending media moves; retry after they finish".into())
}

pub fn delete_album(root_dir: String, album_id: String) -> Result<(), String> {
    use std::path::Component;
    let relative = Path::new(&album_id);
    if album_id.trim().is_empty()
        || !relative.components().all(|part| {
            matches!(part, Component::Normal(name) if !name.to_string_lossy().starts_with(".room237-"))
        })
    {
        return Err("A non-root album id is required".into());
    }
    let requested = PathBuf::from(&root_dir).join(relative);
    let root = PathBuf::from(root_dir).canonicalize().map_err(|e| e.to_string())?;
    let mut target = root.clone();
    for part in relative.components() {
        target.push(part.as_os_str());
        if fs::symlink_metadata(&target).map_err(|e| e.to_string())?.file_type().is_symlink() {
            return Err("Cannot delete a symbolic-link album".into());
        }
    }
    let target = target.canonicalize().map_err(|e| e.to_string())?;
    if target == root || !target.starts_with(&root) || !target.is_dir() {
        return Err("Album path escapes the library or is not a directory".into());
    }
    let identity = crate::media_move::directory_id(&target)?;
    // Stop queued work before waiting for current readers; otherwise workers
    // can keep reacquiring shared leases for hundreds of queued thumbnails.
    drop_preload_for_path(&requested);
    drop_preload_for_path(&target);
    let _guard = prepare_album_tree_rename(&root, &target)?;
    if crate::media_move::directory_id(&target)? != identity {
        return Err("Album changed while waiting for deletion".into());
    }
    // Use the same root-exclusive lease as rename/move, so workers cannot recreate
    // cache directories during remove_dir_all.
    drop_preload_for_path(&requested);
    drop_preload_for_path(&target);
    fs::remove_dir_all(&target).map_err(|e| e.to_string())
}

pub fn rename_album(
    root_dir: String,
    album_id: String,
    new_name: String,
) -> Result<RenamedAlbumResult, String> {
    if album_id.trim().is_empty() {
        return Err("Album id is required".to_string());
    }
    if album_id.eq_ignore_ascii_case("favorites") {
        return Err("Cannot rename favorites album".to_string());
    }
    let trimmed = new_name.trim();
    if trimmed.is_empty() {
        return Err("Album name cannot be empty".to_string());
    }
    let safe_name = trimmed.replace(['/', '\\', ':'], "_");
    validate_file_name(&safe_name)?;
    if safe_name.starts_with(".room237-") {
        return Err("Reserved album name".into());
    }
    let settings = read_settings();
    let cleanup_delay = settings.album.rename_cleanup_delay_secs;

    let root = PathBuf::from(&root_dir);
    if !root.is_dir() {
        return Err(format!("{} is not a directory", root.display()));
    }
    let normalized_root = root
        .canonicalize()
        .map_err(|e| format!("Failed to resolve root: {e}"))?;

    let target = normalized_root.join(Path::new(&album_id));
    let normalized_target = target
        .canonicalize()
        .map_err(|_| "Album not found".to_string())?;

    if normalized_target == normalized_root {
        return Err("Cannot rename or move the root directory".to_string());
    }
    if !normalized_target.starts_with(&normalized_root) {
        return Err("Album path escapes root".to_string());
    }

    let parent_dir = normalized_target
        .parent()
        .ok_or("Cannot rename root directory")?
        .to_path_buf();
    let current_name = normalized_target
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or_default()
        .to_string();

    if current_name == safe_name {
        let rel = normalized_target
            .strip_prefix(&normalized_root)
            .map_err(|e| e.to_string())
            .map(normalized_relative_path)?;
        let parent_rel = if parent_dir == normalized_root {
            None
        } else {
            parent_dir
                .strip_prefix(&normalized_root)
                .ok()
                .map(normalized_relative_path)
        };
        return Ok(RenamedAlbumResult {
            old_path: normalized_target.to_string_lossy().to_string(),
            new_path: normalized_target.to_string_lossy().to_string(),
            old_relative_path: rel.clone(),
            new_relative_path: rel,
            parent: parent_rel,
            name: safe_name,
        });
    }

    let new_path = parent_dir.join(&safe_name);
    if new_path.exists() {
        return Err(format!(
            "Album \"{}\" already exists in {}",
            safe_name,
            parent_dir.display()
        ));
    }

    let _moving = prepare_album_tree_rename(&normalized_root, &normalized_target)?;
    rename_noclobber(&normalized_target, &new_path).map_err(|e| e.to_string())?;

    drop_preload_for_path(&normalized_target);

    if cleanup_delay > 0 {
        let old = normalized_target.clone();
        let newp = new_path.clone();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(cleanup_delay));
            if old != newp && old.exists() {
                if old
                    .read_dir()
                    .map(|mut d| d.next().is_none())
                    .unwrap_or(false)
                {
                    let _ = fs::remove_dir(&old);
                }
            }
        });
    }

    let old_relative_path = normalized_target
        .strip_prefix(&normalized_root)
        .map_err(|e| e.to_string())
        .map(normalized_relative_path)?;
    let new_relative_path = new_path
        .strip_prefix(&normalized_root)
        .map_err(|e| e.to_string())
        .map(normalized_relative_path)?;
    let parent_relative = if parent_dir == normalized_root {
        None
    } else {
        parent_dir
            .strip_prefix(&normalized_root)
            .ok()
            .map(normalized_relative_path)
    };

    Ok(RenamedAlbumResult {
        old_path: normalized_target.to_string_lossy().to_string(),
        new_path: new_path.to_string_lossy().to_string(),
        old_relative_path,
        new_relative_path,
        parent: parent_relative,
        name: safe_name,
    })
}

pub fn move_album(
    root_dir: String,
    album_id: String,
    new_parent_id: Option<String>,
) -> Result<RenamedAlbumResult, String> {
    if album_id.trim().is_empty() {
        return Err("Album id is required".to_string());
    }
    if album_id.eq_ignore_ascii_case("favorites") {
        return Err("Cannot move favorites album".to_string());
    }

    let root = PathBuf::from(&root_dir);
    if !root.is_dir() {
        return Err(format!("{} is not a directory", root.display()));
    }
    let normalized_root = root
        .canonicalize()
        .map_err(|e| format!("Failed to resolve root: {e}"))?;

    let target = normalized_root.join(Path::new(&album_id));
    let normalized_target = target
        .canonicalize()
        .map_err(|_| "Album not found".to_string())?;

    if normalized_target == normalized_root {
        return Err("Cannot rename or move the root directory".to_string());
    }
    if !normalized_target.starts_with(&normalized_root) {
        return Err("Album path escapes root".to_string());
    }

    let album_name = normalized_target
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or("Invalid album name")?
        .to_string();

    if normalized_target.parent().is_none() {
        return Err("Cannot move root directory".to_string());
    }

    let target_parent = new_parent_id
        .filter(|p| !p.trim().is_empty())
        .map(|p| normalized_root.join(Path::new(&p)))
        .unwrap_or(normalized_root.clone());
    let normalized_parent = target_parent
        .canonicalize()
        .map_err(|_| "Target album not found".to_string())?;

    if !normalized_parent.is_dir() {
        return Err("Target album is not a directory".to_string());
    }
    if !normalized_parent.starts_with(&normalized_root) {
        return Err("Target path escapes root".to_string());
    }
    if normalized_parent.starts_with(&normalized_target) {
        return Err("Cannot move album into its own subtree".to_string());
    }

    let new_path = normalized_parent.join(&album_name);
    if new_path == normalized_target {
        let rel = normalized_target
            .strip_prefix(&normalized_root)
            .map_err(|e| e.to_string())
            .map(normalized_relative_path)?;
        let parent_rel = if normalized_parent == normalized_root {
            None
        } else {
            normalized_parent
                .strip_prefix(&normalized_root)
                .ok()
                .map(normalized_relative_path)
        };
        return Ok(RenamedAlbumResult {
            old_path: normalized_target.to_string_lossy().to_string(),
            new_path: normalized_target.to_string_lossy().to_string(),
            old_relative_path: rel.clone(),
            new_relative_path: rel,
            parent: parent_rel,
            name: album_name,
        });
    }

    if new_path.exists() {
        return Err(format!(
            "Album \"{}\" already exists in {}",
            album_name,
            normalized_parent.display()
        ));
    }

    let _moving = prepare_album_tree_rename(&normalized_root, &normalized_target)?;
    rename_noclobber(&normalized_target, &new_path).map_err(|e| e.to_string())?;
    drop_preload_for_path(&normalized_target);

    let old_relative_path = normalized_target
        .strip_prefix(&normalized_root)
        .map_err(|e| e.to_string())
        .map(normalized_relative_path)?;
    let new_relative_path = new_path
        .strip_prefix(&normalized_root)
        .map_err(|e| e.to_string())
        .map(normalized_relative_path)?;
    let parent_relative = if normalized_parent == normalized_root {
        None
    } else {
        normalized_parent
            .strip_prefix(&normalized_root)
            .ok()
            .map(normalized_relative_path)
    };

    Ok(RenamedAlbumResult {
        old_path: normalized_target.to_string_lossy().to_string(),
        new_path: new_path.to_string_lossy().to_string(),
        old_relative_path,
        new_relative_path,
        parent: parent_relative,
        name: album_name,
    })
}

pub fn get_album_size(dir: String) -> Result<u64, String> {
    let dir = PathBuf::from(&dir);
    if !dir.is_dir() {
        return Err(format!("{} is not a directory", dir.display()));
    }
    crate::media_delete::recover_album(&dir)?;

    let mut total = 0;
    for entry in fs::read_dir(&dir).map_err(|e| e.to_string())? {
        let path = entry.map_err(|e| e.to_string())?.path();
        if path.is_file()
            && (has_extension(&path, IMAGE_EXTENSIONS) || has_extension(&path, VIDEO_EXTENSIONS))
        {
            total += path.metadata().map_err(|e| e.to_string())?.len();
        }
    }
    Ok(total)
}

type MoveGroup = Vec<(String, Option<String>)>;
type PlannedMove = (String, String, String);

fn plan_move_group(
    source_dir: &Path,
    target_dir: &Path,
    group: &MoveGroup,
    reserved: &mut std::collections::HashSet<String>,
) -> Result<Vec<PlannedMove>, String> {
    let mut planned = Vec::new();
    let result = (|| -> Result<(), String> {
        for (name, expected) in group {
            let source = source_dir.join(name);
            if !fs::symlink_metadata(&source).is_ok_and(|info| info.is_file()) {
                return Err("Move source is not a regular file".into());
            }
            let revision =
                crate::metadata::source_revision(&source).ok_or("Move source unavailable")?;
            if expected
                .as_ref()
                .is_some_and(|expected| expected != &revision)
            {
                return Err("Bound media changed before move".into());
            }
            let target = unique_filename_reserved(target_dir, name, reserved)?;
            reserved.insert(target.clone());
            planned.push((name.clone(), target, revision));
        }
        Ok(())
    })();
    if let Err(error) = result {
        for (_, target, _) in &planned {
            reserved.remove(target);
        }
        return Err(error);
    }
    Ok(planned)
}

fn logical_move_groups(
    dir: &Path,
    requested: &[String],
    snapshot: &crate::metadata::AlbumMeta,
) -> Result<std::collections::HashMap<String, MoveGroup>, String> {
    let mut outputs: std::collections::HashSet<&str> =
        requested.iter().map(String::as_str).collect();
    for name in requested {
        if let Some(binding) = snapshot
            .files
            .get(name)
            .and_then(|entry| entry.heic_conversion.as_ref())
        {
            outputs.insert(&binding.output_name);
        }
    }
    let mut by_output: std::collections::HashMap<String, MoveGroup> =
        std::collections::HashMap::new();
    for (name, entry) in &snapshot.files {
        let Some(binding) = &entry.heic_conversion else {
            continue;
        };
        if outputs.contains(binding.output_name.as_str())
            && crate::metadata::heic_binding_current(dir, name, binding)?
        {
            let group = by_output
                .entry(binding.output_name.clone())
                .or_insert_with(|| {
                    vec![(
                        binding.output_name.clone(),
                        Some(binding.output_revision.clone()),
                    )]
                });
            group.push((name.clone(), Some(binding.source_revision.clone())));
        }
    }
    for group in by_output.values_mut() {
        group[1..].sort_by(|a, b| a.0.cmp(&b.0));
    }
    let mut groups = std::collections::HashMap::new();
    for name in requested {
        let binding = snapshot
            .files
            .get(name)
            .and_then(|entry| entry.heic_conversion.as_ref());
        let raw_group = binding
            .and_then(|binding| by_output.get(&binding.output_name))
            .filter(|group| group.iter().any(|(raw, _)| raw == name));
        let group = raw_group
            .or_else(|| by_output.get(name))
            .cloned()
            .unwrap_or_else(|| vec![(name.clone(), None)]);
        groups.insert(name.clone(), group);
    }
    Ok(groups)
}

fn rollback_moved_files(
    source: &Path,
    target: &Path,
    records: &[(String, String, String)],
) -> Result<(), String> {
    let mut errors = Vec::new();
    for (old, new, revision) in records.iter().rev() {
        let destination = target.join(new);
        if crate::metadata::source_revision(&destination).as_deref() != Some(revision) {
            errors.push(format!(
                "Moved file changed; preserving {}",
                destination.display()
            ));
            continue;
        }
        if let Err(error) = rename_noclobber(&destination, &source.join(old)) {
            errors.push(format!("Could not restore {old}: {error}"));
        }
    }
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors.join("; "))
    }
}

fn move_media_batch_blocking(
    source_dir: PathBuf,
    target_dir: PathBuf,
    media: Vec<String>,
    move_artifacts: bool,
) -> Result<Vec<String>, String> {
    move_media_batch_with_hook(source_dir, target_dir, media, move_artifacts, |_, _, _| {
        Ok(())
    })
}

fn move_media_batch_with_hook(
    source_dir: PathBuf,
    target_dir: PathBuf,
    media: Vec<String>,
    move_artifacts: bool,
    mut checkpoint: impl FnMut(&str, &Path, &Path) -> Result<(), String>,
) -> Result<Vec<String>, String> {
    let source_dir = source_dir.canonicalize().map_err(|e| e.to_string())?;
    let target_dir = target_dir.canonicalize().map_err(|e| e.to_string())?;
    if !(source_dir.is_dir() && target_dir.is_dir()) {
        return Err("bad dirs".into());
    }
    for name in &media {
        validate_file_name(name)?;
    }
    if source_dir == target_dir {
        return Ok(Vec::new());
    }
    let _tree = crate::album_tree::read(&[&source_dir, &target_dir])?;
    crate::media_delete::recover_album(&source_dir)?;
    crate::media_delete::recover_album(&target_dir)?;
    // Use the same canonical order as deletion. Freeze encoders before album
    // leases, including moves that rebuild rather than transfer derived data.
    let mut directories = [&source_dir, &target_dir];
    directories.sort();
    let _maintenance = directories
        .iter()
        .map(|dir| crate::media_delete::lock_maintenance(dir))
        .collect::<Result<Vec<_>, _>>()?;
    let observed = if move_artifacts {
        std::sync::Arc::new(crate::metadata::AlbumMeta::default())
    } else {
        Arc::new(crate::metadata::read_selected_metadata(
            &source_dir,
            &media,
            true,
        )?)
    };
    let target_has_metadata = !move_artifacts && target_dir.join(".room237-metadata").exists();
    let has_pair = !move_artifacts
        && logical_move_groups(&source_dir, &media, &observed)?
            .values()
            .any(|group| group.len() > 1);
    let (_move_guard, metadata_guard, _store_guard) = if move_artifacts
        || has_pair
        || target_has_metadata
    {
        let (moving, metadata) =
            crate::metadata::lock_metadata_for_move_with_tree(&[&source_dir, &target_dir], &_tree)?;
        (moving, Some(metadata), None)
    } else {
        _tree.ensure_current(&[&source_dir, &target_dir])?;
        let moving = MEDIA_MOVE_LOCK.lock().unwrap();
        let store = STORE_WRITE_LOCK.lock().unwrap();
        (moving, None, Some(store))
    };
    let snapshot = if let Some(guard) = &metadata_guard {
        crate::metadata::read_selected_with_guard(&target_dir, &media, false, guard)?;
        crate::metadata::read_selected_with_guard(&source_dir, &media, true, guard)?
    } else {
        observed
    };
    let groups = logical_move_groups(&source_dir, &media, &snapshot)?;
    let mut failed = Vec::new();
    let mut moved_records = Vec::new();
    let mut journal: Option<crate::media_move::MoveJournal> = None;
    let mut initial_plans = std::collections::HashMap::new();
    let mut initial_records = Vec::new();
    let mut planning_attempted = std::collections::HashSet::new();
    let mut reserved = std::collections::HashSet::new();
    for requested in &media {
        let group = groups.get(requested).expect("requested group was planned");
        let key = &group[0].0;
        if !planning_attempted.insert(key.clone()) {
            continue;
        }
        if let Ok(plan) = plan_move_group(&source_dir, &target_dir, group, &mut reserved) {
            initial_records.extend(plan.iter().cloned());
            initial_plans.insert(key.clone(), plan);
        }
    }
    if !initial_records.is_empty() {
        let mut prepared = crate::media_move::MoveJournal::create(&source_dir, &target_dir)?;
        prepared.record_files_with_artifacts(&initial_records, move_artifacts)?;
        journal = Some(prepared);
    }
    drop(initial_records);
    drop(reserved);
    let mut processed = std::collections::HashSet::new();
    for requested in media {
        if processed.contains(&requested) {
            continue;
        }
        let group = groups.get(&requested).expect("requested group was planned");
        let mut group_records = Vec::new();
        let result = (|| -> Result<(), String> {
            let planned = plan_move_group(
                &source_dir,
                &target_dir,
                group,
                &mut std::collections::HashSet::new(),
            )?;
            if journal.is_none() {
                journal = Some(crate::media_move::MoveJournal::create(
                    &source_dir,
                    &target_dir,
                )?);
            }
            if initial_plans.get(&group[0].0) != Some(&planned) {
                journal
                    .as_mut()
                    .ok_or("Missing move journal")?
                    .record_files_with_artifacts(&planned, move_artifacts)?;
            }
            for (name, target_name, revision) in planned {
                let source = source_dir.join(&name);
                let destination = target_dir.join(&target_name);
                checkpoint("before_rename", &source, &destination)?;
                if crate::metadata::source_revision(&source).as_deref() != Some(&revision) {
                    return Err("Move source changed".into());
                }
                rename_noclobber(&source, &destination).map_err(|e| e.to_string())?;
                group_records.push((name.clone(), target_name, revision.clone()));
                if crate::metadata::source_revision(&destination).as_deref() != Some(&revision) {
                    return Err("Moved file changed".into());
                }
            }
            Ok(())
        })();
        if let Err(error) = result {
            if let Err(rollback) = rollback_moved_files(&source_dir, &target_dir, &group_records) {
                let previous = rollback_moved_files(&source_dir, &target_dir, &moved_records);
                return Err(format!(
                    "{error}; group rollback: {rollback}; previous rollback: {previous:?}"
                ));
            }
            log::warn!("move failed {requested}: {error}");
            failed.push(requested.clone());
            processed.insert(requested);
            continue;
        }
        failed.retain(|failed| !group.iter().any(|(name, _)| name == failed));
        processed.extend(group.iter().map(|(name, _)| name.clone()));
        moved_records.extend(group_records);
    }
    let moved: Vec<(String, String)> = moved_records
        .iter()
        .map(|(source, target, _)| (source.clone(), target.clone()))
        .collect();
    if let Some(guard) = &metadata_guard {
        let result = (|| -> Result<(), String> {
            checkpoint("before_metadata", &source_dir, &target_dir)?;
            for (_, target, revision) in &moved_records {
                if crate::metadata::source_revision(&target_dir.join(target)).as_deref()
                    != Some(revision)
                {
                    return Err("Moved file changed before metadata transfer".into());
                }
            }
            if moved.is_empty() {
                return Ok(());
            }
            crate::metadata::transfer_media_state_journaled(
                &source_dir,
                &target_dir,
                &moved,
                guard,
                move_artifacts,
                journal.as_mut().ok_or("Missing move journal")?,
                |phase| checkpoint(phase, &source_dir, &target_dir),
            )
        })();
        if let Err(error) = result {
            return Err(
                match journal
                    .as_mut()
                    .map(|journal| journal.rollback(Some(guard)))
                    .unwrap_or(Ok(()))
                {
                    Ok(()) => error,
                    Err(rollback) => format!("{error}; file rollback: {rollback}"),
                },
            );
        }
    }
    checkpoint("before_move_commit", &source_dir, &target_dir)?;
    if let Some(journal) = &mut journal {
        journal.commit(|| checkpoint("move_committed", &source_dir, &target_dir))?;
    }
    checkpoint("after_move_commit", &source_dir, &target_dir)?;
    if let Some(journal) = &journal {
        if let Err(error) = journal.finish(|_| Ok(())) {
            // Original data is already committed. Derived work remains
            // best-effort, with the journal retained for the next recovery.
            log::warn!("Failed to finish move artifacts: {error}");
        }
    }
    Ok(failed)
}

pub fn move_media(source: String, target: String, media: String) -> Result<String, String> {
    let failed = move_media_batch_blocking(
        PathBuf::from(source),
        PathBuf::from(target),
        vec![media],
        read_settings().album.move_rename_thumbs_and_meta,
    )?;
    if failed.is_empty() {
        Ok("ok".into())
    } else {
        Err("Failed to move media".into())
    }
}

pub async fn move_media_batch(
    source: String,
    target: String,
    media: Vec<String>,
) -> Result<Vec<String>, String> {
    let source_dir = PathBuf::from(&source);
    let target_dir = PathBuf::from(&target);
    let settings = read_settings();
    let move_artifacts = settings.album.move_rename_thumbs_and_meta;

    let failed = async_runtime::spawn_blocking(move || {
        move_media_batch_blocking(source_dir, target_dir, media, move_artifacts)
    })
    .await
    .map_err(|e| e.to_string())??;

    Ok(failed)
}

pub async fn register_new_media(
    _app: AppHandle<NativeRuntime>,
    album_path: String,
    media_name: String,
) -> Result<DetachedMediaEntry, String> {
    validate_file_name(&media_name)?;
    let name_clone = media_name.clone();
    async_runtime::spawn_blocking(move || {
        let media_path = PathBuf::from(&album_path).join(&media_name);
        if !media_path.is_file() {
            return Err("Changed media is not a file".into());
        }
        invalidate_media_content(&media_path)?;
        add_media_files_blocking(
            album_path.clone(),
            vec![IncomingFile {
                name: name_clone,
                source_path: Some(
                    PathBuf::from(&album_path)
                        .join(&media_name)
                        .to_string_lossy()
                        .into_owned(),
                ),
                data: None,
            }],
        )
        .and_then(|mut result| {
            result.added.pop().ok_or_else(|| {
                result
                    .failed
                    .into_iter()
                    .next()
                    .map(|failure| failure.error)
                    .unwrap_or_else(|| "No media registered".to_string())
            })
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

fn persist_import(
    dir: &Path,
    name: &str,
    temporary: tempfile::NamedTempFile,
) -> Result<(String, PathBuf), String> {
    persist_import_owned(dir, name, temporary).map(|(name, path, _)| (name, path))
}

fn persist_import_owned(
    dir: &Path,
    name: &str,
    mut temporary: tempfile::NamedTempFile,
) -> Result<(String, PathBuf, fs::File), String> {
    for _ in 0..1000 {
        let target_name = next_available_name(dir, name)?;
        let dest = dir.join(&target_name);
        match temporary.persist_noclobber(&dest) {
            Ok(file) => return Ok((target_name, dest, file)),
            Err(error) if error.error.kind() == std::io::ErrorKind::AlreadyExists => {
                temporary = error.file
            }
            Err(error) => return Err(error.to_string()),
        }
    }
    Err("Too many concurrent filename collisions".into())
}

#[allow(dead_code)]
pub(crate) fn ensure_heic_copy(source: &Path) -> Result<PathBuf, String> {
    ensure_heic_copy_with(source, crate::util::generate_heic_jpeg)
}

fn ensure_heic_copy_with(
    source: &Path,
    generate: impl FnOnce(&Path, &Path) -> Result<tempfile::NamedTempFile, String>,
) -> Result<PathBuf, String> {
    let album = source.parent().ok_or("Missing HEIC album")?;
    let _tree = crate::album_tree::read(&[album])?;
    ensure_heic_copy_caller_holds_tree_lease(source, generate, &_tree)
}

fn ensure_heic_copy_caller_holds_tree_lease(
    source: &Path,
    generate: impl FnOnce(&Path, &Path) -> Result<tempfile::NamedTempFile, String>,
    tree: &crate::album_tree::TreeLease,
) -> Result<PathBuf, String> {
    use crate::metadata::{
        source_revision, source_revision_from_info, update_file_meta_if_source_current,
        HeicConversion,
    };
    use crate::util::FileLock;
    use sha2::{Digest, Sha256};
    use std::time::Duration;
    let album = source.parent().ok_or("Missing HEIC album")?;
    let name = source
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or("Invalid HEIC name")?;
    validate_file_name(name)?;
    let thumbs = album.join(".room237-thumb");
    match fs::create_dir(&thumbs) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists && thumbs.is_dir() => {}
        Err(error) => return Err(error.to_string()),
    }
    let timeout = Duration::from_secs(read_settings().ffmpeg.timeout_secs);
    let _maintenance = FileLock::acquire_shared(
        &crate::thumb::thumbnail_maintenance_lock_path(&thumbs),
        timeout,
        Duration::from_millis(5),
    )?;
    tree.ensure_current(&[album])?;
    let lock_name = format!(
        ".room237-heic-{}.lock",
        hex::encode(Sha256::digest(name.as_bytes()))
    );
    let _conversion =
        FileLock::acquire(&thumbs.join(lock_name), timeout, Duration::from_millis(5))?;
    let revision = source_revision(source).ok_or("HEIC original is unavailable")?;
    crate::metadata::prepare_database(album)?;
    let snapshot = crate::metadata::read_file_entry(source)?;
    if let Some(conversion) = snapshot
        .as_ref()
        .and_then(|entry| entry.heic_conversion.as_ref())
    {
        validate_file_name(&conversion.output_name)?;
        let output = album.join(&conversion.output_name);
        if conversion.source_revision == revision
            && has_extension(&output, &["jpeg", "jpg"])
            && source_revision(&output).as_deref() == Some(&conversion.output_revision)
            && source_revision(source).as_deref() == Some(&revision)
        {
            return Ok(output);
        }
    }
    if source_revision(source).as_deref() != Some(&revision) {
        return Err("HEIC original changed before conversion".into());
    }
    // Encode outside the metadata mutex. No existing JPEG is overwritten.
    let temporary = generate(source, &thumbs)?;
    if source_revision(source).as_deref() != Some(&revision) {
        return Err("HEIC original changed during conversion".into());
    }
    let preferred = source.with_extension("jpeg");
    let output_revision = source_revision_from_info(
        &temporary
            .as_file()
            .metadata()
            .map_err(|error| error.to_string())?,
    )
    .ok_or("Cannot stat converted JPEG")?;
    let (output_name, output, _published, output_revision) = {
        let _move_guard = MEDIA_MOVE_LOCK.lock().unwrap();
        let existing = fs::OpenOptions::new().read(true).open(&preferred).ok();
        // A legacy output is adopted only when its bytes equal this conversion.
        // File times alone cannot distinguish it from an unrelated user JPEG.
        let legacy = existing.and_then(|file| {
            let info = file.metadata().ok()?;
            if !info.is_file() {
                return None;
            }
            let old_revision = source_revision_from_info(&info)?;
            if files_equal(&file, temporary.as_file()).ok() != Some(true)
                || source_revision_from_info(&file.metadata().ok()?).as_deref()
                    != Some(&old_revision)
                || source_revision(&preferred).as_deref() != Some(&old_revision)
            {
                return None;
            }
            Some((
                preferred.file_name()?.to_str()?.to_owned(),
                preferred.clone(),
                file,
                old_revision,
            ))
        });
        if let Some(legacy) = legacy {
            legacy
        } else {
            let preferred_name = preferred
                .file_name()
                .and_then(|name| name.to_str())
                .ok_or("Invalid converted JPEG name")?;
            temporary
                .as_file()
                .sync_all()
                .map_err(|error| error.to_string())?;
            let (name, output, published) = persist_import_owned(album, preferred_name, temporary)?;
            (name, output, published, output_revision)
        }
    };
    if source_revision(&output).as_deref() != Some(&output_revision) {
        return Err("Converted JPEG changed before metadata publication".into());
    }
    let expected_source = revision.clone();
    let conversion = HeicConversion {
        output_name,
        source_revision: revision,
        output_revision,
    };
    let expected = conversion.clone();
    let output_to_check = output.clone();
    let saved = update_file_meta_if_source_current(source, &expected_source, move |entry| {
        if source_revision(&output_to_check).as_deref() == Some(&conversion.output_revision) {
            entry.heic_conversion = Some(conversion);
        }
    })?;
    if saved.heic_conversion.as_ref() != Some(&expected)
        || source_revision(&output).as_deref() != Some(&expected.output_revision)
    {
        return Err("Converted JPEG changed during metadata publication".into());
    }
    Ok(output)
}

fn files_equal(a: &fs::File, b: &fs::File) -> std::io::Result<bool> {
    use std::io::{Read, Seek, SeekFrom};
    let size = a.metadata()?.len();
    if size != b.metadata()?.len() {
        return Ok(false);
    }
    let mut a = a.try_clone()?;
    let mut b = b.try_clone()?;
    a.seek(SeekFrom::Start(0))?;
    b.seek(SeekFrom::Start(0))?;
    let mut left = [0; 64 * 1024];
    let mut right = [0; 64 * 1024];
    let mut remaining = size;
    while remaining > 0 {
        let count = remaining.min(left.len() as u64) as usize;
        a.read_exact(&mut left[..count])?;
        b.read_exact(&mut right[..count])?;
        if left[..count] != right[..count] {
            return Ok(false);
        }
        remaining -= count as u64;
    }
    Ok(true)
}

fn add_media_files_blocking(dir: String, files: Vec<IncomingFile>) -> Result<ImportResult, String> {
    let album_path = PathBuf::from(&dir);
    if !album_path.is_dir() {
        return Err(format!("{} is not a directory", album_path.display()));
    }

    let _tree = crate::album_tree::read(&[&album_path])?;

    crate::media_delete::recover_album(&album_path)?;

    for file in &files {
        validate_file_name(file.name.trim())?;
    }
    let maintenance = crate::media_delete::lock_maintenance(&album_path)?;
    _tree.ensure_current(&[&album_path])?;
    let _move_guard = MEDIA_MOVE_LOCK.lock().unwrap();
    crate::util::create_derived_dir(&album_path.join(".room237-thumb"))?;

    let mut copied_files: Vec<String> = Vec::new();
    let mut failed = Vec::new();

    for file in files {
        let name = file.name.trim();
        let copied = (|| -> Result<String, String> {
            if let Some(src) = file.source_path.as_ref() {
                let src_path = PathBuf::from(src);
                if !fs::metadata(&src_path)
                    .map_err(|e| e.to_string())?
                    .is_file()
                {
                    return Err("Import source is not a regular file".into());
                }
                if src_path.parent().map(|p| p == album_path).unwrap_or(false) {
                    return src_path
                        .file_name()
                        .and_then(|name| name.to_str())
                        .map(str::to_string)
                        .ok_or_else(|| "Invalid source filename".into());
                }
                let mut source = fs::File::open(&src_path).map_err(|e| e.to_string())?;
                let mut temporary =
                    tempfile::NamedTempFile::new_in(&album_path).map_err(|e| e.to_string())?;
                std::io::copy(&mut source, temporary.as_file_mut()).map_err(|e| e.to_string())?;
                temporary.as_file().sync_all().map_err(|e| e.to_string())?;
                Ok(persist_import(&album_path, name, temporary)?.0)
            } else if let Some(data) = file.data.as_ref() {
                let mut temporary =
                    tempfile::NamedTempFile::new_in(&album_path).map_err(|e| e.to_string())?;
                temporary.write_all(data).map_err(|e| e.to_string())?;
                temporary.as_file().sync_all().map_err(|e| e.to_string())?;
                Ok(persist_import(&album_path, name, temporary)?.0)
            } else {
                Err("Import file has no data or source".into())
            }
        })();
        match copied {
            Ok(target_name) => copied_files.push(target_name),
            Err(error) => failed.push(ImportFailure {
                name: name.into(),
                error,
            }),
        }
    }

    drop(_move_guard);
    drop(maintenance);
    // A raw HEIC and its in-album JPEG can register the same visible file.
    // Preserve first occurrence order and avoid duplicate DTOs and derived work.
    let mut registered = std::collections::HashSet::new();
    copied_files.retain(|name| registered.insert(name.clone()));
    let album_meta = crate::metadata::read_selected_metadata(&album_path, &copied_files, false)
        .unwrap_or_else(|error| {
            log::warn!("Import metadata unavailable: {error}");
            crate::metadata::AlbumMeta::default()
        });
    let settings = read_settings();

    let added: Vec<DetachedMediaEntry> = copied_files
        .par_iter()
        .filter_map(|target_name| {
            let path = album_path.join(&target_name);
            if !path.is_file() {
                log::error!("add_media_files: {} is not a file", path.display());
                return None;
            }

            let thumb_dir = album_path.join(".room237-thumb");
            let _ = ensure_thumb_with_settings(&path, &thumb_dir, &settings);

            let meta_str = match get_file_metadata_cached(&path) {
                Ok(m) => m,
                Err(e) => {
                    log::error!(
                        "add_media_files: failed to get metadata for {}: {}",
                        target_name,
                        e
                    );
                    // The original has already been published. Return it to
                    // the UI even if derived data is temporarily unavailable.
                    fallback_media_metadata(&path)
                }
            };

            let hash_cfg = settings.hash_config();
            if compute_hash_for_path(&album_meta, &path, &hash_cfg, &settings).is_none() {
                log::warn!("hash not computed for {}", path.display());
            }

            let favorite = album_meta
                .files
                .get(target_name.as_str())
                .map(|e| if e.favorite { Some(true) } else { None })
                .unwrap_or(None);

            log::info!("registered new media {}", path.display());
            Some(DetachedMediaEntry {
                blur_preview: crate::metadata::read_file_entry(&path)
                    .ok()
                    .flatten()
                    .and_then(|entry| entry.blur_preview),
                revision: crate::metadata::media_revision(&path),
                thumbnail_name: crate::thumb::detached_thumbnail_name(&path),
                meta: meta_str,
                name: path.file_name().unwrap().to_string_lossy().into_owned(),
                favorite,
            })
        })
        .collect();

    Ok(ImportResult { added, failed })
}

pub async fn add_media_files(
    _app: AppHandle<NativeRuntime>,
    dir: String,
    files: Vec<IncomingFile>,
) -> Result<ImportResult, String> {
    async_runtime::spawn_blocking(move || add_media_files_blocking(dir, files))
        .await
        .map_err(|e| e.to_string())?
}

#[cfg(test)]
mod tests {
    #[test]
    #[ignore = "requires FFmpeg with libwebp in PATH"]
    fn long_name_imports_and_collision_moves_keep_real_previews_hashes_and_favorites() {
        use image::GenericImageView;
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("source");
        let imported = root.path().join("imported");
        let target = root.path().join("target");
        for dir in [&source, &imported, &target] {
            fs::create_dir(dir).unwrap();
        }
        let name = format!("{}.png", "x".repeat(251));
        let original = source.join(&name);
        let mut image = image::RgbImage::new(96, 64);
        for (x, _, pixel) in image.enumerate_pixels_mut() {
            *pixel = image::Rgb(if x < 48 { [255, 255, 255] } else { [0, 0, 0] });
        }
        image.save(&original).unwrap();
        let bytes = fs::read(&original).unwrap();
        let incoming = || IncomingFile {
            name: name.clone(),
            source_path: Some(original.to_string_lossy().into()),
            data: None,
        };
        let mut first =
            add_media_files_blocking(imported.to_string_lossy().into(), vec![incoming()]).unwrap();
        assert!(first.failed.is_empty());
        let entry = first.added.pop().unwrap();
        assert_eq!(entry.name, name);
        let preview =
            crate::thumb::thumb_path(&imported.join(&name), &imported.join(".room237-thumb"))
                .unwrap();
        assert_eq!(
            entry.thumbnail_name.as_deref(),
            preview.file_name().and_then(|name| name.to_str())
        );
        assert_eq!(image::open(&preview).unwrap().dimensions(), (96, 64));
        let hash = crate::metadata::load_album_file_hash(&imported.join(&name))
            .unwrap()
            .hash;
        let second =
            add_media_files_blocking(imported.to_string_lossy().into(), vec![incoming()]).unwrap();
        assert!(second.failed.is_empty());
        assert_eq!(second.added.len(), 1);
        assert_ne!(second.added[0].name, name);
        assert!(second.added[0].name.ends_with("_1.png"));
        for entry in second.added {
            let path = imported.join(&entry.name);
            assert_eq!(fs::read(&path).unwrap(), bytes);
            let preview =
                crate::thumb::thumb_path(&path, &imported.join(".room237-thumb")).unwrap();
            assert!(image::open(&preview).is_ok());
            assert!(crate::metadata::load_album_file_hash(&path).is_some());
        }
        crate::metadata::set_media_favorite(imported.join(&name).to_string_lossy().into(), true)
            .unwrap();
        fs::write(target.join(&name), b"retained collision").unwrap();
        let target_name = unique_filename(&target, &name).unwrap();
        assert!(move_media_batch_blocking(
            imported.clone(),
            target.clone(),
            vec![name.clone()],
            true
        )
        .unwrap()
        .is_empty());
        let moved = target.join(&target_name);
        assert_eq!(fs::read(&moved).unwrap(), bytes);
        assert_eq!(fs::read(&original).unwrap(), bytes);
        assert_eq!(fs::read(target.join(name)).unwrap(), b"retained collision");
        assert!(read_album_meta(&target).files[&target_name].favorite);
        assert_eq!(
            crate::metadata::load_album_file_hash(&moved).unwrap().hash,
            hash
        );
        let moved_preview =
            crate::thumb::thumb_path(&moved, &target.join(".room237-thumb")).unwrap();
        assert_eq!(image::open(&moved_preview).unwrap().dimensions(), (96, 64));
        assert!(!preview.exists());
    }


    #[test]
    #[cfg(target_os = "macos")]
    fn unicode_collision_names_are_shortened_only_when_the_filesystem_rejects_them() {
        let dir = tempfile::tempdir().unwrap();
        let valid = format!("{}.png", "é".repeat(126));
        fs::write(dir.path().join(&valid), b"retained original").unwrap();
        let expected = format!("{}_1.png", "é".repeat(126));
        assert_eq!(unique_filename(dir.path(), &valid).unwrap(), expected);
        assert_eq!(next_available_name(dir.path(), &valid).unwrap(), expected);
        fs::write(dir.path().join(&expected), b"collision result").unwrap();
        let at_limit = format!("{}ééx.png", "a".repeat(248));
        fs::write(dir.path().join(&at_limit), b"maximum character count").unwrap();
        let shortened = next_available_name(dir.path(), &at_limit).unwrap();
        assert!(shortened.ends_with("_1.png"));
        assert!(shortened.len() <= 255);
        fs::write(dir.path().join(shortened), b"bounded unicode result").unwrap();
        assert_eq!(
            fs::read(dir.path().join(at_limit)).unwrap(),
            b"maximum character count"
        );
    }

    #[test]
    #[cfg(unix)]
    fn import_and_move_collision_checks_keep_dangling_symlinks_intact() {
        let dir = tempfile::tempdir().unwrap();
        let occupied = dir.path().join("photo.png");
        std::os::unix::fs::symlink("missing-source", &occupied).unwrap();
        assert_eq!(
            unique_filename(dir.path(), "photo.png").unwrap(),
            "photo_1.png"
        );
        let mut temporary = tempfile::NamedTempFile::new_in(dir.path()).unwrap();
        std::io::Write::write_all(&mut temporary, b"new original").unwrap();
        let (name, path) = persist_import(dir.path(), "photo.png", temporary).unwrap();
        assert_eq!(name, "photo_1.png");
        assert_eq!(fs::read(path).unwrap(), b"new original");
        assert_eq!(
            fs::read_link(occupied).unwrap(),
            PathBuf::from("missing-source")
        );
    }

    #[test]
    fn maximum_length_names_survive_import_and_artifact_move_collisions() {
        let root = tempfile::tempdir().unwrap();
        let imported = root.path().join("imported");
        let source = root.path().join("source");
        let target = root.path().join("target");
        for dir in [&imported, &source, &target] {
            fs::create_dir(dir).unwrap();
        }
        let name = format!("{}.png", "x".repeat(251));
        fs::write(imported.join(&name), b"retained import").unwrap();
        let mut temporary = tempfile::NamedTempFile::new_in(&imported).unwrap();
        std::io::Write::write_all(&mut temporary, b"new import").unwrap();
        let (import_name, import_path) = persist_import(&imported, &name, temporary).unwrap();
        assert_ne!(import_name, name);
        assert!(import_name.len() <= 255);
        assert_eq!(fs::read(import_path).unwrap(), b"new import");
        assert_eq!(fs::read(imported.join(&name)).unwrap(), b"retained import");

        fs::write(source.join(&name), b"moved original").unwrap();
        fs::write(target.join(&name), b"retained target").unwrap();
        update_file_meta(&source, &name, |entry| entry.favorite = true).unwrap();
        let source_preview =
            crate::thumb::thumb_path(&source.join(&name), &source.join(".room237-thumb")).unwrap();
        fs::create_dir_all(source_preview.parent().unwrap()).unwrap();
        fs::write(&source_preview, b"published preview").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec![name.clone()],
            true
        )
        .unwrap()
        .is_empty());
        let target_name = unique_filename(&imported, &name).unwrap();
        // The import consumed _1; the first move in a different album uses _1.
        assert_ne!(target_name, import_name);
        assert_eq!(
            fs::read(target.join(&import_name)).unwrap(),
            b"moved original"
        );
        assert_eq!(fs::read(target.join(&name)).unwrap(), b"retained target");
        assert!(!source.join(&name).exists());
        assert!(!source_preview.exists());
        let target_preview =
            crate::thumb::thumb_path(&target.join(&import_name), &target.join(".room237-thumb"))
                .unwrap();
        assert_eq!(fs::read(&target_preview).unwrap(), b"published preview");
        assert!(read_album_meta(&target).files[&import_name].favorite);
        assert!(!read_album_meta(&source).files.contains_key(&name));
    }
    use super::*;
    fn fake_jpeg(_: &Path, dir: &Path) -> Result<tempfile::NamedTempFile, String> {
        let mut file = tempfile::NamedTempFile::new_in(dir).unwrap();
        file.write_all(b"converted JPEG fixture").unwrap();
        Ok(file)
    }
    #[test]
    fn heic_conversion_reuses_a_recorded_output_without_reencoding() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("photo.heic");
        fs::write(&source, b"HEIC fixture").unwrap();
        let output = ensure_heic_copy_with(&source, fake_jpeg).unwrap();
        assert_eq!(
            ensure_heic_copy_with(&source, |_, _| panic!("must not reencode")).unwrap(),
            output
        );
        assert_eq!(fs::read(source).unwrap(), b"HEIC fixture");
        assert_eq!(media_files_for_album(dir.path()).unwrap(), vec![output]);
    }
    #[test]
    fn heic_conversion_preserves_a_different_existing_jpeg() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("photo.heic");
        let existing = dir.path().join("photo.jpeg");
        fs::write(&source, b"HEIC fixture").unwrap();
        fs::write(&existing, b"user JPEG").unwrap();
        let output = ensure_heic_copy_with(&source, fake_jpeg).unwrap();
        assert_ne!(output, existing);
        assert_eq!(fs::read(existing).unwrap(), b"user JPEG");
        assert_eq!(fs::read(output).unwrap(), b"converted JPEG fixture");
    }
    #[test]
    fn heic_conversion_adopts_identical_legacy_bytes_and_preserves_user_fields() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("photo.heic");
        let existing = dir.path().join("photo.jpeg");
        fs::write(&source, b"HEIC fixture").unwrap();
        fs::write(&existing, b"converted JPEG fixture").unwrap();
        crate::metadata::update_file_meta(dir.path(), "photo.jpeg", |entry| {
            entry.favorite = true;
            entry.meta = Some("user metadata".into());
        })
        .unwrap();
        assert_eq!(ensure_heic_copy_with(&source, fake_jpeg).unwrap(), existing);
        let snapshot = read_album_meta_checked(dir.path()).unwrap();
        assert!(snapshot.files["photo.jpeg"].favorite);
        assert_eq!(
            snapshot.files["photo.jpeg"].meta.as_deref(),
            Some("user metadata")
        );
        assert_eq!(media_files_for_album(dir.path()).unwrap().len(), 1);
    }
    #[test]
    fn heic_conversion_rejects_a_source_replaced_during_encoding() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("photo.heic");
        fs::write(&source, b"HEIC fixture").unwrap();
        let result = ensure_heic_copy_with(&source, |path, dir| {
            let replacement = dir.join("replacement");
            fs::write(&replacement, b"different HEIC fixture").unwrap();
            fs::rename(replacement, path).unwrap();
            fake_jpeg(path, dir)
        });
        assert!(result.unwrap_err().contains("changed"));
        assert!(!dir.path().join("photo.jpeg").exists());
        assert_eq!(fs::read(source).unwrap(), b"different HEIC fixture");
    }
    #[test]
    fn heic_conversion_ignores_a_stale_binding_and_preserves_a_foreign_edit() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("photo.heic");
        fs::write(&source, b"HEIC fixture").unwrap();
        let old = ensure_heic_copy_with(&source, fake_jpeg).unwrap();
        fs::write(&old, b"edited JPEG fixture").unwrap();
        let new = ensure_heic_copy_with(&source, fake_jpeg).unwrap();
        assert_ne!(old, new);
        assert_eq!(fs::read(old).unwrap(), b"edited JPEG fixture");
        assert_eq!(fs::read(new).unwrap(), b"converted JPEG fixture");
    }
    #[test]
    fn heic_conversion_does_not_encode_against_corrupt_metadata() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("photo.heic");
        fs::write(&source, b"HEIC fixture").unwrap();
        fs::create_dir(dir.path().join(".room237-metadata")).unwrap();
        fs::write(dir.path().join(".room237-metadata/album.json"), b"{broken").unwrap();
        assert!(ensure_heic_copy_with(&source, |_, _| panic!("must not encode")).is_err());
        assert!(!dir.path().join("photo.jpeg").exists());
    }

    #[test]
    fn move_without_artifacts_does_not_require_metadata_leases() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("source");
        let target = root.path().join("target");
        fs::create_dir_all(&source).unwrap();
        fs::create_dir_all(&target).unwrap();
        fs::create_dir(source.join(".room237-metadata.lock")).unwrap();
        fs::create_dir(target.join(".room237-metadata.lock")).unwrap();
        fs::write(source.join("a.jpg"), b"original").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["a.jpg".into()],
            false
        )
        .unwrap()
        .is_empty());
        assert!(!source.join("a.jpg").exists());
        assert_eq!(fs::read(target.join("a.jpg")).unwrap(), b"original");
        assert!(!target.join(".room237-metadata").exists());
    }

    fn heic_move_fixture() -> (tempfile::TempDir, PathBuf, PathBuf, PathBuf) {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("source");
        let target = root.path().join("target");
        fs::create_dir(&source).unwrap();
        fs::create_dir(&target).unwrap();
        fs::write(source.join("photo.heic"), b"HEIC fixture").unwrap();
        let jpeg = ensure_heic_copy_with(&source.join("photo.heic"), fake_jpeg).unwrap();
        crate::metadata::update_file_meta(&source, "photo.jpeg", |entry| {
            entry.favorite = true;
            entry.meta = Some("manual metadata".into());
        })
        .unwrap();
        (root, source, target, jpeg)
    }

    #[test]
    fn move_crash_child() {
        let Ok(root) = std::env::var("ROOM237_TEST_DIR") else {
            return;
        };
        let role = std::env::var("ROOM237_TEST_ROLE").unwrap();
        let phase = role.split(':').next().unwrap();
        let root = Path::new(&root);
        if phase == "tree_renamer" {
            let name = role.split(':').nth(1).unwrap();
            let result = rename_album(root.to_string_lossy().into(), name.into(), "renamed".into());
            fs::write(root.join("done"), serde_json::to_vec(&result).unwrap()).unwrap();
            return;
        }
        if phase == "tree_waiter" || phase == "thumb_waiter" {
            fs::write(root.join("started"), b"started").unwrap();
            let result = if phase == "thumb_waiter" {
                ensure_thumb_with_settings(
                    &root.join("source/photo.jpeg"),
                    &root.join("source/.room237-thumb"),
                    &read_settings(),
                )
                .map(|path| vec![path.to_string_lossy().into_owned()])
            } else {
                move_media_batch_blocking(
                    root.join("source"),
                    root.join("target"),
                    vec!["photo.jpeg".into()],
                    true,
                )
            };
            fs::write(root.join("done"), serde_json::to_vec(&result).unwrap()).unwrap();
            return;
        }
        if phase == "recover" {
            let step = role.split(':').nth(1).unwrap();
            crate::media_move::recover_album_with_hook(&root.join("target"), |current| {
                if current == step {
                    fs::write(root.join("ready"), step.as_bytes()).unwrap();
                    loop {
                        std::thread::sleep(std::time::Duration::from_millis(10));
                    }
                }
                Ok(())
            })
            .unwrap();
            panic!("recovery did not reach crash checkpoint");
        }
        move_media_batch_with_hook(
            root.join("source"),
            root.join("target"),
            vec!["photo.jpeg".into()],
            !role.contains("no-artifacts"),
            |current, source, _| {
                let reached = if phase == "partial_pair" {
                    current == "before_rename" && source.file_name().unwrap() == "photo.heic"
                } else {
                    current == phase
                };
                if reached {
                    fs::write(root.join("ready"), phase.as_bytes()).unwrap();
                    loop {
                        std::thread::sleep(std::time::Duration::from_millis(10));
                    }
                }
                Ok(())
            },
        )
        .unwrap();
        panic!("move did not reach crash checkpoint");
    }

    fn assert_move_recovers_after_process_death(phase: &str) {
        let (root, source, target, _) = heic_move_fixture();
        let mut child =
            crate::test_support::spawn_test("album::tests::move_crash_child", root.path(), phase);
        crate::test_support::wait_for_file(&root.path().join("ready"));
        child.0.kill().unwrap();
        assert!(!child.0.wait().unwrap().success());
        if phase.contains("target-first") {
            get_album_size(target.to_string_lossy().into()).unwrap();
        }
        crate::media_delete::recover_album(&source).unwrap();
        crate::media_delete::recover_album(&target).unwrap();

        let restored = source.join("photo.jpeg").is_file()
            && source.join("photo.heic").is_file()
            && !target.join("photo.jpeg").exists()
            && !target.join("photo.heic").exists();
        let finished = target.join("photo.jpeg").is_file()
            && target.join("photo.heic").is_file()
            && !source.join("photo.jpeg").exists()
            && !source.join("photo.heic").exists();
        assert!(restored || finished, "crash split the HEIC/JPEG pair");
        let committed =
            phase.starts_with("move_committed") || phase.starts_with("after_move_commit");
        if committed {
            assert!(finished, "committed move must stay at destination");
        } else {
            assert!(restored, "uncommitted move must restore originals");
        }
        let owner = if restored { &source } else { &target };
        let metadata = read_album_meta_checked(owner).unwrap();
        if !phase.contains("no-artifacts") || restored {
            let jpeg = metadata.files.get("photo.jpeg").expect("JPEG state lost");
            assert!(jpeg.favorite, "favorite lost after crash");
            assert_eq!(jpeg.meta.as_deref(), Some("manual metadata"));
        } else {
            assert!(!metadata.files.contains_key("photo.jpeg"));
            assert!(read_album_meta_checked(&source).unwrap().files["photo.jpeg"].favorite);
        }
        assert_eq!(
            ensure_heic_copy_with(&owner.join("photo.heic"), |_, _| panic!(
                "recovery must preserve conversion binding"
            ))
            .unwrap(),
            owner.join("photo.jpeg")
        );
        for dir in [&source, &target] {
            assert!(!fs::read_dir(dir).unwrap().any(|entry| entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .starts_with(".room237-move-")));
        }
    }

    #[test]
    fn move_process_death_between_pair_renames_recovers_one_logical_photo() {
        assert_move_recovers_after_process_death("partial_pair");
    }

    #[test]
    fn move_process_death_before_metadata_preserves_photo_state() {
        assert_move_recovers_after_process_death("before_metadata");
    }

    #[test]
    fn move_process_death_after_target_metadata_restores_both_albums() {
        assert_move_recovers_after_process_death("after_target_metadata");
    }

    #[test]
    fn move_process_death_after_source_metadata_restores_both_albums() {
        assert_move_recovers_after_process_death("after_source_metadata");
    }

    #[test]
    fn move_process_death_before_commit_restores_photo() {
        assert_move_recovers_after_process_death("before_move_commit");
    }

    #[test]
    fn move_process_death_after_commit_keeps_photo() {
        assert_move_recovers_after_process_death("move_committed");
    }

    #[test]
    fn committed_move_recovers_previews_and_legacy_metadata_after_process_death() {
        for phase in ["move_committed", "after_move_commit"] {
            let (root, source, target, _) = heic_move_fixture();
            fs::create_dir_all(source.join(".room237-thumb")).unwrap();
            fs::write(source.join(".room237-thumb/photo.jpeg.webp"), b"preview").unwrap();
            fs::write(source.join(".room237-metadata/photo.jpeg.meta"), b"legacy").unwrap();
            let mut child = crate::test_support::spawn_test(
                "album::tests::move_crash_child",
                root.path(),
                phase,
            );
            crate::test_support::wait_for_file(&root.path().join("ready"));
            child.0.kill().unwrap();
            child.0.wait().unwrap();
            crate::media_delete::recover_album(&target).unwrap();
            crate::media_delete::recover_album(&source).unwrap();
            assert_eq!(
                fs::read(target.join(".room237-thumb/photo.jpeg.webp")).unwrap(),
                b"preview"
            );
            assert_eq!(
                fs::read(target.join(".room237-metadata/photo.jpeg.meta")).unwrap(),
                b"legacy"
            );
            assert!(!source.join(".room237-thumb/photo.jpeg.webp").exists());
            assert!(!source.join(".room237-metadata/photo.jpeg.meta").exists());
        }
    }

    #[test]
    fn move_artifact_recovery_respects_collision_rollback_and_disabled_transfer() {
        for role in [
            "move_committed",
            "after_source_metadata",
            "move_committed:no-artifacts",
        ] {
            let (root, source, target, _) = heic_move_fixture();
            fs::write(target.join("photo.jpeg"), b"foreign original").unwrap();
            fs::write(source.join(".room237-thumb/photo.jpeg.webp"), b"preview").unwrap();
            fs::write(source.join(".room237-metadata/photo.jpeg.meta"), b"legacy").unwrap();
            let mut child = crate::test_support::spawn_test(
                "album::tests::move_crash_child",
                root.path(),
                role,
            );
            crate::test_support::wait_for_file(&root.path().join("ready"));
            child.0.kill().unwrap();
            child.0.wait().unwrap();
            crate::media_delete::recover_album(&target).unwrap();
            crate::media_delete::recover_album(&source).unwrap();
            assert_eq!(
                fs::read(target.join("photo.jpeg")).unwrap(),
                b"foreign original"
            );
            if role == "move_committed" {
                assert_eq!(
                    fs::read(target.join(".room237-thumb/photo_1.jpeg.webp")).unwrap(),
                    b"preview"
                );
                assert_eq!(
                    fs::read(target.join(".room237-metadata/photo_1.jpeg.meta")).unwrap(),
                    b"legacy"
                );
                assert!(!source.join(".room237-thumb/photo.jpeg.webp").exists());
            } else {
                assert_eq!(
                    fs::read(source.join(".room237-thumb/photo.jpeg.webp")).unwrap(),
                    b"preview"
                );
                assert_eq!(
                    fs::read(source.join(".room237-metadata/photo.jpeg.meta")).unwrap(),
                    b"legacy"
                );
                assert!(!target.join(".room237-thumb/photo_1.jpeg.webp").exists());
                assert!(!target.join(".room237-metadata/photo_1.jpeg.meta").exists());
            }
        }
    }

    #[test]
    fn committed_artifact_recovery_survives_a_second_process_death() {
        let (root, source, target, _) = heic_move_fixture();
        fs::write(source.join(".room237-thumb/photo.jpeg.webp"), b"preview").unwrap();
        fs::write(source.join(".room237-metadata/photo.jpeg.meta"), b"legacy").unwrap();
        for role in ["move_committed", "recover:move_artifact_renamed"] {
            let signal = root.path().join("ready");
            if signal.exists() {
                fs::remove_file(&signal).unwrap();
            }
            let mut child = crate::test_support::spawn_test(
                "album::tests::move_crash_child",
                root.path(),
                role,
            );
            crate::test_support::wait_for_file(&signal);
            child.0.kill().unwrap();
            child.0.wait().unwrap();
        }
        crate::media_delete::recover_album(&target).unwrap();
        crate::media_delete::recover_album(&source).unwrap();
        assert_eq!(
            fs::read(target.join(".room237-thumb/photo.jpeg.webp")).unwrap(),
            b"preview"
        );
        assert_eq!(
            fs::read(target.join(".room237-metadata/photo.jpeg.meta")).unwrap(),
            b"legacy"
        );
        assert!(!source.join(".room237-thumb/photo.jpeg.webp").exists());
        assert!(!source.join(".room237-metadata/photo.jpeg.meta").exists());
    }

    #[test]
    fn preserved_foreign_preview_is_not_marked_current_for_the_moved_original() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("source");
        let target = root.path().join("target");
        for dir in [&source, &target] {
            fs::create_dir_all(dir.join(".room237-thumb")).unwrap();
        }
        fs::write(source.join("a.jpg"), b"original").unwrap();
        fs::write(source.join(".room237-thumb/a.jpg.webp"), b"own preview").unwrap();
        fs::write(target.join(".room237-thumb/a.jpg.webp"), b"foreign preview").unwrap();
        let revision = crate::metadata::source_revision(&source.join("a.jpg")).unwrap();
        update_file_meta(&source, "a.jpg", |entry| {
            entry.favorite = true;
            entry.thumb_version = Some("current version".into());
            entry.thumb_source_revision = Some(revision);
        })
        .unwrap();
        assert!(
            move_media_batch_blocking(source, target.clone(), vec!["a.jpg".into()], true)
                .unwrap()
                .is_empty()
        );
        assert!(crate::metadata::load_thumb_version(&target.join("a.jpg")).is_none());
        assert!(read_album_meta_checked(&target).unwrap().files["a.jpg"].favorite);
        assert_eq!(
            fs::read(target.join(".room237-thumb/a.jpg.webp")).unwrap(),
            b"foreign preview"
        );
    }

    #[test]
    fn move_process_death_recovers_from_target_with_both_artifact_settings() {
        for phase in [
            "partial_pair",
            "before_metadata",
            "after_target_metadata",
            "after_source_metadata",
            "before_move_commit",
            "move_committed",
        ] {
            for option in ["", ":no-artifacts"] {
                assert_move_recovers_after_process_death(&format!("{phase}:target-first{option}"));
            }
        }
    }

    #[test]
    fn interrupted_move_recovery_is_idempotent_through_metadata_and_locator_cleanup() {
        for phase in [
            "move_recovery_files_restored",
            "move_recovery_metadata_target",
            "move_recovery_metadata_source",
            "move_recovery_settled",
            "move_recovery_peer_removed",
        ] {
            let (root, source, target, _) = heic_move_fixture();
            for role in [
                "after_source_metadata".to_string(),
                format!("recover:{phase}"),
            ] {
                let signal = root.path().join("ready");
                if signal.exists() {
                    fs::remove_file(&signal).unwrap();
                }
                let mut child = crate::test_support::spawn_test(
                    "album::tests::move_crash_child",
                    root.path(),
                    &role,
                );
                crate::test_support::wait_for_file(&signal);
                child.0.kill().unwrap();
                child.0.wait().unwrap();
            }
            get_album_size(target.to_string_lossy().into()).unwrap();
            get_album_size(source.to_string_lossy().into()).unwrap();
            assert_eq!(
                fs::read(source.join("photo.heic")).unwrap(),
                b"HEIC fixture"
            );
            assert!(!target.join("photo.jpeg").exists());
            assert!(read_album_meta_checked(&source).unwrap().files["photo.jpeg"].favorite);
            assert_eq!(
                ensure_heic_copy_with(&source.join("photo.heic"), |_, _| panic!(
                    "must reuse restored binding"
                ))
                .unwrap(),
                source.join("photo.jpeg")
            );
            for album in [&source, &target] {
                assert!(!fs::read_dir(album).unwrap().any(|entry| entry
                    .unwrap()
                    .file_name()
                    .to_string_lossy()
                    .starts_with(".room237-move-")));
            }
        }
    }

    #[test]
    fn renaming_an_album_recovers_its_interrupted_media_move_first() {
        let (root, _source, target, _) = heic_move_fixture();
        let mut child = crate::test_support::spawn_test(
            "album::tests::move_crash_child",
            root.path(),
            "after_source_metadata",
        );
        crate::test_support::wait_for_file(&root.path().join("ready"));
        child.0.kill().unwrap();
        child.0.wait().unwrap();
        let renamed = rename_album(
            root.path().to_string_lossy().into(),
            "source".into(),
            "renamed".into(),
        )
        .unwrap();
        let source = Path::new(&renamed.new_path);
        assert_eq!(
            fs::read(source.join("photo.heic")).unwrap(),
            b"HEIC fixture"
        );
        assert!(!target.join("photo.jpeg").exists());
        assert!(read_album_meta_checked(source).unwrap().files["photo.jpeg"].favorite);
        assert_eq!(
            ensure_heic_copy_with(&source.join("photo.heic"), |_, _| panic!(
                "must reuse binding"
            ))
            .unwrap(),
            source.join("photo.jpeg")
        );
    }

    #[test]
    fn album_tree_rename_guard_excludes_a_media_move_in_another_process() {
        for role in ["tree_waiter", "thumb_waiter"] {
            let (root, source, target, _) = heic_move_fixture();
            let moving = prepare_album_tree_rename(root.path(), &source).unwrap();
            let mut child = crate::test_support::spawn_test(
                "album::tests::move_crash_child",
                root.path(),
                role,
            );
            crate::test_support::wait_for_file(&root.path().join("started"));
            let deadline = std::time::Instant::now() + std::time::Duration::from_millis(500);
            while std::time::Instant::now() < deadline && !root.path().join("done").exists() {
                std::thread::sleep(std::time::Duration::from_millis(5));
            }
            assert!(
                !root.path().join("done").exists(),
                "another process moved originals while tree rename guard was held"
            );
            let renamed = root.path().join("renamed");
            rename_noclobber(&source, &renamed).unwrap();
            drop(moving);
            crate::test_support::wait_for_file(&root.path().join("done"));
            assert!(child.0.wait().unwrap().success());
            let result: Result<Vec<String>, String> =
                serde_json::from_slice(&fs::read(root.path().join("done")).unwrap()).unwrap();
            assert!(
                result.is_err(),
                "old album path must not publish a move after rename"
            );
            assert_eq!(
                fs::read(renamed.join("photo.heic")).unwrap(),
                b"HEIC fixture"
            );
            assert!(renamed.join("photo.jpeg").is_file());
            assert!(!target.join("photo.jpeg").exists());
            assert!(read_album_meta_checked(&renamed).unwrap().files["photo.jpeg"].favorite);
            assert!(!source.exists());
        }
    }

    #[test]
    fn tree_lease_protects_albums_created_after_the_subtree_was_scanned() {
        let (root, source, target, _) = heic_move_fixture();
        let untouched = root.path().join("to-rename");
        fs::create_dir(&untouched).unwrap();
        let moving = prepare_album_tree_rename(root.path(), &untouched).unwrap();
        let fresh = root.path().join("new-parent");
        fs::create_dir(&fresh).unwrap();
        fs::rename(&source, fresh.join("source")).unwrap();
        fs::rename(&target, fresh.join("target")).unwrap();
        let mut child = crate::test_support::spawn_test(
            "album::tests::move_crash_child",
            &fresh,
            "tree_waiter",
        );
        crate::test_support::wait_for_file(&fresh.join("started"));
        std::thread::sleep(std::time::Duration::from_millis(200));
        assert!(!fresh.join("done").exists());
        drop(moving);
        crate::test_support::wait_for_file(&fresh.join("done"));
        assert!(child.0.wait().unwrap().success());
        let result: Result<Vec<String>, String> =
            serde_json::from_slice(&fs::read(fresh.join("done")).unwrap()).unwrap();
        assert!(result.unwrap().is_empty());
        assert!(fresh.join("target/photo.heic").is_file());
        assert!(fresh.join("target/photo.jpeg").is_file());
        assert!(
            read_album_meta_checked(&fresh.join("target"))
                .unwrap()
                .files["photo.jpeg"]
                .favorite
        );
    }

    #[test]
    fn first_tree_lease_waits_for_a_move_that_started_before_the_marker_existed() {
        let (root, source, target, _) = heic_move_fixture();
        let mut child = crate::test_support::spawn_test(
            "album::tests::move_crash_child",
            root.path(),
            "before_metadata",
        );
        crate::test_support::wait_for_file(&root.path().join("ready"));
        let path = root.path().to_string_lossy().into_owned();
        let (tx, rx) = std::sync::mpsc::channel();
        let rename = std::thread::spawn(move || {
            tx.send(rename_album(path, "source".into(), "renamed".into()))
                .unwrap();
        });
        crate::test_support::wait_for_file(&root.path().join(".room237-tree.lock"));
        assert!(matches!(
            rx.recv_timeout(std::time::Duration::from_millis(200)),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout)
        ));
        child.0.kill().unwrap();
        child.0.wait().unwrap();
        let renamed = rx
            .recv_timeout(std::time::Duration::from_secs(15))
            .unwrap()
            .unwrap();
        rename.join().unwrap();
        let renamed = Path::new(&renamed.new_path);
        assert_eq!(
            fs::read(renamed.join("photo.heic")).unwrap(),
            b"HEIC fixture"
        );
        assert!(renamed.join("photo.jpeg").is_file());
        assert!(!source.exists());
        assert!(!target.join("photo.jpeg").exists());
        assert!(read_album_meta_checked(renamed).unwrap().files["photo.jpeg"].favorite);
    }

    #[cfg(unix)]
    #[test]
    fn renaming_a_large_tree_works_with_a_small_descriptor_limit() {
        if std::env::var("ROOM237_TEST_ROLE").as_deref() == Ok("tree-fd-limit") {
            let root = PathBuf::from(std::env::var_os("ROOM237_TEST_DIR").unwrap());
            let source = root.join("source");
            fs::create_dir(&source).unwrap();
            for index in 0..120 {
                let album = source.join(format!("album-{index:03}"));
                fs::create_dir(&album).unwrap();
                fs::write(album.join("a.jpg"), b"original").unwrap();
            }
            let mut saved = libc::rlimit {
                rlim_cur: 0,
                rlim_max: 0,
            };
            assert_eq!(
                unsafe { libc::getrlimit(libc::RLIMIT_NOFILE, &mut saved) },
                0
            );
            let limited = libc::rlimit {
                rlim_cur: 64.min(saved.rlim_max),
                rlim_max: saved.rlim_max,
            };
            assert_eq!(unsafe { libc::setrlimit(libc::RLIMIT_NOFILE, &limited) }, 0);
            let result = rename_album(
                root.to_string_lossy().into(),
                "source".into(),
                "renamed".into(),
            );
            assert_eq!(unsafe { libc::setrlimit(libc::RLIMIT_NOFILE, &saved) }, 0);
            let renamed = result.expect("tree rename must not keep a descriptor for every album");
            for index in 0..120 {
                assert_eq!(
                    fs::read(Path::new(&renamed.new_path).join(format!("album-{index:03}/a.jpg")))
                        .unwrap(),
                    b"original"
                );
            }
            assert!(!source.exists());
            return;
        }
        let root = tempfile::tempdir().unwrap();
        let mut child = crate::test_support::spawn_test(
            "album::tests::renaming_a_large_tree_works_with_a_small_descriptor_limit",
            root.path(),
            "tree-fd-limit",
        );
        assert!(child.0.wait().unwrap().success());
    }

    #[cfg(unix)]
    #[test]
    fn concurrent_tree_renames_recover_a_journal_between_two_library_roots() {
        let (root, source, target, _) = heic_move_fixture();
        let first = root.path().join("first-library");
        let second = root.path().join("second-library");
        fs::create_dir(&first).unwrap();
        fs::create_dir(&second).unwrap();
        fs::rename(&source, first.join("source")).unwrap();
        fs::rename(&target, second.join("target")).unwrap();
        std::os::unix::fs::symlink(first.join("source"), &source).unwrap();
        std::os::unix::fs::symlink(second.join("target"), &target).unwrap();
        crate::album_tree::initialize(&first).unwrap();
        crate::album_tree::initialize(&second).unwrap();
        let mut moving = crate::test_support::spawn_test(
            "album::tests::move_crash_child",
            root.path(),
            "after_source_metadata",
        );
        crate::test_support::wait_for_file(&root.path().join("ready"));
        moving.0.kill().unwrap();
        moving.0.wait().unwrap();
        let mut a = crate::test_support::spawn_test(
            "album::tests::move_crash_child",
            &first,
            "tree_renamer:source",
        );
        let mut b = crate::test_support::spawn_test(
            "album::tests::move_crash_child",
            &second,
            "tree_renamer:target",
        );
        crate::test_support::wait_for_file(&first.join("done"));
        crate::test_support::wait_for_file(&second.join("done"));
        assert!(a.0.wait().unwrap().success() && b.0.wait().unwrap().success());
        for library in [&first, &second] {
            let result: Result<serde_json::Value, String> =
                serde_json::from_slice(&fs::read(library.join("done")).unwrap()).unwrap();
            assert!(result.is_ok(), "concurrent tree recovery: {result:?}");
        }
        let restored = first.join("renamed");
        assert_eq!(
            fs::read(restored.join("photo.heic")).unwrap(),
            b"HEIC fixture"
        );
        assert!(restored.join("photo.jpeg").is_file());
        assert!(read_album_meta_checked(&restored).unwrap().files["photo.jpeg"].favorite);
        assert!(!second.join("renamed/photo.jpeg").exists());
        assert!(!crate::media_move::has_journal(&restored).unwrap());
        assert!(!crate::media_move::has_journal(&second.join("renamed")).unwrap());
    }

    #[test]
    fn moving_a_parent_album_recovers_journals_in_its_children() {
        let (fixture, _, _, _) = heic_move_fixture();
        let root = tempfile::tempdir().unwrap();
        let container = root.path().join("container");
        fs::rename(fixture.path(), &container).unwrap();
        fs::create_dir(root.path().join("new-parent")).unwrap();
        let mut child = crate::test_support::spawn_test(
            "album::tests::move_crash_child",
            &container,
            "after_source_metadata",
        );
        crate::test_support::wait_for_file(&container.join("ready"));
        child.0.kill().unwrap();
        child.0.wait().unwrap();
        let moved = move_album(
            root.path().to_string_lossy().into(),
            "container".into(),
            Some("new-parent".into()),
        )
        .unwrap();
        let source = Path::new(&moved.new_path).join("source");
        assert_eq!(
            fs::read(source.join("photo.heic")).unwrap(),
            b"HEIC fixture"
        );
        assert!(read_album_meta_checked(&source).unwrap().files["photo.jpeg"].favorite);
        assert!(!Path::new(&moved.new_path)
            .join("target/photo.jpeg")
            .exists());
    }

    #[test]
    fn move_without_artifacts_rejects_corrupt_relationship_metadata_before_moving_files() {
        let (_root, source, target, _) = heic_move_fixture();
        fs::write(source.join(".room237-metadata/album.sqlite"), b"{").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into()],
            false
        )
        .is_err());
        assert!(source.join("photo.jpeg").is_file());
        assert!(source.join("photo.heic").is_file());
        assert!(!target.join("photo.jpeg").exists());
    }

    #[test]
    fn move_without_artifacts_rejects_unreadable_target_metadata_before_moving_files() {
        let (_root, source, target, _) = heic_move_fixture();
        fs::create_dir(target.join(".room237-metadata")).unwrap();
        fs::write(target.join(".room237-metadata/album.json"), b"{").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into()],
            false
        )
        .is_err());
        assert!(source.join("photo.jpeg").is_file());
        assert!(!target.join("photo.jpeg").exists());
        assert_eq!(
            fs::read(target.join(".room237-metadata/album.json")).unwrap(),
            b"{"
        );
    }

    #[test]
    fn planning_multiple_heic_companions_reserves_collision_names() {
        let (_root, source, target, _) = heic_move_fixture();
        fs::write(source.join("photo_1.heic"), b"second HEIC").unwrap();
        let mut binding = read_album_meta_checked(&source).unwrap().files["photo.heic"]
            .heic_conversion
            .clone()
            .unwrap();
        binding.source_revision =
            crate::metadata::source_revision(&source.join("photo_1.heic")).unwrap();
        crate::metadata::update_file_meta(&source, "photo_1.heic", |entry| {
            entry.heic_conversion = Some(binding)
        })
        .unwrap();
        fs::write(target.join("photo.heic"), b"foreign HEIC").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into()],
            true
        )
        .unwrap()
        .is_empty());
        assert_eq!(
            fs::read(target.join("photo.heic")).unwrap(),
            b"foreign HEIC"
        );
        assert_eq!(
            fs::read(target.join("photo_1.heic")).unwrap(),
            b"HEIC fixture"
        );
        assert_eq!(
            fs::read(target.join("photo_2.heic")).unwrap(),
            b"second HEIC"
        );
        assert!(read_album_meta_checked(&target).unwrap().files["photo.jpeg"].favorite);
    }

    #[test]
    fn moved_heic_pair_does_not_reappear_and_reuses_its_target_binding() {
        let (_root, source, target, jpeg) = heic_move_fixture();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into()],
            true
        )
        .unwrap()
        .is_empty());
        assert!(!jpeg.exists());
        assert!(!source.join("photo.heic").exists());
        assert!(ensure_heic_copy_with(&source.join("photo.heic"), fake_jpeg).is_err());
        assert_eq!(
            ensure_heic_copy_with(&target.join("photo.heic"), |_, _| panic!(
                "must reuse binding"
            ))
            .unwrap(),
            target.join("photo.jpeg")
        );
        let metadata = read_album_meta_checked(&target).unwrap();
        assert!(metadata.files["photo.jpeg"].favorite);
        assert_eq!(
            metadata.files["photo.jpeg"].meta.as_deref(),
            Some("manual metadata")
        );
        assert!(media_files_for_album(&source).unwrap().is_empty());
        assert_eq!(
            media_files_for_album(&target).unwrap(),
            vec![target.join("photo.jpeg")]
        );
    }

    #[test]
    fn moving_heic_with_artifacts_disabled_transfers_only_the_binding() {
        let (_root, source, target, _) = heic_move_fixture();
        let preview = source.join(".room237-thumb/photo.jpeg.webp");
        fs::write(&preview, b"preview").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into()],
            false
        )
        .unwrap()
        .is_empty());
        assert_eq!(
            ensure_heic_copy_with(&target.join("photo.heic"), |_, _| panic!("must not encode"))
                .unwrap(),
            target.join("photo.jpeg")
        );
        assert!(preview.exists());
        assert!(!target.join(".room237-thumb/photo.jpeg.webp").exists());
        let target_meta = read_album_meta_checked(&target).unwrap();
        assert!(!target_meta.files.contains_key("photo.jpeg"));
        assert!(!target_meta.files["photo.heic"].favorite);
        assert!(target_meta.files["photo.heic"].meta.is_none());
        assert!(target_meta.files["photo.heic"].hash.is_none());
        assert!(read_album_meta_checked(&source).unwrap().files["photo.jpeg"].favorite);
        assert!(!source.join("photo.heic").exists());
    }

    #[test]
    fn heic_pair_collision_remaps_both_names_and_preserves_foreign_files() {
        let (_root, source, target, _) = heic_move_fixture();
        fs::write(target.join("photo.jpeg"), b"foreign JPEG").unwrap();
        fs::write(target.join("photo.heic"), b"foreign HEIC").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into()],
            true
        )
        .unwrap()
        .is_empty());
        assert_eq!(
            fs::read(target.join("photo.jpeg")).unwrap(),
            b"foreign JPEG"
        );
        assert_eq!(
            fs::read(target.join("photo.heic")).unwrap(),
            b"foreign HEIC"
        );
        let metadata = read_album_meta_checked(&target).unwrap();
        let (raw, binding) = metadata
            .files
            .iter()
            .find_map(|(name, entry)| {
                entry
                    .heic_conversion
                    .as_ref()
                    .map(|binding| (name, binding))
            })
            .unwrap();
        assert_ne!(raw, "photo.heic");
        assert_ne!(binding.output_name, "photo.jpeg");
        assert_eq!(
            ensure_heic_copy_with(&target.join(raw), |_, _| panic!(
                "must reuse remapped binding"
            ))
            .unwrap(),
            target.join(&binding.output_name)
        );
        assert!(metadata.files[&binding.output_name].favorite);
    }

    #[test]
    fn selecting_a_raw_and_its_jpeg_moves_the_pair_once() {
        let (_root, source, target, _) = heic_move_fixture();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec![
                "photo.heic".into(),
                "photo.jpeg".into(),
                "photo.heic".into()
            ],
            true
        )
        .unwrap()
        .is_empty());
        assert_eq!(
            fs::read(target.join("photo.heic")).unwrap(),
            b"HEIC fixture"
        );
        assert_eq!(
            fs::read(target.join("photo.jpeg")).unwrap(),
            b"converted JPEG fixture"
        );
        assert_eq!(media_files_for_album(&target).unwrap().len(), 1);
    }

    #[test]
    fn pair_rename_failure_restores_the_jpeg_and_continues_other_photos() {
        let (_root, source, target, _) = heic_move_fixture();
        fs::write(source.join("other.png"), b"other original").unwrap();
        let failed = move_media_batch_with_hook(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into(), "other.png".into()],
            true,
            |phase, original, destination| {
                if phase == "before_rename" && original.extension().is_some_and(|ext| ext == "heic")
                {
                    fs::write(destination, b"racing foreign file").unwrap();
                }
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(failed, vec!["photo.jpeg"]);
        assert_eq!(
            fs::read(source.join("photo.jpeg")).unwrap(),
            b"converted JPEG fixture"
        );
        assert_eq!(
            fs::read(source.join("photo.heic")).unwrap(),
            b"HEIC fixture"
        );
        assert_eq!(
            fs::read(target.join("photo.heic")).unwrap(),
            b"racing foreign file"
        );
        assert_eq!(
            fs::read(target.join("other.png")).unwrap(),
            b"other original"
        );
    }

    #[test]
    fn failed_pair_metadata_publication_restores_both_original_files() {
        let (_root, source, target, _) = heic_move_fixture();
        let result = move_media_batch_with_hook(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into()],
            true,
            |phase, _, destination| {
                if phase == "before_metadata" {
                    fs::create_dir_all(destination.join(".room237-metadata/album.sqlite"))
                        .map_err(|error| error.to_string())?;
                }
                Ok(())
            },
        );
        assert!(result.is_err());
        assert_eq!(
            fs::read(source.join("photo.jpeg")).unwrap(),
            b"converted JPEG fixture"
        );
        assert_eq!(
            fs::read(source.join("photo.heic")).unwrap(),
            b"HEIC fixture"
        );
        assert!(!target.join("photo.jpeg").exists());
        assert!(!target.join("photo.heic").exists());
        assert!(read_album_meta_checked(&source).unwrap().files["photo.jpeg"].favorite);
    }

    #[test]
    fn externally_changed_heic_is_not_moved_as_a_companion() {
        let (_root, source, target, _) = heic_move_fixture();
        fs::write(source.join("photo.heic"), b"changed HEIC original").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into()],
            true
        )
        .unwrap()
        .is_empty());
        assert_eq!(
            fs::read(source.join("photo.heic")).unwrap(),
            b"changed HEIC original"
        );
        assert!(!target.join("photo.heic").exists());
    }

    #[test]
    fn a_companion_changed_after_group_planning_is_not_moved() {
        let (_root, source, target, _) = heic_move_fixture();
        let failed = move_media_batch_with_hook(
            source.clone(),
            target.clone(),
            vec!["photo.jpeg".into()],
            true,
            |phase, original, _| {
                if phase == "before_rename" && original.extension().is_some_and(|ext| ext == "jpeg")
                {
                    fs::write(source.join("photo.heic"), b"foreign replacement").unwrap();
                }
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(failed, vec!["photo.jpeg"]);
        assert_eq!(
            fs::read(source.join("photo.heic")).unwrap(),
            b"foreign replacement"
        );
        assert_eq!(
            fs::read(source.join("photo.jpeg")).unwrap(),
            b"converted JPEG fixture"
        );
        assert!(!target.join("photo.heic").exists());
    }

    #[test]
    fn move_does_not_inherit_metadata_of_an_absent_destination_photo() {
        for move_artifacts in [true, false] {
            let root = tempfile::tempdir().unwrap();
            let source = root.path().join("source");
            let target = root.path().join("target");
            fs::create_dir(&source).unwrap();
            fs::create_dir(&target).unwrap();
            fs::write(source.join("new.jpg"), b"new photo").unwrap();
            crate::metadata::update_file_meta(&target, "new.jpg", |entry| {
                entry.favorite = true;
                entry.meta = Some("old photo metadata".into());
            })
            .unwrap();
            assert!(move_media_batch_blocking(
                source.clone(),
                target.clone(),
                vec!["new.jpg".into()],
                move_artifacts
            )
            .unwrap()
            .is_empty());
            assert_eq!(fs::read(target.join("new.jpg")).unwrap(), b"new photo");
            assert!(!read_album_meta_checked(&target)
                .unwrap()
                .files
                .contains_key("new.jpg"));
            if !move_artifacts {
                assert!(!source.join(".room237-metadata").exists());
            }
        }
    }

    #[test]
    fn a_successful_retry_through_the_other_pair_member_clears_its_old_failure() {
        let (_root, source, target, _) = heic_move_fixture();
        let mut injected = false;
        let failed = move_media_batch_with_hook(
            source.clone(),
            target.clone(),
            vec!["photo.heic".into(), "photo.jpeg".into()],
            true,
            |phase, original, destination| {
                if !injected
                    && phase == "before_rename"
                    && original.extension().is_some_and(|ext| ext == "heic")
                {
                    fs::write(destination, b"foreign file").map_err(|error| error.to_string())?;
                    injected = true;
                }
                Ok(())
            },
        )
        .unwrap();
        assert!(failed.is_empty());
        assert!(!source.join("photo.jpeg").exists());
        assert!(!source.join("photo.heic").exists());
        assert_eq!(
            fs::read(target.join("photo.heic")).unwrap(),
            b"foreign file"
        );
        assert_eq!(media_files_for_album(&target).unwrap().len(), 1);
    }

    #[test]
    #[ignore = "native filesystem and metadata move benchmark"]
    fn benchmark_large_album_move_before_and_after() {
        let tree_marker = std::env::var_os("ROOM237_BENCH_TREE_MARKER").is_some();
        for sample in 0..3 {
            for improved in if sample % 2 == 0 {
                [false, true]
            } else {
                [true, false]
            } {
                let root = tempfile::tempdir().unwrap();
                if tree_marker {
                    crate::album_tree::initialize(root.path()).unwrap();
                }
                let source = root.path().join("source");
                let target = root.path().join("target");
                fs::create_dir(&source).unwrap();
                fs::create_dir(&target).unwrap();
                let source = source.canonicalize().unwrap();
                let target = target.canonicalize().unwrap();
                crate::metadata::update_album_meta(&source, |album| {
                    for index in 0..10000 {
                        let name = format!("{index:05}.jpg");
                        fs::write(source.join(&name), b"fixture").unwrap();
                        album.files.insert(
                            name,
                            FileMetaEntry {
                                favorite: index % 2 == 0,
                                meta: Some("0".into()),
                                ..Default::default()
                            },
                        );
                    }
                })
                .unwrap();
                let names: Vec<String> = (0..100).map(|index| format!("{index:05}.jpg")).collect();
                let start = std::time::Instant::now();
                if improved {
                    assert!(move_media_batch_blocking(
                        source.clone(),
                        target.clone(),
                        names.clone(),
                        true
                    )
                    .unwrap()
                    .is_empty());
                } else {
                    // Baseline move algorithm, with the same JSON engine.
                    let _moving = MEDIA_MOVE_LOCK.lock().unwrap();
                    let guard =
                        crate::metadata::lock_metadata_for_write(&[&source, &target]).unwrap();
                    let mut moved = Vec::new();
                    for name in &names {
                        assert!(fs::symlink_metadata(source.join(name)).unwrap().is_file());
                        let destination = unique_filename(&target, name).unwrap();
                        rename_noclobber(&source.join(name), &target.join(&destination)).unwrap();
                        moved.push((name.clone(), destination));
                    }
                    transfer_media_metadata_batch_caller_holds_lock(
                        &source, &target, &moved, &guard,
                    )
                    .unwrap();
                    for (old, new) in &moved {
                        for (from, to) in [
                            (
                                crate::thumb::thumb_path(
                                    &source.join(old),
                                    &source.join(".room237-thumb"),
                                )
                                .unwrap(),
                                crate::thumb::thumb_path(
                                    &target.join(new),
                                    &target.join(".room237-thumb"),
                                )
                                .unwrap(),
                            ),
                            (
                                source.join(".room237-metadata").join(format!("{old}.meta")),
                                target.join(".room237-metadata").join(format!("{new}.meta")),
                            ),
                        ] {
                            if from.is_file() {
                                fs::rename(from, to).unwrap();
                            }
                        }
                    }
                }
                let elapsed = start.elapsed();
                assert_eq!(read_album_meta_checked(&source).unwrap().files.len(), 9900);
                let metadata = read_album_meta_checked(&target).unwrap();
                assert_eq!(metadata.files.len(), 100);
                for (index, name) in names.iter().enumerate() {
                    assert!(!source.join(name).exists());
                    assert_eq!(fs::read(target.join(name)).unwrap(), b"fixture");
                    assert_eq!(metadata.files[name].favorite, index % 2 == 0);
                }
                eprintln!("move paired sample={sample}, improved={improved}, tree_marker={tree_marker}, actual_files=10000, selected=100, elapsed={elapsed:?}");
            }
        }
    }

    #[test]
    #[cfg(target_os = "macos")]
    #[ignore = "requires macOS ImageIO and FFmpeg with libwebp"]
    fn real_heic_pair_moves_preserve_the_date_and_honor_artifact_settings() {
        use crate::util::run_prepared_command_with_timeout;
        use image::GenericImageView;
        use std::{process::Command, time::Duration};
        let root = tempfile::tempdir().unwrap();
        crate::album_tree::initialize(root.path()).unwrap();
        let png = root.path().join("seed.png");
        let heic = root.path().join("seed.heic");
        image::RgbImage::from_pixel(64, 32, image::Rgb([255, 0, 0]))
            .save(&png)
            .unwrap();
        let mut command = Command::new("sips");
        command
            .args(["-s", "format", "heic"])
            .arg(&png)
            .arg("--out")
            .arg(&heic);
        run_prepared_command_with_timeout(command, Duration::from_secs(20)).unwrap();
        let raw_bytes = fs::read(&heic).unwrap();
        for artifacts in [true, false] {
            let source = root.path().join(format!("source-{artifacts}"));
            let target = root.path().join(format!("target-{artifacts}"));
            fs::create_dir(&source).unwrap();
            fs::create_dir(&target).unwrap();
            // A pair as 6.x imports left it: the HEIC original and its JPEG copy.
            fs::copy(&heic, source.join("photo.heic")).unwrap();
            let output_name = &ensure_heic_copy(&source.join("photo.heic"))
                .unwrap()
                .file_name()
                .unwrap()
                .to_string_lossy()
                .into_owned();
            crate::thumb::ensure_thumb(&source.join(output_name), &source.join(".room237-thumb"))
                .unwrap();
            crate::metadata::set_media_favorite(
                source.join(output_name).to_string_lossy().into(),
                true,
            )
            .unwrap();
            crate::metadata::set_media_timestamp(
                source.to_string_lossy().into(),
                vec![output_name.clone()],
                123,
            )
            .unwrap();
            let source_preview =
                crate::thumb::thumb_path(&source.join(output_name), &source.join(".room237-thumb"))
                    .unwrap();
            image::RgbImage::from_pixel(64, 32, image::Rgb([0, 0, 255]))
                .save(target.join(output_name))
                .unwrap();
            let foreign_bytes = fs::read(target.join(output_name)).unwrap();
            assert!(move_media_batch_blocking(
                source.clone(),
                target.clone(),
                vec![output_name.clone()],
                artifacts
            )
            .unwrap()
            .is_empty());
            assert!(!source.join("photo.heic").exists());
            assert!(!source.join(output_name).exists());
            assert_eq!(fs::read(target.join(output_name)).unwrap(), foreign_bytes);
            let meta = read_album_meta_checked(&target).unwrap();
            let binding = meta.files["photo.heic"].heic_conversion.as_ref().unwrap();
            let moved = target.join(&binding.output_name);
            assert_ne!(&binding.output_name, output_name);
            assert_eq!(ensure_heic_copy(&target.join("photo.heic")).unwrap(), moved);
            assert_eq!(fs::read(target.join("photo.heic")).unwrap(), raw_bytes);
            assert_eq!(image::open(&moved).unwrap().dimensions(), (64, 32));
            assert_eq!(crate::metadata::read_embedded_timestamp(&moved), Some(123));
            let packed = crate::metadata::get_file_metadata(&moved.to_string_lossy()).unwrap();
            assert_eq!(
                crate::metadata::unpack_file_meta(&packed).unwrap().s,
                Some(123)
            );
            assert_eq!(
                read_album_meta_checked(&target).unwrap().files[&binding.output_name].favorite,
                artifacts
            );
            let preview = crate::thumb::thumb_path(&moved, &target.join(".room237-thumb")).unwrap();
            assert_eq!(preview.exists(), artifacts);
            assert_eq!(source_preview.exists(), !artifacts);
            if artifacts {
                assert_eq!(image::open(&preview).unwrap().dimensions(), (64, 32));
            }
            crate::media_delete::delete_media(moved.to_string_lossy().into()).unwrap();
            assert!(!target.join("photo.heic").exists());
            assert!(!moved.exists());
            assert!(!preview.exists());
            let after_delete = read_album_meta_checked(&target).unwrap();
            assert!(!after_delete.files.contains_key("photo.heic"));
            assert!(!after_delete.files.contains_key(&binding.output_name));
            assert_eq!(fs::read(target.join(output_name)).unwrap(), foreign_bytes);
        }
    }
    #[test]
    fn exhausted_collision_suffix_preserves_both_files_and_continues_the_move_batch() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("source");
        let target = root.path().join("target");
        fs::create_dir(&source).unwrap();
        fs::create_dir(&target).unwrap();
        let name = format!("photo_{}.jpg", usize::MAX);
        fs::write(source.join(&name), b"source original").unwrap();
        fs::write(target.join(&name), b"existing target").unwrap();
        fs::write(source.join("other.jpg"), b"second original").unwrap();
        for file in [name.as_str(), "other.jpg"] {
            crate::metadata::update_file_meta(&source, file, |entry| entry.favorite = true)
                .unwrap();
        }
        let failed = move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec![name.clone(), "other.jpg".into()],
            true,
        )
        .unwrap();
        assert_eq!(failed, vec![name.clone()]);
        assert_eq!(fs::read(source.join(&name)).unwrap(), b"source original");
        assert_eq!(fs::read(target.join(&name)).unwrap(), b"existing target");
        assert_eq!(
            fs::read(target.join("other.jpg")).unwrap(),
            b"second original"
        );
        assert!(!source.join("other.jpg").exists());
        assert!(read_album_meta_checked(&source).unwrap().files[&name].favorite);
        assert!(read_album_meta_checked(&target).unwrap().files["other.jpg"].favorite);
        assert!(!read_album_meta_checked(&source)
            .unwrap()
            .files
            .contains_key("other.jpg"));
    }
    #[test]
    fn album_previews_ignore_deleted_sources_and_unpublished_temporary_files() {
        let root = tempfile::tempdir().unwrap();
        let thumbs = root.path().join(".room237-thumb");
        fs::create_dir(&thumbs).unwrap();
        fs::write(thumbs.join("deleted.jpg.webp"), b"old image").unwrap();
        fs::write(
            thumbs.join(".room237-thumbnail-incomplete.webp"),
            b"incomplete",
        )
        .unwrap();
        assert_eq!(album_thumbnail(&[], &thumbs), None);
        let source = root.path().join("photo.jpg");
        fs::write(&source, b"image source").unwrap();
        let preview = thumbs.join("photo.jpg.webp");
        fs::write(&preview, b"published preview").unwrap();
        assert_eq!(
            album_thumbnail(&[source], &thumbs),
            Some(preview.to_string_lossy().into_owned())
        );
    }

    #[test]
    fn corrupt_first_image_does_not_fail_the_album_preview_lookup() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("corrupt.jpg");
        fs::write(&source, b"invalid image").unwrap();
        assert_eq!(
            album_thumbnail(&[source], &root.path().join(".room237-thumb")),
            None
        );
    }
    #[test]
    fn partial_import_reports_added_files_and_failures_and_continues_after_a_failed_file() {
        let root = tempfile::tempdir().unwrap();
        let dest = root.path().join("album");
        fs::create_dir(&dest).unwrap();
        let source = root.path().join("source.png");
        image::DynamicImage::new_rgb8(32, 32).save(&source).unwrap();
        let imported = add_media_files_blocking(
            dest.to_string_lossy().into(),
            vec![
                IncomingFile {
                    name: "first_001.png".into(),
                    source_path: Some(source.to_string_lossy().into()),
                    data: None,
                },
                IncomingFile {
                    name: "missing.png".into(),
                    source_path: Some(root.path().join("missing.png").to_string_lossy().into()),
                    data: None,
                },
                IncomingFile {
                    name: "third.png".into(),
                    source_path: Some(source.to_string_lossy().into()),
                    data: None,
                },
            ],
        )
        .unwrap();
        assert_eq!(imported.added.len(), 2);
        assert_eq!(imported.failed.len(), 1);
        assert_eq!(imported.failed[0].name, "missing.png");
        assert!(!imported.failed[0].error.is_empty());
        assert!(imported
            .added
            .iter()
            .any(|entry| entry.name == "first_001.png"));
        assert!(dest.join("third.png").is_file());
        assert!(!dest.join("missing.png").exists());
        assert_eq!(
            fs::read(dest.join("first_001.png")).unwrap(),
            fs::read(&source).unwrap()
        );
        let retry = add_media_files_blocking(
            dest.to_string_lossy().into(),
            vec![IncomingFile {
                name: "missing.png".into(),
                source_path: Some(source.to_string_lossy().into()),
                data: None,
            }],
        )
        .unwrap();
        assert_eq!(retry.added.len(), 1);
        assert!(retry.failed.is_empty());
        assert!(!dest.join("first_001_1.png").exists());
    }

    #[test]
    fn published_original_is_reported_even_when_metadata_cannot_be_saved() {
        let root = tempfile::tempdir().unwrap();
        fs::create_dir(root.path().join(".room237-metadata")).unwrap();
        fs::write(
            root.path().join(".room237-metadata/album.json"),
            b"corrupt metadata",
        )
        .unwrap();
        let imported = add_media_files_blocking(
            root.path().to_string_lossy().into(),
            vec![IncomingFile {
                name: "broken.jpg".into(),
                source_path: None,
                data: Some(b"invalid image".to_vec()),
            }],
        )
        .unwrap();
        assert_eq!(imported.added.len(), 1);
        assert!(imported.failed.is_empty());
        assert_eq!(
            fs::read(root.path().join("broken.jpg")).unwrap(),
            b"invalid image"
        );
        assert_eq!(
            fs::read(root.path().join(".room237-metadata/album.json")).unwrap(),
            b"corrupt metadata"
        );
        assert!(
            crate::metadata::unpack_file_meta(&imported.added[0].meta)
                .unwrap()
                .i
        );
    }
    #[test]
    fn album_listing_rejects_metadata_from_old_content_or_parse_settings() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("photo.jpg");
        fs::write(&path, b"initial content").unwrap();
        let settings = crate::settings::AdvancedSettings::default();
        let entry = FileMetaEntry {
            meta: Some(
                crate::metadata::DetachedFileMeta {
                    a: Some(1),
                    s: Some(2),
                    i: true,
                    v: false,
                    w: Some(64),
                    h: Some(48),
                }
                .pack(),
            ),
            meta_source_revision: crate::metadata::source_revision(&path),
            meta_settings_version: Some(settings.metadata_version()),
            ..Default::default()
        };
        assert!(current_album_file_meta(&path, &entry, &settings).is_some());
        let mut changed = settings.clone();
        changed.metadata.parse_creation_time = !changed.metadata.parse_creation_time;
        assert!(current_album_file_meta(&path, &entry, &changed).is_none());
        fs::write(&path, b"replacement with different size").unwrap();
        assert!(current_album_file_meta(&path, &entry, &settings).is_none());
    }
    use crate::metadata::update_file_meta;
    #[test]
    fn batch_collision_preserves_files_and_favorites() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("source");
        let target = dir.path().join("target");
        fs::create_dir(&source).unwrap();
        fs::create_dir(&target).unwrap();
        fs::write(source.join("a.jpg"), b"original a").unwrap();
        fs::write(source.join("a_1.jpg"), b"original a1").unwrap();
        fs::write(target.join("a.jpg"), b"existing").unwrap();
        update_file_meta(&source, "a.jpg", |entry| entry.favorite = true).unwrap();
        update_file_meta(&source, "a_1.jpg", |entry| entry.meta = Some("123".into())).unwrap();
        let failed = move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["a.jpg".into(), "a_1.jpg".into()],
            true,
        )
        .unwrap();
        assert!(failed.is_empty());
        assert_eq!(fs::read(target.join("a.jpg")).unwrap(), b"existing");
        assert_eq!(fs::read(target.join("a_1.jpg")).unwrap(), b"original a");
        assert_eq!(fs::read(target.join("a_2.jpg")).unwrap(), b"original a1");
        let metadata = read_album_meta(&target);
        assert!(metadata.files.get("a_1.jpg").unwrap().favorite);
        assert_eq!(
            metadata.files.get("a_2.jpg").unwrap().meta.as_deref(),
            Some("123")
        );
        assert!(read_album_meta(&source).files.is_empty());
    }
    #[test]
    fn move_rolls_back_file_when_metadata_destination_is_unwritable() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("source");
        let target = dir.path().join("target");
        fs::create_dir(&source).unwrap();
        fs::create_dir(&target).unwrap();
        fs::write(source.join("a.jpg"), b"original").unwrap();
        update_file_meta(&source, "a.jpg", |entry| entry.favorite = true).unwrap();
        fs::write(target.join(".room237-metadata"), b"blocked").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target.clone(),
            vec!["a.jpg".into()],
            true
        )
        .is_err());
        assert_eq!(fs::read(source.join("a.jpg")).unwrap(), b"original");
        assert!(!target.join("a.jpg").exists());
        assert!(
            read_album_meta(&source)
                .files
                .get("a.jpg")
                .unwrap()
                .favorite
        );
    }
    #[test]
    fn rejects_names_before_moving_any_files() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("source");
        let target = dir.path().join("target");
        fs::create_dir(&source).unwrap();
        fs::create_dir(&target).unwrap();
        fs::write(source.join("a.jpg"), b"original").unwrap();
        assert!(move_media_batch_blocking(
            source.clone(),
            target,
            vec!["a.jpg".into(), "../secret.jpg".into()],
            false
        )
        .is_err());
        assert!(source.join("a.jpg").exists());
    }
    #[cfg(unix)]
    #[test]
    fn ignores_symlink_cycles_and_directories_with_media_extensions() {
        let dir = tempfile::tempdir().unwrap();
        let album = dir.path().join("album");
        fs::create_dir(&album).unwrap();
        std::os::unix::fs::symlink(dir.path(), album.join("loop")).unwrap();
        fs::create_dir(album.join("not-a-file.jpg")).unwrap();
        let paths = walk_album_paths(dir.path()).unwrap();
        assert_eq!(paths.len(), 2);
        assert!(media_files_for_album(&album).unwrap().is_empty());
    }
    #[test]
    fn rejects_renaming_root_through_dot_identifier() {
        let dir = tempfile::tempdir().unwrap();
        assert!(rename_album(
            dir.path().to_string_lossy().into(),
            ".".into(),
            "renamed".into()
        )
        .is_err());
        assert!(dir.path().exists());
    }
}
