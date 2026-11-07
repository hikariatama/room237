//! Similarity index and group membership are disk backed. Working memory is
//! independent of the number of photographs, candidates and duplicate groups.
use crate::{
    constants::IMAGE_EXTENSIONS,
    duplicates::within_threshold,
    metadata::DetachedMediaEntry,
    settings::AdvancedSettings,
    util::{has_extension, FileLock},
};
use rusqlite::{params, Connection, OptionalExtension};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::{
    fs,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicU64, Ordering},
        Mutex,
    },
    time::Duration,
};

#[path = "duplicate_cache.rs"]
mod cache;

#[path = "duplicate_mih.rs"]
mod mih;

#[path = "duplicate_repair.rs"]
mod repair;

#[cfg(test)]
#[path = "duplicate_profile.rs"]
mod profile;

macro_rules! measured {
    ($label:literal, $expression:expr) => {{
        #[cfg(test)]
        let _profile_span = profile::span($label);
        $expression
    }};
}

#[cfg(test)]
std::thread_local! { static TEST_SCAN_ONLY: std::cell::Cell<bool> = const { std::cell::Cell::new(false) }; }

const PAGE: usize = 128;
static REQUEST: AtomicU64 = AtomicU64::new(0);
static SCAN: Mutex<()> = Mutex::new(());

fn check(request: u64) -> Result<(), String> {
    if REQUEST.load(Ordering::Relaxed) == request {
        Ok(())
    } else {
        Err("Duplicate scan superseded".into())
    }
}
fn open(path: &Path) -> Result<Connection, String> {
    let conn = Connection::open(path).map_err(|e| e.to_string())?;
    conn.busy_timeout(Duration::from_secs(30))
        .map_err(|e| e.to_string())?;
    conn.execute_batch("PRAGMA cache_size=-2048; PRAGMA temp_store=FILE; PRAGMA temp.cache_size=-2048; PRAGMA mmap_size=0;")
        .map_err(|e| e.to_string())?;
    Ok(conn)
}

/// One directory listing with every file's source and preview stamps. Stamps
/// are read on a small pool: on network and external volumes each stat is a
/// round trip, so a serial pass over a large album dominates every refresh.
struct Item {
    name: String,
    /// Stamp of the original alone; previews are regenerated without changing it.
    source: String,
    /// Original and preview together, exactly as `media_revision` reports it.
    revision: String,
}
fn inventory(dir: &Path, request: u64) -> Result<Vec<Item>, String> {
    use rayon::prelude::*;
    let mut names = Vec::new();
    let hidden_heic = crate::metadata::hidden_heic_names(dir)?;
    for file in fs::read_dir(dir).map_err(|e| e.to_string())? {
        check(request)?;
        let file = file.map_err(|e| e.to_string())?;
        let path = file.path();
        if !file.file_type().map_err(|e| e.to_string())?.is_file()
            || !has_extension(&path, IMAGE_EXTENSIONS)
            || hidden_heic.contains(&*file.file_name().to_string_lossy())
        {
            continue;
        }
        names.push(file.file_name().to_string_lossy().into_owned());
    }
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(8)
        .build()
        .map_err(|e| e.to_string())?;
    let items: Vec<Option<Item>> = pool.install(|| {
        names
            .into_par_iter()
            .map(|name| {
                if check(request).is_err() {
                    return None;
                }
                let (source, preview) = crate::metadata::media_revision_parts(&dir.join(&name));
                let revision = format!("{source}-{preview}");
                Some(Item {
                    name,
                    source,
                    revision,
                })
            })
            .collect()
    });
    check(request)?;
    Ok(items.into_iter().flatten().collect())
}

// Keep the v3 fingerprint compatible with already computed results. Marking a
// group can extend the ignores digest inside the same cross-database transaction.
fn source_fingerprint(items: &[Item], settings: &AdvancedSettings) -> Sha256 {
    let mut sha = Sha256::new();
    sha.update("disk-duplicate-groups-v3");
    sha.update(settings.hash_config().hash_version);
    sha.update(settings.duplicates.effective_threshold().to_le_bytes());
    for item in items {
        sha.update(item.name.as_bytes());
        sha.update([0]);
        sha.update(item.revision.as_bytes());
        sha.update([0]);
    }
    sha
}
fn finish_fingerprint(mut sha: Sha256, conn: &Connection, table: &str) -> Result<String, String> {
    // table is a caller-owned literal, never input from IPC.
    let mut stmt = conn
        .prepare(&format!("SELECT a,b FROM {table} ORDER BY a,b"))
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))
        .map_err(|e| e.to_string())?;
    for row in rows {
        let (a, b) = row.map_err(|e| e.to_string())?;
        sha.update(a);
        sha.update([0]);
        sha.update(b);
        sha.update([0]);
    }
    Ok(hex::encode(sha.finalize()))
}
fn fingerprint(items: &[Item], dir: &Path, settings: &AdvancedSettings) -> Result<String, String> {
    let conn = open(&dir.join(".room237-metadata/album.sqlite"))?;
    finish_fingerprint(source_fingerprint(items, settings), &conn, "ignored_pairs")
}

fn bytes(bits: &[u64]) -> Vec<u8> {
    bits.iter().flat_map(|n| n.to_le_bytes()).collect()
}
const HASH_BLOCK_FILES: usize = 1024;
const HASH_CACHE_BYTES: usize = 1024 * 1024;
const CANDIDATE_HASH_PAGE: usize = 64;

// Fixed memory budget, fully used at each hash length. MIH reads fine pages
// with SQLite substr(), so a sparse candidate does not decode 1024 hashes.
struct HashBlocks {
    words: usize,
    files: usize,
    cached: cache::Pages<usize>,
}
impl HashBlocks {
    fn new(words: usize) -> Self {
        Self::with_page_size(words, HASH_BLOCK_FILES)
    }
    fn with_page_size(words: usize, files: usize) -> Self {
        Self {
            words,
            files,
            cached: cache::Pages::new(HASH_CACHE_BYTES / (files * words * 8)),
        }
    }
    fn get(&mut self, conn: &Connection, block: usize) -> Result<&[u64], String> {
        let words = self.words;
        let files = self.files;
        self.cached.get(block, || {
            let source_block = block * files / HASH_BLOCK_FILES;
            let start = (block * files % HASH_BLOCK_FILES) * words * 8 + 1;
            let size = files * words * 8;
            let data: Vec<u8> = conn
                .prepare_cached("SELECT substr(data,?2,?3) FROM hash_blocks WHERE block=?1")
                .map_err(|e| e.to_string())?
                .query_row(
                    params![source_block as i64, start as i64, size as i64],
                    |r| r.get(0),
                )
                .map_err(|e| e.to_string())?;
            if data.is_empty() || data.len() % (words * 8) != 0 || data.len() > size {
                return Err("Invalid duplicate hash block".into());
            }
            Ok(data
                .chunks_exact(8)
                .map(|bytes| u64::from_le_bytes(bytes.try_into().unwrap()))
                .collect())
        })
    }
}

fn merge(conn: &Connection, a: &str, b: &str) -> Result<(), String> {
    let lookup="SELECT n.root,c.restricted FROM nodes n JOIN components c ON c.root=n.root WHERE n.name=?1";
    let (left, left_restricted): (String, bool) = conn
        .prepare_cached(lookup)
        .map_err(|e| e.to_string())?
        .query_row([a], |r| Ok((r.get(0)?, r.get(1)?)))
        .map_err(|e| e.to_string())?;
    let (right, right_restricted): (String, bool) = conn
        .prepare_cached(lookup)
        .map_err(|e| e.to_string())?
        .query_row([b], |r| Ok((r.get(0)?, r.get(1)?)))
        .map_err(|e| e.to_string())?;
    if left == right {
        return Ok(());
    }
    // An exclusion can cross the components only if both contain endpoints.
    let blocked:bool=left_restricted && right_restricted && conn.prepare_cached("SELECT EXISTS(
        SELECT 1 FROM nodes a INDEXED BY roots CROSS JOIN ignored i CROSS JOIN nodes b
        WHERE a.root=?1 AND i.a=a.name AND b.name=i.b AND b.root=?2
        UNION ALL
        SELECT 1 FROM nodes a INDEXED BY roots CROSS JOIN ignored i INDEXED BY ignored_b CROSS JOIN nodes b
        WHERE a.root=?1 AND i.b=a.name AND b.name=i.a AND b.root=?2)")
        .map_err(|e|e.to_string())?.query_row(params![left,right],|r|r.get(0)).map_err(|e|e.to_string())?;
    if !blocked {
        let (keep, remove) = if left < right {
            (&left, &right)
        } else {
            (&right, &left)
        };
        conn.prepare_cached("UPDATE nodes SET root=?1 WHERE root=?2")
            .map_err(|e| e.to_string())?
            .execute(params![keep, remove])
            .map_err(|e| e.to_string())?;
        conn.prepare_cached("UPDATE components SET restricted=?2 WHERE root=?1")
            .map_err(|e| e.to_string())?
            .execute(params![keep, left_restricted || right_restricted])
            .map_err(|e| e.to_string())?;
        conn.prepare_cached("DELETE FROM components WHERE root=?1")
            .map_err(|e| e.to_string())?
            .execute([remove])
            .map_err(|e| e.to_string())?;
    }
    Ok(())
}

fn verify_candidates(
    conn: &Connection,
    cache: &mut HashBlocks,
    bits: &[u64],
    name: &str,
    ids: &[u64],
    threshold: u32,
    request: u64,
    comparisons: &mut i64,
) -> Result<(), String> {
    let files = cache.files;
    let mut first = 0;
    while first < ids.len() {
        check(request)?;
        let block = ids[first] as usize / files;
        let mut end = first + 1;
        while end < ids.len() && ids[end] as usize / files == block {
            end += 1;
        }
        let hashes = measured!("match.hash_block_access", cache.get(conn, block))?;
        #[cfg(test)]
        let compare_loop = profile::span("match.distance_scan");
        for id in &ids[first..end] {
            let offset = (*id as usize % files) * bits.len();
            let other = hashes
                .get(offset..offset + bits.len())
                .ok_or("Invalid MIH candidate ID")?;
            *comparisons += 1;
            if within_threshold(bits, other, threshold) {
                #[cfg(test)]
                let merge_scope = profile::span("match.merge_sql");
                let other: String = conn
                    .prepare_cached("SELECT name FROM hash_names WHERE id=?1")
                    .map_err(|e| e.to_string())?
                    .query_row([i64::try_from(*id).map_err(|e| e.to_string())?], |r| {
                        r.get(0)
                    })
                    .map_err(|e| e.to_string())?;
                merge(conn, name, &other)?;
                #[cfg(test)]
                drop(merge_scope);
            }
        }
        #[cfg(test)]
        drop(compare_loop);
        first = end;
    }
    Ok(())
}

fn build(dir: &Path, settings: &AdvancedSettings, request: u64) -> Result<PathBuf, String> {
    use rayon::prelude::*;
    #[cfg(test)]
    let _total = profile::span("total");
    #[cfg(test)]
    let input_phase = profile::span("phase.input_validation");
    crate::metadata::prepare_database(dir)?;
    let service = dir.join(".room237-metadata");
    let path = service.join("duplicates.sqlite");
    let items = inventory(dir, request)?;
    let signature = fingerprint(&items, dir, settings)?;
    if path.is_file() {
        if let Ok(mut conn) = open(&path) {
            let current =
                conn.query_row("SELECT signature FROM state", [], |r| r.get::<_, String>(0));
            if current.as_deref() == Ok(signature.as_str()) {
                repair::remember_configuration(&conn, settings)?;
                return Ok(path);
            }
            if repair::prune_deletions(dir, &mut conn, settings, &signature, &items, request)? {
                return Ok(path);
            }
        }
    }
    #[cfg(test)]
    drop(input_phase);
    #[cfg(test)]
    let setup_phase = profile::span("phase.setup_database");
    let temporary = tempfile::Builder::new()
        .prefix(".room237-duplicates-")
        .tempfile_in(&service)
        .map_err(|e| e.to_string())?;
    let mut conn = open(temporary.path())?;
    conn.execute_batch(
        "CREATE TABLE nodes(name TEXT PRIMARY KEY,hash BLOB,root TEXT NOT NULL,revision TEXT NOT NULL DEFAULT '',visited INTEGER NOT NULL DEFAULT 0);
        CREATE INDEX roots ON nodes(root,name); CREATE INDEX hashes ON nodes(hash,visited);
        CREATE TABLE hash_names(id INTEGER PRIMARY KEY,name TEXT NOT NULL UNIQUE);
        CREATE TABLE hash_blocks(block INTEGER PRIMARY KEY,data BLOB NOT NULL);
        CREATE TABLE ignored(a TEXT,b TEXT,PRIMARY KEY(a,b)); CREATE INDEX ignored_b ON ignored(b,a);
        CREATE TABLE restricted(name TEXT PRIMARY KEY);
        CREATE TABLE components(root TEXT PRIMARY KEY,restricted INTEGER NOT NULL);
        CREATE TABLE state(signature TEXT NOT NULL,comparisons INTEGER NOT NULL DEFAULT 0,exact_folds INTEGER NOT NULL DEFAULT 0,search_method TEXT NOT NULL DEFAULT 'scan',mih_probes INTEGER NOT NULL DEFAULT 0,mih_fallbacks INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE groups(root TEXT PRIMARY KEY,count INTEGER NOT NULL,ordinal INTEGER UNIQUE NOT NULL);",
    )
    .map_err(|e| e.to_string())?;
    conn.execute(
        "ATTACH DATABASE ?1 AS metadata",
        [service.join("album.sqlite").to_string_lossy().as_ref()],
    )
    .map_err(|e| e.to_string())?;
    conn.execute(
        "INSERT INTO ignored SELECT a,b FROM metadata.ignored_pairs",
        [],
    )
    .map_err(|e| e.to_string())?;
    let have_ignores: bool = conn
        .query_row("SELECT EXISTS(SELECT 1 FROM ignored)", [], |r| r.get(0))
        .map_err(|e| e.to_string())?;
    conn.execute_batch("INSERT OR IGNORE INTO restricted SELECT a FROM ignored; INSERT OR IGNORE INTO restricted SELECT b FROM ignored;").map_err(|e|e.to_string())?;
    #[cfg(test)]
    drop(setup_phase);
    // Inventory is streamed into disk storage before deterministic ordered scan.
    #[cfg(test)]
    let inventory_phase = profile::span("phase.inventory");
    {
        let tx = conn.transaction().map_err(|e| e.to_string())?;
        let hidden_heic = crate::metadata::hidden_heic_names(dir)?;
        for file in fs::read_dir(dir).map_err(|e| e.to_string())? {
            check(request)?;
            let file = file.map_err(|e| e.to_string())?;
            let path = file.path();
            if file.file_type().map_err(|e| e.to_string())?.is_file()
                && has_extension(&path, IMAGE_EXTENSIONS)
                && !hidden_heic.contains(&*file.file_name().to_string_lossy())
            {
                let name = file.file_name().to_string_lossy().into_owned();
                tx.execute("INSERT INTO nodes(name,root) VALUES(?1,?1)", [name])
                    .map_err(|e| e.to_string())?;
            }
        }
        tx.commit().map_err(|e| e.to_string())?;
    }
    conn.execute_batch("INSERT INTO components SELECT name,EXISTS(SELECT 1 FROM restricted r WHERE r.name=nodes.name) FROM nodes;").map_err(|e|e.to_string())?;
    #[cfg(test)]
    drop(inventory_phase);
    #[cfg(test)]
    let hashes_phase = profile::span("phase.read_and_pack_hashes");
    let cfg = settings.hash_config();
    let words = cfg.bits as usize / 64;
    let mut reader = crate::album_db::FileReader::new(&service.join("album.sqlite"))?;
    let mut last = String::new();
    let mut count = 0usize;
    let mut packed = Vec::with_capacity(HASH_BLOCK_FILES * words * 8);
    let hash_pool = rayon::ThreadPoolBuilder::new()
        .num_threads(usize::from(settings.preload.hash_workers).clamp(1, 8))
        .build()
        .map_err(|e| e.to_string())?;
    // First stream the cached/computed hashes into compact disk blocks. IDs
    // follow filename order so exclusions retain the previous merge order.
    loop {
        check(request)?;
        let names: Vec<String> = {
            let mut stmt = conn
                .prepare_cached("SELECT name FROM nodes WHERE name>?1 ORDER BY name LIMIT 128")
                .map_err(|e| e.to_string())?;
            let rows = stmt
                .query_map([&last], |r| r.get(0))
                .map_err(|e| e.to_string())?;
            rows.collect::<Result<_, _>>().map_err(|e| e.to_string())?
        };
        if names.is_empty() {
            break;
        }
        // Missing hashes decode originals or previews; run those concurrently
        // (decoders are still bounded by `DecodePermit`). Cached hashes pass
        // through the same path at the cost of a validation each.
        let mut entries = Vec::with_capacity(names.len());
        for name in &names {
            check(request)?;
            entries.push(measured!("hash.metadata_read", reader.get(name))?);
        }
        let computed: Vec<(Option<Vec<u64>>, String)> = measured!(
            "hash.cache_validation_and_decode",
            hash_pool.install(|| {
                names
                    .par_iter()
                    .zip(entries.par_iter())
                    .map(|(name, entry)| {
                        if check(request).is_err() {
                            return (None, String::new());
                        }
                        let file = dir.join(name);
                        let bits = crate::duplicates::compute_hash_from_entry(
                            &file,
                            &cfg,
                            settings,
                            entry.as_ref(),
                        );
                        // Hashing can create the preview; take the stamp afterwards.
                        (bits, crate::metadata::media_revision(&file))
                    })
                    .collect()
            })
        );
        check(request)?;
        let tx = conn.transaction().map_err(|e| e.to_string())?;
        for (name, (bits, revision)) in names.into_iter().zip(computed) {
            last = name.clone();
            let Some(bits) = bits else {
                tx.prepare_cached("UPDATE nodes SET revision=?1 WHERE name=?2")
                    .map_err(|e| e.to_string())?
                    .execute(params![revision, name])
                    .map_err(|e| e.to_string())?;
                continue;
            };
            let hash = bytes(&bits);
            #[cfg(test)]
            let store_hash = profile::span("hash.sqlite_and_packing");
            tx.prepare_cached("UPDATE nodes SET hash=?1,revision=?3 WHERE name=?2")
                .map_err(|e| e.to_string())?
                .execute(params![hash, name, revision])
                .map_err(|e| e.to_string())?;
            tx.prepare_cached("INSERT INTO hash_names VALUES(?1,?2)")
                .map_err(|e| e.to_string())?
                .execute(params![count as i64, name])
                .map_err(|e| e.to_string())?;
            packed.extend_from_slice(&hash);
            count += 1;
            if count % HASH_BLOCK_FILES == 0 {
                tx.prepare_cached("INSERT INTO hash_blocks VALUES(?1,?2)")
                    .map_err(|e| e.to_string())?
                    .execute(params![(count / HASH_BLOCK_FILES - 1) as i64, packed])
                    .map_err(|e| e.to_string())?;
                packed.clear();
            }
            #[cfg(test)]
            drop(store_hash);
        }
        measured!("hash.transaction_commit", tx.commit()).map_err(|e| e.to_string())?;
    }
    if !packed.is_empty() {
        conn.execute(
            "INSERT INTO hash_blocks VALUES(?1,?2)",
            params![(count / HASH_BLOCK_FILES) as i64, packed],
        )
        .map_err(|e| e.to_string())?;
    }
    drop(packed);
    drop(reader);
    #[cfg(test)]
    drop(hashes_phase);
    #[cfg(test)]
    let force_scan =
        TEST_SCAN_ONLY.get() || std::env::var_os("ROOM237_FORCE_DUPLICATE_SCAN").is_some();
    #[cfg(not(test))]
    let force_scan = false;
    let first_hash: Option<Vec<u8>> = conn
        .query_row(
            "SELECT hash FROM nodes WHERE hash IS NOT NULL ORDER BY hash LIMIT 1",
            [],
            |r| r.get(0),
        )
        .optional()
        .map_err(|e| e.to_string())?;
    let last_hash: Option<Vec<u8>> = conn
        .query_row(
            "SELECT hash FROM nodes WHERE hash IS NOT NULL ORDER BY hash DESC LIMIT 1",
            [],
            |r| r.get(0),
        )
        .optional()
        .map_err(|e| e.to_string())?;
    let mut mih = if let Some(plan) = (!force_scan && first_hash != last_hash)
        .then(|| mih::Plan::choose(count, cfg.bits as usize, cfg.effective_threshold))
        .flatten()
    {
        Some(measured!(
            "phase.mih_index_build",
            mih::Index::build(&mut conn, plan, count, words, || check(request))
        )?)
    } else {
        None
    };
    #[cfg(test)]
    let matching_phase = profile::span("phase.match_hashes");
    let mut cache = if mih.is_some() {
        HashBlocks::with_page_size(words, CANDIDATE_HASH_PAGE)
    } else {
        HashBlocks::new(words)
    };
    let mut next = 0usize;
    let mut comparisons = 0i64;
    let mut exact_folds = 0i64;
    loop {
        check(request)?;
        let names: Vec<(usize, String)> = {
            let mut stmt = conn
                .prepare_cached("SELECT id,name FROM hash_names WHERE id>=?1 ORDER BY id LIMIT 128")
                .map_err(|e| e.to_string())?;
            let rows = stmt
                .query_map([next as i64], |r| {
                    Ok((r.get::<_, i64>(0)? as usize, r.get(1)?))
                })
                .map_err(|e| e.to_string())?;
            rows.collect::<Result<_, _>>().map_err(|e| e.to_string())?
        };
        if names.is_empty() {
            break;
        }
        let tx = conn.transaction().map_err(|e| e.to_string())?;
        for (id, name) in names {
            check(request)?;
            next = id + 1;
            let block = id / cache.files;
            let position = (id % cache.files) * words;
            let mut current = [0u64; 16];
            current[..words].copy_from_slice(
                &measured!("match.hash_block_access", cache.get(&tx, block))?
                    [position..position + words],
            );
            let bits = &current[..words];
            let hash = bytes(bits);
            #[cfg(test)]
            let exact_scope = profile::span("match.exact_lookup_and_visited_write");
            let exact: Option<String> = if have_ignores {
                tx.prepare_cached(
                    "SELECT n.name FROM nodes n JOIN components c ON c.root=n.root
                    WHERE n.hash=?1 AND n.visited=1 AND c.restricted=0
                    AND (SELECT restricted FROM components WHERE root=?2)=0 LIMIT 1",
                )
                .map_err(|e| e.to_string())?
                .query_row(params![hash, name], |r| r.get(0))
                .optional()
                .map_err(|e| e.to_string())?
            } else {
                tx.prepare_cached("SELECT name FROM nodes WHERE hash=?1 AND visited=1 LIMIT 1")
                    .map_err(|e| e.to_string())?
                    .query_row([&hash], |r| r.get(0))
                    .optional()
                    .map_err(|e| e.to_string())?
            };
            tx.prepare_cached("UPDATE nodes SET visited=1 WHERE name=?1")
                .map_err(|e| e.to_string())?
                .execute([&name])
                .map_err(|e| e.to_string())?;
            #[cfg(test)]
            drop(exact_scope);
            if let Some(exact) = exact {
                measured!("match.merge_sql", merge(&tx, &name, &exact))?;
                exact_folds += 1;
                continue;
            }
            // At a zero threshold the full-hash index is already exact; do not
            // run a quadratic distance scan over unrelated hashes.
            if cfg.effective_threshold == 0 {
                let mut previous = String::new();
                loop {
                    check(request)?;
                    let matches: Vec<String> = {
                        let mut stmt=tx.prepare_cached("SELECT name FROM nodes WHERE hash=?1 AND visited=1 AND name<?2 AND name>?3 ORDER BY name LIMIT 256").map_err(|e|e.to_string())?;
                        let rows = stmt
                            .query_map(params![hash, name, previous], |r| r.get(0))
                            .map_err(|e| e.to_string())?;
                        rows.collect::<Result<_, _>>().map_err(|e| e.to_string())?
                    };
                    if matches.is_empty() {
                        break;
                    }
                    for other in matches {
                        previous = other.clone();
                        merge(&tx, &name, &other)?;
                    }
                }
                continue;
            }
            if let Some(index) = mih.as_mut() {
                if let Some(candidates) = measured!(
                    "match.mih_query",
                    index.query(&tx, bits, id as u64, || check(request))
                )? {
                    match candidates {
                        mih::Candidates::Memory(ids) => {
                            for ids in ids.chunks(256) {
                                verify_candidates(
                                    &tx,
                                    &mut cache,
                                    bits,
                                    &name,
                                    ids,
                                    cfg.effective_threshold,
                                    request,
                                    &mut comparisons,
                                )?;
                            }
                        }
                        mih::Candidates::Disk => {
                            let mut after = None;
                            loop {
                                check(request)?;
                                let ids = mih::Candidates::page(&tx, after)?;
                                if ids.is_empty() {
                                    break;
                                }
                                after = ids.last().copied();
                                verify_candidates(
                                    &tx,
                                    &mut cache,
                                    bits,
                                    &name,
                                    &ids,
                                    cfg.effective_threshold,
                                    request,
                                    &mut comparisons,
                                )?;
                            }
                        }
                    }
                    continue;
                }
            }
            for previous_block in 0..=block {
                check(request)?;
                let files = cache.files;
                let hashes = measured!("match.hash_block_access", cache.get(&tx, previous_block))?;
                let limit = if previous_block == block {
                    id % files
                } else {
                    hashes.len() / words
                };
                #[cfg(test)]
                let compare_loop = profile::span("match.distance_scan");
                for (offset, other) in hashes.chunks_exact(words).take(limit).enumerate() {
                    if offset % 256 == 0 {
                        check(request)?;
                    }
                    comparisons += 1;
                    if within_threshold(bits, other, cfg.effective_threshold) {
                        #[cfg(test)]
                        let merge_scope = profile::span("match.merge_sql");
                        let previous = previous_block * files + offset;
                        // SQL is used only for actual matches, never rejections.
                        let other: String = tx
                            .prepare_cached("SELECT name FROM hash_names WHERE id=?1")
                            .map_err(|e| e.to_string())?
                            .query_row([previous as i64], |r| r.get(0))
                            .map_err(|e| e.to_string())?;
                        merge(&tx, &name, &other)?;
                        #[cfg(test)]
                        drop(merge_scope);
                    }
                }
                #[cfg(test)]
                drop(compare_loop);
            }
        }
        measured!("match.transaction_commit", tx.commit()).map_err(|e| e.to_string())?;
    }
    #[cfg(test)]
    drop(matching_phase);
    check(request)?;
    #[cfg(test)]
    let grouping_phase = profile::span("phase.group_summaries");
    conn.execute("INSERT INTO groups SELECT root,count(*),row_number() OVER(ORDER BY root) FROM nodes GROUP BY root HAVING count(*)>1",[]).map_err(|e|e.to_string())?;
    #[cfg(test)]
    drop(grouping_phase);
    // Hashing can create previews; record the final revision after publication.
    let signature = measured!(
        "phase.final_fingerprint",
        inventory(dir, request).and_then(|items| fingerprint(&items, dir, settings))
    )?;
    #[cfg(test)]
    let publish_phase = profile::span("phase.publish");
    conn.execute(
        "INSERT INTO state(signature,comparisons,exact_folds,search_method,mih_probes,mih_fallbacks) VALUES(?1,?2,?3,?4,?5,?6)",
        params![signature,comparisons,exact_folds,if mih.is_some(){"mih"}else{"scan"},i64::try_from(mih.as_ref().map_or(0,|m|m.probes)).map_err(|e|e.to_string())?,i64::try_from(mih.as_ref().map_or(0,|m|m.fallbacks)).map_err(|e|e.to_string())?],
    )
    .map_err(|e| e.to_string())?;
    repair::remember_configuration(&conn, settings)?;
    conn.close().map_err(|(_, e)| e.to_string())?;
    temporary.as_file().sync_all().map_err(|e| e.to_string())?;
    temporary.persist(&path).map_err(|e| e.to_string())?;
    #[cfg(test)]
    drop(publish_phase);
    Ok(path)
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DuplicatePage {
    pub groups: Vec<Vec<String>>,
    pub items: Vec<DetachedMediaEntry>,
    pub total: u64,
    pub page: u64,
    pub page_size: usize,
}

pub(crate) fn page(
    dir: &Path,
    page: u64,
    settings: &AdvancedSettings,
    request: u64,
) -> Result<DuplicatePage, String> {
    let _tree = crate::album_tree::read(&[dir])?;
    let _lease = FileLock::acquire(
        &dir.join(".room237-duplicates.lock"),
        Duration::from_secs(30),
        Duration::from_millis(10),
    )?;
    let path = build(dir, settings, request)?;
    let conn = open(&path)?;
    let total: i64 = conn
        .query_row("SELECT count(*) FROM nodes WHERE root<>name", [], |r| {
            r.get(0)
        })
        .map_err(|e| e.to_string())?;
    let page = page.min((total as u64).saturating_sub(1) / PAGE as u64);
    let mut stmt = conn
        .prepare(
            "SELECT root,name FROM nodes WHERE root<>name ORDER BY root,name LIMIT ?1 OFFSET ?2",
        )
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(params![PAGE as i64, (page * PAGE as u64) as i64], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?))
        })
        .map_err(|e| e.to_string())?;
    let mut groups: Vec<Vec<String>> = Vec::new();
    for row in rows {
        let (root, name) = row.map_err(|e| e.to_string())?;
        if groups
            .last()
            .is_some_and(|group| group.first() == Some(&root))
        {
            groups.last_mut().unwrap().push(name);
        } else {
            groups.push(vec![root, name]);
        }
    }
    let mut items = Vec::new();
    for name in groups.iter().flatten() {
        let path = dir.join(name);
        let entry = crate::metadata::read_file_entry(&path)?.unwrap_or_default();
        let meta = entry.meta.unwrap_or_else(|| {
            crate::metadata::DetachedFileMeta {
                a: None,
                s: None,
                i: true,
                v: false,
                w: None,
                h: None,
            }
            .pack()
        });
        items.push(DetachedMediaEntry {
            blur_preview: entry.blur_preview,
            revision: crate::metadata::media_revision(&path),
            thumbnail_name: crate::thumb::detached_thumbnail_name(&path),
            name: name.clone(),
            meta,
            favorite: Some(entry.favorite),
        });
    }
    Ok(DuplicatePage {
        groups,
        items,
        total: total as u64,
        page,
        page_size: PAGE,
    })
}

pub(crate) async fn legacy_page(dir: String) -> Result<DuplicatePage, String> {
    tokio::task::spawn_blocking(move || {
        let _scan = SCAN.lock().unwrap();
        let request = REQUEST.load(Ordering::SeqCst);
        page(
            Path::new(&dir),
            0,
            &crate::settings::read_settings(),
            request,
        )
    })
    .await
    .map_err(|e| e.to_string())?
}

pub async fn find_duplicate_page(dir: String, page_index: u64) -> Result<DuplicatePage, String> {
    let request = REQUEST.fetch_add(1, Ordering::SeqCst) + 1;
    tokio::task::spawn_blocking(move || {
        let _scan = SCAN.lock().unwrap();
        check(request)?;
        page(
            Path::new(&dir),
            page_index,
            &crate::settings::read_settings(),
            request,
        )
    })
    .await
    .map_err(|e| e.to_string())?
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::Engine;
    #[test]
    fn giant_group_is_paged_with_a_stable_anchor_and_reuses_disk_index() {
        let _scan = SCAN.lock().unwrap();
        let request = REQUEST.load(Ordering::SeqCst);
        let dir = tempfile::tempdir().unwrap();
        let settings = crate::settings::read_settings();
        let cfg = settings.hash_config();
        let mut album = crate::metadata::AlbumMeta::default();
        for n in 0..300 {
            let name = format!("{n:05}.jpg");
            let path = dir.path().join(&name);
            fs::write(&path, b"cached test source").unwrap();
            album.files.insert(
                name,
                crate::metadata::FileMetaEntry {
                    hash: Some(base64::engine::general_purpose::STANDARD.encode(vec![
                        0;
                        (cfg.bits / 8)
                            as usize
                    ])),
                    hash_bits: Some(cfg.bits),
                    hash_version: Some(cfg.hash_version.clone()),
                    hash_source_revision: Some(crate::metadata::media_revision(&path)),
                    ..Default::default()
                },
            );
        }
        let service = dir.path().join(".room237-metadata");
        fs::create_dir(&service).unwrap();
        crate::album_db::initialize(&service.join("album.sqlite"), &album).unwrap();
        let first = page(dir.path(), 0, &settings, request).unwrap();
        let index = service.join("duplicates.sqlite");
        let revision = crate::metadata::source_revision(&index);
        let second = page(dir.path(), 1, &settings, request).unwrap();
        let last = page(dir.path(), u64::MAX, &settings, request).unwrap();
        assert_eq!(first.total, 299);
        assert_eq!(first.items.len(), 129);
        assert_eq!(second.items.len(), 129);
        assert_eq!(last.items.len(), 44);
        assert_eq!(first.groups[0][0], second.groups[0][0]);
        assert!(!first.groups[0].contains(&second.groups[0][1]));
        assert_eq!(revision, crate::metadata::source_revision(&index));
    }
}

#[derive(Serialize)]
pub struct DuplicateGroup {
    root: String,
    count: u64,
    items: Vec<DetachedMediaEntry>,
}
#[derive(Serialize)]
pub struct DuplicateGroups {
    groups: Vec<DuplicateGroup>,
    total: u64,
    #[serde(rename = "filesTotal")]
    files_total: u64,
    offset: u64,
    signature: String,
}
fn checked_index(dir: &Path, signature: &str) -> Result<Connection, String> {
    let conn = open(&dir.join(".room237-metadata/duplicates.sqlite"))?;
    let current: String = conn
        .query_row("SELECT signature FROM state", [], |r| r.get(0))
        .map_err(|e| e.to_string())?;
    if current != signature {
        return Err("Duplicate index changed; refresh results".into());
    }
    Ok(conn)
}
fn duplicate_entry(
    dir: &Path,
    name: String,
    revision: String,
    reader: &mut crate::album_db::FileReader,
) -> Result<DetachedMediaEntry, String> {
    let entry = reader.get(&name)?.unwrap_or_default();
    let path = dir.join(&name);
    Ok(DetachedMediaEntry {
        blur_preview: entry.blur_preview,
        name,
        revision,
        thumbnail_name: crate::thumb::detached_thumbnail_name(&path),
        meta: entry.meta.unwrap_or_else(|| {
            crate::metadata::DetachedFileMeta {
                a: None,
                s: None,
                i: true,
                v: false,
                w: None,
                h: None,
            }
            .pack()
        }),
        favorite: Some(entry.favorite),
    })
}
fn group_members(
    conn: &Connection,
    dir: &Path,
    root: &str,
    offset: u64,
    limit: i64,
) -> Result<Vec<DetachedMediaEntry>, String> {
    let mut reader = crate::album_db::FileReader::new(&dir.join(".room237-metadata/album.sqlite"))?;
    let mut stmt = conn
        .prepare("SELECT name,revision FROM nodes WHERE root=?1 ORDER BY name LIMIT ?2 OFFSET ?3")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map(
            params![
                root,
                limit,
                i64::try_from(offset).map_err(|e| e.to_string())?
            ],
            |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)),
        )
        .map_err(|e| e.to_string())?;
    let mut items = Vec::new();
    for row in rows {
        let (name, revision) = row.map_err(|e| e.to_string())?;
        items.push(duplicate_entry(dir, name, revision, &mut reader)?);
    }
    Ok(items)
}
fn read_groups(conn: &Connection, dir: &Path, offset: u64) -> Result<DuplicateGroups, String> {
    let signature: String = conn
        .query_row("SELECT signature FROM state", [], |r| r.get(0))
        .map_err(|e| e.to_string())?;
    let total: i64 = conn
        .query_row("SELECT coalesce(max(ordinal),0) FROM groups", [], |r| {
            r.get(0)
        })
        .map_err(|e| e.to_string())?;
    let files_total: i64 = conn
        .query_row("SELECT coalesce(sum(count),0) FROM groups", [], |r| {
            r.get(0)
        })
        .map_err(|e| e.to_string())?;
    let offset = offset.min((total as u64).saturating_sub(1));
    let mut stmt = conn
        .prepare("SELECT root,count FROM groups WHERE ordinal>?1 ORDER BY ordinal LIMIT 64")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map([i64::try_from(offset).map_err(|e| e.to_string())?], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?))
        })
        .map_err(|e| e.to_string())?;
    let mut groups = Vec::new();
    for row in rows {
        let (root, count) = row.map_err(|e| e.to_string())?;
        let items = group_members(&conn, dir, &root, 0, 6)?;
        groups.push(DuplicateGroup {
            root,
            count: count as u64,
            items,
        });
    }
    Ok(DuplicateGroups {
        groups,
        total: total as u64,
        files_total: files_total as u64,
        offset,
        signature,
    })
}
pub async fn find_duplicate_groups(
    dir: String,
    offset: u64,
    signature: Option<String>,
) -> Result<DuplicateGroups, String> {
    let request = if signature.is_none() {
        REQUEST.fetch_add(1, Ordering::SeqCst) + 1
    } else {
        REQUEST.load(Ordering::SeqCst)
    };
    tokio::task::spawn_blocking(move || {
        let _scan = SCAN.lock().unwrap();
        let dir = Path::new(&dir);
        let _tree = crate::album_tree::read(&[dir])?;
        let _index_lease = FileLock::acquire(
            &dir.join(".room237-duplicates.lock"),
            Duration::from_secs(1800),
            Duration::from_millis(20),
        )?;
        let conn = if let Some(signature) = signature {
            checked_index(dir, &signature)?
        } else {
            open(&build(dir, &crate::settings::read_settings(), request)?)?
        };
        read_groups(&conn, dir, offset)
    })
    .await
    .map_err(|e| e.to_string())?
}
pub async fn duplicate_group_members(
    dir: String,
    root: String,
    signature: String,
    offset: u64,
) -> Result<Vec<DetachedMediaEntry>, String> {
    tokio::task::spawn_blocking(move || {
        let dir = Path::new(&dir);
        let _tree = crate::album_tree::read(&[dir])?;
        let conn = checked_index(dir, &signature)?;
        group_members(&conn, dir, &root, offset, 256)
    })
    .await
    .map_err(|e| e.to_string())?
}
pub async fn mark_duplicate_group_different(
    dir: String,
    root: String,
    signature: String,
    offset: Option<u64>,
) -> Result<DuplicateGroups, String> {
    tokio::task::spawn_blocking(move || {
        let _scan = SCAN.lock().unwrap();
        let request = REQUEST.load(Ordering::SeqCst);
        let dir = Path::new(&dir);
        let _tree = crate::album_tree::read(&[dir])?;
        let _index_lease = FileLock::acquire(
            &dir.join(".room237-duplicates.lock"),
            Duration::from_secs(1800),
            Duration::from_millis(20),
        )?;
        let _guard = crate::metadata::lock_metadata_for_write(&[dir])?;
        let mut conn = checked_index(dir, &signature)?;
        conn.execute(
            "ATTACH DATABASE ?1 AS album",
            [dir.join(".room237-metadata/album.sqlite")
                .to_string_lossy()
                .as_ref()],
        )
        .map_err(|e| e.to_string())?;
        let settings = crate::settings::read_settings();
        let before = source_fingerprint(&inventory(dir, request)?, &settings);
        let tx = conn
            .transaction_with_behavior(rusqlite::TransactionBehavior::Immediate)
            .map_err(|e| e.to_string())?;
        if finish_fingerprint(before.clone(), &tx, "album.ignored_pairs")? != signature {
            return Err(
                "Album changed; refresh duplicate results before marking this group".into(),
            );
        }
        let ordinal: i64 = tx
            .query_row("SELECT ordinal FROM groups WHERE root=?1", [&root], |r| {
                r.get(0)
            })
            .map_err(|e| e.to_string())?;
        // Persist the user's decision and remove only its connected component.
        // Cross-database rollback journals make both changes commit together;
        // existing groups, hashes and the candidate index remain untouched.
        tx.execute(
            "INSERT OR IGNORE INTO album.ignored_pairs
             SELECT a.name,b.name FROM nodes a JOIN nodes b
             ON b.root=a.root AND a.name<b.name WHERE a.root=?1",
            [&root],
        )
        .map_err(|e| e.to_string())?;
        tx.execute(
            "INSERT OR IGNORE INTO ignored SELECT a,b FROM album.ignored_pairs",
            [],
        )
        .map_err(|e| e.to_string())?;
        tx.execute("UPDATE nodes SET root=name WHERE root=?1", [&root])
            .map_err(|e| e.to_string())?;
        tx.execute("DELETE FROM groups WHERE root=?1", [&root])
            .map_err(|e| e.to_string())?;
        // Avoid UNIQUE conflicts regardless of SQLite's UPDATE traversal order.
        tx.execute(
            "UPDATE groups SET ordinal=-ordinal WHERE ordinal>?1",
            [ordinal],
        )
        .map_err(|e| e.to_string())?;
        tx.execute("UPDATE groups SET ordinal=-ordinal-1 WHERE ordinal<0", [])
            .map_err(|e| e.to_string())?;
        // The stamps were taken under the metadata lease and the transaction only
        // edits the index; a file that changes meanwhile fails the next signature check.
        let next = finish_fingerprint(before, &tx, "album.ignored_pairs")?;
        tx.execute("UPDATE state SET signature=?1", [next])
            .map_err(|e| e.to_string())?;
        tx.commit().map_err(|e| e.to_string())?;
        read_groups(&conn, dir, offset.unwrap_or(0))
    })
    .await
    .map_err(|e| e.to_string())?
}
pub async fn delete_duplicate_groups(dir: String, signature: String) -> Result<u64, String> {
    tokio::task::spawn_blocking(move||{
        let _scan=SCAN.lock().unwrap();let dir=Path::new(&dir);let _tree=crate::album_tree::read(&[dir])?;
        let _index_lease=FileLock::acquire(&dir.join(".room237-duplicates.lock"),Duration::from_secs(1800),Duration::from_millis(20))?;
        let conn=checked_index(dir,&signature)?;let mut last_root=String::new();let mut last_name=String::new();let mut deleted=0;
        loop{
            let mut stmt=conn.prepare("SELECT n.root,n.name,n.revision,a.revision FROM nodes n JOIN nodes a ON a.name=n.root WHERE n.root<>n.name AND (n.root>?1 OR (n.root=?1 AND n.name>?2)) ORDER BY n.root,n.name LIMIT 128").map_err(|e|e.to_string())?;
            let rows=stmt.query_map(params![last_root,last_name],|r|Ok((r.get::<_,String>(0)?,r.get::<_,String>(1)?,r.get::<_,String>(2)?,r.get::<_,String>(3)?))).map_err(|e|e.to_string())?;
            let mut targets=Vec::new();let mut anchors=std::collections::HashMap::new();
            for row in rows{let(root,name,revision,anchor)=row.map_err(|e|e.to_string())?;last_root=root.clone();last_name=name.clone();targets.push(crate::media_delete::DeleteTarget{name,revision:Some(revision)});anchors.insert(root,anchor);}
            drop(stmt);if targets.is_empty(){break;}
            let observed=anchors.into_iter().map(|(name,revision)|crate::metadata::ObservedMedia{path:dir.join(name).to_string_lossy().into_owned(),revision}).collect();
            let result=crate::media_delete::delete_media_batch(dir.to_string_lossy().into_owned(),targets,observed)?;
            deleted+=result.deleted.len() as u64;if let Some(error)=result.error{return Err(format!("Deleted {deleted} files; {error}"));}
        }
        Ok(deleted)
    }).await.map_err(|e|e.to_string())?
}

#[cfg(test)]
mod streaming_group_tests {
    use super::*;
    use base64::Engine;
    async fn fixture() -> (tempfile::TempDir, String) {
        let dir = tempfile::tempdir().unwrap();
        let cfg = crate::settings::read_settings().hash_config();
        crate::metadata::update_album_meta(dir.path(), |album| {
            for n in 0..300 {
                let name = format!("{n:05}.jpg");
                let path = dir.path().join(&name);
                fs::write(&path, b"cached source").unwrap();
                album.files.insert(
                    name,
                    crate::metadata::FileMetaEntry {
                        hash: Some(base64::engine::general_purpose::STANDARD.encode(vec![
                                0;
                                (cfg.bits / 8)
                                    as usize
                            ])),
                        hash_bits: Some(cfg.bits),
                        hash_version: Some(cfg.hash_version.clone()),
                        hash_source_revision: Some(crate::metadata::media_revision(&path)),
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
        legacy_page(dir.path().to_string_lossy().into())
            .await
            .unwrap();
        let conn = open(&dir.path().join(".room237-metadata/duplicates.sqlite")).unwrap();
        let signature = conn
            .query_row("SELECT signature FROM state", [], |r| r.get::<_, String>(0))
            .unwrap();
        (dir, signature)
    }
    #[tokio::test]
    async fn summaries_do_not_load_a_giant_group_and_bulk_delete_retains_its_anchor() {
        let (dir, signature) = fixture().await;
        let path = dir.path().to_string_lossy().into_owned();
        let result = find_duplicate_groups(path.clone(), 0, Some(signature.clone()))
            .await
            .unwrap();
        assert_eq!(result.total, 1);
        assert_eq!(result.groups[0].count, 300);
        assert_eq!(result.groups[0].items.len(), 6);
        let members =
            duplicate_group_members(path.clone(), "00000.jpg".into(), signature.clone(), 200)
                .await
                .unwrap();
        assert_eq!(members.len(), 100);
        assert_eq!(members[0].name, "00200.jpg");
        assert_eq!(delete_duplicate_groups(path, signature).await.unwrap(), 299);
        assert!(dir.path().join("00000.jpg").is_file());
        assert!(!dir.path().join("00299.jpg").exists());
    }
    #[tokio::test]
    async fn bulk_delete_rejects_a_changed_source_before_touching_the_group() {
        let (dir, signature) = fixture().await;
        fs::write(dir.path().join("00001.jpg"), b"replacement").unwrap();
        assert!(
            delete_duplicate_groups(dir.path().to_string_lossy().into(), signature)
                .await
                .is_err()
        );
        assert_eq!(
            fs::read(dir.path().join("00001.jpg")).unwrap(),
            b"replacement"
        );
        assert!(dir.path().join("00299.jpg").exists());
    }
}

#[cfg(test)]
mod performance_tests {
    use super::*;
    use base64::Engine;
    #[test]
    #[ignore = "synthetic cached-hash duplicate benchmark; no application or media decoder"]
    fn benchmark_5882_cached_photos() {
        let count = std::env::var("ROOM237_DUPLICATE_BENCH_COUNT")
            .ok()
            .map(|v| v.parse::<usize>().unwrap())
            .unwrap_or(5882);
        assert!(count >= 1024);
        let _scan = SCAN.lock().unwrap();
        let request = REQUEST.load(Ordering::SeqCst);
        let settings = crate::settings::read_settings();
        let cfg = settings.hash_config();
        let dir = tempfile::tempdir().unwrap();
        let mut state = 237u64;
        let mut previous = Vec::new();
        crate::metadata::update_album_meta(dir.path(), |album| {
            album
                .duplicates_ignore
                .push(["00000.jpg".into(), "00001.jpg".into()]);
            for n in 0..count {
                let mut bits = Vec::new();
                for _ in 0..cfg.bits / 64 {
                    state = state.wrapping_add(0x9e3779b97f4a7c15);
                    let mut z = state;
                    z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
                    z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
                    bits.push(z ^ (z >> 31));
                }
                if n % 97 == 96 {
                    bits = previous.clone();
                }
                previous = bits.clone();
                let name = format!("{n:05}.jpg");
                let file = dir.path().join(&name);
                fs::write(&file, b"cached fixture").unwrap();
                album.files.insert(
                    name,
                    crate::metadata::FileMetaEntry {
                        hash: Some(base64::engine::general_purpose::STANDARD.encode(bytes(&bits))),
                        hash_bits: Some(cfg.bits),
                        hash_version: Some(cfg.hash_version.clone()),
                        hash_source_revision: Some(crate::metadata::media_revision(&file)),
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
        let start = std::time::Instant::now();
        let (built, report) = profile::measure(
            std::env::var_os("ROOM237_PROFILE_DUPLICATES").is_some(),
            || build(dir.path(), &settings, request),
        );
        let path = built.unwrap();
        let first = start.elapsed();
        if let Some(report) = report {
            eprintln!("DUPLICATE_PROFILE_JSON={report}");
        }
        let conn = open(&path).unwrap();
        let groups: i64 = conn
            .query_row("SELECT count(*) FROM groups", [], |r| r.get(0))
            .unwrap();
        assert_eq!(groups, count as i64 / 97);
        let (comparisons, folded): (i64, i64) = conn
            .query_row("SELECT comparisons,exact_folds FROM state", [], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .unwrap();
        let (method, probes, fallbacks): (String, i64, i64) = conn
            .query_row(
                "SELECT search_method,mih_probes,mih_fallbacks FROM state",
                [],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            )
            .unwrap();
        eprintln!("method={method}; hash comparisons={comparisons}; exact folds={folded}; mih probes={probes}; fallbacks={fallbacks}");
        drop(conn);
        let revision = crate::metadata::source_revision(&path);
        let start = std::time::Instant::now();
        build(dir.path(), &settings, request).unwrap();
        let reused = start.elapsed();
        assert_eq!(revision, crate::metadata::source_revision(&path));
        let conn = open(&path).unwrap();
        let signature: String = conn
            .query_row("SELECT signature FROM state", [], |r| r.get(0))
            .unwrap();
        let root: String = conn
            .query_row("SELECT root FROM groups ORDER BY root LIMIT 1", [], |r| {
                r.get(0)
            })
            .unwrap();
        drop(conn);
        drop(_scan);
        let start = std::time::Instant::now();
        let reviewed = tokio::runtime::Runtime::new()
            .unwrap()
            .block_on(mark_duplicate_group_different(
                dir.path().to_string_lossy().into(),
                root,
                signature,
                Some(0),
            ))
            .unwrap();
        let marked = start.elapsed();
        assert_eq!(reviewed.total, count as u64 / 97 - 1);
        let _scan = SCAN.lock().unwrap();
        let stamp = crate::metadata::source_revision(&path);
        let start = std::time::Instant::now();
        build(dir.path(), &settings, REQUEST.load(Ordering::SeqCst)).unwrap();
        let reuse_after_mark = start.elapsed();
        assert_eq!(stamp, crate::metadata::source_revision(&path));
        let doomed: String = open(&path)
            .unwrap()
            .query_row(
                "SELECT name FROM nodes WHERE root<>name ORDER BY name DESC LIMIT 1",
                [],
                |r| r.get(0),
            )
            .unwrap();
        fs::remove_file(dir.path().join(doomed)).unwrap();
        let start = std::time::Instant::now();
        build(dir.path(), &settings, REQUEST.load(Ordering::SeqCst)).unwrap();
        let deletion = start.elapsed();
        let conn = open(&path).unwrap();
        assert_eq!(
            conn.query_row("SELECT comparisons FROM state", [], |r| r.get::<_, i64>(0))
                .unwrap(),
            comparisons
        );
        assert_eq!(
            read_groups(&conn, dir.path(), 0).unwrap().total,
            count as u64 / 97 - 2
        );
        eprintln!("deletion-only repair={deletion:?}; no new album-wide comparisons");
        eprintln!("{count} cached photos (duplicate pairs every 97 files, one unrelated exclusion): build={first:?}; reuse={reused:?}; mark={marked:?}; reuse_after_mark={reuse_after_mark:?}");
    }
}

#[cfg(test)]
mod review_tests {
    use super::*;
    use base64::Engine;

    fn cached_album(hashes: Vec<Vec<u64>>, ignored: Vec<[String; 2]>) -> tempfile::TempDir {
        cached_album_with_settings(hashes, ignored, &crate::settings::read_settings())
    }
    fn cached_album_with_settings(
        hashes: Vec<Vec<u64>>,
        ignored: Vec<[String; 2]>,
        settings: &AdvancedSettings,
    ) -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        let cfg = settings.hash_config();
        crate::metadata::update_album_meta(dir.path(), |album| {
            album.duplicates_ignore = ignored;
            for (n, bits) in hashes.iter().enumerate() {
                let name = format!("{n:05}.jpg");
                let file = dir.path().join(&name);
                fs::write(&file, b"cached fixture").unwrap();
                album.files.insert(
                    name,
                    crate::metadata::FileMetaEntry {
                        hash: Some(base64::engine::general_purpose::STANDARD.encode(bytes(bits))),
                        hash_bits: Some(cfg.bits),
                        hash_version: Some(cfg.hash_version.clone()),
                        hash_source_revision: Some(crate::metadata::media_revision(&file)),
                        ..Default::default()
                    },
                );
            }
        })
        .unwrap();
        dir
    }
    fn initial() -> (tempfile::TempDir, String) {
        let _scan = SCAN.lock().unwrap();
        let dir = cached_album(
            vec![
                vec![0; 4],
                vec![0; 4],
                vec![0; 4],
                vec![u64::MAX; 4],
                vec![u64::MAX; 4],
                vec![0xaaaaaaaaaaaaaaaa; 4],
                vec![0xaaaaaaaaaaaaaaaa; 4],
            ],
            vec![],
        );
        let path = build(
            dir.path(),
            &crate::settings::read_settings(),
            REQUEST.load(Ordering::SeqCst),
        )
        .unwrap();
        let conn = open(&path).unwrap();
        conn.execute_batch(
            "CREATE TABLE reuse_sentinel(value INTEGER); INSERT INTO reuse_sentinel VALUES(237);",
        )
        .unwrap();
        let signature = conn
            .query_row("SELECT signature FROM state", [], |r| r.get(0))
            .unwrap();
        (dir, signature)
    }
    fn signature(dir: &Path) -> String {
        open(&dir.join(".room237-metadata/duplicates.sqlite"))
            .unwrap()
            .query_row("SELECT signature FROM state", [], |r| r.get(0))
            .unwrap()
    }
    fn ignored_count(dir: &Path) -> i64 {
        open(&dir.join(".room237-metadata/album.sqlite"))
            .unwrap()
            .query_row("SELECT count(*) FROM ignored_pairs", [], |r| r.get(0))
            .unwrap()
    }

    #[test]
    fn regenerated_previews_do_not_force_a_rebuild_after_a_deletion() {
        let _scan = SCAN.lock().unwrap();
        let request = REQUEST.load(Ordering::SeqCst);
        let settings = crate::settings::read_settings();
        let dir = cached_album_with_settings(
            vec![vec![0; 4], vec![0; 4], vec![0; 4], vec![u64::MAX; 4]],
            vec![],
            &settings,
        );
        let index = build(dir.path(), &settings, request).unwrap();
        let conn = open(&index).unwrap();
        conn.execute_batch(
            "CREATE TABLE rebuild_sentinel(value); INSERT INTO rebuild_sentinel VALUES(1);",
        )
        .unwrap();
        drop(conn);
        // A preview appears for an unrelated file, then another file is deleted.
        let thumbs = dir.path().join(".room237-thumb");
        fs::create_dir_all(&thumbs).unwrap();
        fs::write(thumbs.join("00003.jpg.webp"), b"preview").unwrap();
        fs::remove_file(dir.path().join("00002.jpg")).unwrap();
        build(dir.path(), &settings, request).unwrap();
        let conn = open(&index).unwrap();
        assert_eq!(
            conn.query_row("SELECT value FROM rebuild_sentinel", [], |r| r
                .get::<_, i64>(0))
                .unwrap(),
            1,
            "the index must be repaired in place"
        );
        assert_eq!(read_groups(&conn, dir.path(), 0).unwrap().total, 1);
        // Published stamps follow the new preview so a deletion is not refused.
        let published: String = conn
            .query_row(
                "SELECT revision FROM nodes WHERE name='00003.jpg'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(
            published,
            crate::metadata::media_revision(&dir.path().join("00003.jpg"))
        );
    }

    #[test]
    fn deletion_repairs_only_affected_components_and_splits_a_removed_bridge() {
        let _scan = SCAN.lock().unwrap();
        let request = REQUEST.load(Ordering::SeqCst);
        let mut settings = crate::settings::read_settings();
        settings.duplicates.threshold = 1;
        let dir = cached_album_with_settings(
            vec![
                vec![0; 4],
                vec![1, 0, 0, 0],
                vec![3, 0, 0, 0],
                vec![u64::MAX; 4],
                vec![u64::MAX; 4],
            ],
            vec![],
            &settings,
        );
        let index = build(dir.path(), &settings, request).unwrap();
        let conn = open(&index).unwrap();
        conn.execute_batch(
            "CREATE TABLE deletion_sentinel(value); INSERT INTO deletion_sentinel VALUES(237);",
        )
        .unwrap();
        let comparisons: i64 = conn
            .query_row("SELECT comparisons FROM state", [], |r| r.get(0))
            .unwrap();
        drop(conn);
        fs::remove_file(dir.path().join("00001.jpg")).unwrap();
        build(dir.path(), &settings, request).unwrap();
        let conn = open(&index).unwrap();
        assert_eq!(
            conn.query_row("SELECT value FROM deletion_sentinel", [], |r| r
                .get::<_, i64>(0))
                .unwrap(),
            237
        );
        assert_eq!(
            conn.query_row("SELECT comparisons FROM state", [], |r| r.get::<_, i64>(0))
                .unwrap(),
            comparisons
        );
        let result = read_groups(&conn, dir.path(), 0).unwrap();
        assert_eq!(result.total, 1);
        assert_eq!(result.groups[0].root, "00003.jpg");
        assert_eq!(
            conn.query_row("SELECT root FROM nodes WHERE name='00002.jpg'", [], |r| {
                r.get::<_, String>(0)
            })
            .unwrap(),
            "00002.jpg"
        );
        drop(conn);
        // Deleting a group's anchor promotes the survivor without another search.
        fs::remove_file(dir.path().join("00003.jpg")).unwrap();
        build(dir.path(), &settings, request).unwrap();
        let conn = open(&index).unwrap();
        assert_eq!(read_groups(&conn, dir.path(), 0).unwrap().total, 0);
        assert_eq!(
            conn.query_row("SELECT root FROM nodes WHERE name='00004.jpg'", [], |r| {
                r.get::<_, String>(0)
            })
            .unwrap(),
            "00004.jpg"
        );
        assert!(conn.prepare("SELECT * FROM deletion_sentinel").is_ok());
        let stamp = crate::metadata::source_revision(&index);
        drop(conn);
        build(dir.path(), &settings, request).unwrap();
        assert_eq!(stamp, crate::metadata::source_revision(&index));
    }

    #[test]
    fn deletion_of_an_exact_group_reuses_hashes_with_unrelated_exclusions() {
        let _scan = SCAN.lock().unwrap();
        let request = REQUEST.load(Ordering::SeqCst);
        let settings = crate::settings::read_settings();
        let mut hashes = vec![vec![0; 4]; 300];
        hashes.extend([vec![u64::MAX; 4], vec![u64::MAX; 4]]);
        let dir = cached_album_with_settings(
            hashes,
            vec![["00300.jpg".into(), "00301.jpg".into()]],
            &settings,
        );
        let index = build(dir.path(), &settings, request).unwrap();
        let conn = open(&index).unwrap();
        conn.execute_batch(
            "CREATE TABLE deletion_sentinel(value); INSERT INTO deletion_sentinel VALUES(237);",
        )
        .unwrap();
        drop(conn);
        fs::remove_file(dir.path().join("00000.jpg")).unwrap();
        build(dir.path(), &settings, request).unwrap();
        let conn = open(&index).unwrap();
        let result = read_groups(&conn, dir.path(), 0).unwrap();
        assert_eq!(result.groups[0].root, "00001.jpg");
        assert_eq!(result.groups[0].count, 299);
        assert_eq!(result.files_total, 299);
        assert!(conn.prepare("SELECT * FROM deletion_sentinel").is_ok());
    }

    #[test]
    fn deletion_repairs_match_a_fresh_search_with_and_without_exclusions() {
        let _scan = SCAN.lock().unwrap();
        let request = REQUEST.load(Ordering::SeqCst);
        let mut settings = crate::settings::read_settings();
        settings.duplicates.threshold = 2;
        for seed in 0..8_u64 {
            let hashes = (0..48_u64)
                .map(|n| vec![((n * 13 + seed * 7) % 64), n / 12, 0, 0])
                .collect();
            let ignored = if seed % 2 == 0 {
                vec![]
            } else {
                vec![
                    ["00000.jpg".into(), "00003.jpg".into()],
                    ["00010.jpg".into(), "00011.jpg".into()],
                ]
            };
            let dir = cached_album_with_settings(hashes, ignored, &settings);
            let index = build(dir.path(), &settings, request).unwrap();
            for n in 0..48_u64 {
                if (n + seed) % 7 == 0 {
                    fs::remove_file(dir.path().join(format!("{n:05}.jpg"))).unwrap();
                }
            }
            build(dir.path(), &settings, request).unwrap();
            let memberships = |path: &Path| -> Vec<(String, String)> {
                let conn = open(path).unwrap();
                let mut stmt = conn
                    .prepare("SELECT name,root FROM nodes ORDER BY name")
                    .unwrap();
                stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?)))
                    .unwrap()
                    .collect::<Result<Vec<_>, _>>()
                    .unwrap()
            };
            let repaired = memberships(&index);
            fs::rename(&index, dir.path().join("before-rebuild.sqlite")).unwrap();
            build(dir.path(), &settings, request).unwrap();
            assert_eq!(repaired, memberships(&index), "seed {seed}");
        }
    }

    #[test]
    fn source_replacement_and_settings_changes_cannot_use_deletion_repair() {
        let _scan = SCAN.lock().unwrap();
        let request = REQUEST.load(Ordering::SeqCst);
        let settings = crate::settings::read_settings();
        let dir = cached_album_with_settings(vec![vec![0; 4]; 3], vec![], &settings);
        let index = build(dir.path(), &settings, request).unwrap();
        let mut conn = open(&index).unwrap();
        fs::write(
            dir.path().join("00001.jpg"),
            b"a replacement with different contents",
        )
        .unwrap();
        fs::remove_file(dir.path().join("00002.jpg")).unwrap();
        let items = inventory(dir.path(), request).unwrap();
        let changed = fingerprint(&items, dir.path(), &settings).unwrap();
        assert!(!repair::prune_deletions(
            dir.path(),
            &mut conn,
            &settings,
            &changed,
            &items,
            request
        )
        .unwrap());
        let mut other_settings = settings.clone();
        other_settings.duplicates.threshold += 1;
        assert!(!repair::prune_deletions(
            dir.path(),
            &mut conn,
            &other_settings,
            &changed,
            &items,
            request
        )
        .unwrap());
        assert_eq!(
            conn.query_row("SELECT count(*) FROM nodes", [], |r| r.get::<_, i64>(0))
                .unwrap(),
            3
        );
    }

    #[tokio::test]
    async fn review_preserves_other_groups_reuses_the_index_and_delete_all_keeps_reviewed_files() {
        let (dir, old) = initial();
        let root = dir.path().to_string_lossy().into_owned();
        // This also exercises a cached v3 index from the installed version,
        // which did not have the build-time components/restricted tables.
        open(&dir.path().join(".room237-metadata/duplicates.sqlite"))
            .unwrap()
            .execute_batch("DROP TABLE components; DROP TABLE restricted;")
            .unwrap();
        let reviewed =
            mark_duplicate_group_different(root.clone(), "00000.jpg".into(), old.clone(), Some(0))
                .await
                .unwrap();
        assert_eq!(reviewed.total, 2);
        assert_eq!(reviewed.files_total, 4);
        assert_ne!(reviewed.signature, old);
        assert_eq!(
            reviewed
                .groups
                .iter()
                .map(|g| g.root.as_str())
                .collect::<Vec<_>>(),
            ["00003.jpg", "00005.jpg"]
        );
        assert_eq!(ignored_count(dir.path()), 3);
        assert!(delete_duplicate_groups(root.clone(), old).await.is_err());
        let index = dir.path().join(".room237-metadata/duplicates.sqlite");
        let stamp = crate::metadata::source_revision(&index);
        {
            let _scan = SCAN.lock().unwrap();
            build(
                dir.path(),
                &crate::settings::read_settings(),
                REQUEST.load(Ordering::SeqCst),
            )
            .unwrap();
        }
        assert_eq!(stamp, crate::metadata::source_revision(&index));
        assert_eq!(
            open(&index)
                .unwrap()
                .query_row("SELECT value FROM reuse_sentinel", [], |r| r
                    .get::<_, i64>(0))
                .unwrap(),
            237
        );
        assert_eq!(
            delete_duplicate_groups(root, reviewed.signature)
                .await
                .unwrap(),
            2
        );
        for name in [
            "00000.jpg",
            "00001.jpg",
            "00002.jpg",
            "00003.jpg",
            "00005.jpg",
        ] {
            assert!(dir.path().join(name).is_file(), "lost {name}");
        }
        for name in ["00004.jpg", "00006.jpg"] {
            assert!(!dir.path().join(name).exists());
        }
    }
    #[tokio::test]
    async fn ignored_group_survives_loss_and_rebuild_of_the_derived_index() {
        let (dir, old) = initial();
        mark_duplicate_group_different(
            dir.path().to_string_lossy().into(),
            "00000.jpg".into(),
            old,
            None,
        )
        .await
        .unwrap();
        assert_eq!(ignored_count(dir.path()), 3);
        // The authoritative exclusions live in album.sqlite, independently of
        // the UI and the disposable duplicate-search cache.
        std::fs::remove_file(dir.path().join(".room237-metadata/duplicates.sqlite")).unwrap();
        let _scan = SCAN.lock().unwrap();
        let path = build(
            dir.path(),
            &crate::settings::read_settings(),
            REQUEST.load(Ordering::SeqCst),
        )
        .unwrap();
        let groups = read_groups(&open(&path).unwrap(), dir.path(), 0).unwrap();
        assert_eq!(groups.total, 2);
        assert_eq!(
            groups
                .groups
                .iter()
                .map(|g| g.root.as_str())
                .collect::<Vec<_>>(),
            ["00003.jpg", "00005.jpg"]
        );
        assert_eq!(ignored_count(dir.path()), 3);
    }

    #[tokio::test]
    async fn repeated_marks_keep_dense_ordinals_and_an_empty_result_reusable() {
        let (dir, mut current) = initial();
        let root = dir.path().to_string_lossy().into_owned();
        for (name, total) in [("00003.jpg", 2), ("00005.jpg", 1), ("00000.jpg", 0)] {
            let result =
                mark_duplicate_group_different(root.clone(), name.into(), current, Some(999))
                    .await
                    .unwrap();
            current = result.signature;
            assert_eq!(result.total, total);
            assert!(result.offset < total || total == 0);
            let conn = open(&dir.path().join(".room237-metadata/duplicates.sqlite")).unwrap();
            let ordinals: Vec<i64> = conn
                .prepare("SELECT ordinal FROM groups ORDER BY ordinal")
                .unwrap()
                .query_map([], |r| r.get(0))
                .unwrap()
                .collect::<Result<_, _>>()
                .unwrap();
            assert_eq!(ordinals, (1..=total as i64).collect::<Vec<_>>());
        }
        let _scan = SCAN.lock().unwrap();
        let path = build(
            dir.path(),
            &crate::settings::read_settings(),
            REQUEST.load(Ordering::SeqCst),
        )
        .unwrap();
        assert_eq!(
            read_groups(&open(&path).unwrap(), dir.path(), 0)
                .unwrap()
                .total,
            0
        );
    }
    #[tokio::test]
    async fn failed_index_update_rolls_back_the_persisted_user_decision() {
        let (dir, old) = initial();
        open(&dir.path().join(".room237-metadata/duplicates.sqlite")).unwrap().execute_batch("CREATE TRIGGER fail_review BEFORE DELETE ON groups BEGIN SELECT RAISE(ABORT,'injected write failure'); END;").unwrap();
        let result = mark_duplicate_group_different(
            dir.path().to_string_lossy().into(),
            "00000.jpg".into(),
            old.clone(),
            None,
        )
        .await;
        assert!(result.err().unwrap().contains("injected write failure"));
        assert_eq!(ignored_count(dir.path()), 0);
        assert_eq!(signature(dir.path()), old);
        let conn = open(&dir.path().join(".room237-metadata/duplicates.sqlite")).unwrap();
        assert_eq!(
            conn.query_row(
                "SELECT count(*) FROM nodes WHERE root='00000.jpg'",
                [],
                |r| r.get::<_, i64>(0)
            )
            .unwrap(),
            3
        );
    }
    #[tokio::test]
    async fn changed_sources_cannot_be_blessed_as_a_current_index_by_marking_a_group() {
        let (dir, old) = initial();
        fs::write(
            dir.path().join("00006.jpg"),
            b"replacement of another group",
        )
        .unwrap();
        assert!(mark_duplicate_group_different(
            dir.path().to_string_lossy().into(),
            "00000.jpg".into(),
            old.clone(),
            None
        )
        .await
        .is_err());
        assert_eq!(ignored_count(dir.path()), 0);
        assert_eq!(signature(dir.path()), old);
    }
    #[test]
    fn mih_and_scan_produce_identical_groups_with_transitive_ignored_constraints() {
        let _scan = SCAN.lock().unwrap();
        let settings = crate::settings::read_settings();
        let mut seed = 237u64;
        let bases: Vec<Vec<u64>> = (0..60)
            .map(|_| {
                (0..4)
                    .map(|_| {
                        seed = seed.wrapping_add(0x9e3779b97f4a7c15);
                        let mut z = seed;
                        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
                        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
                        z ^ (z >> 31)
                    })
                    .collect()
            })
            .collect();
        let hashes = (0..1800)
            .map(|n| {
                let mut bits = bases[n % 60].clone();
                let bit = (n / 60) % 256;
                bits[bit / 64] ^= 1u64 << (bit % 64);
                bits
            })
            .collect();
        let ignored = vec![
            ["00000.jpg".into(), "00060.jpg".into()],
            ["00001.jpg".into(), "00121.jpg".into()],
            ["00002.jpg".into(), "00003.jpg".into()],
        ];
        let dir = cached_album(hashes, ignored);
        let request = REQUEST.load(Ordering::SeqCst);
        let snapshot = |path: &Path| {
            let conn = open(path).unwrap();
            let rows = conn
                .prepare("SELECT name,root FROM nodes ORDER BY name")
                .unwrap()
                .query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))
                .unwrap()
                .collect::<Result<Vec<_>, _>>()
                .unwrap();
            rows
        };
        let path = build(dir.path(), &settings, request).unwrap();
        let mih_groups = snapshot(&path);
        let mih_comparisons: i64 = open(&path)
            .unwrap()
            .query_row("SELECT comparisons FROM state", [], |r| r.get(0))
            .unwrap();
        assert_eq!(
            open(&path)
                .unwrap()
                .query_row("SELECT search_method FROM state", [], |r| r
                    .get::<_, String>(0))
                .unwrap(),
            "mih"
        );
        std::fs::remove_file(&path).unwrap();
        struct Reset(bool);
        impl Drop for Reset {
            fn drop(&mut self) {
                TEST_SCAN_ONLY.set(self.0);
            }
        }
        let _reset = Reset(TEST_SCAN_ONLY.replace(true));
        let path = build(dir.path(), &settings, request).unwrap();
        assert_eq!(
            open(&path)
                .unwrap()
                .query_row("SELECT search_method FROM state", [], |r| r
                    .get::<_, String>(0))
                .unwrap(),
            "scan"
        );
        assert_eq!(mih_groups, snapshot(&path));
        let scan_comparisons: i64 = open(&path)
            .unwrap()
            .query_row("SELECT comparisons FROM state", [], |r| r.get(0))
            .unwrap();
        assert!(
            mih_comparisons < scan_comparisons,
            "the reference test must exercise pruning, not just fallback scans"
        );
    }

    #[test]
    fn fresh_hashes_can_be_persisted_while_a_search_batch_is_open() {
        let _scan = SCAN.lock().unwrap();
        let dir = tempfile::tempdir().unwrap();
        for name in ["a.png", "b.png", "c.png"] {
            image::RgbImage::from_pixel(32, 32, image::Rgb([17, 42, 91]))
                .save(dir.path().join(name))
                .unwrap();
        }
        let mut settings = crate::settings::read_settings();
        settings.duplicates.use_thumbnails_first = false;
        let path = build(dir.path(), &settings, REQUEST.load(Ordering::SeqCst)).unwrap();
        let conn = open(&path).unwrap();
        assert_eq!(
            conn.query_row("SELECT count FROM groups", [], |r| r.get::<_, i64>(0))
                .unwrap(),
            3
        );
        let mut reader =
            crate::album_db::FileReader::new(&dir.path().join(".room237-metadata/album.sqlite"))
                .unwrap();
        for name in ["a.png", "b.png", "c.png"] {
            let entry = reader.get(name).unwrap().unwrap();
            assert!(entry.hash.is_some());
            assert_eq!(
                entry.hash_version,
                Some(settings.hash_config().hash_version.clone())
            );
        }
    }

    #[test]
    fn packed_search_preserves_distances_and_exclusions_at_all_hash_sizes() {
        use crate::settings::HashSize;
        let _scan = SCAN.lock().unwrap();
        for size in [HashSize::S8x8, HashSize::S16x16, HashSize::S32x32] {
            for threshold in [0, 32, 128] {
                let mut settings = crate::settings::read_settings();
                settings.duplicates.hash_size = size;
                settings.duplicates.threshold = threshold;
                let words = settings.hash_config().bits as usize / 64;
                let mut hashes = vec![vec![0; words]; 4];
                let mut near = vec![0; words];
                near[words - 1] = 1;
                hashes.push(near);
                let dir = cached_album_with_settings(
                    hashes,
                    vec![["00000.jpg".into(), "00001.jpg".into()]],
                    &settings,
                );
                let path = build(dir.path(), &settings, REQUEST.load(Ordering::SeqCst)).unwrap();
                let conn = open(&path).unwrap();
                let a: String = conn
                    .query_row("SELECT root FROM nodes WHERE name='00000.jpg'", [], |r| {
                        r.get(0)
                    })
                    .unwrap();
                let b: String = conn
                    .query_row("SELECT root FROM nodes WHERE name='00001.jpg'", [], |r| {
                        r.get(0)
                    })
                    .unwrap();
                assert_ne!(a, b);
                for name in ["00002.jpg", "00003.jpg"] {
                    let group: String = conn
                        .query_row("SELECT root FROM nodes WHERE name=?1", [name], |r| r.get(0))
                        .unwrap();
                    assert_eq!(group, a);
                }
                let near: String = conn
                    .query_row("SELECT root FROM nodes WHERE name='00004.jpg'", [], |r| {
                        r.get(0)
                    })
                    .unwrap();
                assert_eq!(near == a, threshold > 0);
            }
        }
    }

    #[test]
    fn unrelated_exclusions_do_not_disable_exact_hash_folding() {
        let _scan = SCAN.lock().unwrap();
        let mut hashes = vec![vec![u64::MAX; 4], vec![0xaaaaaaaaaaaaaaaa; 4]];
        hashes.extend(vec![vec![0; 4]; 1000]);
        let dir = cached_album(hashes, vec![["00000.jpg".into(), "00001.jpg".into()]]);
        let path = build(
            dir.path(),
            &crate::settings::read_settings(),
            REQUEST.load(Ordering::SeqCst),
        )
        .unwrap();
        let conn = open(&path).unwrap();
        let folded: i64 = conn
            .query_row("SELECT exact_folds FROM state", [], |r| r.get(0))
            .unwrap();
        assert_eq!(folded, 999);
        assert_eq!(
            conn.query_row("SELECT count FROM groups", [], |r| r.get::<_, i64>(0))
                .unwrap(),
            1000
        );
    }
    #[test]
    fn identical_hashes_do_not_build_an_unused_mih_index() {
        let _scan = SCAN.lock().unwrap();
        let dir = cached_album(vec![vec![0; 4]; 1000], vec![]);
        let path = build(
            dir.path(),
            &crate::settings::read_settings(),
            REQUEST.load(Ordering::SeqCst),
        )
        .unwrap();
        let conn = open(&path).unwrap();
        assert_eq!(
            conn.query_row("SELECT search_method FROM state", [], |r| r
                .get::<_, String>(0))
                .unwrap(),
            "scan"
        );
        assert_eq!(
            conn.query_row("SELECT count FROM groups", [], |r| r.get::<_, i64>(0))
                .unwrap(),
            1000
        );
        assert_eq!(
            conn.query_row(
                "SELECT count(*) FROM sqlite_master WHERE name='mih_postings'",
                [],
                |r| r.get::<_, i64>(0)
            )
            .unwrap(),
            0
        );
    }

    #[test]
    fn packed_hash_blocks_match_brute_force_across_block_boundaries() {
        let _scan = SCAN.lock().unwrap();
        let mut seed = 237u64;
        let mut hashes = Vec::new();
        for _ in 0..1200 {
            let mut bits = vec![0; 2];
            for _ in 0..2 {
                seed = seed
                    .wrapping_mul(6364136223846793005)
                    .wrapping_add(1442695040888963407);
                bits.push(seed);
            }
            hashes.push(bits);
        }
        let mut near = hashes[1023].clone();
        near[3] ^= 1;
        hashes.push(near);
        let mut parent: Vec<usize> = (0..hashes.len()).collect();
        fn root(parent: &[usize], mut n: usize) -> usize {
            while parent[n] != n {
                n = parent[n];
            }
            n
        }
        for n in 0..hashes.len() {
            for m in 0..n {
                if within_threshold(&hashes[n], &hashes[m], 32) {
                    let a = root(&parent, n);
                    let b = root(&parent, m);
                    parent[a] = b;
                }
            }
        }
        let dir = cached_album(hashes, vec![]);
        let path = build(
            dir.path(),
            &crate::settings::read_settings(),
            REQUEST.load(Ordering::SeqCst),
        )
        .unwrap();
        let conn = open(&path).unwrap();
        let mut seen = std::collections::HashMap::new();
        for n in 0..parent.len() {
            let actual: String = conn
                .query_row(
                    "SELECT root FROM nodes WHERE name=?1",
                    [format!("{n:05}.jpg")],
                    |r| r.get(0),
                )
                .unwrap();
            let expected = root(&parent, n);
            if let Some(previous) = seen.insert(expected, actual.clone()) {
                assert_eq!(actual, previous);
            }
        }
        let unique = seen.values().collect::<std::collections::HashSet<_>>();
        assert_eq!(unique.len(), seen.len());
    }
}

#[cfg(test)]
mod packed_hash_tests {
    use super::*;
    #[test]
    fn fine_hash_pages_cross_physical_blocks_and_evict_within_one_mebibyte() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch("CREATE TABLE hash_blocks(block INTEGER PRIMARY KEY,data BLOB);")
            .unwrap();
        for block in 0..12 {
            let data: Vec<u64> = (0..HASH_BLOCK_FILES)
                .flat_map(|n| [0, 0, 0, (block * HASH_BLOCK_FILES + n) as u64])
                .collect();
            conn.execute(
                "INSERT INTO hash_blocks VALUES(?1,?2)",
                params![block as i64, bytes(&data)],
            )
            .unwrap();
        }
        let mut cache = HashBlocks::with_page_size(4, 64);
        for page in 0..12 * 16 {
            let hash = cache.get(&conn, page).unwrap();
            assert_eq!(hash[3], (page * 64) as u64);
            assert_eq!(hash[63 * 4 + 3], (page * 64 + 63) as u64);
            assert!(cache.cached.bytes() <= HASH_CACHE_BYTES);
        }
        let mut cache = HashBlocks::with_page_size(16, 64);
        // With 1024-bit hashes, 129 fine pages exceed the fixed 1 MiB cache.
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch("CREATE TABLE hash_blocks(block INTEGER PRIMARY KEY,data BLOB);")
            .unwrap();
        for block in 0..9 {
            let data: Vec<u64> = (0..HASH_BLOCK_FILES * 16)
                .map(|n| (block * HASH_BLOCK_FILES * 16 + n) as u64)
                .collect();
            conn.execute(
                "INSERT INTO hash_blocks VALUES(?1,?2)",
                params![block as i64, bytes(&data)],
            )
            .unwrap();
        }
        for page in 0..9 * 16 {
            assert_eq!(cache.get(&conn, page).unwrap()[0], (page * 64 * 16) as u64);
            assert!(cache.cached.bytes() <= HASH_CACHE_BYTES);
        }
        assert!(!cache.cached.contains(0));
        assert_eq!(cache.get(&conn, 0).unwrap()[0], 0);
    }

    #[test]
    fn hash_cache_evicts_blocks_at_a_fixed_bound_and_reloads_them() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch("CREATE TABLE hash_blocks(block INTEGER PRIMARY KEY,data BLOB);")
            .unwrap();
        for id in 0..(HASH_CACHE_BYTES / (16 * HASH_BLOCK_FILES * 8)) + 3 {
            let mut block = vec![0; 16 * HASH_BLOCK_FILES];
            block[0] = id as u64;
            conn.execute(
                "INSERT INTO hash_blocks VALUES(?1,?2)",
                params![id as i64, bytes(&block)],
            )
            .unwrap();
        }
        let mut cache = HashBlocks::new(16);
        for id in 0..(HASH_CACHE_BYTES / (16 * HASH_BLOCK_FILES * 8)) + 3 {
            assert_eq!(cache.get(&conn, id).unwrap()[0], id as u64);
            assert!(cache.cached.len() <= HASH_CACHE_BYTES / (16 * HASH_BLOCK_FILES * 8));
            assert!(cache.cached.bytes() <= 1024 * 1024);
        }
        assert!(!cache.cached.contains(0));
        assert_eq!(cache.get(&conn, 0).unwrap()[0], 0);
        conn.execute("INSERT INTO hash_blocks VALUES(999,?1)", [vec![0u8; 3]])
            .unwrap();
        assert!(cache.get(&conn, 999).is_err());
    }
}
