#[cfg(test)]
use crate::metadata::update_album_meta;
#[cfg(test)]
use crate::settings::read_settings;
#[cfg(test)]
use std::fs;
use std::path::{Path, PathBuf};

use base64::Engine;
use img_hash::{HasherConfig, ImageHash};

use crate::{
    metadata::{
        hash_cache_current, mark_hash_failed_if_current, media_revision, read_file_entry,
        source_revision, write_album_file_hash, AlbumMeta, FileMetaEntry,
    },
    settings::HashConfig,
    thumb::ensure_thumb_with_settings,
};

/// Dimensions from the WebP header, then RGB pixels. The perceptual hash itself is unchanged.
fn decode_webp_rgb(bytes: &[u8], pixel_budget: u64) -> Option<image::RgbImage> {
    unsafe {
        let (mut w, mut h) = (0 as std::os::raw::c_int, 0 as std::os::raw::c_int);
        if libwebp_sys::WebPGetInfo(bytes.as_ptr(), bytes.len(), &mut w, &mut h) == 0 || w as u64 * h as u64 > pixel_budget {
            return None;
        }
        let mut features = std::mem::zeroed::<libwebp_sys::WebPBitstreamFeatures>();
        let transparent = libwebp_sys::WebPGetFeatures(bytes.as_ptr(), bytes.len(), &mut features) == libwebp_sys::VP8StatusCode::VP8_STATUS_OK
            && features.has_alpha != 0;
        let rgb = if transparent {
            // What lies under alpha 0 is not part of the picture: an encoder may keep it (FFmpeg) or clear it (ImageIO).
            let data = libwebp_sys::WebPDecodeRGBA(bytes.as_ptr(), bytes.len(), &mut w, &mut h);
            if data.is_null() {
                return None;
            }
            let rgba = std::slice::from_raw_parts(data, w as usize * h as usize * 4);
            let mut out = Vec::with_capacity(w as usize * h as usize * 3);
            for px in rgba.chunks_exact(4) {
                if px[3] == 0 { out.extend_from_slice(&[0, 0, 0]); } else { out.extend_from_slice(&px[..3]); }
            }
            libwebp_sys::WebPFree(data as *mut std::os::raw::c_void);
            out
        } else {
            let data = libwebp_sys::WebPDecodeRGB(bytes.as_ptr(), bytes.len(), &mut w, &mut h);
            if data.is_null() {
                return None;
            }
            let out = std::slice::from_raw_parts(data, w as usize * h as usize * 3).to_vec();
            libwebp_sys::WebPFree(data as *mut std::os::raw::c_void);
            out
        };
        image::RgbImage::from_raw(w as u32, h as u32, rgb)
    }
}

pub fn hash_image(p: &Path, cfg: &HashConfig) -> Result<ImageHash, String> {
    log::debug!("hashing image {}", p.display());
    let hasher = HasherConfig::new()
        .hash_size(cfg.size.0, cfg.size.1)
        .resize_filter(cfg.resize_filter.to_filter_type())
        .hash_alg(cfg.alg.to_img_hash_alg())
        .to_hasher();
    // image 0.23 decodes a whole WebP frame just to report dimensions, then again to read pixels.
    // Its decoder also fails on lossless and transparent WebP, which libwebp reads on every platform.
    if p.extension().is_some_and(|e| e.eq_ignore_ascii_case("webp")) {
        let bytes = std::fs::read(p).map_err(|e| format!("{}: {}", p.display(), e))?;
        let _permit = crate::util::DecodePermit::acquire();
        if let Some(rgb) = decode_webp_rgb(&bytes, 8_000_000) {
            return Ok(hasher.hash_image(&rgb));
        }
    }
    let (width, height) = image::image_dimensions(p).map_err(|e| e.to_string())?;
    if u64::from(width) * u64::from(height) > 8_000_000 {
        return Err("Direct decode exceeds image memory budget; using thumbnail fallback".into());
    }
    let _permit = crate::util::DecodePermit::acquire();
    let img = image::open(p).map_err(|e| format!("{}: {}", p.display(), e))?;
    Ok(hasher.hash_image(&img))
}

fn decode_hash_bits_base64(s: &str, expected_bits: u32) -> Option<Vec<u64>> {
    let mut buf = Vec::new();
    buf.resize((expected_bits as usize + 7) / 8, 0);
    let decoded = base64::engine::general_purpose::STANDARD
        .decode_slice(s.as_bytes(), &mut buf)
        .ok()?;
    if decoded * 8 != expected_bits as usize {
        return None;
    }
    if decoded % 8 != 0 {
        return None;
    }
    let mut out = Vec::new();
    for chunk in buf.chunks(8) {
        let mut tmp = [0u8; 8];
        tmp.copy_from_slice(chunk);
        out.push(u64::from_le_bytes(tmp));
    }
    Some(out)
}

fn load_cached_bits_from_meta(
    meta: &AlbumMeta,
    name: &str,
    path: &Path,
    cfg: &HashConfig,
) -> Option<Vec<u64>> {
    load_cached_bits(meta.files.get(name)?, path, cfg)
}

fn load_cached_bits(entry: &FileMetaEntry, path: &Path, cfg: &HashConfig) -> Option<Vec<u64>> {
    if entry.hash_failed || !hash_cache_current(path, entry) {
        return None;
    }
    if entry.hash_version.as_deref() != Some(&cfg.hash_version) {
        return None;
    }
    if let Some(bits) = entry.hash_bits {
        if bits != cfg.bits {
            return None;
        }
    }
    entry
        .hash
        .as_deref()
        .and_then(|h| decode_hash_bits_base64(h, cfg.bits))
}

fn hash_to_chunks(h: &ImageHash) -> Option<Vec<u64>> {
    let bytes = h.as_bytes();
    if bytes.len() % 8 != 0 {
        return None;
    }
    let mut out = Vec::new();
    for chunk in bytes.chunks(8) {
        let mut tmp = [0u8; 8];
        tmp.copy_from_slice(chunk);
        out.push(u64::from_le_bytes(tmp));
    }
    Some(out)
}

fn save_hash(
    p: &Path,
    h: &ImageHash,
    cfg: &HashConfig,
    expected_revision: &str,
) -> Result<(), String> {
    write_album_file_hash(
        p,
        h.to_base64(),
        cfg.hash_version.clone(),
        cfg.bits,
        expected_revision,
    )
}

pub(crate) fn within_threshold(a: &[u64], b: &[u64], threshold: u32) -> bool {
    let mut distance = 0;
    for (a, b) in a.iter().zip(b) {
        distance += (a ^ b).count_ones();
        if distance > threshold {
            return false;
        }
    }
    distance <= threshold
}

pub(crate) fn compute_hash_for_path(
    meta: &AlbumMeta,
    p: &Path,
    cfg: &HashConfig,
    settings: &crate::settings::AdvancedSettings,
) -> Option<Vec<u64>> {
    let name = p.file_name()?.to_str()?;
    compute_hash_with_cached_bits(
        p,
        cfg,
        settings,
        load_cached_bits_from_meta(meta, name, p, cfg),
    )
}

pub(crate) fn compute_hash_for_file(
    p: &Path,
    cfg: &HashConfig,
    settings: &crate::settings::AdvancedSettings,
) -> Option<Vec<u64>> {
    let entry = match read_file_entry(p) {
        Ok(entry) => entry,
        Err(error) => {
            log::error!("Cannot read hash metadata for {}: {error}", p.display());
            return None;
        }
    };
    let cached = entry
        .as_ref()
        .and_then(|entry| load_cached_bits(entry, p, cfg));
    compute_hash_with_cached_bits(p, cfg, settings, cached)
}

pub(crate) fn compute_hash_from_entry(
    p: &Path,
    cfg: &HashConfig,
    settings: &crate::settings::AdvancedSettings,
    entry: Option<&FileMetaEntry>,
) -> Option<Vec<u64>> {
    compute_hash_with_cached_bits(
        p,
        cfg,
        settings,
        entry.and_then(|entry| load_cached_bits(entry, p, cfg)),
    )
}

fn compute_hash_with_cached_bits(
    p: &Path,
    cfg: &HashConfig,
    settings: &crate::settings::AdvancedSettings,
    cached: Option<Vec<u64>>,
) -> Option<Vec<u64>> {
    if cached.is_some() {
        return cached;
    }

    let source = source_revision(p)?;
    let thumb_dir = p.parent()?.join(".room237-thumb");
    let mut last_error: Option<String> = None;

    let try_hash_path = |target: &Path| -> Result<Vec<u64>, String> {
        if source_revision(p).as_deref() != Some(&source) {
            return Err("Source changed during hash computation".into());
        }
        let revision = media_revision(p);
        let hash = hash_image(target, cfg)?;
        let bits = hash_to_chunks(&hash).ok_or_else(|| "unexpected hash length".to_string())?;
        save_hash(p, &hash, cfg, &revision).map_err(|e| {
            log::error!("Failed to persist hash for {}: {}", p.display(), e);
            e
        })?;
        Ok(bits)
    };

    let attempt_thumb = |last_error: &mut Option<String>| -> Option<Vec<u64>> {
        let thumb_path = match ensure_thumb_with_settings(p, &thumb_dir, settings) {
            Ok(thumbnail) => thumbnail,
            Err(error) => {
                *last_error = Some(error);
                return None;
            }
        };
        match try_hash_path(&thumb_path) {
            Ok(bits) => Some(bits),
            Err(e) => {
                *last_error = Some(e);
                None
            }
        }
    };

    let attempt_original = |last_error: &mut Option<String>| -> Option<Vec<u64>> {
        match try_hash_path(p) {
            Ok(bits) => Some(bits),
            Err(e) => {
                *last_error = Some(e);
                None
            }
        }
    };

    let result = if cfg.use_thumbnails_first {
        attempt_thumb(&mut last_error).or_else(|| attempt_original(&mut last_error))
    } else {
        attempt_original(&mut last_error).or_else(|| attempt_thumb(&mut last_error))
    };

    if result.is_none() && source_revision(p).as_deref() == Some(&source) {
        if let Some(err) = last_error {
            log::error!("Failed to hash {}: {}", p.display(), err);
        }
        let _ =
            mark_hash_failed_if_current(p, &media_revision(p), cfg, &settings.processing_version());
    }

    result
}

pub async fn find_duplicates(dir: String) -> Result<Vec<Vec<String>>, String> {
    crate::disk_duplicates::legacy_page(dir)
        .await
        .map(|page| page.groups)
}

pub fn mark_non_duplicates(dir: String, files: Vec<String>) -> Result<(), String> {
    let dirp = PathBuf::from(&dir);
    if !dirp.is_dir() {
        return Err(format!("{} is not a directory", dirp.display()));
    }
    if files.len() < 2 {
        return Ok(());
    }

    for file in &files {
        crate::util::validate_file_name(file)?;
    }
    crate::metadata::prepare_database(&dirp)?;
    let _guard = crate::metadata::lock_metadata_for_write(&[&dirp])?;
    crate::album_db::ignore_pairs(&dirp.join(".room237-metadata/album.sqlite"), &files)
}

#[cfg(test)]
mod candidate_tests {
    use super::*;
    async fn scan_cached_hashes(
        hashes: Vec<Vec<u64>>,
        ignored: Vec<[String; 2]>,
    ) -> Vec<Vec<String>> {
        let dir = tempfile::tempdir().unwrap();
        fs::create_dir_all(dir.path().join(".room237-thumb")).unwrap();
        let cfg = read_settings().hash_config();
        update_album_meta(dir.path(), |album| {
            album.duplicates_ignore = ignored;
            for (n, bits) in hashes.iter().enumerate() {
                let name = format!("{n:05}.jpg");
                fs::write(dir.path().join(&name), []).unwrap();
                let bytes = bits
                    .iter()
                    .flat_map(|word| word.to_le_bytes())
                    .collect::<Vec<_>>();
                album.files.insert(
                    name.clone(),
                    crate::metadata::FileMetaEntry {
                        hash: Some(base64::engine::general_purpose::STANDARD.encode(bytes)),
                        hash_bits: Some(cfg.bits),
                        hash_version: Some(cfg.hash_version.clone()),
                        hash_source_revision: Some(media_revision(&dir.path().join(&name))),
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
        find_duplicates(dir.path().to_string_lossy().into())
            .await
            .unwrap()
    }
    #[tokio::test]
    async fn actual_scan_returns_heic_without_a_recorded_companion() {
        let dir = tempfile::tempdir().unwrap();
        let cfg = read_settings().hash_config();
        let names = ["camera.HEIC", "camera.jpeg", "copy.jpg"];
        update_album_meta(dir.path(), |album| {
            for name in names {
                let path = dir.path().join(name);
                fs::write(&path, b"fixture").unwrap();
                album.files.insert(
                    name.into(),
                    crate::metadata::FileMetaEntry {
                        hash: Some(base64::engine::general_purpose::STANDARD.encode(vec![
                                0;
                                (cfg.bits / 8)
                                    as usize
                            ])),
                        hash_bits: Some(cfg.bits),
                        hash_version: Some(cfg.hash_version.clone()),
                        hash_source_revision: Some(media_revision(&path)),
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
        let mut groups = find_duplicates(dir.path().to_string_lossy().into())
            .await
            .unwrap();
        assert_eq!(groups.len(), 1);
        groups[0].sort();
        assert_eq!(groups[0], vec!["camera.HEIC", "camera.jpeg", "copy.jpg"]);
    }

    #[tokio::test]
    async fn actual_scan_finds_pair_previously_excluded_by_the_index() {
        let groups =
            scan_cached_hashes(vec![vec![0; 4], vec![0x0001000100010001; 4]], vec![]).await;
        assert_eq!(groups, vec![vec!["00000.jpg", "00001.jpg"]]);
    }
    #[tokio::test]
    async fn identical_hash_collapsing_retains_all_names_and_respects_ignored_pairs() {
        let hashes = vec![vec![0; 4]; 3];
        let groups = scan_cached_hashes(hashes.clone(), vec![]).await;
        assert_eq!(groups[0].len(), 3);
        let groups = scan_cached_hashes(
            hashes[..2].to_vec(),
            vec![["00000.jpg".into(), "00001.jpg".into()]],
        )
        .await;
        assert!(groups.is_empty());
    }
    #[tokio::test]
    async fn ignored_pair_cannot_rejoin_through_a_third_duplicate() {
        let groups = scan_cached_hashes(
            vec![vec![0; 4]; 3],
            vec![["00000.jpg".into(), "00001.jpg".into()]],
        )
        .await;
        assert!(
            !groups.is_empty(),
            "the unignored third file should still have a match"
        );
        assert!(
            groups
                .iter()
                .all(|group| !(group.contains(&"00000.jpg".into())
                    && group.contains(&"00001.jpg".into()))),
            "ignored pair returned in {groups:?}"
        );
    }

    #[tokio::test]
    async fn ignored_constraints_survive_multiple_component_merges() {
        let ignored = vec![
            ["00000.jpg".into(), "00005.jpg".into()],
            ["00001.jpg".into(), "00003.jpg".into()],
            ["00002.jpg".into(), "00004.jpg".into()],
        ];
        let groups = scan_cached_hashes(vec![vec![0; 4]; 6], ignored.clone()).await;
        assert!(!groups.is_empty());
        for group in groups {
            for pair in &ignored {
                assert!(
                    !(group.contains(&pair[0]) && group.contains(&pair[1])),
                    "ignored pair {pair:?} returned in {group:?}"
                );
            }
        }
    }

    #[tokio::test]
    #[ignore = "cached duplicate scan benchmark"]
    async fn benchmark_ten_thousand_identical_hashes() {
        let start = std::time::Instant::now();
        let groups = scan_cached_hashes(vec![vec![0; 4]; 10000], vec![]).await;
        assert_eq!(groups.len(), 1);
        assert_eq!(groups[0].len(), 129);
        eprintln!("duplicate benchmark: 10000 identical cached hashes, including temporary file creation and metadata writes: {:?}", start.elapsed());
    }
}
