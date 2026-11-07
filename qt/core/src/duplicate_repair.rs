//! Repair deletion-only changes without hashing or matching the rest of the album.
use super::*;

pub(super) fn configuration(settings: &AdvancedSettings) -> String {
    format!(
        "{}:{}",
        settings.hash_config().hash_version,
        settings.duplicates.effective_threshold()
    )
}

pub(super) fn remember_configuration(
    conn: &Connection,
    settings: &AdvancedSettings,
) -> Result<(), String> {
    // Upgrade a validated legacy index without throwing away its expensive search.
    conn.execute_batch("CREATE TABLE IF NOT EXISTS configuration(value TEXT NOT NULL)")
        .map_err(|e| e.to_string())?;
    conn.execute(
        "INSERT INTO configuration SELECT ?1 WHERE NOT EXISTS(SELECT 1 FROM configuration)",
        [configuration(settings)],
    )
    .map_err(|e| e.to_string())?;
    Ok(())
}

pub(super) fn prune_deletions(
    dir: &Path,
    conn: &mut Connection,
    settings: &AdvancedSettings,
    signature: &str,
    items: &[Item],
    request: u64,
) -> Result<bool, String> {
    let known: Option<String> = conn
        .query_row("SELECT value FROM configuration", [], |r| r.get(0))
        .ok();
    if known.as_deref() != Some(configuration(settings).as_str()) {
        return Ok(false);
    }
    let _metadata = crate::metadata::lock_metadata_for_write(&[dir])?;
    let album = open(&dir.join(".room237-metadata/album.sqlite"))?;
    if finish_fingerprint(Sha256::new(), &album, "ignored_pairs")?
        != finish_fingerprint(Sha256::new(), conn, "ignored")?
    {
        return Ok(false);
    }
    let tx = conn.transaction().map_err(|e| e.to_string())?;
    tx.execute_batch(
        "CREATE TEMP TABLE live(name TEXT PRIMARY KEY,source TEXT NOT NULL,revision TEXT NOT NULL)",
    )
    .map_err(|e| e.to_string())?;
    for item in items {
        check(request)?;
        tx.prepare_cached("INSERT INTO live VALUES(?1,?2,?3)")
            .map_err(|e| e.to_string())?
            .execute(params![item.name, item.source, item.revision])
            .map_err(|e| e.to_string())?;
    }
    // Only the original decides whether a hash still describes the file. A
    // regenerated preview must not throw away the index.
    let changed: bool = tx.query_row(
        "SELECT EXISTS(SELECT 1 FROM live l LEFT JOIN nodes n ON n.name=l.name WHERE n.name IS NULL OR substr(n.revision,1,length(l.source)+1)<>l.source||'-')", [], |r|r.get(0))
        .map_err(|e|e.to_string())?;
    if changed {
        return Ok(false);
    }
    // Members are published with these stamps and checked before a deletion.
    tx.execute_batch("UPDATE nodes SET revision=(SELECT revision FROM live WHERE live.name=nodes.name) WHERE name IN(SELECT n.name FROM nodes n JOIN live l ON l.name=n.name WHERE n.revision<>l.revision)")
        .map_err(|e|e.to_string())?;
    let removed: bool = tx
        .query_row(
            "SELECT EXISTS(SELECT 1 FROM nodes WHERE name NOT IN(SELECT name FROM live))",
            [],
            |r| r.get(0),
        )
        .map_err(|e| e.to_string())?;
    if !removed {
        tx.execute("UPDATE state SET signature=?1", [signature])
            .map_err(|e| e.to_string())?;
        tx.commit().map_err(|e| e.to_string())?;
        return Ok(true);
    }
    tx.execute_batch("CREATE TEMP TABLE affected(root TEXT PRIMARY KEY);
        INSERT INTO affected SELECT DISTINCT root FROM nodes WHERE name NOT IN(SELECT name FROM live);")
        .map_err(|e|e.to_string())?;
    // With user exclusions, removing an endpoint can change the greedy merge
    // order across components. Rebuild only in that case; unrelated exclusions
    // do not invalidate a deletion repair.
    let restricted: bool = tx.query_row("SELECT EXISTS(SELECT 1 FROM nodes n JOIN affected a ON a.root=n.root WHERE EXISTS(SELECT 1 FROM ignored i WHERE i.a=n.name OR i.b=n.name))", [], |r|r.get(0))
        .map_err(|e|e.to_string())?;
    if restricted {
        return Ok(false);
    }
    tx.execute_batch("DELETE FROM nodes WHERE name NOT IN(SELECT name FROM live);
        CREATE TEMP TABLE repair AS SELECT name,hash,root AS old_root FROM nodes WHERE root IN(SELECT root FROM affected);
        CREATE INDEX repair_hashes ON repair(old_root,hash,name);
        CREATE TEMP TABLE representatives AS SELECT old_root,hash,min(name) AS name FROM repair GROUP BY old_root,hash;
        CREATE INDEX representative_names ON representatives(old_root,name);
        CREATE TABLE IF NOT EXISTS components(root TEXT PRIMARY KEY,restricted INTEGER NOT NULL);
        DELETE FROM components WHERE root IN(SELECT root FROM affected);
        UPDATE nodes SET root=(SELECT r.name FROM representatives r JOIN repair p ON p.old_root=r.old_root AND p.hash=r.hash WHERE p.name=nodes.name) WHERE name IN(SELECT name FROM repair);
        INSERT INTO components SELECT DISTINCT root,0 FROM nodes WHERE name IN(SELECT name FROM repair);")
        .map_err(|e|e.to_string())?;
    // Identical hashes are folded in SQL. Only distinct hashes in affected
    // components need distance checks, including the bridge-deletion case.
    let mut stmt = tx
        .prepare("SELECT old_root,name,hash FROM representatives ORDER BY old_root,name")
        .map_err(|e| e.to_string())?;
    let mut rows = stmt.query([]).map_err(|e| e.to_string())?;
    while let Some(row) = rows.next().map_err(|e| e.to_string())? {
        check(request)?;
        let root: String = row.get(0).map_err(|e| e.to_string())?;
        let name: String = row.get(1).map_err(|e| e.to_string())?;
        let hash: Vec<u8> = row.get(2).map_err(|e| e.to_string())?;
        let bits: Vec<u64> = hash
            .chunks_exact(8)
            .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
            .collect();
        let mut others = tx
            .prepare_cached(
                "SELECT name,hash FROM representatives WHERE old_root=?1 AND name<?2 ORDER BY name",
            )
            .map_err(|e| e.to_string())?;
        let mut others = others
            .query(params![root, name])
            .map_err(|e| e.to_string())?;
        while let Some(other) = others.next().map_err(|e| e.to_string())? {
            check(request)?;
            let other_name: String = other.get(0).map_err(|e| e.to_string())?;
            let other_hash: Vec<u8> = other.get(1).map_err(|e| e.to_string())?;
            let other_bits: Vec<u64> = other_hash
                .chunks_exact(8)
                .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
                .collect();
            if within_threshold(
                &bits,
                &other_bits,
                settings.duplicates.effective_threshold(),
            ) {
                merge(&tx, &name, &other_name)?;
            }
        }
    }
    drop(rows);
    drop(stmt);
    tx.execute_batch("DELETE FROM groups;
        INSERT INTO groups SELECT root,count(*),row_number() OVER(ORDER BY root) FROM nodes GROUP BY root HAVING count(*)>1;")
        .map_err(|e|e.to_string())?;
    tx.execute("UPDATE state SET signature=?1", [signature])
        .map_err(|e| e.to_string())?;
    tx.commit().map_err(|e| e.to_string())?;
    Ok(true)
}
