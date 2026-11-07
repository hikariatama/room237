//! One portable database per album. Connections are short-lived so moving or
//! clearing an album never leaves an open handle to an obsolete database.
use std::{collections::HashMap, path::Path, time::Duration};

use rusqlite::{params, Connection, OpenFlags, OptionalExtension};

use crate::metadata::{AlbumMeta, FileMetaEntry};

fn connect(path: &Path, writable: bool) -> Result<Connection, String> {
    // Readers open read/write when allowed so SQLite can recover a hot
    // rollback journal left by a killed writer. Fall back for read-only albums.
    let mut conn = match Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_WRITE) {
        Ok(conn) => conn,
        Err(_) if !writable => Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY)
            .map_err(|e| e.to_string())?,
        Err(error) => return Err(error.to_string()),
    };
    conn.busy_timeout(Duration::from_secs(5))
        .map_err(|e| e.to_string())?;
    let version: i64 = conn
        .pragma_query_value(None, "user_version", |row| row.get(0))
        .map_err(|e| e.to_string())?;
    if version == 1 && !conn.is_readonly("main").map_err(|e| e.to_string())? {
        let tx = conn
            .transaction_with_behavior(rusqlite::TransactionBehavior::Immediate)
            .map_err(|e| e.to_string())?;
        // Another connection may have completed the migration while we waited.
        let current: i64 = tx
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .map_err(|e| e.to_string())?;
        if current == 1 {
            tx.execute_batch("CREATE TABLE ignored_pairs(a TEXT NOT NULL,b TEXT NOT NULL,PRIMARY KEY(a,b));
                CREATE INDEX ignored_pairs_b ON ignored_pairs(b);
                INSERT OR IGNORE INTO ignored_pairs SELECT json_extract(value,'$[0]'),json_extract(value,'$[1]') FROM album,json_each(album.ignores);
                CREATE INDEX IF NOT EXISTS files_heic_output ON files(json_extract(data,'$.heic_conversion.output_name'));
                UPDATE album SET ignores='[]'; PRAGMA user_version=2;") .map_err(|e|e.to_string())?;
        } else if current != 2 {
            return Err(format!("Unsupported album database version: {current}"));
        }
        tx.commit().map_err(|e| e.to_string())?;
    } else if version != 2 {
        return Err(format!("Unsupported album database version: {version}"));
    }
    Ok(conn)
}

#[cfg(test)]
pub(crate) fn initialize(path: &Path, album: &AlbumMeta) -> Result<(), String> {
    initialize_with(path, |tx| {
        put_rows(tx, &album.files)?;
        set_ignores(tx, &album.duplicates_ignore)?;
        Ok(())
    })
}

fn initialize_with(
    path: &Path,
    populate: impl FnOnce(&Connection) -> Result<(), String>,
) -> Result<(), String> {
    // Publish only a complete database. A crash before publication leaves the
    // legacy metadata authoritative, including on the next launch.
    let temporary = tempfile::Builder::new()
        .prefix(".room237-db-")
        .tempfile_in(path.parent().ok_or("Missing database directory")?)
        .map_err(|e| e.to_string())?;
    let mut conn = Connection::open(temporary.path()).map_err(|e| e.to_string())?;
    // Rollback journal keeps the album portable without a persistent WAL file.
    conn.execute_batch(
        "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;
        CREATE TABLE files (name TEXT PRIMARY KEY NOT NULL, data TEXT NOT NULL);
        CREATE TABLE album (id INTEGER PRIMARY KEY CHECK(id=1), ignores TEXT NOT NULL);
        CREATE INDEX files_heic_output ON files(json_extract(data,'$.heic_conversion.output_name'));
        CREATE TABLE ignored_pairs(a TEXT NOT NULL,b TEXT NOT NULL,PRIMARY KEY(a,b));
        CREATE INDEX ignored_pairs_b ON ignored_pairs(b);
        PRAGMA user_version=2;",
    )
    .map_err(|e| e.to_string())?;
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    tx.execute("INSERT INTO album VALUES(1,'[]')", [])
        .map_err(|e| e.to_string())?;
    populate(&tx)?;
    tx.commit().map_err(|e| e.to_string())?;
    conn.close().map_err(|(_, e)| e.to_string())?;
    temporary.as_file().sync_all().map_err(|e| e.to_string())?;
    temporary
        .persist_noclobber(path)
        .map_err(|e| e.to_string())?;
    #[cfg(unix)]
    std::fs::File::open(path.parent().unwrap())
        .and_then(|dir| dir.sync_all())
        .map_err(|e| e.to_string())?;
    Ok(())
}

/// Stream the old map directly into SQLite: migration memory is one row.
pub(crate) fn initialize_legacy(path: &Path) -> Result<(), String> {
    use serde::de::{DeserializeSeed, MapAccess, Visitor};
    struct Files<'a>(&'a Connection);
    impl<'de> Visitor<'de> for Files<'_> {
        type Value = ();
        fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
            f.write_str("file metadata map")
        }
        fn visit_map<M: MapAccess<'de>>(self, mut map: M) -> Result<(), M::Error> {
            while let Some((name, entry)) = map.next_entry::<String, FileMetaEntry>()? {
                put_rows(self.0, &HashMap::from([(name, entry)]))
                    .map_err(serde::de::Error::custom)?;
            }
            Ok(())
        }
    }
    impl<'de> DeserializeSeed<'de> for Files<'_> {
        type Value = ();
        fn deserialize<D: serde::Deserializer<'de>>(self, deserializer: D) -> Result<(), D::Error> {
            deserializer.deserialize_map(self)
        }
    }
    struct Pairs<'a>(&'a Connection);
    impl<'de> DeserializeSeed<'de> for Pairs<'_> {
        type Value = ();
        fn deserialize<D: serde::Deserializer<'de>>(self, d: D) -> Result<(), D::Error> {
            d.deserialize_seq(self)
        }
    }
    impl<'de> Visitor<'de> for Pairs<'_> {
        type Value = ();
        fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
            f.write_str("ignored pairs")
        }
        fn visit_seq<M: serde::de::SeqAccess<'de>>(self, mut seq: M) -> Result<(), M::Error> {
            while let Some([a, b]) = seq.next_element::<[String; 2]>()? {
                self.0
                    .execute(
                        "INSERT OR IGNORE INTO ignored_pairs VALUES(?1,?2)",
                        params![a, b],
                    )
                    .map_err(serde::de::Error::custom)?;
            }
            Ok(())
        }
    }
    struct Album<'a>(&'a Connection);
    impl<'de> Visitor<'de> for Album<'_> {
        type Value = ();
        fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
            f.write_str("album metadata")
        }
        fn visit_map<M: MapAccess<'de>>(self, mut map: M) -> Result<(), M::Error> {
            while let Some(key) = map.next_key::<String>()? {
                match key.as_str() {
                    "files" => map.next_value_seed(Files(self.0))?,
                    "duplicates_ignore" => {
                        map.next_value_seed(Pairs(self.0))?;
                    }
                    _ => {
                        map.next_value::<serde::de::IgnoredAny>()?;
                    }
                }
            }
            Ok(())
        }
    }
    impl<'de> DeserializeSeed<'de> for Album<'_> {
        type Value = ();
        fn deserialize<D: serde::Deserializer<'de>>(self, deserializer: D) -> Result<(), D::Error> {
            deserializer.deserialize_map(self)
        }
    }
    initialize_with(path, |conn| {
        let dir = path.parent().ok_or("Missing metadata directory")?;
        let json = dir.join("album.json");
        if json.try_exists().map_err(|e| e.to_string())? {
            let mut deserializer = serde_json::Deserializer::from_reader(std::io::BufReader::new(
                std::fs::File::open(&json).map_err(|e| e.to_string())?,
            ));
            Album(conn)
                .deserialize(&mut deserializer)
                .map_err(|e| format!("Invalid metadata in {}: {e}", json.display()))?;
            deserializer.end().map_err(|e| e.to_string())?;
        } else {
            for file in std::fs::read_dir(dir).map_err(|e| e.to_string())? {
                let file = file.map_err(|e| e.to_string())?;
                if !file.file_type().map_err(|e| e.to_string())?.is_file() {
                    continue;
                }
                let filename = file.file_name().to_string_lossy().into_owned();
                let Some(name) = filename.strip_suffix(".meta") else {
                    continue;
                };
                let entry: FileMetaEntry = serde_json::from_reader(std::io::BufReader::new(
                    std::fs::File::open(file.path()).map_err(|e| e.to_string())?,
                ))
                .map_err(|e| e.to_string())?;
                put_rows(conn, &HashMap::from([(name.to_string(), entry)]))?;
            }
        }
        Ok(())
    })
}

pub(crate) struct FileReader {
    conn: Connection,
}
impl FileReader {
    pub(crate) fn new(path: &Path) -> Result<Self, String> {
        Ok(Self {
            conn: connect(path, false)?,
        })
    }
    pub(crate) fn get(&mut self, name: &str) -> Result<Option<FileMetaEntry>, String> {
        get_row(&self.conn, name)
    }
}

pub(crate) fn subset(path: &Path, names: &[String], related: bool) -> Result<AlbumMeta, String> {
    let conn = connect(path, false)?;
    let mut files = HashMap::new();
    for name in names {
        if let Some(entry) = get_row(&conn, name)? {
            files.insert(name.clone(), entry);
        }
        if related {
            let mut stmt = conn.prepare_cached("SELECT name,data FROM files WHERE json_extract(data,'$.heic_conversion.output_name')=?1").map_err(|e| e.to_string())?;
            let rows = stmt
                .query_map([name], |r| {
                    Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?))
                })
                .map_err(|e| e.to_string())?;
            for row in rows {
                let (name, data) = row.map_err(|e| e.to_string())?;
                files.insert(
                    name,
                    serde_json::from_str(&data).map_err(|e| e.to_string())?,
                );
            }
        }
    }
    Ok(AlbumMeta {
        files,
        duplicates_ignore: Vec::new(),
    })
}

/// Apply a named patch without reading or deleting unrelated metadata.
pub(crate) fn patch(path: &Path, names: &[String], album: &AlbumMeta) -> Result<(), String> {
    let mut conn = connect(path, true)?;
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    for name in names {
        if let Some(entry) = album.files.get(name) {
            put_rows(&tx, &HashMap::from([(name.clone(), entry.clone())]))?;
        } else {
            tx.execute("DELETE FROM files WHERE name=?1", [name])
                .map_err(|e| e.to_string())?;
        }
    }
    tx.commit().map_err(|e| e.to_string())?;
    if let Some(dir) = path.parent().and_then(Path::parent) {
        for name in names {
            crate::catalog::publish(
                dir,
                name,
                album.files.get(name).unwrap_or(&FileMetaEntry::default()),
            );
        }
    }
    Ok(())
}

pub(crate) fn remove(path: &Path, names: &[String]) -> Result<(), String> {
    let mut conn = connect(path, true)?;
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    for name in names {
        tx.execute("DELETE FROM files WHERE name=?1", [name])
            .map_err(|e| e.to_string())?;
        tx.execute("DELETE FROM ignored_pairs WHERE a=?1 OR b=?1", [name])
            .map_err(|e| e.to_string())?;
    }
    tx.commit().map_err(|e| e.to_string())
}

#[cfg(test)]
fn set_ignores(conn: &Connection, pairs: &[[String; 2]]) -> Result<(), String> {
    conn.execute("DELETE FROM ignored_pairs", [])
        .map_err(|e| e.to_string())?;
    for [a, b] in pairs {
        conn.execute(
            "INSERT OR IGNORE INTO ignored_pairs VALUES(?1,?2)",
            params![a, b],
        )
        .map_err(|e| e.to_string())?;
    }
    Ok(())
}

pub(crate) fn ignore_pairs(path: &Path, names: &[String]) -> Result<(), String> {
    let mut conn = connect(path, true)?;
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    for (i, a) in names.iter().enumerate() {
        for b in &names[i + 1..] {
            tx.execute(
                "INSERT OR IGNORE INTO ignored_pairs VALUES(?1,?2)",
                params![a, b],
            )
            .map_err(|e| e.to_string())?;
        }
    }
    tx.commit().map_err(|e| e.to_string())
}

pub(crate) fn clear_ignores(path: &Path) -> Result<bool, String> {
    let conn = connect(path, true)?;
    conn.execute("DELETE FROM ignored_pairs", [])
        .map(|n| n > 0)
        .map_err(|e| e.to_string())
}

fn get_row(conn: &Connection, name: &str) -> Result<Option<FileMetaEntry>, String> {
    let data: Option<String> = conn
        .query_row("SELECT data FROM files WHERE name=?1", [name], |row| {
            row.get(0)
        })
        .optional()
        .map_err(|e| e.to_string())?;
    data.map(|data| serde_json::from_str(&data).map_err(|e| e.to_string()))
        .transpose()
}

pub(crate) fn read_file(path: &Path, name: &str) -> Result<Option<FileMetaEntry>, String> {
    get_row(&connect(path, false)?, name)
}

#[cfg(test)]
fn read_conn(conn: &Connection) -> Result<AlbumMeta, String> {
    let mut files = HashMap::new();
    let mut stmt = conn
        .prepare("SELECT name, data FROM files")
        .map_err(|e| e.to_string())?;
    let rows = stmt
        .query_map([], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        })
        .map_err(|e| e.to_string())?;
    for row in rows {
        let (name, data) = row.map_err(|e| e.to_string())?;
        files.insert(
            name,
            serde_json::from_str(&data).map_err(|e| e.to_string())?,
        );
    }
    let mut stmt = conn
        .prepare("SELECT a,b FROM ignored_pairs ORDER BY rowid")
        .map_err(|e| e.to_string())?;
    let duplicates_ignore = stmt
        .query_map([], |row| Ok([row.get(0)?, row.get(1)?]))
        .map_err(|e| e.to_string())?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|e| e.to_string())?;
    Ok(AlbumMeta {
        files,
        duplicates_ignore,
    })
}

#[cfg(test)]
pub(crate) fn read(path: &Path) -> Result<AlbumMeta, String> {
    let mut conn = connect(path, false)?;
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    read_conn(&tx)
}

fn put_rows(conn: &Connection, rows: &HashMap<String, FileMetaEntry>) -> Result<(), String> {
    let mut stmt = conn
        .prepare_cached(
            "INSERT INTO files(name, data) VALUES (?1, ?2)
        ON CONFLICT(name) DO UPDATE SET data=excluded.data WHERE files.data<>excluded.data",
        )
        .map_err(|e| e.to_string())?;
    for (name, entry) in rows {
        stmt.execute(params![
            name,
            serde_json::to_string(entry).map_err(|e| e.to_string())?
        ])
        .map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// The hot path touches only the requested primary keys, even in a huge album.
pub(crate) fn update_rows<T>(
    path: &Path,
    update: impl FnOnce(&Connection, &mut HashMap<String, FileMetaEntry>) -> Result<T, String>,
) -> Result<T, String> {
    let mut conn = connect(path, true)?;
    let tx = conn
        .transaction_with_behavior(rusqlite::TransactionBehavior::Immediate)
        .map_err(|e| e.to_string())?;
    let mut rows = HashMap::new();
    let result = update(&tx, &mut rows)?;
    put_rows(&tx, &rows)?;
    tx.commit().map_err(|e| e.to_string())?;
    if let Some(album) = path.parent().and_then(Path::parent) {
        for (name, entry) in &rows {
            crate::catalog::publish(album, name, entry);
        }
    }
    Ok(result)
}

pub(crate) fn entry(conn: &Connection, name: &str) -> Result<FileMetaEntry, String> {
    Ok(get_row(conn, name)?.unwrap_or_default())
}

/// Bulk user operations retain their snapshot API, but persist only differences.
#[cfg(test)]
pub(crate) fn replace(path: &Path, album: &AlbumMeta) -> Result<(), String> {
    let mut conn = connect(path, true)?;
    let tx = conn
        .transaction_with_behavior(rusqlite::TransactionBehavior::Immediate)
        .map_err(|e| e.to_string())?;
    let previous = read_conn(&tx)?;
    for name in previous
        .files
        .keys()
        .filter(|name| !album.files.contains_key(*name))
    {
        tx.execute("DELETE FROM files WHERE name=?1", [name])
            .map_err(|e| e.to_string())?;
    }
    let changed = album
        .files
        .iter()
        .filter(|(name, entry)| previous.files.get(*name) != Some(*entry))
        .map(|(name, entry)| (name.clone(), entry.clone()))
        .collect();
    put_rows(&tx, &changed)?;
    if previous.duplicates_ignore != album.duplicates_ignore {
        set_ignores(&tx, &album.duplicates_ignore)?;
    }
    tx.commit().map_err(|e| e.to_string())?;
    if let Some(dir) = path.parent().and_then(Path::parent) {
        for (name, entry) in &changed {
            crate::catalog::publish(dir, name, entry);
        }
        let deleted: Vec<String> = previous
            .files
            .keys()
            .filter(|name| !album.files.contains_key(*name))
            .cloned()
            .collect();
        crate::catalog::forget(dir, &deleted)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::metadata::{read_album_meta_checked, update_file_meta};
    use std::fs;

    #[test]
    fn migration_preserves_all_fields_and_keeps_legacy_backup_unchanged() {
        let dir = tempfile::tempdir().unwrap();
        let meta = dir.path().join(".room237-metadata");
        fs::create_dir(&meta).unwrap();
        let mut album = AlbumMeta::default();
        album.files.insert(
            "photo.jpg".into(),
            FileMetaEntry {
                favorite: true,
                meta: Some("123".into()),
                hash: Some("visual hash".into()),
                thumb_version: Some("preview version".into()),
                ..Default::default()
            },
        );
        album
            .duplicates_ignore
            .push(["photo.jpg".into(), "other.jpg".into()]);
        let json = serde_json::to_vec(&album).unwrap();
        fs::write(meta.join("album.json"), &json).unwrap();
        let migrated = read_album_meta_checked(dir.path()).unwrap();
        assert_eq!(migrated.files, album.files);
        assert_eq!(migrated.duplicates_ignore, album.duplicates_ignore);
        update_file_meta(dir.path(), "photo.jpg", |entry| {
            entry.meta = Some("456".into())
        })
        .unwrap();
        assert_eq!(fs::read(meta.join("album.json")).unwrap(), json);
        let disk = read(&meta.join("album.sqlite")).unwrap();
        assert_eq!(disk.files["photo.jpg"].meta.as_deref(), Some("456"));
        assert!(disk.files["photo.jpg"].favorite);
        // Older JSON writers cannot overwrite the authoritative database.
        fs::write(meta.join("album.json"), b"{broken backup").unwrap();
        assert_eq!(
            read_album_meta_checked(dir.path()).unwrap().files,
            disk.files
        );
    }

    #[test]
    fn concurrent_readers_migrate_v1_once_and_preserve_ignored_pairs() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("album.sqlite");
        let conn = Connection::open(&path).unwrap();
        conn.execute_batch(
            "CREATE TABLE files(name TEXT PRIMARY KEY,data TEXT NOT NULL);
            CREATE TABLE album(id INTEGER PRIMARY KEY,ignores TEXT NOT NULL);
            INSERT INTO album VALUES(1,'[[\"a.jpg\",\"b.jpg\"]]'); PRAGMA user_version=1;",
        )
        .unwrap();
        drop(conn);
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(8));
        std::thread::scope(|scope| {
            for _ in 0..8 {
                let barrier = barrier.clone();
                let path = &path;
                scope.spawn(move || {
                    barrier.wait();
                    let conn = connect(path, false).unwrap();
                    let count: i64 = conn
                        .query_row("SELECT count(*) FROM ignored_pairs", [], |r| r.get(0))
                        .unwrap();
                    assert_eq!(count, 1);
                });
            }
        });
    }

    #[test]
    fn legacy_migration_streams_many_rows_and_sparse_patch_preserves_the_rest() {
        use std::io::Write;
        let dir = tempfile::tempdir().unwrap();
        let service = dir.path().join(".room237-metadata");
        fs::create_dir(&service).unwrap();
        let mut json =
            std::io::BufWriter::new(fs::File::create(service.join("album.json")).unwrap());
        write!(json, "{{\"files\":{{").unwrap();
        for n in 0..20_000 {
            if n > 0 {
                write!(json, ",").unwrap();
            }
            write!(json, "\"{n}.jpg\":{{\"favorite\":true}}").unwrap();
        }
        write!(json, "}},\"duplicates_ignore\":[]}}").unwrap();
        drop(json);
        crate::metadata::prepare_database(dir.path()).unwrap();
        let path = service.join("album.sqlite");
        let conn = connect(&path, false).unwrap();
        assert_eq!(
            conn.query_row("SELECT count(*) FROM files", [], |r| r.get::<_, i64>(0))
                .unwrap(),
            20_000
        );
        drop(conn);
        let mut sparse = subset(&path, &["123.jpg".into()], false).unwrap();
        sparse.files.get_mut("123.jpg").unwrap().favorite = false;
        patch(&path, &["123.jpg".into()], &sparse).unwrap();
        let conn = connect(&path, false).unwrap();
        assert_eq!(
            conn.query_row(
                "SELECT count(*) FROM files WHERE json_extract(data,'$.favorite')=1",
                [],
                |r| r.get::<_, i64>(0)
            )
            .unwrap(),
            19_999
        );
    }

    #[test]
    fn corrupt_legacy_metadata_never_publishes_a_database() {
        let dir = tempfile::tempdir().unwrap();
        let meta = dir.path().join(".room237-metadata");
        fs::create_dir(&meta).unwrap();
        fs::write(meta.join("album.json"), b"{broken").unwrap();
        assert!(update_file_meta(dir.path(), "photo.jpg", |_| {}).is_err());
        assert!(!meta.join("album.sqlite").exists());
        assert_eq!(fs::read(meta.join("album.json")).unwrap(), b"{broken");
    }

    #[test]
    fn hot_updates_touch_only_one_row_and_failed_transactions_roll_back() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("album.sqlite");
        let mut album = AlbumMeta::default();
        for n in 0..10_000 {
            album
                .files
                .insert(format!("{n}.jpg"), FileMetaEntry::default());
        }
        initialize(&path, &album).unwrap();
        let conn = connect(&path, true).unwrap();
        conn.execute_batch(
            "CREATE TABLE changes (name TEXT);
            CREATE TRIGGER count_updates AFTER UPDATE ON files BEGIN
            INSERT INTO changes VALUES (new.name); END;",
        )
        .unwrap();
        drop(conn);
        update_rows(&path, |conn, rows| {
            let mut file = entry(conn, "123.jpg")?;
            file.favorite = true;
            rows.insert("123.jpg".into(), file);
            Ok(())
        })
        .unwrap();
        let conn = connect(&path, false).unwrap();
        let count: i64 = conn
            .query_row("SELECT count(*) FROM changes", [], |row| row.get(0))
            .unwrap();
        assert_eq!(count, 1);
        drop(conn);
        assert!(update_rows(&path, |conn, _| {
            conn.execute("DELETE FROM files", [])
                .map_err(|e| e.to_string())?;
            Err::<(), _>("injected failure".into())
        })
        .is_err());
        let disk = read(&path).unwrap();
        assert_eq!(disk.files.len(), 10_000);
        assert!(disk.files["123.jpg"].favorite);
    }

    #[test]
    fn killed_sqlite_writer_is_recovered_by_the_next_reader() {
        use crate::test_support::{spawn_test, wait_for_file};
        if std::env::var("ROOM237_TEST_ROLE").as_deref() == Ok("sqlite-crash") {
            let dir = std::path::PathBuf::from(std::env::var_os("ROOM237_TEST_DIR").unwrap());
            let mut conn = connect(&dir.join("album.sqlite"), true).unwrap();
            // Force dirty pages out to the database before commit, leaving a
            // real hot journal when the process is killed.
            conn.execute_batch("PRAGMA cache_size=1").unwrap();
            let tx = conn.transaction().unwrap();
            tx.execute(
                "UPDATE files SET data=?1",
                [serde_json::json!({"meta":"x".repeat(100_000)}).to_string()],
            )
            .unwrap();
            std::fs::write(dir.join("sqlite-writing"), b"ready").unwrap();
            loop {
                std::thread::park();
            }
        }
        let dir = tempfile::tempdir().unwrap();
        let mut album = AlbumMeta::default();
        album.files.insert(
            "photo.jpg".into(),
            FileMetaEntry {
                favorite: true,
                ..Default::default()
            },
        );
        initialize(&dir.path().join("album.sqlite"), &album).unwrap();
        let mut writer = spawn_test(
            "album_db::tests::killed_sqlite_writer_is_recovered_by_the_next_reader",
            dir.path(),
            "sqlite-crash",
        );
        wait_for_file(&dir.path().join("sqlite-writing"));
        writer.0.kill().unwrap();
        writer.0.wait().unwrap();
        let recovered = read(&dir.path().join("album.sqlite")).unwrap();
        assert_eq!(recovered.files, album.files);
        assert_eq!(recovered.duplicates_ignore, album.duplicates_ignore);
    }

    #[test]
    #[ignore = "paired persistence benchmark"]
    fn benchmark_json_rewrite_vs_sqlite_rows() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("album.sqlite");
        let mut album = AlbumMeta::default();
        for n in 0..10_000 {
            album.files.insert(
                format!("{n}.jpg"),
                FileMetaEntry {
                    meta: Some("12345678901234567890".into()),
                    hash: Some("1234567890abcdef".into()),
                    ..Default::default()
                },
            );
        }
        initialize(&path, &album).unwrap();
        let start = std::time::Instant::now();
        for n in 0..100 {
            album.files.get_mut(&format!("{n}.jpg")).unwrap().favorite = true;
            crate::util::atomic_write(
                &dir.path().join("album.json"),
                &serde_json::to_vec_pretty(&album).unwrap(),
            )
            .unwrap();
        }
        let json = start.elapsed();
        let start = std::time::Instant::now();
        for n in 0..100 {
            update_rows(&path, |conn, rows| {
                let name = format!("{n}.jpg");
                let mut file = entry(conn, &name)?;
                file.favorite = true;
                rows.insert(name, file);
                Ok(())
            })
            .unwrap();
        }
        eprintln!(
            "10,000 files, 100 persisted updates: JSON={json:?}, SQLite={:?}",
            start.elapsed()
        );
        assert_eq!(read(&path).unwrap().files, album.files);
    }
}
