//! Rebuildable library index. Sorting, filtering and inventory live on disk;
//! IPC returns at most one page, never a whole album or favorites collection.
use crate::{
    constants::{IMAGE_EXTENSIONS, VIDEO_EXTENSIONS},
    metadata::{DetachedMediaEntry, FileMetaEntry},
    util::has_extension,
};
use rusqlite::{functions::FunctionFlags, params, Connection, OptionalExtension};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
    time::Duration,
};

pub(crate) const PAGE_SIZE: usize = 256;
static PAGE_REQUEST: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
const DIRECTORY: &str = ".room237-catalog";

fn open(root: &Path) -> Result<Connection, String> {
    if !crate::util::qa_path_allowed(root) {
        return Err("QA fence refused catalog root".into());
    }
    // An empty or relative root would put the index into the process's working directory.
    if !root.is_absolute() || !root.is_dir() {
        return Err(format!("Library root is not a directory: {}", root.display()));
    }
    let dir = root.join(DIRECTORY);
    if !dir.exists() {
        fs::create_dir(&dir).map_err(|e| e.to_string())?;
    }
    let conn = Connection::open(dir.join("index.sqlite")).map_err(|e| e.to_string())?;
    conn.busy_timeout(Duration::from_secs(30))
        .map_err(|e| e.to_string())?;
    conn.execute_batch(
        "PRAGMA cache_size=-2048; PRAGMA temp_store=FILE; PRAGMA temp.cache_size=-2048; PRAGMA mmap_size=0;
        CREATE TABLE IF NOT EXISTS media (
            album TEXT NOT NULL, name TEXT NOT NULL, meta TEXT NOT NULL,
            favorite INTEGER NOT NULL, added INTEGER NOT NULL, shoot INTEGER NOT NULL, blur_preview TEXT,
            seen INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(album,name));
        CREATE INDEX IF NOT EXISTS media_name ON media(album,name COLLATE NOCASE,name);
        CREATE INDEX IF NOT EXISTS media_added ON media(album,added,name);
        CREATE INDEX IF NOT EXISTS media_shoot ON media(album,shoot,name);
        CREATE INDEX IF NOT EXISTS media_favorites ON media(favorite,album,name);
        CREATE TABLE IF NOT EXISTS scanned (album TEXT PRIMARY KEY, generation INTEGER NOT NULL,signature TEXT NOT NULL DEFAULT '');",
    )
    .map_err(|e| e.to_string())?;
    if conn
        .prepare("SELECT signature FROM scanned LIMIT 0")
        .is_err()
    {
        conn.execute(
            "ALTER TABLE scanned ADD COLUMN signature TEXT NOT NULL DEFAULT ''",
            [],
        )
        .map_err(|e| e.to_string())?;
    }
    if conn
        .prepare("SELECT blur_preview FROM media LIMIT 0")
        .is_err()
    {
        if let Err(error) = conn.execute("ALTER TABLE media ADD COLUMN blur_preview TEXT", []) {
            // Another indexing worker may have completed the migration first.
            if conn
                .prepare("SELECT blur_preview FROM media LIMIT 0")
                .is_err()
            {
                return Err(error.to_string());
            }
        }
    }
    conn.create_scalar_function(
        "album_label",
        1,
        FunctionFlags::SQLITE_DETERMINISTIC | FunctionFlags::SQLITE_UTF8,
        |ctx| {
            let album: String = ctx.get(0)?;
            Ok(album.rsplit('/').next().unwrap_or(&album).to_lowercase())
        },
    )
    .map_err(|e| e.to_string())?;
    conn.create_scalar_function(
        "shuffle",
        2,
        FunctionFlags::SQLITE_DETERMINISTIC | FunctionFlags::SQLITE_UTF8,
        |ctx| {
            let name: String = ctx.get(0)?;
            let seed: i64 = ctx.get(1)?;
            let mut hash = seed as u64 ^ 0xcbf29ce484222325;
            for byte in name.bytes() {
                hash = (hash ^ byte as u64).wrapping_mul(0x100000001b3);
            }
            Ok(hash as i64)
        },
    )
    .map_err(|e| e.to_string())?;
    Ok(conn)
}

fn album_id(root: &Path, album: &Path) -> Result<String, String> {
    Ok(album
        .strip_prefix(root)
        .map_err(|_| "Album outside library")?
        .to_string_lossy()
        .replace('\\', "/"))
}

pub(crate) fn library_root(album: &Path) -> Option<PathBuf> {
    album
        .ancestors()
        .find(|path| path.join(DIRECTORY).is_dir())
        .map(Path::to_path_buf)
}

fn put(
    conn: &Connection,
    album: &str,
    name: &str,
    path: &Path,
    entry: &FileMetaEntry,
    seen: Option<i64>,
) -> Result<(), String> {
    let album_dir = path.parent().unwrap_or(path);
    if !has_extension(path, IMAGE_EXTENSIONS) && !has_extension(path, VIDEO_EXTENSIONS)
        || (has_extension(path, &["heic"])
            && crate::metadata::heic_hidden_by_entry(album_dir, entry))
    {
        return Ok(());
    }
    if !path.is_file() {
        conn.execute(
            "DELETE FROM media WHERE album=?1 AND name=?2",
            params![album, name],
        )
        .map_err(|e| e.to_string())?;
        return Ok(());
    }
    let fallback = || crate::metadata::DetachedFileMeta {
        a: path
            .metadata()
            .ok()
            .and_then(|m| m.created().ok())
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_secs()),
        s: crate::metadata::read_embedded_timestamp(path),
        i: has_extension(path, IMAGE_EXTENSIONS),
        v: has_extension(path, VIDEO_EXTENSIONS),
        w: None,
        h: None,
    };
    let meta = entry
        .meta
        .as_deref()
        .filter(|_| {
            entry.meta_source_revision.is_some()
                && entry.meta_source_revision == crate::metadata::source_revision(path)
        })
        .and_then(|s| crate::metadata::unpack_file_meta(s).ok())
        .unwrap_or_else(fallback);
    let added = meta.a.map(|n| n as i64).or(meta.s).unwrap_or(0);
    let shoot = meta.s.or(meta.a.map(|n| n as i64)).unwrap_or(0);
    conn.execute("INSERT INTO media(album,name,meta,favorite,added,shoot,seen,blur_preview) VALUES(?1,?2,?3,?4,?5,?6,coalesce(?7,0),?8)
        ON CONFLICT(album,name) DO UPDATE SET meta=excluded.meta,favorite=excluded.favorite,added=excluded.added,shoot=excluded.shoot,seen=coalesce(?7,media.seen),blur_preview=excluded.blur_preview WHERE media.blur_preview IS NOT excluded.blur_preview OR media.meta<>excluded.meta OR media.favorite<>excluded.favorite OR media.added<>excluded.added OR media.shoot<>excluded.shoot OR (?7 IS NOT NULL AND media.seen<>excluded.seen)",
        params![album,name,meta.pack(),entry.favorite,added,shoot,seen,entry.blur_preview]).map_err(|e| e.to_string())?;
    Ok(())
}

fn pending_marker(root: &Path, id: &str) -> PathBuf {
    use sha2::{Digest, Sha256};
    root.join(DIRECTORY)
        .join("pending")
        .join(hex::encode(Sha256::digest(id.as_bytes())))
}

// Metadata is authoritative. A failed derived-index write is retried from disk
// before the next page or favorite-count request, including after a restart.
fn repair_pending(root: &Path) -> Result<(), String> {
    let pending = root.join(DIRECTORY).join("pending");
    if !pending.is_dir() {
        return Ok(());
    }
    for file in fs::read_dir(pending).map_err(|e| e.to_string())? {
        let file = file.map_err(|e| e.to_string())?;
        let id = match fs::read_to_string(file.path()) {
            Ok(id) => id,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
            Err(e) => return Err(e.to_string()),
        };
        let album = root.join(&id);
        if album.is_dir() {
            if !album
                .canonicalize()
                .map_err(|e| e.to_string())?
                .starts_with(root.canonicalize().map_err(|e| e.to_string())?)
            {
                return Err("Pending album outside library".into());
            }
            scan_album(root, &album)?;
        } else {
            let conn = open(root)?;
            conn.execute("DELETE FROM media WHERE album=?1", [&id])
                .map_err(|e| e.to_string())?;
            match fs::remove_file(file.path()) {
                Ok(()) => (),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
                Err(e) => return Err(e.to_string()),
            }
        }
    }
    Ok(())
}

/// Caller owns metadata admission; this derived index never locks metadata.
pub(crate) fn publish(album: &Path, name: &str, entry: &FileMetaEntry) {
    let Some(root) = library_root(album) else {
        return;
    };
    let result = (|| {
        let conn = open(&root)?;
        put(
            &conn,
            &album_id(&root, album)?,
            name,
            &album.join(name),
            entry,
            None,
        )
    })();
    if let Err(error) = result {
        log::warn!("Catalog update failed for {}: {error}", album.display());
        if let Ok(id) = album_id(&root, album) {
            let marker = pending_marker(&root, &id);
            if let Err(retry_error) =
                fs::create_dir_all(marker.parent().unwrap()).and_then(|_| fs::write(marker, id))
            {
                log::error!("Cannot record pending catalog repair: {retry_error}");
            }
        }
    }
}

pub(crate) fn forget(album: &Path, names: &[String]) -> Result<(), String> {
    let Some(root) = library_root(album) else {
        return Ok(());
    };
    let mut conn = open(&root)?;
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    let id = album_id(&root, album)?;
    for name in names {
        tx.execute(
            "DELETE FROM media WHERE album=?1 AND name=?2",
            params![id, name],
        )
        .map_err(|e| e.to_string())?;
    }
    tx.commit().map_err(|e| e.to_string())
}

/// Compares the directory with the catalog without touching derived data. Rescans only on a real
/// difference, so a watcher wakeup or the end of an own operation costs a listing, not a rebuild.
pub async fn reconcile_album(
    app: crate::host::AppHandle,
    root_dir: String,
    album_id: String,
) -> Result<bool, String> {
    tokio::task::spawn_blocking(move || {
        let root = PathBuf::from(root_dir);
        initialize(&root)?;
        let album = root.join(&album_id);
        if !album.is_dir() {
            let albums = crate::album::walk_album_paths(&root)?;
            retain_albums(
                &root,
                albums.iter().map(|album| {
                    album.strip_prefix(&root).unwrap().to_string_lossy().replace('\\', "/")
                }),
            )?;
            return Ok(true);
        }
        let canonical = album.canonicalize().map_err(|e| e.to_string())?;
        if !canonical.starts_with(root.canonicalize().map_err(|e| e.to_string())?) {
            return Err("Album outside library".into());
        }
        let mut known = std::collections::HashSet::new();
        {
            let conn = open(&root)?;
            let mut statement = conn.prepare("SELECT name FROM media WHERE album=?1").map_err(|e| e.to_string())?;
            let rows = statement.query_map([&album_id], |row| row.get::<_, String>(0)).map_err(|e| e.to_string())?;
            for row in rows {
                known.insert(row.map_err(|e| e.to_string())?);
            }
        }
        let hidden_heic = crate::metadata::hidden_heic_names(&album)?;
        let database = album.join(".room237-metadata/album.sqlite");
        let mut reader = if database.is_file() { Some(crate::album_db::FileReader::new(&database)?) } else { None };
        let mut seen = 0usize;
        let mut changed = false;
        let mut children = std::collections::HashSet::new();
        for entry in fs::read_dir(&album).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            let path = entry.path();
            if crate::album::is_album_dir(&path) {
                children.insert(format!("{album_id}/{}", entry.file_name().to_string_lossy()));
                continue;
            }
            if !entry.file_type().map_err(|e| e.to_string())?.is_file()
                || !(has_extension(&path, IMAGE_EXTENSIONS) || has_extension(&path, VIDEO_EXTENSIONS))
            {
                continue;
            }
            let name = entry.file_name().to_string_lossy().into_owned();
            if hidden_heic.contains(&name) {
                continue;
            }
            seen += 1;
            if !known.contains(&name) {
                changed = true;
                break;
            }
            // An entry without a stored revision is still being processed, which is not a change.
            let stored = match reader.as_mut() {
                Some(reader) => reader.get(&name)?.and_then(|entry| entry.meta_source_revision),
                None => None,
            };
            if stored.is_some_and(|stored| Some(stored) != crate::metadata::source_revision(&path)) {
                changed = true;
                break;
            }
        }
        if !changed && seen == known.len() {
            // A nested album renamed or moved here changes only this listing; the caller refreshes the tree.
            let conn = open(&root)?;
            let prefix = format!("{album_id}/");
            let mut statement = conn.prepare("SELECT album FROM scanned").map_err(|e| e.to_string())?;
            let rows = statement.query_map([], |row| row.get::<_, String>(0)).map_err(|e| e.to_string())?;
            let mut scanned = std::collections::HashSet::new();
            for row in rows {
                let id = row.map_err(|e| e.to_string())?;
                if id.strip_prefix(&prefix).is_some_and(|rest| !rest.contains('/')) {
                    scanned.insert(id);
                }
            }
            return Ok(scanned != children);
        }
        scan_album(&root, &album)?;
        crate::preload::drop_preload_for_path(&album);
        crate::preload::enqueue_preload(&album);
        crate::preload::start_preloader_worker(app);
        Ok(true)
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Streaming inventory, bounded batches; old index survives an interrupted scan.
pub(crate) fn scan_album(root: &Path, album: &Path) -> Result<usize, String> {
    scan_album_if_needed(root, album, true)
}

pub(crate) fn scan_album_if_needed(
    root: &Path,
    album: &Path,
    force: bool,
) -> Result<usize, String> {
    let _tree = crate::album_tree::read(&[album])?;
    let _inventory = crate::util::FileLock::acquire(
        &album.join(".room237-inventory.lock"),
        Duration::from_secs(1800),
        Duration::from_millis(20),
    )?;
    crate::media_delete::recover_album(album)?;
    let id = album_id(root, album)?;
    let signature = format!(
        "{}|{}",
        crate::metadata::source_revision(album).unwrap_or_default(),
        crate::settings::read_settings().processing_version()
    );
    if !force && !pending_marker(root, &id).is_file() {
        let conn = open(root)?;
        let old: Option<String> = conn
            .query_row("SELECT signature FROM scanned WHERE album=?1", [&id], |r| {
                r.get(0)
            })
            .optional()
            .map_err(|e| e.to_string())?;
        if old.as_deref() == Some(&signature) {
            return conn
                .query_row("SELECT count(*) FROM media WHERE album=?1", [&id], |r| {
                    r.get::<_, i64>(0).map(|n| n as usize)
                })
                .map_err(|e| e.to_string());
        }
    }
    crate::metadata::prepare_database(album)?;
    let mut metadata =
        crate::album_db::FileReader::new(&album.join(".room237-metadata/album.sqlite"))?;
    let mut conn = open(root)?;
    let id = album_id(root, album)?;
    let generation = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos() as i64;
    let mut count = 0;
    let mut batch = Vec::with_capacity(PAGE_SIZE);
    let mut flush = |batch: &mut Vec<String>| -> Result<(), String> {
        let _guard = crate::metadata::lock_metadata_for_write(&[album])?;
        let tx = conn.transaction().map_err(|e| e.to_string())?;
        for name in batch.drain(..) {
            let entry = metadata.get(&name)?.unwrap_or_default();
            put(
                &tx,
                &id,
                &name,
                &album.join(&name),
                &entry,
                Some(generation),
            )?;
        }
        tx.commit().map_err(|e| e.to_string())
    };
    let hidden_heic = crate::metadata::hidden_heic_names(album)?;
    for entry in fs::read_dir(album).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        if !entry.file_type().map_err(|e| e.to_string())?.is_file()
            || !(has_extension(&path, IMAGE_EXTENSIONS) || has_extension(&path, VIDEO_EXTENSIONS))
        {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        if hidden_heic.contains(&name) {
            continue;
        }
        batch.push(name);
        count += 1;
        if batch.len() == PAGE_SIZE {
            flush(&mut batch)?;
        }
    }
    flush(&mut batch)?;
    let _guard = crate::metadata::lock_metadata_for_write(&[album])?;
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    tx.execute(
        "DELETE FROM media WHERE album=?1 AND seen<>?2",
        params![id, generation],
    )
    .map_err(|e| e.to_string())?;
    tx.execute("INSERT INTO scanned(album,generation,signature) VALUES(?1,?2,?3) ON CONFLICT(album) DO UPDATE SET generation=excluded.generation,signature=excluded.signature", params![id,generation,format!("{}|{}",crate::metadata::source_revision(album).unwrap_or_default(),crate::settings::read_settings().processing_version())]).map_err(|e| e.to_string())?;
    tx.commit().map_err(|e| e.to_string())?;
    match fs::remove_file(pending_marker(root, &id)) {
        Ok(()) => (),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
        Err(e) => return Err(e.to_string()),
    }
    Ok(count)
}

pub(crate) fn initialize(root: &Path) -> Result<(), String> {
    open(root).map(|_| ())
}

pub(crate) fn retain_albums(
    root: &Path,
    albums: impl Iterator<Item = String>,
) -> Result<(), String> {
    let mut conn = open(root)?;
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    tx.execute_batch("CREATE TEMP TABLE live_albums(id TEXT PRIMARY KEY)")
        .map_err(|e| e.to_string())?;
    for id in albums {
        tx.execute("INSERT INTO live_albums VALUES(?1)", [id])
            .map_err(|e| e.to_string())?;
    }
    tx.execute(
        "DELETE FROM media WHERE album NOT IN(SELECT id FROM live_albums)",
        [],
    )
    .map_err(|e| e.to_string())?;
    tx.execute(
        "DELETE FROM scanned WHERE album NOT IN(SELECT id FROM live_albums)",
        [],
    )
    .map_err(|e| e.to_string())?;
    tx.commit().map_err(|e| e.to_string())
}

pub(crate) fn clear(root: &Path) -> Result<(), String> {
    let conn = open(root)?;
    conn.execute_batch("DELETE FROM media; DELETE FROM scanned;")
        .map_err(|e| e.to_string())
}

pub async fn rescan_inventory(
    app: crate::host::AppHandle,
    root_dir: String,
    album_id: Option<String>,
) -> Result<(), String> {
    tokio::task::spawn_blocking(move || {
        let root = PathBuf::from(root_dir);
        initialize(&root)?;
        if let Some(id) = album_id {
            let album = root.join(id);
            let canonical = album.canonicalize().map_err(|e| e.to_string())?;
            if !canonical.starts_with(root.canonicalize().map_err(|e| e.to_string())?) {
                return Err("Album outside library".into());
            }
            scan_album(&root, &album)?;
            crate::preload::drop_preload_for_path(&album);
            crate::preload::enqueue_preload(&album);
        } else {
            let albums = crate::album::walk_album_paths(&root)?;
            for album in &albums {
                scan_album(&root, album)?;
                crate::preload::drop_preload_for_path(album);
                crate::preload::enqueue_preload(album);
            }
            retain_albums(
                &root,
                albums.iter().map(|album| {
                    album
                        .strip_prefix(&root)
                        .unwrap()
                        .to_string_lossy()
                        .replace('\\', "/")
                }),
            )?;
        }
        crate::preload::start_preloader_worker(app);
        Ok(())
    })
    .await
    .map_err(|e| e.to_string())?
}

pub async fn refresh_catalog_paths(root_dir: String, paths: Vec<String>) -> Result<(), String> {
    if paths.len() > PAGE_SIZE {
        return Err("Too many changed paths in one batch".into());
    }
    tokio::task::spawn_blocking(move || {
        let root = Path::new(&root_dir)
            .canonicalize()
            .map_err(|e| e.to_string())?;
        for path in paths {
            let path = Path::new(&path);
            let album = path
                .parent()
                .ok_or("Missing album")?
                .canonicalize()
                .map_err(|e| e.to_string())?;
            if !album.starts_with(&root) {
                return Err("Album outside library".into());
            }
            let name = path
                .file_name()
                .and_then(|n| n.to_str())
                .ok_or("Invalid media name")?;
            let path = album.join(name);
            if path.is_file() {
                let entry = crate::metadata::read_file_entry(&path)?.unwrap_or_default();
                publish(&album, name, &entry);
            } else {
                forget(&album, &[name.to_string()])?;
            }
        }
        Ok(())
    })
    .await
    .map_err(|e| e.to_string())?
}

#[derive(Deserialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct PageQuery {
    pub root_dir: String,
    pub album_id: Option<String>,
    pub favorites_only: bool,
    pub sort_key: String,
    pub sort_dir: String,
    pub random_seed: f64,
    pub page: u64,
    #[serde(default)]
    pub limit_pages: Option<usize>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PageItem {
    #[serde(flatten)]
    pub media: DetachedMediaEntry,
    pub album_path: String,
    pub album_id: String,
    pub album_name: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MediaPage {
    pub items: Vec<PageItem>,
    pub total: u64,
    pub page: u64,
    pub page_size: usize,
}

pub(crate) fn query_page(query: PageQuery) -> Result<MediaPage, String> {
    query_page_inner(query, None)
}

fn query_page_inner(query: PageQuery, request: Option<u64>) -> Result<MediaPage, String> {
    let root = Path::new(&query.root_dir);
    let conn = open(root)?;
    if let Some(request) = request {
        if PAGE_REQUEST.load(std::sync::atomic::Ordering::Relaxed) != request {
            return Err("Page request superseded".into());
        }
        conn.progress_handler(
            1000,
            Some(move || PAGE_REQUEST.load(std::sync::atomic::Ordering::Relaxed) != request),
        )
        .map_err(|e| e.to_string())?;
    }
    conn.execute_batch("BEGIN DEFERRED")
        .map_err(|e| e.to_string())?;

    let direction = if query.sort_dir == "desc" {
        "DESC"
    } else {
        "ASC"
    };
    let order = match query.sort_key.as_str() {
        "added" => "added",
        "shoot" => "shoot",
        "random" => "shuffle(album || '/' || name, ?3)",
        _ => "name COLLATE NOCASE",
    };
    let filter = if query.album_id.is_some() {
        "album=?1 AND (?2=0 OR favorite=1)"
    } else {
        "?1 IS NULL AND (?2=0 OR favorite=1)"
    };
    let total: u64 = conn
        .query_row(
            &format!("SELECT count(*) FROM media WHERE {filter}"),
            params![query.album_id, query.favorites_only],
            |r| r.get::<_, i64>(0).map(|n| n as u64),
        )
        .map_err(|e| e.to_string())?;
    let page = query.page.min(total.saturating_sub(1) / PAGE_SIZE as u64);
    let limit = PAGE_SIZE * query.limit_pages.unwrap_or(1).clamp(1, 5);
    let seed = (query.random_seed * 1_000_000.0) as i64;
    let group_order = if query.album_id.is_none() {
        "album_label(album) ASC, album ASC, "
    } else {
        ""
    };
    let sql = format!("SELECT album,name,meta,favorite,blur_preview FROM media WHERE {filter} ORDER BY {group_order}{order} {direction}, album {direction}, name {direction} LIMIT ?4 OFFSET ?5");
    let mut stmt = conn.prepare(&sql).map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(
            params![
                query.album_id,
                query.favorites_only,
                seed,
                limit as i64,
                (page * PAGE_SIZE as u64) as i64
            ],
            |r| {
                Ok((
                    r.get::<_, String>(0)?,
                    r.get::<_, String>(1)?,
                    r.get::<_, String>(2)?,
                    r.get::<_, bool>(3)?,
                    r.get::<_, Option<String>>(4)?,
                ))
            },
        )
        .map_err(|e| e.to_string())?;
    let rows = rows
        .collect::<Result<Vec<_>, _>>()
        .map_err(|e| e.to_string())?;
    drop(stmt);
    conn.execute_batch("COMMIT").map_err(|e| e.to_string())?;
    drop(conn);
    let mut items = Vec::with_capacity(limit);
    for (album, name, meta, favorite, blur_preview) in rows {
        let album_path = root.join(&album);
        let path = album_path.join(&name);
        items.push(PageItem {
            media: DetachedMediaEntry {
                blur_preview,
                revision: crate::metadata::media_revision(&path),
                thumbnail_name: crate::thumb::detached_thumbnail_name(&path),
                name,
                meta,
                favorite: Some(favorite),
            },
            album_path: album_path.to_string_lossy().into_owned(),
            album_id: album.clone(),
            album_name: album.rsplit('/').next().unwrap_or(&album).to_string(),
        });
    }
    Ok(MediaPage {
        items,
        total,
        page,
        page_size: PAGE_SIZE,
    })
}

/// Names and source stamps of the whole query. Does not compete with paged gallery requests.
pub async fn get_inventory_print(query: PageQuery) -> Result<String, String> {
    use sha2::{Digest, Sha256};
    tokio::task::spawn_blocking(move || {
        let mut hasher = Sha256::new();
        let mut query = query;
        query.page = 0;
        loop {
            let page = query_page(query.clone())?;
            for item in &page.items {
                hasher.update(item.media.name.as_bytes());
                let path = Path::new(&item.album_path).join(&item.media.name);
                hasher.update(crate::metadata::source_revision(&path).unwrap_or_default().as_bytes());
            }
            let seen = (query.page + 1) * page.page_size as u64;
            if page.items.is_empty() || seen >= page.total {
                return Ok(hex::encode(hasher.finalize()));
            }
            query.page += 1;
        }
    })
    .await
    .map_err(|e| e.to_string())?
}

pub async fn get_media_page(query: PageQuery) -> Result<MediaPage, String> {
    let request = PAGE_REQUEST.fetch_add(1, std::sync::atomic::Ordering::SeqCst) + 1;
    tokio::task::spawn_blocking(move || {
        repair_pending(Path::new(&query.root_dir))?;
        let page = query_page_inner(query, Some(request))?;
        let settings = crate::settings::read_settings();
        let version = settings.thumb_version();
        let mut reader: Option<(PathBuf, crate::album_db::FileReader)> = None;
        for item in &page.items {
            if PAGE_REQUEST.load(std::sync::atomic::Ordering::Relaxed) != request {
                return Err("Page request superseded".into());
            }
            let album = Path::new(&item.album_path);
            let path = album.join(&item.media.name);
            if !path.is_file() {
                continue;
            }
            if !reader.as_ref().is_some_and(|(dir, _)| dir == album) {
                reader = Some((
                    album.to_path_buf(),
                    crate::album_db::FileReader::new(
                        &album.join(".room237-metadata/album.sqlite"),
                    )?,
                ));
            }
            let entry = reader
                .as_mut()
                .unwrap()
                .1
                .get(&item.media.name)?
                .unwrap_or_default();
            let thumbs = album.join(".room237-thumb");
            if !crate::thumb::cached_thumb_current(&path, &thumbs, &entry, &version) {
                crate::preload::enqueue_thumb_task(
                    path.clone(),
                    thumbs,
                    false,
                    crate::preload::TaskPriority::High,
                );
            }
            if !crate::metadata::metadata_cache_current_with_settings(&path, &entry, &settings) {
                crate::preload::enqueue_meta_task(path, false, crate::preload::TaskPriority::High);
            }
        }
        Ok(page)
    })
    .await
    .map_err(|e| e.to_string())?
}

pub async fn favorite_counts(
    root_dir: String,
) -> Result<std::collections::HashMap<String, u64>, String> {
    tokio::task::spawn_blocking(move || {
        repair_pending(Path::new(&root_dir))?;
        let conn = open(Path::new(&root_dir))?;
        let mut stmt = conn
            .prepare("SELECT album,count(*) FROM media WHERE favorite=1 GROUP BY album")
            .map_err(|e| e.to_string())?;
        let rows = stmt
            .query_map([], |r| Ok((r.get(0)?, r.get::<_, i64>(1)? as u64)))
            .map_err(|e| e.to_string())?;
        rows.collect::<Result<_, _>>().map_err(|e| e.to_string())
    })
    .await
    .map_err(|e| e.to_string())?
}

#[cfg(test)]
mod tests {
    use super::*;
    fn query(root: &Path, page: u64) -> PageQuery {
        PageQuery {
            root_dir: root.to_string_lossy().into_owned(),
            album_id: Some("a".into()),
            favorites_only: false,
            sort_key: "name".into(),
            sort_dir: "asc".into(),
            random_seed: 0.25,
            page,
            limit_pages: None,
        }
    }
    fn seed(root: &Path, start: usize, end: usize) {
        let mut conn = open(root).unwrap();
        let tx = conn.transaction().unwrap();
        {
            let mut insert = tx
                .prepare("INSERT INTO media(album,name,meta,favorite,added,shoot,seen) VALUES('a',?1,'0',?2,?3,?3,0)")
                .unwrap();
            for n in start..end {
                insert
                    .execute(params![format!("{n:09}.jpg"), n % 3 == 0, n as i64])
                    .unwrap();
            }
        }
        tx.commit().unwrap();
    }
    #[tokio::test]
    async fn blur_previews_survive_index_migration_paging_favorites_and_selection() {
        let root = tempfile::tempdir().unwrap();
        fs::create_dir(root.path().join(DIRECTORY)).unwrap();
        let old = Connection::open(root.path().join(DIRECTORY).join("index.sqlite")).unwrap();
        old.execute_batch("CREATE TABLE media(album TEXT NOT NULL,name TEXT NOT NULL,meta TEXT NOT NULL,favorite INTEGER NOT NULL,added INTEGER NOT NULL,shoot INTEGER NOT NULL,seen INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(album,name)); INSERT INTO media VALUES('a','photo.jpg','0',1,0,0,0);").unwrap();
        drop(old);
        let conn = open(root.path()).unwrap();
        conn.execute("UPDATE media SET blur_preview='dGlueS13ZWJw'", [])
            .unwrap();
        let page = query_page(query(root.path(), 0)).unwrap();
        assert_eq!(
            page.items[0].media.blur_preview.as_deref(),
            Some("dGlueS13ZWJw")
        );
        assert_eq!(
            serde_json::to_value(&page.items[0]).unwrap()["blurPreview"],
            "dGlueS13ZWJw"
        );
        let mut favorites = query(root.path(), 0);
        favorites.album_id = None;
        favorites.favorites_only = true;
        assert_eq!(
            query_page(favorites).unwrap().items[0].media.blur_preview,
            page.items[0].media.blur_preview
        );
        let q = query(root.path(), 0);
        let root_string = root.path().to_string_lossy().to_string();
        create_media_selection(q.clone(), "blur-test".into())
            .await
            .unwrap();
        let selected = media_selection_batch(root_string, "blur-test".into(), 0)
            .await
            .unwrap();
        assert_eq!(
            selected[0].media.blur_preview,
            page.items[0].media.blur_preview
        );
        let neighbor = get_media_neighbor(
            q,
            root.path().join("a/photo.jpg").to_string_lossy().into(),
            1,
        )
        .await
        .unwrap()
        .unwrap();
        assert_eq!(
            neighbor.media.blur_preview,
            page.items[0].media.blur_preview
        );
    }

    #[test]
    fn pages_are_bounded_globally_sorted_and_filter_before_pagination() {
        let root = tempfile::tempdir().unwrap();
        seed(root.path(), 0, 1000);
        let first = query_page(query(root.path(), 0)).unwrap();
        let second = query_page(query(root.path(), 1)).unwrap();
        assert_eq!(first.total, 1000);
        assert_eq!(first.items.len(), 256);
        assert_eq!(second.items.len(), 256);
        assert_eq!(first.items[255].media.name, "000000255.jpg");
        assert_eq!(second.items[0].media.name, "000000256.jpg");
        let mut q = query(root.path(), 0);
        q.sort_key = "shoot".into();
        q.sort_dir = "desc".into();
        q.favorites_only = true;
        let favorites = query_page(q).unwrap();
        assert_eq!(favorites.total, 334);
        assert_eq!(favorites.items[0].media.name, "000000999.jpg");
        let last = query_page(query(root.path(), u64::MAX)).unwrap();
        assert_eq!(last.page, 3);
        assert_eq!(last.items.len(), 232);
        let mut q = query(root.path(), 1);
        q.sort_key = "random".into();
        let a = query_page(q.clone()).unwrap();
        let b = query_page(q).unwrap();
        assert_eq!(
            a.items.iter().map(|i| &i.media.name).collect::<Vec<_>>(),
            b.items.iter().map(|i| &i.media.name).collect::<Vec<_>>()
        );
    }
    #[test]
    fn sparse_writes_and_deletion_update_the_library_index() {
        let root = tempfile::tempdir().unwrap();
        let album = root.path().join("a");
        fs::create_dir(&album).unwrap();
        fs::write(album.join("photo.jpg"), b"source").unwrap();
        scan_album(root.path(), &album).unwrap();
        crate::metadata::update_file_meta(&album, "photo.jpg", |entry| entry.favorite = true)
            .unwrap();
        let mut q = query(root.path(), 0);
        q.favorites_only = true;
        assert_eq!(query_page(q.clone()).unwrap().total, 1);
        fs::remove_file(album.join("photo.jpg")).unwrap();
        let guard = crate::metadata::lock_metadata_for_write(&[&album]).unwrap();
        crate::metadata::remove_deleted_metadata_with_guard(&album, &["photo.jpg".into()], &guard)
            .unwrap();
        assert_eq!(query_page(q).unwrap().total, 0);
    }
    #[test]
    fn derived_hash_updates_do_not_rewrite_display_rows_and_unchanged_inventory_is_reused() {
        let root = tempfile::tempdir().unwrap();
        let album = root.path().join("a");
        fs::create_dir(&album).unwrap();
        fs::write(album.join("photo.jpg"), b"source").unwrap();
        scan_album(root.path(), &album).unwrap();
        let conn = open(root.path()).unwrap();
        conn.execute_batch("CREATE TABLE changes(name TEXT); CREATE TRIGGER changed AFTER UPDATE ON media BEGIN INSERT INTO changes VALUES(new.name); END;").unwrap();
        drop(conn);
        crate::metadata::update_file_meta(&album, "photo.jpg", |e| {
            e.hash = Some("visual hash".into())
        })
        .unwrap();
        scan_album_if_needed(root.path(), &album, false).unwrap();
        let conn = open(root.path()).unwrap();
        assert_eq!(
            conn.query_row("SELECT count(*) FROM changes", [], |r| r.get::<_, i64>(0))
                .unwrap(),
            0
        );
    }
    #[test]
    fn pending_derived_index_write_is_repaired_before_reading() {
        let root = tempfile::tempdir().unwrap();
        let album = root.path().join("a");
        fs::create_dir(&album).unwrap();
        fs::write(album.join("photo.jpg"), b"source").unwrap();
        scan_album(root.path(), &album).unwrap();
        crate::metadata::update_file_meta(&album, "photo.jpg", |entry| entry.favorite = true)
            .unwrap();
        let conn = open(root.path()).unwrap();
        conn.execute("UPDATE media SET favorite=0", []).unwrap();
        drop(conn);
        let marker = pending_marker(root.path(), "a");
        fs::create_dir_all(marker.parent().unwrap()).unwrap();
        fs::write(&marker, "a").unwrap();
        repair_pending(root.path()).unwrap();
        let mut q = query(root.path(), 0);
        q.favorites_only = true;
        assert_eq!(query_page(q).unwrap().total, 1);
        assert!(!marker.exists());
    }
    #[test]
    #[ignore = "disk index scale and RSS benchmark"]
    fn benchmark_page_memory_at_one_hundred_thousand_and_one_million_rows() {
        let root = tempfile::tempdir().unwrap();
        let mut previous = 0;
        for total in [100_000, 1_000_000] {
            seed(root.path(), previous, total);
            previous = total;
            let started = std::time::Instant::now();
            let result = query_page(query(root.path(), (total / 256 - 1) as u64)).unwrap();
            assert_eq!(result.items.len(), 256);
            assert_eq!(result.total, total as u64);
            let rss = std::process::Command::new("ps")
                .args(["-o", "rss=", "-p", &std::process::id().to_string()])
                .output()
                .unwrap();
            eprintln!(
                "catalog rows={total}, returned={}, late-page query={:?}, RSS KiB={}",
                result.items.len(),
                started.elapsed(),
                String::from_utf8_lossy(&rss.stdout).trim()
            );
        }
    }
}

#[derive(Serialize, Deserialize)]
pub struct ScrollMark {
    page: u64,
    y: f64,
    cursor: serde_json::Value,
}

fn scroll_store(root: &Path) -> Result<Connection, String> {
    let conn = open(root)?;
    conn.execute_batch("CREATE TABLE IF NOT EXISTS scroll_marks(session TEXT NOT NULL,page INTEGER NOT NULL,y REAL NOT NULL,data TEXT NOT NULL,PRIMARY KEY(session,page)); CREATE INDEX IF NOT EXISTS scroll_marks_y ON scroll_marks(session,y);").map_err(|e|e.to_string())?;
    Ok(conn)
}

pub async fn begin_gallery_scroll(root_dir: String, session: String) -> Result<(), String> {
    tokio::task::spawn_blocking(move || {
        let conn = scroll_store(Path::new(&root_dir))?;
        conn.execute("DELETE FROM scroll_marks WHERE session<>?1", [session])
            .map_err(|e| e.to_string())?;
        Ok(())
    })
    .await
    .map_err(|e| e.to_string())?
}

pub async fn save_gallery_scroll(
    root_dir: String,
    session: String,
    marks: Vec<ScrollMark>,
) -> Result<(), String> {
    if marks.len() > 6 {
        return Err("Too many scroll checkpoints".into());
    }
    tokio::task::spawn_blocking(move||{
        let mut conn=scroll_store(Path::new(&root_dir))?;
        let tx=conn.transaction().map_err(|e|e.to_string())?;
        for mark in marks{
            if !mark.y.is_finite() || mark.y<0.0 || mark.cursor.to_string().len()>65536{return Err("Invalid scroll checkpoint".into());}
            tx.execute("INSERT INTO scroll_marks VALUES(?1,?2,?3,?4) ON CONFLICT(session,page) DO UPDATE SET y=excluded.y,data=excluded.data",params![session,i64::try_from(mark.page).map_err(|e|e.to_string())?,mark.y,mark.cursor.to_string()]).map_err(|e|e.to_string())?;
        }
        tx.commit().map_err(|e|e.to_string())
    }).await.map_err(|e|e.to_string())?
}

pub async fn read_gallery_scroll(
    root_dir: String,
    session: String,
    y: f64,
) -> Result<Option<ScrollMark>, String> {
    tokio::task::spawn_blocking(move||{
        let conn=scroll_store(Path::new(&root_dir))?;
        let value:Option<(u64,f64,String)>=conn.query_row("SELECT page,y,data FROM scroll_marks WHERE session=?1 AND y<=?2 ORDER BY y DESC LIMIT 1",params![session,y.max(0.0)],|r|Ok((r.get::<_,i64>(0)? as u64,r.get(1)?,r.get(2)?))).optional().map_err(|e|e.to_string())?;
        value.map(|(page,y,data)|Ok(ScrollMark{page,y,cursor:serde_json::from_str(&data).map_err(|e|e.to_string())?})).transpose()
    }).await.map_err(|e|e.to_string())?
}

pub async fn get_viewer_media(
    mut query: PageQuery,
    index: u64,
) -> Result<Option<PageItem>, String> {
    tokio::task::spawn_blocking(move || {
        query.page = index / PAGE_SIZE as u64;
        let page = query_page(query)?;
        let index = index.min(page.total.saturating_sub(1));
        Ok(page
            .items
            .into_iter()
            .nth((index % PAGE_SIZE as u64) as usize))
    })
    .await
    .map_err(|e| e.to_string())?
}

fn selection_store(root: &Path) -> Result<Connection, String> {
    let conn = open(root)?;
    conn.execute_batch("CREATE TABLE IF NOT EXISTS selection_rows(token TEXT NOT NULL,idx INTEGER NOT NULL,album TEXT NOT NULL,name TEXT NOT NULL,meta TEXT NOT NULL,favorite INTEGER NOT NULL,PRIMARY KEY(token,idx)); CREATE INDEX IF NOT EXISTS selection_names ON selection_rows(token,album,name);").map_err(|e|e.to_string())?;
    Ok(conn)
}

pub async fn create_media_selection(query: PageQuery, token: String) -> Result<u64, String> {
    tokio::task::spawn_blocking(move||{
        let root=Path::new(&query.root_dir);let mut conn=selection_store(root)?;
        let direction=if query.sort_dir=="desc"{"DESC"}else{"ASC"};
        let key=match query.sort_key.as_str(){"added"=>"added","shoot"=>"shoot","random"=>"shuffle(album || '/' || name, ?4)",_=>"name COLLATE NOCASE"};
        let group=if query.album_id.is_none(){"album_label(album) ASC, album ASC, "}else{""};
        let order=format!("{group}{key} {direction},album {direction},name {direction}");
        let tx=conn.transaction().map_err(|e|e.to_string())?;
        tx.execute("DELETE FROM selection_rows WHERE token=?1",[&token]).map_err(|e|e.to_string())?;
        tx.execute(&format!("INSERT INTO selection_rows SELECT ?1,row_number() OVER(ORDER BY {order}),album,name,meta,favorite FROM media WHERE (?2 IS NULL OR album=?2) AND (?3=0 OR favorite=1) AND (?4 IS NOT NULL)"),params![token,query.album_id,query.favorites_only,(query.random_seed*1_000_000.0) as i64]).map_err(|e|e.to_string())?;
        let count:i64=tx.query_row("SELECT count(*) FROM selection_rows WHERE token=?1",[&token],|r|r.get(0)).map_err(|e|e.to_string())?;
        tx.commit().map_err(|e|e.to_string())?;Ok(count as u64)
    }).await.map_err(|e|e.to_string())?
}

pub async fn drop_media_selection(root_dir: String, token: String) -> Result<(), String> {
    tokio::task::spawn_blocking(move || {
        selection_store(Path::new(&root_dir))?
            .execute("DELETE FROM selection_rows WHERE token=?1", [token])
            .map_err(|e| e.to_string())?;
        Ok(())
    })
    .await
    .map_err(|e| e.to_string())?
}

pub async fn media_selection_flags(
    root_dir: String,
    token: String,
    paths: Vec<String>,
) -> Result<Vec<bool>, String> {
    if paths.len() > PAGE_SIZE * 5 + 12 {
        return Err("Too many selection paths".into());
    }
    tokio::task::spawn_blocking(move||{
        let root=Path::new(&root_dir);let conn=selection_store(root)?;let mut flags=Vec::with_capacity(paths.len());
        for path in paths{let path=Path::new(&path);let album=path.parent().ok_or("Missing album")?;let name=path.file_name().and_then(|n|n.to_str()).ok_or("Missing media name")?;let id=album_id(root,album)?;flags.push(conn.query_row("SELECT EXISTS(SELECT 1 FROM selection_rows WHERE token=?1 AND album=?2 AND name=?3)",params![token,id,name],|r|r.get(0)).map_err(|e|e.to_string())?);}
        Ok(flags)
    }).await.map_err(|e|e.to_string())?
}

pub async fn media_selection_batch(
    root_dir: String,
    token: String,
    offset: u64,
) -> Result<Vec<PageItem>, String> {
    tokio::task::spawn_blocking(move||{
        let root=Path::new(&root_dir);let conn=selection_store(root)?;
        let mut stmt=conn.prepare("SELECT s.album,s.name,s.meta,s.favorite,m.blur_preview FROM selection_rows s LEFT JOIN media m ON m.album=s.album AND m.name=s.name WHERE s.token=?1 AND s.idx>?2 ORDER BY s.idx LIMIT 256").map_err(|e|e.to_string())?;
        let rows=stmt.query_map(params![token,i64::try_from(offset).map_err(|e|e.to_string())?],|r|Ok((r.get::<_,String>(0)?,r.get::<_,String>(1)?,r.get::<_,String>(2)?,r.get::<_,bool>(3)?,r.get::<_,Option<String>>(4)?))).map_err(|e|e.to_string())?;
        let mut items=Vec::new();for row in rows{let(album,name,meta,favorite,blur_preview)=row.map_err(|e|e.to_string())?;let dir=root.join(&album);let path=dir.join(&name);items.push(PageItem{media:DetachedMediaEntry{blur_preview,name,meta,favorite:Some(favorite),revision:crate::metadata::media_revision(&path),thumbnail_name:crate::thumb::detached_thumbnail_name(&path)},album_path:dir.to_string_lossy().into_owned(),album_id:album.clone(),album_name:album});}Ok(items)
    }).await.map_err(|e|e.to_string())?
}

pub async fn copy_media_selection(
    root_dir: String,
    token: String,
    excluded: Vec<String>,
    extra: Vec<String>,
) -> Result<(), String> {
    tokio::task::spawn_blocking(move || {
        let root = Path::new(&root_dir);
        let conn = selection_store(root)?;
        let mut stmt = conn
            .prepare("SELECT album,name FROM selection_rows WHERE token=?1 ORDER BY idx")
            .map_err(|e| e.to_string())?;
        let excluded: std::collections::HashSet<_> = excluded.into_iter().collect();
        let rows = stmt
            .query_map([token], |r| {
                Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?))
            })
            .map_err(|e| e.to_string())?;
        // Clipboard APIs require their output file list. Keep this native and
        // copy only paths, without loading album DTOs or any source images.
        let mut paths = Vec::new();
        for row in rows {
            let (album, name) = row.map_err(|e| e.to_string())?;
            let path = root.join(album).join(name).to_string_lossy().into_owned();
            if !excluded.contains(&path) {
                paths.push(path);
            }
        }
        paths.extend(extra);
        crate::clipboard::set_clipboard_files(paths)
    })
    .await
    .map_err(|e| e.to_string())?
}

pub async fn consume_media_selection(
    root_dir: String,
    token: String,
    paths: Vec<String>,
) -> Result<usize, String> {
    if paths.len() > PAGE_SIZE {
        return Err("Too many selection paths".into());
    }
    tokio::task::spawn_blocking(move || {
        let root = Path::new(&root_dir);
        let mut conn = selection_store(root)?;
        let tx = conn.transaction().map_err(|e| e.to_string())?;
        let mut removed = 0;
        for path in paths {
            let path = Path::new(&path);
            let album = path.parent().ok_or("Missing album")?;
            let name = path
                .file_name()
                .and_then(|n| n.to_str())
                .ok_or("Missing media name")?;
            let id = album_id(root, album)?;
            removed += tx
                .execute(
                    "DELETE FROM selection_rows WHERE token=?1 AND album=?2 AND name=?3",
                    params![token, id, name],
                )
                .map_err(|e| e.to_string())?;
        }
        tx.commit().map_err(|e| e.to_string())?;
        Ok(removed)
    })
    .await
    .map_err(|e| e.to_string())?
}

pub async fn prune_media_selection(root_dir: String, token: String) -> Result<usize, String> {
    tokio::task::spawn_blocking(move||{let root=Path::new(&root_dir);repair_pending(root)?;let conn=selection_store(root)?;conn.execute("DELETE FROM selection_rows WHERE token=?1 AND NOT EXISTS(SELECT 1 FROM media WHERE media.album=selection_rows.album AND media.name=selection_rows.name)",[token]).map_err(|e|e.to_string())}).await.map_err(|e|e.to_string())?
}

#[cfg(test)]
mod streaming_selection_tests {
    use super::*;
    #[tokio::test]
    async fn whole_query_selection_is_disk_backed_bounded_and_keeps_stable_membership() {
        let dir = tempfile::tempdir().unwrap();
        let mut conn = open(dir.path()).unwrap();
        let tx = conn.transaction().unwrap();
        for n in 0..2000 {
            tx.execute(
                "INSERT INTO media(album,name,meta,favorite,added,shoot,seen) VALUES('a',?1,'0',1,?2,?2,0)",
                params![format!("{n:05}.jpg"), n],
            )
            .unwrap();
        }
        tx.commit().unwrap();
        drop(conn);
        let root = dir.path().to_string_lossy().into_owned();
        let token = "snapshot".to_string();
        let query = PageQuery {
            root_dir: root.clone(),
            album_id: Some("a".into()),
            favorites_only: false,
            sort_key: "name".into(),
            sort_dir: "asc".into(),
            random_seed: 0.5,
            page: 0,
            limit_pages: None,
        };
        assert_eq!(
            create_media_selection(query, token.clone()).await.unwrap(),
            2000
        );
        let conn = open(dir.path()).unwrap();
        conn.execute("INSERT INTO media(album,name,meta,favorite,added,shoot,seen) VALUES('a','new.jpg','0',1,0,0,0)", [])
            .unwrap();
        drop(conn);
        let items = media_selection_batch(root.clone(), token.clone(), 0)
            .await
            .unwrap();
        assert_eq!(items.len(), 256);
        assert_eq!(items[0].media.name, "00000.jpg");
        let paths = vec![format!("{root}/a/00000.jpg"), format!("{root}/a/new.jpg")];
        assert_eq!(
            media_selection_flags(root.clone(), token.clone(), paths.clone())
                .await
                .unwrap(),
            vec![true, false]
        );
        assert_eq!(
            consume_media_selection(root.clone(), token.clone(), vec![paths[0].clone()])
                .await
                .unwrap(),
            1
        );
        assert_eq!(
            media_selection_flags(root.clone(), token.clone(), paths)
                .await
                .unwrap(),
            vec![false, false]
        );
        drop_media_selection(root.clone(), token).await.unwrap();
        assert_eq!(
            selection_store(dir.path())
                .unwrap()
                .query_row("SELECT count(*) FROM selection_rows", [], |r| r
                    .get::<_, i64>(0))
                .unwrap(),
            0
        );
    }
}

pub async fn get_media_neighbor(
    query: PageQuery,
    current_path: String,
    delta: i32,
) -> Result<Option<PageItem>, String> {
    tokio::task::spawn_blocking(move||{
        let root=Path::new(&query.root_dir);let current=Path::new(&current_path);let dir=current.parent().ok_or("Missing album")?;let current_album=album_id(root,dir)?;let current_name=current.file_name().and_then(|n|n.to_str()).ok_or("Missing file name")?;
        let conn=open(root)?;let seed=(query.random_seed*1_000_000.0) as i64;
        let key=match query.sort_key.as_str(){"added"=>"added","shoot"=>"shoot","random"=>"shuffle(album || '/' || name, ?1)",_=>"name COLLATE NOCASE"};
        let value:Option<rusqlite::types::Value>=conn.query_row(&format!("SELECT {key} FROM media WHERE (?1 IS NOT NULL) AND album=?2 AND name=?3"),params![seed,current_album,current_name],|r|r.get(0)).optional().map_err(|e|e.to_string())?;
        let Some(value)=value else{return Ok(None)};
        let forward=delta>=0;let ascending=(query.sort_dir!="desc")==forward;let comparison=if ascending{">"}else{"<"};let direction=if ascending{"ASC"}else{"DESC"};let group_comparison=if forward{">"}else{"<"};let group_direction=if forward{"ASC"}else{"DESC"};
        let after=format!("({key}{comparison}?4 OR ({key}=?4 AND (album{comparison}?5 OR (album=?5 AND name{comparison}?6))))");
        let after=if query.album_id.is_none(){format!("(album_label(album){group_comparison}?7 OR (album_label(album)=?7 AND (album{group_comparison}?5 OR (album=?5 AND {after}))))")}else{after};
        let grouping=if query.album_id.is_none(){format!("album_label(album) {group_direction},album {group_direction}, ")}else{String::new()};
        let label=current_album.rsplit('/').next().unwrap_or(&current_album).to_lowercase();
        let mut found:Option<(String,String,String,bool,Option<String>)>=None;
        let local_after=format!("({key},name){comparison}(?4,?6)");
        let attempts=if query.album_id.is_some(){vec![(true,local_after),(true,"(?4 IS NOT NULL AND ?5 IS NOT NULL AND ?6 IS NOT NULL)".into())]}else{vec![(true,local_after),(false,after),(false,"(?4 IS NOT NULL AND ?5 IS NOT NULL AND ?6 IS NOT NULL)".into())]};
        for (local,condition) in attempts {
            let filter=if local{"album=?2"}else{"?2 IS NULL"};
            let grouping=if local{String::new()}else{grouping.clone()};
            let album_filter=if local{Some(current_album.clone())}else{None};
            let sql=format!("SELECT album,name,meta,favorite,blur_preview FROM media WHERE (?1 IS NOT NULL) AND {filter} AND (?3=0 OR favorite=1) AND (?7 IS NOT NULL) AND {condition} ORDER BY {grouping}{key} {direction},album {direction},name {direction} LIMIT 1");
            found=conn.query_row(&sql,params![seed,album_filter,query.favorites_only,value,current_album,current_name,label],|r|Ok((r.get(0)?,r.get(1)?,r.get(2)?,r.get(3)?,r.get(4)?))).optional().map_err(|e|e.to_string())?;
            if found.is_some(){break;}
        }
        Ok(found.map(|(album,name,meta,favorite,blur_preview)|{let dir=root.join(&album);let path=dir.join(&name);PageItem{media:DetachedMediaEntry{blur_preview,name,meta,favorite:Some(favorite),revision:crate::metadata::media_revision(&path),thumbnail_name:crate::thumb::detached_thumbnail_name(&path)},album_path:dir.to_string_lossy().into_owned(),album_name:album.rsplit('/').next().unwrap_or(&album).to_string(),album_id:album}}))
    }).await.map_err(|e|e.to_string())?
}

#[cfg(test)]
mod neighbor_tests {
    use super::*;
    fn query(root: &Path) -> PageQuery {
        PageQuery {
            root_dir: root.to_string_lossy().into(),
            album_id: Some("a".into()),
            favorites_only: false,
            sort_key: "name".into(),
            sort_dir: "asc".into(),
            random_seed: 0.5,
            page: 0,
            limit_pages: None,
        }
    }
    #[tokio::test]
    async fn neighbor_uses_live_sort_keys_and_wraps_without_loading_the_gallery() {
        let root = tempfile::tempdir().unwrap();
        let conn = open(root.path()).unwrap();
        for n in 0..10 {
            conn.execute(
                "INSERT INTO media(album,name,meta,favorite,added,shoot,seen) VALUES('a',?1,'0',?2,?3,?3,0)",
                params![format!("{n}.jpg"), n % 2 == 0, n],
            )
            .unwrap();
        }
        for (name, dir, delta, expected) in [
            ("5", "asc", 1, "6.jpg"),
            ("9", "asc", 1, "0.jpg"),
            ("0", "asc", -1, "9.jpg"),
            ("5", "desc", 1, "4.jpg"),
            ("5", "desc", -1, "6.jpg"),
        ] {
            let mut q = query(root.path());
            q.sort_dir = dir.into();
            let result = get_media_neighbor(
                q,
                root.path()
                    .join(format!("a/{name}.jpg"))
                    .to_string_lossy()
                    .into(),
                delta,
            )
            .await
            .unwrap()
            .unwrap();
            assert_eq!(result.media.name, expected);
        }
        conn.execute("UPDATE media SET shoot=1000 WHERE name='5.jpg'", [])
            .unwrap();
        let mut q = query(root.path());
        q.sort_key = "shoot".into();
        let result = get_media_neighbor(q, root.path().join("a/5.jpg").to_string_lossy().into(), 1)
            .await
            .unwrap()
            .unwrap();
        assert_eq!(result.media.name, "0.jpg");
        let mut q = query(root.path());
        q.favorites_only = true;
        let result = get_media_neighbor(q, root.path().join("a/4.jpg").to_string_lossy().into(), 1)
            .await
            .unwrap()
            .unwrap();
        assert_eq!(result.media.name, "6.jpg");
    }
    #[tokio::test]
    async fn favorites_keep_album_names_and_group_order() {
        let root = tempfile::tempdir().unwrap();
        let conn = open(root.path()).unwrap();
        for album in ["z/aaa", "a/zzz"] {
            conn.execute(
                "INSERT INTO media(album,name,meta,favorite,added,shoot,seen) VALUES(?1,'photo.jpg','0',1,0,0,0)",
                [album],
            )
            .unwrap();
        }
        let mut q = query(root.path());
        q.album_id = None;
        q.favorites_only = true;
        let page = query_page(q.clone()).unwrap();
        assert_eq!(page.items[0].album_name, "aaa");
        assert_eq!(page.items[1].album_name, "zzz");
        let next = get_media_neighbor(
            q,
            root.path().join("z/aaa/photo.jpg").to_string_lossy().into(),
            1,
        )
        .await
        .unwrap()
        .unwrap();
        assert_eq!(next.album_id, "a/zzz");
    }
    #[test]
    fn internal_window_query_is_capped() {
        let root = tempfile::tempdir().unwrap();
        let mut conn = open(root.path()).unwrap();
        let tx = conn.transaction().unwrap();
        for n in 0..2000 {
            tx.execute(
                "INSERT INTO media(album,name,meta,favorite,added,shoot,seen) VALUES('a',?1,'0',1,0,0,0)",
                [format!("{n:05}.jpg")],
            )
            .unwrap();
        }
        tx.commit().unwrap();
        let mut q = query(root.path());
        q.limit_pages = Some(usize::MAX);
        let result = query_page(q).unwrap();
        assert_eq!(result.items.len(), 1280);
        assert_eq!(result.total, 2000);
    }
}

/// Small immutable geometry projection for native layouts. No original-file stat,
/// blur payload or CGImage is retained for offscreen records.
#[derive(Serialize)]
#[serde(rename_all="camelCase")]
pub struct NativeGeometry { name: String, revision: String, width: u32, height: u32, album_id: String, album_name: String, favorite: bool }
pub async fn native_geometry(query: PageQuery) -> Result<Vec<NativeGeometry>,String> {
    tokio::task::spawn_blocking(move || {
        let conn=open(Path::new(&query.root_dir))?;
        let direction=if query.sort_dir=="desc" {"DESC"} else {"ASC"};
        let order=match query.sort_key.as_str(){"added"=>"added","shoot"=>"shoot","random"=>"shuffle(album || '/' || name, ?3)",_=>"name COLLATE NOCASE"};
        let filter=if query.album_id.is_some(){"album=?1 AND (?2=0 OR favorite=1)"}else{"?1 IS NULL AND (?2=0 OR favorite=1)"};
        let grouping=if query.album_id.is_none(){"album_label(album) ASC, album ASC, "}else{""};
        let mut statement=conn.prepare(&format!("SELECT meta,album,name,favorite FROM media WHERE {filter} AND (?3 IS NOT NULL) ORDER BY {grouping}{order} {direction}, album {direction}, name {direction}")).map_err(|e|e.to_string())?;
        let rows=statement.query_map(params![query.album_id,query.favorites_only,(query.random_seed*1_000_000.0) as i64],|row|Ok((row.get::<_,String>(0)?,row.get::<_,String>(1)?,row.get::<_,String>(2)?,row.get::<_,bool>(3)?))).map_err(|e|e.to_string())?;
        let mut result=Vec::new();
        for row in rows {let (packed,album_id,name,favorite)=row.map_err(|e|e.to_string())?;let meta=crate::metadata::unpack_file_meta(&packed)?;
            let album_name=album_id.rsplit('/').next().unwrap_or(&album_id).to_owned();
            let revision=crate::metadata::media_revision(&Path::new(&query.root_dir).join(&album_id).join(&name));
            result.push(NativeGeometry{name,revision,favorite,width:meta.w.unwrap_or(1),height:meta.h.unwrap_or(1),album_id,album_name});
        }
        Ok(result)
    }).await.map_err(|e|e.to_string())?
}
