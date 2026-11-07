use crate::host::{AppHandle, NativeRuntime};

use crate::{
    album::{self, FavoriteDetachedMediaEntry, RenamedAlbumResult},
    duplicates,
    metadata::{self, DetachedAlbum, DetachedMediaEntry, ObservedMedia},
};

// Filesystem work, FFmpeg and OS lock waits must not occupy the IPC thread.
async fn blocking<T: Send + 'static>(
    job: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    tokio::task::spawn_blocking(job)
        .await
        .map_err(|error| error.to_string())?
}

pub async fn get_file_metadata(path: String) -> Result<String, String> {
    blocking(move || metadata::get_file_metadata(&path)).await
}

pub async fn get_albums_detached(
    app: AppHandle<NativeRuntime>,
    root_dir: String,
) -> Result<Vec<DetachedAlbum>, String> {
    blocking(move || album::get_albums_detached(app, root_dir)).await
}

pub async fn list_favorites(
    app: AppHandle<NativeRuntime>,
    root_dir: String,
) -> Result<Vec<FavoriteDetachedMediaEntry>, String> {
    blocking(move || album::list_favorites(app, root_dir)).await
}

pub async fn rename_album(
    root_dir: String,
    album_id: String,
    new_name: String,
) -> Result<RenamedAlbumResult, String> {
    blocking(move || album::rename_album(root_dir, album_id, new_name)).await
}

pub async fn move_album(
    root_dir: String,
    album_id: String,
    new_parent_id: Option<String>,
) -> Result<RenamedAlbumResult, String> {
    blocking(move || album::move_album(root_dir, album_id, new_parent_id)).await
}

pub async fn get_album_size(dir: String) -> Result<u64, String> {
    blocking(move || album::get_album_size(dir)).await
}

pub async fn move_media(source: String, target: String, media: String) -> Result<String, String> {
    blocking(move || album::move_media(source, target, media)).await
}

pub async fn set_media_timestamp(
    album_path: String,
    names: Vec<String>,
    timestamp: i64,
) -> Result<Vec<DetachedMediaEntry>, String> {
    blocking(move || metadata::set_media_timestamp(album_path, names, timestamp)).await
}

pub async fn set_media_favorite(
    path: String,
    favorite: bool,
) -> Result<DetachedMediaEntry, String> {
    blocking(move || metadata::set_media_favorite(path, favorite)).await
}

pub async fn validate_media_revisions(observed: Vec<ObservedMedia>) -> Result<(), String> {
    blocking(move || metadata::validate_media_revisions(observed)).await
}

pub async fn delete_media(path: String) -> Result<(), String> {
    blocking(move || crate::media_delete::delete_media(path)).await
}

pub async fn delete_media_batch(
    album_path: String,
    targets: Vec<crate::media_delete::DeleteTarget>,
    observed: Vec<ObservedMedia>,
) -> Result<crate::media_delete::DeleteBatchResult, String> {
    blocking(move || crate::media_delete::delete_media_batch(album_path, targets, observed)).await
}

pub async fn delete_media_if_unchanged(
    path: String,
    revision: String,
    observed: Vec<ObservedMedia>,
) -> Result<(), String> {
    blocking(move || metadata::delete_media_if_unchanged(path, revision, observed)).await
}

pub async fn mark_non_duplicates(dir: String, files: Vec<String>) -> Result<(), String> {
    blocking(move || duplicates::mark_non_duplicates(dir, files)).await
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[tokio::test(flavor = "current_thread")]
    async fn queued_batch_deletion_does_not_block_other_command_futures() {
        let a = tempfile::tempdir().unwrap();
        let b = tempfile::tempdir().unwrap();
        std::fs::write(a.path().join("photo.png"), b"photo").unwrap();
        let lease = metadata::lock_album_metadata(a.path()).unwrap();
        let album_path = a.path().to_string_lossy().into_owned();
        let deletion = tokio::spawn(delete_media_batch(
            album_path,
            vec![crate::media_delete::DeleteTarget {
                name: "photo.png".into(),
                revision: None,
            }],
            Vec::new(),
        ));
        tokio::time::sleep(Duration::from_millis(100)).await;
        assert!(!deletion.is_finished());
        let independent = tokio::time::timeout(
            Duration::from_secs(2),
            get_album_size(b.path().to_string_lossy().into()),
        )
        .await;
        drop(lease);
        assert_eq!(independent.unwrap().unwrap(), 0);
        let result = tokio::time::timeout(Duration::from_secs(5), deletion)
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        assert_eq!(result.deleted, ["photo.png"]);
        assert!(!a.path().join("photo.png").exists());
    }

    #[tokio::test(flavor = "current_thread")]
    async fn waiting_favorite_write_allows_unrelated_filesystem_command_to_complete() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("fixture.txt");
        std::fs::write(&path, b"fixture").unwrap();
        let source = metadata::source_revision(&path).unwrap();
        metadata::update_file_meta(dir.path(), "fixture.txt", |entry| {
            entry.meta = Some(
                metadata::DetachedFileMeta {
                    a: None,
                    s: None,
                    i: false,
                    v: false,
                    w: None,
                    h: None,
                }
                .pack(),
            );
            entry.meta_source_revision = Some(source);
            entry.meta_settings_version = Some(crate::settings::read_settings().metadata_version());
        })
        .unwrap();

        let lease = metadata::lock_album_metadata(dir.path()).unwrap();
        let favorite = tokio::spawn(set_media_favorite(path.to_string_lossy().into(), true));
        tokio::time::sleep(Duration::from_millis(100)).await;
        assert!(
            !favorite.is_finished(),
            "favorite must wait for the held lease"
        );
        let size = tokio::time::timeout(
            Duration::from_secs(2),
            get_album_size(dir.path().to_string_lossy().into()),
        )
        .await;
        // Release even if the responsiveness assertion fails.
        drop(lease);
        assert_eq!(size.unwrap().unwrap(), 0);
        let result = tokio::time::timeout(Duration::from_secs(5), favorite)
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        assert_eq!(result.favorite, Some(true));
        assert!(metadata::read_album_meta(dir.path()).files["fixture.txt"].favorite);
    }
}
