use std::{fs, path::PathBuf};

use rayon::prelude::*;
use crate::host::{AppHandle, NativeRuntime};

use crate::{
    album::walk_album_paths,
    constants::{IMAGE_EXTENSIONS, VIDEO_EXTENSIONS},
    metadata::{get_file_metadata, lock_metadata_for_write},
    settings::read_settings,
    thumb::{ensure_thumb, thumbnail_maintenance_lock_path, THUMBNAIL_TEMP_PREFIX},
    util::{has_extension, FileLock},
};

fn clear_thumbnail_files(thumb_dir: &std::path::Path) -> Result<(), String> {
    if !thumb_dir.exists() {
        return Ok(());
    }
    let _directory_lease = FileLock::acquire(
        &thumbnail_maintenance_lock_path(thumb_dir),
        std::time::Duration::from_secs(read_settings().ffmpeg.timeout_secs * 2 + 5),
        std::time::Duration::from_millis(5),
    )?;
    // Generators hold the lease shared, so none is running here.
    for entry in fs::read_dir(thumb_dir).map_err(|error| error.to_string())? {
        let entry = entry.map_err(|error| error.to_string())?;
        let path = entry.path();
        let name = entry.file_name();
        let name = name.to_string_lossy();
        // The directory's own leases stay. Unleased temps from older binaries may still have an active owner.
        if (name.starts_with(".room237-") && name.ends_with(".lock"))
            || (name.starts_with(".room237-thumbnail-") && !name.starts_with(THUMBNAIL_TEMP_PREFIX))
        {
            continue;
        }
        if entry
            .file_type()
            .map_err(|error| error.to_string())?
            .is_dir()
        {
            fs::remove_dir_all(path).map_err(|error| error.to_string())?;
        } else {
            fs::remove_file(path).map_err(|error| error.to_string())?;
        }
    }
    Ok(())
}

pub async fn rebuild_thumbnails(root_dir: String) -> Result<u64, String> {
    tokio::task::spawn_blocking(move || rebuild_thumbnails_blocking(root_dir))
        .await
        .map_err(|error| error.to_string())?
}

fn rebuild_thumbnails_blocking(root_dir: String) -> Result<u64, String> {
    let root = PathBuf::from(&root_dir);
    if !root.is_dir() {
        return Err(format!("{} is not a directory", root.display()));
    }

    let mut written = 0_u64;

    for album in walk_album_paths(&root)? {
        let thumb_dir = album.join(".room237-thumb");
        clear_thumbnail_files(&thumb_dir)?;
        crate::util::create_derived_dir(&thumb_dir)?;
        let hidden_heic = crate::metadata::hidden_heic_names(&album)?;

        let media = fs::read_dir(&album)
            .map_err(|e| e.to_string())?
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| {
                !p.file_name().is_some_and(|name| hidden_heic.contains(&*name.to_string_lossy()))
                    && (has_extension(p, IMAGE_EXTENSIONS) || has_extension(p, VIDEO_EXTENSIONS))
            });

        written += media
            .par_bridge()
            .map(|p| ensure_thumb(&p, &thumb_dir).map(|_| 1_u64).unwrap_or(0))
            .sum::<u64>();
    }

    Ok(written)
}

pub async fn rebuild_metadata(app: AppHandle<NativeRuntime>, root_dir: String) -> Result<u64, String> {
    let _ = &app;
    tokio::task::spawn_blocking(move || rebuild_metadata_blocking(root_dir))
        .await
        .map_err(|error| error.to_string())?
}

fn rebuild_metadata_blocking(root_dir: String) -> Result<u64, String> {
    let root = PathBuf::from(&root_dir);
    if !root.is_dir() {
        return Err(format!("{} is not a directory", root.display()));
    }

    let mut written = 0_u64;

    for album in walk_album_paths(&root)? {
        let mut visited = std::collections::HashSet::new();
        let hidden_heic = crate::metadata::hidden_heic_names(&album)?;
        for entry in fs::read_dir(&album).map_err(|e| e.to_string())? {
            let path = entry.map_err(|e| e.to_string())?.path();
            if path.file_name().is_some_and(|name| hidden_heic.contains(&*name.to_string_lossy())) {
                continue;
            }
            if !has_extension(&path, IMAGE_EXTENSIONS) && !has_extension(&path, VIDEO_EXTENSIONS) {
                continue;
            }
            if !visited.insert(path.clone()) {
                continue;
            }

            let _ = get_file_metadata(&path.to_string_lossy())?;
            written += 1;
        }
    }

    Ok(written)
}

pub async fn reset_duplicates(root_dir: String) -> Result<u64, String> {
    tokio::task::spawn_blocking(move || reset_duplicates_blocking(root_dir))
        .await
        .map_err(|error| error.to_string())?
}

fn reset_duplicates_blocking(root_dir: String) -> Result<u64, String> {
    let root = PathBuf::from(&root_dir);
    if !root.is_dir() {
        return Err(format!("{} is not a directory", root.display()));
    }

    let mut removed = 0_u64;

    for album in walk_album_paths(&root)? {
        crate::metadata::prepare_database(&album)?;
        let _guard = lock_metadata_for_write(&[&album])?;
        if crate::album_db::clear_ignores(&album.join(".room237-metadata/album.sqlite"))? {
            removed += 1;
        }
    }

    Ok(removed)
}

pub async fn clear_room237_artifacts(root_dir: String) -> Result<u64, String> {
    tokio::task::spawn_blocking(move || clear_room237_artifacts_blocking(root_dir))
        .await
        .map_err(|error| error.to_string())?
}

fn clear_room237_artifacts_blocking(root_dir: String) -> Result<u64, String> {
    let root = PathBuf::from(&root_dir);
    if !root.is_dir() {
        return Err(format!("{} is not a directory", root.display()));
    }

    let mut cleared = 0_u64;

    let mut dirs = walk_album_paths(&root)?;
    dirs.push(root.clone());
    for dir in dirs {
        let guard = lock_metadata_for_write(&[&dir])?;
        let meta_dir = dir.join(".room237-metadata");
        if meta_dir.exists() {
            if let Err(e) = fs::remove_dir_all(&meta_dir) {
                log::warn!("failed to remove {}: {}", meta_dir.display(), e);
            } else {
                cleared += 1;
            }
        }

        let legacy_meta_file = dir.join(".room237-meta.json");
        if legacy_meta_file.exists() {
            if let Err(e) = fs::remove_file(&legacy_meta_file) {
                log::warn!("failed to remove {}: {}", legacy_meta_file.display(), e);
            } else {
                cleared += 1;
            }
        }

        let thumb_dir = dir.join(".room237-thumb");
        // A thumbnail worker persists metadata while holding its preview lock.
        // Release metadata locks before waiting on any preview lock.
        drop(guard);
        if thumb_dir.exists() {
            if let Err(e) = clear_thumbnail_files(&thumb_dir) {
                log::warn!("failed to remove {}: {}", thumb_dir.display(), e);
            } else {
                cleared += 1;
            }
        }
    }

    crate::catalog::clear(&root)?;
    Ok(cleared)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn thumbnail_cleanup_waits_for_shared_owners_and_removes_temps_after_owner_death() {
        use crate::test_support::{spawn_test, wait_for_file};
        use std::time::Duration;
        if let Ok(dir) = std::env::var("ROOM237_TEST_DIR") {
            let dir = std::path::PathBuf::from(dir);
            let _lease = FileLock::acquire_shared(
                &thumbnail_maintenance_lock_path(&dir),
                Duration::from_secs(2),
                Duration::from_millis(1),
            )
            .unwrap();
            fs::write(
                dir.join(".room237-thumbnail-v2-child.webp"),
                b"active child",
            )
            .unwrap();
            fs::write(dir.join("ready"), b"ready").unwrap();
            loop {
                std::thread::sleep(Duration::from_millis(20));
            }
        }
        let root = tempfile::tempdir().unwrap();
        let lease = FileLock::acquire_shared(
            &thumbnail_maintenance_lock_path(root.path()),
            Duration::ZERO,
            Duration::from_millis(1),
        )
        .unwrap();
        let mut child = spawn_test(
            "debugging::tests::thumbnail_cleanup_waits_for_shared_owners_and_removes_temps_after_owner_death",
            root.path(),
            "generator",
        );
        wait_for_file(&root.path().join("ready"));
        let child_temp = root.path().join(".room237-thumbnail-v2-child.webp");
        let parent_temp = root.path().join(".room237-thumbnail-v2-parent.webp");
        fs::write(&parent_temp, b"active parent").unwrap();
        let legacy_temp = root.path().join(".room237-thumbnail-legacy.webp");
        fs::write(&legacy_temp, b"unleased legacy owner").unwrap();
        let (sender, receiver) = std::sync::mpsc::channel();
        let dir = root.path().to_path_buf();
        let cleaner = std::thread::spawn(move || sender.send(clear_thumbnail_files(&dir)).unwrap());
        std::thread::sleep(Duration::from_millis(30));
        assert!(receiver.try_recv().is_err());
        assert_eq!(fs::read(&parent_temp).unwrap(), b"active parent");
        assert_eq!(fs::read(&child_temp).unwrap(), b"active child");
        drop(lease);
        std::thread::sleep(Duration::from_millis(30));
        assert!(receiver.try_recv().is_err());
        assert!(child_temp.exists());
        child.0.kill().unwrap();
        child.0.wait().unwrap();
        receiver
            .recv_timeout(Duration::from_secs(2))
            .unwrap()
            .unwrap();
        cleaner.join().unwrap();
        assert!(!child_temp.exists());
        assert!(!parent_temp.exists());
        assert!(legacy_temp.exists());
        assert!(thumbnail_maintenance_lock_path(root.path()).is_file());
        let _lease = FileLock::acquire_shared(
            &thumbnail_maintenance_lock_path(root.path()),
            Duration::ZERO,
            Duration::from_millis(1),
        )
        .unwrap();
    }
    #[test]
    fn thumbnail_maintenance_waits_for_a_generator_and_removes_legacy_locks() {
        let root = tempfile::tempdir().unwrap();
        let preview = root.path().join("photo.jpg.webp");
        let legacy_lock = preview.with_extension("lock");
        fs::write(&preview, b"preview").unwrap();
        fs::write(&legacy_lock, b"").unwrap();
        let generator = FileLock::acquire_shared(
            &thumbnail_maintenance_lock_path(root.path()),
            std::time::Duration::ZERO,
            std::time::Duration::from_millis(1),
        )
        .unwrap();
        let active_temp = root.path().join(".room237-thumbnail-active.webp");
        fs::write(&active_temp, b"incomplete").unwrap();
        let (sender, receiver) = std::sync::mpsc::channel();
        let dir = root.path().to_path_buf();
        let worker = std::thread::spawn(move || sender.send(clear_thumbnail_files(&dir)).unwrap());
        std::thread::sleep(std::time::Duration::from_millis(30));
        assert!(receiver.try_recv().is_err());
        assert!(preview.is_file());
        drop(generator);
        receiver
            .recv_timeout(std::time::Duration::from_secs(2))
            .unwrap()
            .unwrap();
        worker.join().unwrap();
        assert!(!preview.exists());
        assert!(!legacy_lock.exists());
        assert!(active_temp.is_file());
    }
}
