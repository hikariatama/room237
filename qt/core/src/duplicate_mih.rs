//! Exact multi-index Hamming search. Directories and postings are persisted;
//! every in-memory structure has a size bound independent of the album size.
use super::cache::Pages;
use rusqlite::{params, params_from_iter, types::Value, Connection};

const DIRECTORY_BUDGET: usize = 12 * 1024 * 1024;
const POSTING_IDS: usize = 512;
const POSTING_PAGES: usize = 1024; // 4 MiB of decoded IDs.
const CANDIDATE_IDS: usize = 8192; // 64 KiB; excess candidates spill to SQLite.
const MAX_PROBES: u64 = 4096;

#[derive(Clone, Debug)]
pub(super) struct Part {
    offset: usize,
    bits: u8,
    radius: u8,
}
#[derive(Clone, Debug)]
pub(super) struct Plan {
    parts: Vec<Part>,
    pub probes: u64,
}
impl Plan {
    fn partition(bits: usize, tables: usize, threshold: u32) -> Self {
        let small = bits / tables;
        let long = bits % tables;
        let radius = threshold as usize / tables;
        let high = threshold as usize % tables + 1;
        let mut offset = 0;
        let mut parts = Vec::new();
        for table in 0..tables {
            let width = small + usize::from(table < long);
            let r = if table < high {
                Some(radius)
            } else {
                radius.checked_sub(1)
            };
            if let Some(r) = r {
                parts.push(Part {
                    offset,
                    bits: width as u8,
                    radius: r.min(width) as u8,
                });
            }
            offset += width;
        }
        let probes = parts.iter().map(|p| volume(p.bits, p.radius)).sum();
        Self { parts, probes }
    }
    pub fn choose(count: usize, bits: usize, threshold: u32) -> Option<Self> {
        if count < 512 || threshold == 0 {
            return None;
        }
        let pairs = (count as f64) * (count - 1) as f64 / 2.0;
        let mut best: Option<(f64, Self)> = None;
        for tables in bits.div_ceil(16)..=bits.min((threshold as usize + 1).max(bits.div_ceil(16)))
        {
            let plan = Self::partition(bits, tables, threshold);
            if plan.probes > MAX_PROBES || plan.directory_bytes() > DIRECTORY_BUDGET {
                continue;
            }
            let no_hit = plan.parts.iter().fold(1.0, |p, part| {
                p * (1.0 - volume(part.bits, part.radius) as f64 / (1u64 << part.bits) as f64)
            });
            // A candidate costs more than a tight scan comparison: its posting
            // must be fetched, deduplicated, sorted and verified. Directory
            // probes are contiguous RAM reads after loading the bounded cache.
            // Real bucket populations are guarded again at query time.
            let work = count as f64 * plan.probes as f64 + pairs * (1.0 - no_hit) * 4.0;
            if work < pairs * 0.6 && best.as_ref().is_none_or(|(cost, _)| work < *cost) {
                best = Some((work, plan));
            }
        }
        best.map(|(_, plan)| plan)
    }
    fn directory_bytes(&self) -> usize {
        self.parts
            .iter()
            .map(|p| ((1usize << p.bits) + 1) * 8)
            .sum()
    }
}
fn volume(bits: u8, radius: u8) -> u64 {
    let mut term = 1u64;
    let mut sum = 1;
    for k in 1..=radius.min(bits) {
        term = term * (bits - k + 1) as u64 / k as u64;
        sum += term;
    }
    sum
}
fn extract(words: &[u64], part: &Part) -> u16 {
    let word = part.offset / 64;
    let shift = part.offset % 64;
    let mut value = words[word] >> shift;
    if shift + part.bits as usize > 64 {
        value |= words[word + 1] << (64 - shift);
    }
    (value & ((1u64 << part.bits) - 1)) as u16
}
struct Masks {
    bits: u8,
    radius: u8,
    weight: u8,
    next: Option<u32>,
}
impl Masks {
    fn new(bits: u8, radius: u8) -> Self {
        Self {
            bits,
            radius: radius.min(bits),
            weight: 0,
            next: Some(0),
        }
    }
}
impl Iterator for Masks {
    type Item = u16;
    fn next(&mut self) -> Option<u16> {
        let value = self.next?;
        let following = if value == 0 {
            1
        } else {
            let low = value & value.wrapping_neg();
            let ripple = value + low;
            (((ripple ^ value) >> 2) / low) | ripple
        };
        if value == 0 || following >= 1u32 << self.bits {
            self.weight += 1;
            self.next = if self.weight <= self.radius {
                Some((1u32 << self.weight) - 1)
            } else {
                None
            };
        } else {
            self.next = Some(following);
        }
        Some(value as u16)
    }
}
fn encode(ids: &[u64]) -> Vec<u8> {
    ids.iter().flat_map(|id| id.to_le_bytes()).collect()
}
fn decode(data: &[u8]) -> Result<Vec<u64>, String> {
    if data.len() % 8 != 0 {
        return Err("Invalid MIH integer block".into());
    }
    Ok(data
        .chunks_exact(8)
        .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
        .collect())
}
fn integer(value: u64) -> Result<i64, String> {
    i64::try_from(value).map_err(|e| e.to_string())
}

struct PostingCache {
    pages: Pages<(usize, u64)>,
}
impl Default for PostingCache {
    fn default() -> Self {
        Self {
            pages: Pages::new(POSTING_PAGES),
        }
    }
}
impl PostingCache {
    fn get(&mut self, conn: &Connection, key: (usize, u64)) -> Result<&[u64], String> {
        self.pages.get(key, || {
            let data: Vec<u8> = conn
                .prepare_cached("SELECT data FROM mih_postings WHERE part=?1 AND page=?2")
                .map_err(|e| e.to_string())?
                .query_row(params![key.0 as i64, integer(key.1)?], |r| r.get(0))
                .map_err(|e| e.to_string())?;
            if data.is_empty() || data.len() > POSTING_IDS * 8 {
                return Err("Invalid MIH posting page".into());
            }
            decode(&data)
        })
    }
}

pub(super) struct Index {
    plan: Plan,
    directories: Vec<Vec<u64>>,
    postings: PostingCache,
    pub probes: u64,
    pub fallbacks: u64,
}
pub(super) enum Candidates {
    Memory(Vec<u64>),
    Disk,
}
impl Candidates {
    pub fn page(conn: &Connection, after: Option<u64>) -> Result<Vec<u64>, String> {
        let mut stmt = conn
            .prepare_cached("SELECT id FROM mih_candidates WHERE id>?1 ORDER BY id LIMIT 256")
            .map_err(|e| e.to_string())?;
        let rows = stmt
            .query_map([after.map(integer).transpose()?.unwrap_or(-1)], |r| {
                r.get::<_, i64>(0).map(|v| v as u64)
            })
            .map_err(|e| e.to_string())?;
        rows.collect::<Result<_, _>>().map_err(|e| e.to_string())
    }
}
struct Collector {
    values: Vec<u64>,
    spilled: bool,
}
impl Collector {
    fn new() -> Self {
        Self {
            values: Vec::with_capacity(CANDIDATE_IDS),
            spilled: false,
        }
    }
    fn push(&mut self, conn: &Connection, id: u64) -> Result<(), String> {
        self.values.push(id);
        if self.values.len() == CANDIDATE_IDS {
            self.flush(conn)?;
            self.spilled = true;
        }
        Ok(())
    }
    fn flush(&mut self, conn: &Connection) -> Result<(), String> {
        self.values.sort_unstable();
        self.values.dedup();
        for chunk in self.values.chunks(128) {
            let sql = format!(
                "INSERT OR IGNORE INTO mih_candidates VALUES {}",
                (0..chunk.len())
                    .map(|n| format!("(?{})", n + 1))
                    .collect::<Vec<_>>()
                    .join(",")
            );
            let args: Vec<_> = chunk
                .iter()
                .map(|id| integer(*id).map(Value::Integer))
                .collect::<Result<_, _>>()?;
            conn.prepare_cached(&sql)
                .map_err(|e| e.to_string())?
                .execute(params_from_iter(args))
                .map_err(|e| e.to_string())?;
        }
        self.values.clear();
        Ok(())
    }
    fn finish(mut self, conn: &Connection) -> Result<Candidates, String> {
        if self.spilled {
            self.flush(conn)?;
            Ok(Candidates::Disk)
        } else {
            self.values.sort_unstable();
            self.values.dedup();
            Ok(Candidates::Memory(self.values))
        }
    }
}
impl Index {
    pub fn build(
        conn: &mut Connection,
        plan: Plan,
        count: usize,
        words: usize,
        mut cancel: impl FnMut() -> Result<(), String>,
    ) -> Result<Self, String> {
        let tx = conn.transaction().map_err(|e| e.to_string())?;
        tx.execute_batch("CREATE TABLE mih_plan(part INTEGER PRIMARY KEY,offset INTEGER,bits INTEGER,radius INTEGER);
            CREATE TABLE mih_directories(part INTEGER PRIMARY KEY,data BLOB NOT NULL);
            CREATE TABLE mih_postings(part INTEGER,page INTEGER,data BLOB NOT NULL,PRIMARY KEY(part,page)) WITHOUT ROWID;
            CREATE TEMP TABLE mih_keys(part INTEGER,key INTEGER,id INTEGER);
            CREATE TEMP TABLE mih_candidates(id INTEGER PRIMARY KEY) WITHOUT ROWID;") .map_err(|e|e.to_string())?;
        let insert = format!(
            "INSERT INTO mih_keys VALUES {}",
            (0..plan.parts.len())
                .map(|n| format!("(?{},?{},?{})", n * 3 + 1, n * 3 + 2, n * 3 + 3))
                .collect::<Vec<_>>()
                .join(",")
        );
        let mut id = 0usize;
        let mut args = Vec::with_capacity(plan.parts.len() * 3);
        while id < count {
            cancel()?;
            let data: Vec<u8> = tx
                .prepare_cached("SELECT data FROM hash_blocks WHERE block=?1")
                .map_err(|e| e.to_string())?
                .query_row([(id / 1024) as i64], |r| r.get(0))
                .map_err(|e| e.to_string())?;
            let data = decode(&data)?;
            if data.is_empty() || data.len() % words != 0 || data.len() > 1024 * words {
                return Err("Invalid source hash block for MIH".into());
            }
            for hash in data.chunks_exact(words) {
                if id >= count {
                    break;
                }
                if id % 256 == 0 {
                    cancel()?;
                }
                args.clear();
                for (part, p) in plan.parts.iter().enumerate() {
                    args.extend([
                        Value::Integer(part as i64),
                        Value::Integer(extract(hash, p) as i64),
                        Value::Integer(integer(id as u64)?),
                    ]);
                }
                tx.prepare_cached(&insert)
                    .map_err(|e| e.to_string())?
                    .execute(params_from_iter(args.iter()))
                    .map_err(|e| e.to_string())?;
                id += 1;
            }
        }
        tx.execute_batch("CREATE INDEX temp.mih_keys_order ON mih_keys(part,key,id);")
            .map_err(|e| e.to_string())?;
        for (part, p) in plan.parts.iter().enumerate() {
            cancel()?;
            let mut directory = vec![0u64; (1usize << p.bits) + 1];
            let mut key = 0usize;
            let mut offset = 0u64;
            let mut page = 0u64;
            let mut ids = Vec::with_capacity(POSTING_IDS);
            let mut stmt = tx
                .prepare_cached("SELECT key,id FROM mih_keys WHERE part=?1 ORDER BY key,id")
                .map_err(|e| e.to_string())?;
            let rows = stmt
                .query_map([part as i64], |r| {
                    Ok((r.get::<_, i64>(0)? as usize, r.get::<_, i64>(1)? as u64))
                })
                .map_err(|e| e.to_string())?;
            for row in rows {
                if offset % 256 == 0 {
                    cancel()?;
                }
                let (next, id) = row.map_err(|e| e.to_string())?;
                while key <= next {
                    directory[key] = offset;
                    key += 1;
                }
                ids.push(id);
                offset += 1;
                if ids.len() == POSTING_IDS {
                    tx.prepare_cached("INSERT INTO mih_postings VALUES(?1,?2,?3)")
                        .map_err(|e| e.to_string())?
                        .execute(params![part as i64, integer(page)?, encode(&ids)])
                        .map_err(|e| e.to_string())?;
                    page += 1;
                    ids.clear();
                }
            }
            if !ids.is_empty() {
                tx.prepare_cached("INSERT INTO mih_postings VALUES(?1,?2,?3)")
                    .map_err(|e| e.to_string())?
                    .execute(params![part as i64, integer(page)?, encode(&ids)])
                    .map_err(|e| e.to_string())?;
            }
            while key < directory.len() {
                directory[key] = offset;
                key += 1;
            }
            tx.execute(
                "INSERT INTO mih_directories VALUES(?1,?2)",
                params![part as i64, encode(&directory)],
            )
            .map_err(|e| e.to_string())?;
            tx.execute(
                "INSERT INTO mih_plan VALUES(?1,?2,?3,?4)",
                params![part as i64, p.offset as i64, p.bits, p.radius],
            )
            .map_err(|e| e.to_string())?;
        }
        tx.execute_batch("DROP TABLE mih_keys;")
            .map_err(|e| e.to_string())?;
        tx.commit().map_err(|e| e.to_string())?;
        let mut directories = Vec::with_capacity(plan.parts.len());
        for part in 0..plan.parts.len() {
            let raw: Vec<u8> = conn
                .query_row(
                    "SELECT data FROM mih_directories WHERE part=?1",
                    [part as i64],
                    |r| r.get(0),
                )
                .map_err(|e| e.to_string())?;
            directories.push(decode(&raw)?);
        }
        Ok(Self {
            plan,
            directories,
            postings: PostingCache::default(),
            probes: 0,
            fallbacks: 0,
        })
    }
    pub fn query(
        &mut self,
        conn: &Connection,
        bits: &[u64],
        before: u64,
        mut cancel: impl FnMut() -> Result<(), String>,
    ) -> Result<Option<Candidates>, String> {
        cancel()?;
        if before < self.plan.probes * 2 {
            self.fallbacks += 1;
            return Ok(None);
        }
        conn.execute("DELETE FROM mih_candidates", [])
            .map_err(|e| e.to_string())?;
        let mut collector = Collector::new();
        let mut hits = 0u64;
        let budget = before / 2;
        for part in 0..self.plan.parts.len() {
            let p = &self.plan.parts[part];
            let value = extract(bits, p);
            for mask in Masks::new(p.bits, p.radius) {
                if self.probes % 32 == 0 {
                    cancel()?;
                }
                self.probes += 1;
                let key = (value ^ mask) as usize;
                let start = self.directories[part][key];
                let end = self.directories[part][key + 1];
                let mut position = start;
                while position < end {
                    let page = position / POSTING_IDS as u64;
                    let ids = self.postings.get(conn, (part, page))?;
                    let offset = (position % POSTING_IDS as u64) as usize;
                    if offset >= ids.len() {
                        return Err("Invalid MIH posting range".into());
                    }
                    let length = (end - position).min((ids.len() - offset) as u64) as usize;
                    let mut finished = false;
                    for id in &ids[offset..offset + length] {
                        if *id >= before {
                            finished = true;
                            break;
                        }
                        hits += 1;
                        if hits % 256 == 0 {
                            cancel()?;
                        }
                        if hits > budget {
                            self.fallbacks += 1;
                            return Ok(None);
                        }
                        collector.push(conn, *id)?;
                    }
                    if finished {
                        break;
                    }
                    position += length as u64;
                }
            }
        }
        collector.finish(conn).map(Some)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::duplicates::within_threshold;
    use std::collections::HashSet;
    fn sample(count: usize, words: usize) -> Vec<Vec<u64>> {
        let mut state = 237u64;
        (0..count)
            .map(|_| {
                (0..words)
                    .map(|_| {
                        state = state.wrapping_add(0x9e3779b97f4a7c15);
                        let mut z = state;
                        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
                        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
                        z ^ (z >> 31)
                    })
                    .collect()
            })
            .collect()
    }
    fn index(hashes: &[Vec<u64>], plan: Plan) -> (Connection, Index) {
        let mut conn = Connection::open_in_memory().unwrap();
        conn.execute_batch("PRAGMA temp_store=FILE; CREATE TABLE hash_blocks(block INTEGER PRIMARY KEY,data BLOB);").unwrap();
        for (block, hashes) in hashes.chunks(1024).enumerate() {
            let data: Vec<_> = hashes.iter().flatten().copied().collect();
            conn.execute(
                "INSERT INTO hash_blocks VALUES(?1,?2)",
                params![block as i64, encode(&data)],
            )
            .unwrap();
        }
        let index =
            Index::build(&mut conn, plan, hashes.len(), hashes[0].len(), || Ok(())).unwrap();
        (conn, index)
    }
    fn all(conn: &Connection, candidates: Candidates) -> Vec<u64> {
        match candidates {
            Candidates::Memory(ids) => ids,
            Candidates::Disk => {
                let mut result = Vec::new();
                let mut after = None;
                loop {
                    let batch = Candidates::page(conn, after).unwrap();
                    if batch.is_empty() {
                        break;
                    }
                    after = batch.last().copied();
                    result.extend(batch);
                }
                result
            }
        }
    }
    #[test]
    fn mask_iterator_enumerates_each_nearby_key_once() {
        for bits in 1..=16 {
            for radius in 0..=bits {
                let masks: HashSet<_> = Masks::new(bits, radius).collect();
                assert_eq!(masks.len() as u64, volume(bits, radius));
                assert!(masks.iter().all(
                    |mask| mask.count_ones() <= radius as u32 && (*mask as u32) < 1u32 << bits
                ));
            }
        }
    }
    #[test]
    fn substring_extraction_crosses_word_boundaries_without_losing_bits() {
        let words = &sample(1, 16)[0];
        for offset in 0..1024 {
            for bits in 1..=16 {
                if offset + bits > 1024 {
                    continue;
                }
                let part = Part {
                    offset,
                    bits: bits as u8,
                    radius: 0,
                };
                let mut expected = 0u16;
                for bit in 0..bits {
                    expected |=
                        (((words[(offset + bit) / 64] >> ((offset + bit) % 64)) & 1) as u16) << bit;
                }
                assert_eq!(extract(words, &part), expected);
            }
        }
    }
    #[test]
    fn uneven_probe_radii_cover_all_pairs_in_a_small_hamming_space() {
        for threshold in 0..=8 {
            for tables in 1..=8 {
                let plan = Plan::partition(8, tables, threshold);
                for a in 0u64..256 {
                    for b in 0u64..256 {
                        if (a ^ b).count_ones() > threshold {
                            continue;
                        }
                        assert!(
                            plan.parts
                                .iter()
                                .any(|p| (extract(&[a], p) ^ extract(&[b], p)).count_ones()
                                    <= p.radius as u32),
                            "a={a},b={b},t={threshold},m={tables}"
                        );
                    }
                }
            }
        }
    }
    #[test]
    fn candidate_queries_match_brute_force_for_every_supported_hash_length() {
        for words in [1, 4, 16] {
            for threshold in [1, words as u32 * 8, words as u32 * 16] {
                let mut hashes = sample(6000, words);
                let mut near = hashes[1000].clone();
                near[words - 1] ^= 1;
                hashes[5999] = near;
                let plan = Plan::choose(hashes.len(), words * 64, threshold)
                    .unwrap_or_else(|| Plan::partition(words * 64, words * 4, threshold));
                let (conn, mut index) = index(&hashes, plan);
                for id in [512, 1000, 1023, 1024, 5999] {
                    let expected: Vec<_> = (0..id)
                        .filter(|n| within_threshold(&hashes[id], &hashes[*n], threshold))
                        .map(|n| n as u64)
                        .collect();
                    if let Some(candidates) = index
                        .query(&conn, &hashes[id], id as u64, || Ok(()))
                        .unwrap()
                    {
                        let candidates = all(&conn, candidates);
                        assert!(candidates.windows(2).all(|w| w[0] < w[1]));
                        assert!(candidates.iter().all(|n| *n < id as u64));
                        let exact: Vec<_> = candidates
                            .into_iter()
                            .filter(|n| {
                                within_threshold(&hashes[id], &hashes[*n as usize], threshold)
                            })
                            .collect();
                        assert_eq!(exact, expected);
                    }
                }
            }
        }
    }
    #[test]
    fn broad_or_tiny_searches_avoid_building_an_unhelpful_index() {
        assert!(Plan::choose(100, 256, 32).is_none());
        assert!(Plan::choose(5882, 256, 128).is_none());
        assert!(Plan::choose(5882, 256, 0).is_none());
        for bits in [64, 256, 1024] {
            for count in [1000, 5882, 1000000] {
                if let Some(plan) = Plan::choose(count, bits, (bits / 8) as u32) {
                    assert!(plan.directory_bytes() <= DIRECTORY_BUDGET);
                    assert!(plan.probes <= MAX_PROBES);
                    assert!(plan.parts.iter().all(|p| p.bits <= 16));
                } else {
                    assert!(count < 5882);
                }
            }
        }
    }
    #[test]
    fn dense_postings_fall_back_without_dropping_any_neighbors() {
        let hashes = vec![vec![0; 4]; 1500];
        let plan = Plan::choose(hashes.len(), 256, 32).unwrap();
        let (conn, mut index) = index(&hashes, plan);
        assert!(index
            .query(&conn, &hashes[1400], 1400, || Ok(()))
            .unwrap()
            .is_none());
        assert_eq!(index.fallbacks, 1);
    }
    #[test]
    fn candidate_spill_deduplicates_and_streams_sorted_pages() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TEMP TABLE mih_candidates(id INTEGER PRIMARY KEY) WITHOUT ROWID;",
        )
        .unwrap();
        let mut collector = Collector::new();
        for id in (0..20000).rev() {
            collector.push(&conn, id).unwrap();
            collector.push(&conn, id).unwrap();
            assert!(collector.values.len() < CANDIDATE_IDS);
        }
        let candidates = collector.finish(&conn).unwrap();
        assert!(matches!(&candidates, Candidates::Disk));
        assert_eq!(all(&conn, candidates), (0..20000).collect::<Vec<_>>());
    }
    #[test]
    fn posting_lru_stays_bounded_and_reloads_evicted_pages() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch("CREATE TABLE mih_postings(part INTEGER,page INTEGER,data BLOB,PRIMARY KEY(part,page));").unwrap();
        for page in 0..POSTING_PAGES + 3 {
            conn.execute(
                "INSERT INTO mih_postings VALUES(0,?1,?2)",
                params![page as i64, encode(&[page as u64; POSTING_IDS])],
            )
            .unwrap();
        }
        let mut cache = PostingCache::default();
        for page in 0..POSTING_PAGES + 3 {
            assert_eq!(cache.get(&conn, (0, page as u64)).unwrap()[0], page as u64);
            assert!(cache.pages.len() <= POSTING_PAGES);
            assert!(cache.pages.bytes() <= POSTING_IDS * POSTING_PAGES * 8);
        }
        assert!(!cache.pages.contains((0, 0)));
        assert_eq!(cache.get(&conn, (0, 0)).unwrap()[0], 0);
        assert!(cache.pages.contains((0, 0)));
    }
    #[test]
    fn cancellation_aborts_index_build_and_query() {
        let hashes = sample(1500, 4);
        let plan = Plan::choose(hashes.len(), 256, 32).unwrap();
        let (conn, mut index) = index(&hashes, plan);
        assert!(index
            .query(&conn, &hashes[1499], 1499, || Err("cancelled".into()))
            .is_err());
        let mut conn = Connection::open_in_memory().unwrap();
        conn.execute_batch("CREATE TABLE hash_blocks(block INTEGER PRIMARY KEY,data BLOB);")
            .unwrap();
        assert!(Index::build(
            &mut conn,
            Plan::choose(1500, 256, 32).unwrap(),
            1500,
            4,
            || Err("cancelled".into())
        )
        .is_err());
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
}
