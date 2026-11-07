//! Tests using real FFmpeg, deliberately opt-in for environments without it.
use crate::{metadata, settings::AdvancedSettings, thumb, util};
use image::GenericImageView;
use std::fs;

#[test]
#[cfg(unix)]
#[ignore = "requires FFmpeg with libwebp in PATH"]
fn real_pre_epoch_dates_and_external_hardlinks_refresh_without_changing_media() {
    let root = tempfile::tempdir().unwrap();
    let a = root.path().join("a");
    let b = root.path().join("b");
    fs::create_dir(&a).unwrap();
    fs::create_dir(&b).unwrap();
    let path = a.join("old.jpg");
    image::RgbImage::from_pixel(64, 32, image::Rgb([255, 0, 0]))
        .save(&path)
        .unwrap();
    let jpeg = fs::read(&path).unwrap();
    let mut tiff = b"II\x2a\x00\x08\x00\x00\x00".to_vec();
    tiff.extend_from_slice(&1u16.to_le_bytes());
    tiff.extend_from_slice(&0x0132u16.to_le_bytes());
    tiff.extend_from_slice(&2u16.to_le_bytes());
    tiff.extend_from_slice(&20u32.to_le_bytes());
    tiff.extend_from_slice(&26u32.to_le_bytes());
    tiff.extend_from_slice(&0u32.to_le_bytes());
    tiff.extend_from_slice(b"1960:01:01 00:00:00\0");
    let mut tagged = jpeg[..2].to_vec();
    tagged.extend_from_slice(&[0xff, 0xe1]);
    tagged.extend_from_slice(&((tiff.len() + 8) as u16).to_be_bytes());
    tagged.extend_from_slice(b"Exif\0\0");
    tagged.extend_from_slice(&tiff);
    tagged.extend_from_slice(&jpeg[2..]);
    fs::write(&path, &tagged).unwrap();
    assert_eq!(metadata::datetime_original(&path), Some(-315619200));
    let source = metadata::source_revision(&path).unwrap();
    // Simulate the wrong unsigned packed date cached by an older binary.
    let wrapped = ((1u128 << 40) - 315619200) << 40;
    metadata::update_file_meta(&a, "old.jpg", |entry| {
        entry.meta = Some((wrapped | (1u128 << 123)).to_string());
        entry.meta_source_revision = Some(source.clone());
        entry.meta_settings_version = Some("creation-time:true".into());
        entry.favorite = true;
        entry.hash = Some("retained visual hash".into());
    })
    .unwrap();
    let corrected = metadata::get_metadata_with_favorite(&path).unwrap();
    let decoded = metadata::unpack_file_meta(&corrected.meta).unwrap();
    assert_eq!(decoded.s, Some(-315619200));
    assert_eq!((decoded.w, decoded.h), (Some(64), Some(32)));
    assert!(corrected.favorite);
    let preview = thumb::ensure_thumb(&path, &a.join(".room237-thumb")).unwrap();
    let preview_bytes = fs::read(&preview).unwrap();
    let revision = metadata::media_revision(&path);
    metadata::set_media_timestamp(a.to_string_lossy().into(), vec!["old.jpg".into()], -1).unwrap();
    let alias = b.join("linked.jpg");
    fs::hard_link(&path, &alias).unwrap();
    metadata::set_media_favorite(alias.to_string_lossy().into(), false).unwrap();
    let read_date = |path: &std::path::Path| {
        let stored = metadata::get_metadata_with_favorite(path).unwrap();
        metadata::unpack_file_meta(&stored.meta).unwrap().s
    };
    assert_eq!(read_date(&path), Some(-1));
    assert_eq!(read_date(&alias), Some(-1));
    xattr::set(&alias, "user.room237.shoot_ts", b"-42").unwrap();
    assert_eq!(read_date(&path), Some(-42));
    assert_eq!(read_date(&alias), Some(-42));
    xattr::remove(&alias, "user.room237.shoot_ts").unwrap();
    assert_eq!(read_date(&path), Some(-315619200));
    assert_eq!(read_date(&alias), Some(-315619200));
    assert_eq!(
        metadata::source_revision(&path).as_deref(),
        Some(source.as_str())
    );
    assert_eq!(metadata::media_revision(&path), revision);
    assert_eq!(fs::read(&path).unwrap(), tagged);
    assert_eq!(fs::read(&alias).unwrap(), tagged);
    assert_eq!(fs::read(&preview).unwrap(), preview_bytes);
    let album = metadata::read_album_meta(&a);
    assert!(album.files["old.jpg"].favorite);
    assert_eq!(
        album.files["old.jpg"].hash.as_deref(),
        Some("retained visual hash")
    );
    assert!(!metadata::read_album_meta(&b).files["linked.jpg"].favorite);
}

#[test]
#[ignore = "requires FFmpeg with libx264 in PATH"]
fn real_mov_metadata_preserves_pre_epoch_creation_time() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("old.mov");
    let output = std::process::Command::new(ffmpeg_sidecar::paths::ffmpeg_path())
        .args([
            "-hide_banner",
            "-f",
            "lavfi",
            "-i",
            "color=red:s=64x32:r=2",
            "-frames:v",
            "3",
            "-c:v",
            "libx264",
            "-metadata",
            "creation_time=1960-01-01T00:00:00Z",
        ])
        .arg(&path)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let bytes = fs::read(&path).unwrap();
    let packed = metadata::get_file_metadata(path.to_str().unwrap()).unwrap();
    let decoded = metadata::unpack_file_meta(&packed).unwrap();
    assert_eq!(decoded.s, Some(-315619200));
    assert_eq!((decoded.w, decoded.h), (Some(64), Some(32)));
    assert_eq!(fs::read(&path).unwrap(), bytes);
    for extension in ["mp4", "m4v"] {
        let alias = root.path().join(format!("old.{extension}"));
        fs::write(&alias, &bytes).unwrap();
        let packed = metadata::get_file_metadata(alias.to_str().unwrap()).unwrap();
        assert_eq!(
            metadata::unpack_file_meta(&packed).unwrap().s,
            Some(-315619200)
        );
    }
    let mut disabled = AdvancedSettings::default();
    disabled.metadata.parse_creation_time = false;
    let packed = metadata::get_file_metadata_cached_with_settings(&path, &disabled).unwrap();
    assert_eq!(metadata::unpack_file_meta(&packed).unwrap().s, None);
    let packed = metadata::get_file_metadata(path.to_str().unwrap()).unwrap();
    assert_eq!(
        metadata::unpack_file_meta(&packed).unwrap().s,
        Some(-315619200)
    );

    // Keep the existing precedence for a distinct stream creation tag. Only
    // the exact FFmpeg epoch shift of the movie's date should be corrected.
    let mut other_track = bytes.clone();
    let mdhd = other_track
        .windows(4)
        .position(|part| part == b"mdhd")
        .unwrap();
    assert_eq!(other_track[mdhd + 4], 0);
    let timestamp = 1743510896i64;
    other_track[mdhd + 8..mdhd + 12]
        .copy_from_slice(&((timestamp + crate::quicktime::EPOCH_OFFSET) as u32).to_be_bytes());
    let different = root.path().join("different-track.mov");
    fs::write(&different, other_track).unwrap();
    let packed = metadata::get_file_metadata(different.to_str().unwrap()).unwrap();
    assert_eq!(
        metadata::unpack_file_meta(&packed).unwrap().s,
        Some(timestamp)
    );
}

#[test]
#[ignore = "requires FFmpeg with libwebp in PATH"]
fn maximum_length_media_names_generate_distinct_previews_and_refresh_revisions() {
    let root = tempfile::tempdir().unwrap();
    let thumbs = root.path().join(".room237-thumb");
    let mut previews = Vec::new();
    for (suffix, color) in [('a', [255, 0, 0]), ('b', [0, 0, 255])] {
        let name = format!("{}{suffix}.png", "x".repeat(250));
        let original = root.path().join(&name);
        image::RgbImage::from_pixel(64, 32, image::Rgb(color))
            .save(&original)
            .unwrap();
        let source = fs::read(&original).unwrap();
        metadata::set_media_favorite(original.to_string_lossy().into(), true).unwrap();
        let source_revision = metadata::source_revision(&original);
        metadata::update_file_meta(root.path(), &name, |entry| {
            entry.thumb_failed = true;
            entry.thumb_failed_source_revision = source_revision;
            entry.thumb_failed_processing_version =
                Some(AdvancedSettings::default().processing_version());
        })
        .unwrap();
        assert!(!metadata::is_thumb_failed(&original));
        let before = metadata::media_revision(&original);
        let preview = thumb::ensure_thumb(&original, &thumbs).unwrap();
        assert_eq!(image::open(&preview).unwrap().dimensions(), (64, 32));
        assert_ne!(before, metadata::media_revision(&original));
        assert_eq!(thumb::ensure_thumb(&original, &thumbs).unwrap(), preview);
        assert_eq!(fs::read(&original).unwrap(), source);
        assert!(metadata::read_album_meta(root.path()).files[&name].favorite);
        assert!(!metadata::is_thumb_failed(&original));
        let dto = metadata::set_media_favorite(original.to_string_lossy().into(), true).unwrap();
        let serialized = serde_json::to_value(dto).unwrap();
        assert_eq!(
            serialized["thumbnailName"],
            preview.file_name().unwrap().to_str().unwrap()
        );
        assert_eq!(serialized["name"], name);
        previews.push(preview);
    }
    assert_ne!(previews[0], previews[1]);
    assert_ne!(
        fs::read(&previews[0]).unwrap(),
        fs::read(&previews[1]).unwrap()
    );
}

#[test]
#[ignore = "requires FFmpeg with libwebp in PATH"]
fn real_background_metadata_and_thumbnail_complete_while_another_album_lease_is_busy() {
    use std::{
        sync::mpsc,
        thread,
        time::{Duration, Instant},
    };
    let root = tempfile::tempdir().unwrap();
    let blocked = root.path().join("blocked");
    let free = root.path().join("free");
    for dir in [&blocked, &free] {
        fs::create_dir(dir).unwrap();
        image::DynamicImage::new_rgb8(64, 32)
            .save(dir.join("photo.png"))
            .unwrap();
        metadata::update_file_meta(dir, "photo.png", |entry| entry.favorite = true).unwrap();
    }
    let original = fs::read(blocked.join("photo.png")).unwrap();
    let lease = metadata::lock_album_metadata(&blocked).unwrap();
    let (started, ready) = mpsc::channel();
    let (sent_a, result_a) = mpsc::channel();
    let a = blocked.join("photo.png");
    let blocked_worker = thread::spawn(move || {
        let _background = metadata::BackgroundMetadataWrites::enter();
        started.send(()).unwrap();
        sent_a
            .send(metadata::get_file_metadata(a.to_str().unwrap()))
            .unwrap();
    });
    ready.recv().unwrap();
    thread::sleep(Duration::from_millis(200));
    let (sent_b, result_b) = mpsc::channel();
    let b = free.join("photo.png");
    let previews = free.join(".room237-thumb");
    let independent_started = Instant::now();
    let free_worker = thread::spawn(move || {
        let _background = metadata::BackgroundMetadataWrites::enter();
        let result = (|| {
            let packed = metadata::get_file_metadata(b.to_str().unwrap())?;
            let preview = thumb::ensure_thumb(&b, &previews)?;
            Ok::<_, String>((packed, preview))
        })();
        sent_b.send(result).unwrap();
    });
    let independent = result_b.recv_timeout(Duration::from_secs(5));
    let elapsed = independent_started.elapsed();
    let waited = matches!(result_a.try_recv(), Err(mpsc::TryRecvError::Empty));
    drop(lease);
    let packed_a = result_a
        .recv_timeout(Duration::from_secs(10))
        .unwrap()
        .unwrap();
    blocked_worker.join().unwrap();
    free_worker.join().unwrap();
    assert!(waited);
    let (packed_b, preview) = independent.unwrap().unwrap();
    for packed in [packed_a, packed_b] {
        let meta = metadata::unpack_file_meta(&packed).unwrap();
        assert_eq!((meta.w, meta.h), (Some(64), Some(32)));
    }
    let image = image::open(&preview).unwrap();
    assert_eq!((image.width(), image.height()), (64, 32));
    let disk: metadata::AlbumMeta =
        crate::album_db::read(&free.join(".room237-metadata/album.sqlite")).unwrap();
    assert!(disk.files["photo.png"].favorite);
    assert_eq!(
        disk.files["photo.png"].thumb_version.as_deref(),
        Some(AdvancedSettings::default().thumb_version().as_str())
    );
    assert_eq!(fs::read(blocked.join("photo.png")).unwrap(), original);
    eprintln!("real background FFmpeg: independent metadata + thumbnail + persisted acknowledgement={elapsed:?} while another album lease remains held");
}

#[test]
#[ignore = "requires FFmpeg in PATH"]
fn real_thumbnail_generation_waits_for_maintenance_before_creating_temporary_data() {
    use std::time::Duration;
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("photo.png");
    image::DynamicImage::new_rgb8(64, 32).save(&path).unwrap();
    let thumbs = root.path().join(".room237-thumb");
    fs::create_dir(&thumbs).unwrap();
    let lease = util::FileLock::acquire(
        &thumb::thumbnail_maintenance_lock_path(&thumbs),
        Duration::ZERO,
        Duration::from_millis(1),
    )
    .unwrap();
    let (started, ready) = std::sync::mpsc::channel();
    let (sent, result) = std::sync::mpsc::channel();
    let thumb_dir = thumbs.clone();
    let original = path.clone();
    let generator = std::thread::spawn(move || {
        started.send(()).unwrap();
        sent.send(thumb::ensure_thumb(&original, &thumb_dir))
            .unwrap();
    });
    ready.recv().unwrap();
    std::thread::sleep(Duration::from_millis(40));
    assert!(result.try_recv().is_err());
    assert_eq!(fs::read_dir(&thumbs).unwrap().count(), 1);
    drop(lease);
    let preview = result
        .recv_timeout(Duration::from_secs(10))
        .unwrap()
        .unwrap();
    generator.join().unwrap();
    let image = image::open(&preview).unwrap();
    assert_eq!((image.width(), image.height()), (64, 32));
    assert_eq!(
        metadata::load_thumb_version(&path),
        Some(AdvancedSettings::default().thumb_version())
    );
    assert!(!fs::read_dir(&thumbs).unwrap().flatten().any(|entry| entry
        .file_name()
        .to_string_lossy()
        .starts_with(thumb::THUMBNAIL_TEMP_PREFIX)));
}

#[test]
#[ignore = "requires FFmpeg in PATH"]
fn real_image_metadata_thumbnail_and_favorite_preserve_manual_date() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("photo with spaces.jpg");
    image::DynamicImage::new_rgb8(320, 180).save(&path).unwrap();
    let packed = metadata::get_file_metadata(path.to_str().unwrap()).unwrap();
    let meta = metadata::unpack_file_meta(&packed).unwrap();
    assert_eq!((meta.w, meta.h), (Some(320), Some(180)));
    let settings = AdvancedSettings::default();
    let thumb_dir = root.path().join(".room237-thumb");
    let result = thumb::ensure_thumb_with_settings(&path, &thumb_dir, &settings).unwrap();
    assert!(fs::metadata(&result).unwrap().len() > 0);
    assert_eq!(
        metadata::load_thumb_version(&path),
        Some(settings.thumb_version())
    );
    metadata::set_media_timestamp(
        root.path().to_string_lossy().into(),
        vec!["photo with spaces.jpg".into()],
        123456789,
    )
    .unwrap();
    let favorite = metadata::set_media_favorite(path.to_string_lossy().into(), true).unwrap();
    assert_eq!(
        metadata::unpack_file_meta(&favorite.meta).unwrap().s,
        Some(123456789)
    );
    assert_eq!(favorite.favorite, Some(true));
    assert_eq!(
        thumb::ensure_thumb_with_settings(&path, &thumb_dir, &settings).unwrap(),
        result
    );
    image::DynamicImage::new_rgb8(160, 90).save(&path).unwrap();
    // Simulate a replacement while the app was closed or a missed watcher event.
    let refreshed = metadata::get_metadata_with_favorite(&path).unwrap();
    let meta = metadata::unpack_file_meta(&refreshed.meta).unwrap();
    assert_eq!((meta.w, meta.h), (Some(160), Some(90)));
    assert_eq!(meta.s, Some(123456789));
    assert!(refreshed.favorite);
    thumb::ensure_thumb_with_settings(&path, &thumb_dir, &settings).unwrap();
}

#[test]
#[ignore = "requires FFmpeg in PATH"]
fn invalid_image_does_not_publish_a_thumbnail_or_success_version() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("broken.jpg");
    fs::write(&path, b"invalid jpeg").unwrap();
    let thumb_dir = root.path().join(".room237-thumb");
    assert!(
        thumb::ensure_thumb_with_settings(&path, &thumb_dir, &AdvancedSettings::default()).is_err()
    );
    assert!(!thumb::thumb_path(&path, &thumb_dir).unwrap().exists());
    assert_eq!(metadata::load_thumb_version(&path), None);
    assert!(fs::read_dir(&thumb_dir).unwrap().all(|entry| entry
        .unwrap()
        .file_name()
        .to_string_lossy()
        .starts_with(".room237-")));
}

#[test]
fn conversion_does_not_overwrite_existing_jpeg_even_if_source_is_newer() {
    let root = tempfile::tempdir().unwrap();
    let source = root.path().join("photo.heic");
    let target = root.path().join("photo.jpeg");
    fs::write(&target, b"existing photo").unwrap();
    std::thread::sleep(std::time::Duration::from_millis(20));
    fs::write(&source, b"newer source").unwrap();
    assert!(crate::album::ensure_heic_copy(&source).is_err());
    assert_eq!(fs::read(&target).unwrap(), b"existing photo");
}
#[test]
fn media_revision_changes_for_source_and_thumbnail_replacements() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("photo.jpg");
    fs::write(&path, b"first").unwrap();
    let first = metadata::media_revision(&path);
    assert_eq!(first, metadata::media_revision(&path));
    fs::write(&path, b"replacement").unwrap();
    let replaced = metadata::media_revision(&path);
    assert_ne!(first, replaced);
    let thumbnails = root.path().join(".room237-thumb");
    fs::create_dir(&thumbnails).unwrap();
    fs::write(thumbnails.join("photo.jpg.webp"), b"preview").unwrap();
    assert_ne!(replaced, metadata::media_revision(&path));
}

#[test]
#[ignore = "requires FFmpeg with libwebp in PATH"]
fn short_video_uses_first_frame_but_long_video_retains_configured_seek() {
    let root = tempfile::tempdir().unwrap();
    let short = root.path().join("short.mp4");
    let long = root.path().join("long.mp4");
    let status = std::process::Command::new("ffmpeg")
        .args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=c=red:s=64x64:d=0.3",
            "-y",
        ])
        .arg(&short)
        .status()
        .unwrap();
    assert!(status.success());
    let status = std::process::Command::new("ffmpeg")
        .args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=c=red:s=64x64:d=0.5",
            "-f",
            "lavfi",
            "-i",
            "color=c=blue:s=64x64:d=1.5",
            "-filter_complex",
            "[0:v][1:v]concat=n=2:v=1:a=0",
            "-y",
        ])
        .arg(&long)
        .status()
        .unwrap();
    assert!(status.success());
    let thumb_dir = root.path().join(".room237-thumb");
    let settings = AdvancedSettings::default();
    for (path, expected_channel) in [(&short, 0usize), (&long, 2usize)] {
        let thumbnail = thumb::ensure_thumb_with_settings(path, &thumb_dir, &settings).unwrap();
        // image 0.23's WebP decoder exposes luma only; decode RGB through FFmpeg.
        let decoded = root.path().join(format!("decoded-{expected_channel}.png"));
        assert!(std::process::Command::new("ffmpeg")
            .args(["-hide_banner", "-loglevel", "error", "-i"])
            .arg(&thumbnail)
            .arg(&decoded)
            .status()
            .unwrap()
            .success());
        let image = image::open(&decoded).unwrap().to_rgb8();
        let pixel = image.get_pixel(32, 32);
        assert!(pixel[expected_channel] > 180, "wrong seek frame: {pixel:?}");
        assert_eq!(
            metadata::load_thumb_version(path),
            Some(settings.thumb_version())
        );
    }
    let broken = root.path().join("broken.mp4");
    fs::write(&broken, b"invalid video").unwrap();
    assert!(thumb::ensure_thumb_with_settings(&broken, &thumb_dir, &settings).is_err());
    assert!(!thumb::thumb_path(&broken, &thumb_dir).unwrap().exists());
    assert_eq!(metadata::load_thumb_version(&broken), None);
    assert!(!thumb_dir.join("broken.mp4.lock").exists());
}

#[test]
#[ignore = "requires FFmpeg with libwebp in PATH"]
fn thumbnail_generation_is_not_blocked_by_a_lock_file_of_an_older_version() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("valid.png");
    image::DynamicImage::new_rgb8(64, 64).save(&path).unwrap();
    let thumbs = root.path().join(".room237-thumb");
    fs::create_dir(&thumbs).unwrap();
    fs::write(thumbs.join("valid.png.lock"), b"").unwrap();
    let held = util::FileLock::acquire(
        &thumbs.join("valid.png.lock"),
        std::time::Duration::ZERO,
        std::time::Duration::from_millis(1),
    )
    .unwrap();
    let generated =
        thumb::ensure_thumb_with_settings(&path, &thumbs, &AdvancedSettings::default()).unwrap();
    drop(held);
    assert!(fs::metadata(&generated).unwrap().len() > 0);
    assert_eq!(
        metadata::load_thumb_version(&path),
        Some(AdvancedSettings::default().thumb_version())
    );
    thumb::remove_legacy_preview_locks(&thumbs);
    assert!(!thumbs.join("valid.png.lock").exists());
}

#[test]
fn duplicate_deletion_refuses_a_same_name_replacement_with_an_old_revision() {
    let root = tempfile::tempdir().unwrap();
    let file = root.path().join("photo.jpg");
    let retained = root.path().join("retained.jpg");
    fs::write(&file, b"original").unwrap();
    fs::write(&retained, b"original").unwrap();
    let old = metadata::media_revision(&file);
    fs::write(&file, b"new unique contents").unwrap();
    assert!(metadata::delete_media_if_unchanged(
        file.to_string_lossy().into(),
        old.clone(),
        vec![
            metadata::ObservedMedia {
                path: file.to_string_lossy().into(),
                revision: old
            },
            metadata::ObservedMedia {
                path: retained.to_string_lossy().into(),
                revision: metadata::media_revision(&retained),
            }
        ]
    )
    .is_err());
    assert_eq!(fs::read(&file).unwrap(), b"new unique contents");
    let fresh = metadata::media_revision(&file);
    fs::copy(&file, &retained).unwrap();
    metadata::delete_media_if_unchanged(
        file.to_string_lossy().into(),
        fresh.clone(),
        vec![
            metadata::ObservedMedia {
                path: file.to_string_lossy().into(),
                revision: fresh,
            },
            metadata::ObservedMedia {
                path: retained.to_string_lossy().into(),
                revision: metadata::media_revision(&retained),
            },
        ],
    )
    .unwrap();
    assert!(!file.exists());
    assert_eq!(fs::read(retained).unwrap(), b"new unique contents");
}

#[test]
fn duplicate_deletion_also_checks_the_retained_group_member() {
    let root = tempfile::tempdir().unwrap();
    let kept = root.path().join("kept.jpg");
    let deleted = root.path().join("deleted.jpg");
    fs::write(&kept, b"same photo").unwrap();
    fs::write(&deleted, b"same photo").unwrap();
    let kept_revision = metadata::media_revision(&kept);
    let deleted_revision = metadata::media_revision(&deleted);
    fs::write(&kept, b"different unique photo").unwrap();
    let observed = vec![
        metadata::ObservedMedia {
            path: kept.to_string_lossy().into(),
            revision: kept_revision,
        },
        metadata::ObservedMedia {
            path: deleted.to_string_lossy().into(),
            revision: deleted_revision.clone(),
        },
    ];
    assert!(metadata::delete_media_if_unchanged(
        deleted.to_string_lossy().into(),
        deleted_revision,
        observed
    )
    .is_err());
    assert_eq!(fs::read(&deleted).unwrap(), b"same photo");
}

#[test]
#[ignore = "requires FFmpeg with libwebp in PATH"]
fn actual_hashes_and_thumbnails_refresh_after_an_unobserved_replacement() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("replaced.png");
    let mut settings = AdvancedSettings::default();
    settings.duplicates.use_thumbnails_first = true;
    let cfg = settings.hash_config();
    let mut first = image::RgbImage::new(96, 96);
    for (x, _, pixel) in first.enumerate_pixels_mut() {
        *pixel = image::Rgb(if x < 48 { [255, 255, 255] } else { [0, 0, 0] });
    }
    first.save(&path).unwrap();
    let first_hash = crate::duplicates::compute_hash_for_path(
        &metadata::read_album_meta(root.path()),
        &path,
        &cfg,
        &settings,
    )
    .unwrap();
    let thumb = thumb::thumb_path(&path, &root.path().join(".room237-thumb")).unwrap();
    let first_thumb = fs::read(&thumb).unwrap();
    let mut replacement = image::RgbImage::new(96, 96);
    for (_, y, pixel) in replacement.enumerate_pixels_mut() {
        *pixel = image::Rgb(if y < 48 { [255, 255, 255] } else { [0, 0, 0] });
    }
    replacement.save(&path).unwrap();
    // No invalidate_media_content/register_new_media call: simulate a missed event.
    assert!(metadata::load_album_file_hash(&path).is_none());
    let new_hash = crate::duplicates::compute_hash_for_path(
        &metadata::read_album_meta(root.path()),
        &path,
        &cfg,
        &settings,
    )
    .unwrap();
    assert_ne!(first_hash, new_hash);
    assert_ne!(first_thumb, fs::read(&thumb).unwrap());
    assert!(metadata::load_album_file_hash(&path).is_some());
}

#[test]
#[ignore = "requires FFmpeg with video encoders and libwebp"]
fn configured_video_containers_have_video_metadata_and_decodable_previews() {
    use std::{process::Command, time::Duration};
    let root = tempfile::tempdir().unwrap();
    for extension in crate::constants::VIDEO_EXTENSIONS {
        let codec = match *extension {
            "mp4" | "mov" | "mkv" => "libx264",
            "m4v" | "avi" => "mpeg4",
            "webm" => "libvpx",
            "flv" => "flv",
            "ogg" | "ogv" => "libtheora",
            "wmv" => "wmv2",
            "mpg" | "mpeg" => "mpeg1video",
            "3gp" => "mpeg4",
            "mts" | "m2ts" => "mpeg2video",
            other => panic!("no fixture encoder for .{other}"),
        };
        let original = root.path().join(format!("clip.{extension}"));
        let mut command = Command::new("ffmpeg");
        command
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "testsrc2=size=64x32:rate=25",
                "-t",
                "2",
                "-c:v",
                codec,
                "-threads",
                "1",
            ])
            .arg(&original);
        let output =
            util::run_prepared_command_with_timeout(command, Duration::from_secs(20)).unwrap();
        assert!(
            output.status.success(),
            "{extension}: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        let bytes = fs::read(&original).unwrap();
        let packed = metadata::get_file_metadata(original.to_str().unwrap()).unwrap();
        let meta = metadata::unpack_file_meta(&packed).unwrap();
        assert!(meta.v && !meta.i, "{extension}: wrong classification");
        assert_eq!((meta.w, meta.h), (Some(64), Some(32)), "{extension}");
        let preview = thumb::ensure_thumb(&original, &root.path().join(".room237-thumb")).unwrap();
        assert_eq!(
            image::open(&preview).unwrap().dimensions(),
            (64, 32),
            "{extension}"
        );
        assert_eq!(
            fs::read(&original).unwrap(),
            bytes,
            "{extension}: original changed"
        );
    }
}
