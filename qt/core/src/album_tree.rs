use crate::{media_move::directory_id, settings::read_settings, util::FileLock};
use once_cell::sync::Lazy;
use std::{
    collections::{BTreeSet, HashMap},
    fs,
    path::{Path, PathBuf},
    sync::{Arc, Mutex, Weak},
    time::{Duration, Instant},
};

// Persistent in the chosen library root, outside metadata/cache cleanup. Readers
// find existing ancestor markers; ordinary reads never create locks in parents.
const LOCK: &str = ".room237-tree.lock";

pub(crate) struct TreeLease {
    _readers: Vec<(PathBuf, Arc<FileLock>)>,
    _writer: Option<(PathBuf, FileLock)>,
    markers: BTreeSet<PathBuf>,
}

static READERS: Lazy<Mutex<HashMap<PathBuf, Weak<FileLock>>>> =
    Lazy::new(|| Mutex::new(HashMap::new()));

impl Drop for TreeLease {
    fn drop(&mut self) {
        self._readers.clear();
        // Do not retain path keys for libraries that no longer have readers.
        READERS
            .lock()
            .unwrap()
            .retain(|_, lease| lease.strong_count() > 0);
    }
}

impl TreeLease {
    pub(crate) fn ensure_current(&self, albums: &[&Path]) -> Result<(), String> {
        let (markers, _) = discover(albums, None)?;
        if !markers.is_subset(&self.markers) {
            return Err("Album tree lease appeared during admission; retry operation".into());
        }
        for marker in markers {
            if !self
                ._readers
                .iter()
                .any(|(path, lease)| path == &marker && lease.owns_path(&marker))
                && !self
                    ._writer
                    .as_ref()
                    .is_some_and(|(path, lease)| path == &marker && lease.owns_path(&marker))
            {
                return Err("Album tree lock changed during admission".into());
            }
        }
        Ok(())
    }
}

pub(crate) fn read(albums: &[&Path]) -> Result<TreeLease, String> {
    acquire(albums, None, false)?.ok_or("Album tree is busy".into())
}

pub(crate) fn try_read(albums: &[&Path]) -> Result<Option<TreeLease>, String> {
    acquire(albums, None, true)
}

pub(crate) fn write(root: &Path) -> Result<TreeLease, String> {
    let root = root.canonicalize().map_err(|e| e.to_string())?;
    acquire(&[&root], Some(&root), false)?.ok_or("Album tree is busy".into())
}

pub(crate) fn initialize(root: &Path) -> Result<(), String> {
    let canonical = root.canonicalize().map_err(|e| e.to_string())?;
    let identity = directory_id(&canonical)?;
    let _lease = FileLock::try_acquire_shared(&canonical.join(LOCK))?;
    if directory_id(&canonical)? != identity {
        return Err("Library root changed".into());
    }
    Ok(())
}

type Identities<'a> = Vec<(&'a Path, PathBuf, String)>;

fn discover<'a>(
    albums: &[&'a Path],
    exclusive: Option<&Path>,
) -> Result<(BTreeSet<PathBuf>, Identities<'a>), String> {
    let mut locks = BTreeSet::new();
    let mut identities = Vec::new();
    for requested in albums {
        let album = requested.canonicalize().map_err(|e| e.to_string())?;
        identities.push((*requested, album.clone(), directory_id(&album)?));
        for ancestor in album.ancestors() {
            let lock = ancestor.join(LOCK);
            match fs::symlink_metadata(&lock) {
                Ok(info) if info.is_file() => {
                    locks.insert(lock);
                }
                Ok(_) => return Err("Album tree lock is not a regular file".into()),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                    if Some(ancestor) == exclusive {
                        locks.insert(lock);
                    }
                }
                Err(e) => return Err(e.to_string()),
            }
        }
    }
    Ok((locks, identities))
}

fn try_shared(path: &Path) -> Result<Option<Arc<FileLock>>, String> {
    let cached = READERS
        .lock()
        .unwrap()
        .get(path)
        .and_then(Weak::upgrade)
        .filter(|lease| lease.owns_path(path));
    if cached.is_some() {
        return Ok(cached);
    }
    let Some(lease) = FileLock::try_acquire_shared(path)? else {
        return Ok(None);
    };
    let mut cache = READERS.lock().unwrap();
    // Another thread can publish its shared lease during the system call.
    if let Some(existing) = cache
        .get(path)
        .and_then(Weak::upgrade)
        .filter(|lease| lease.owns_path(path))
    {
        return Ok(Some(existing));
    }
    let lease = Arc::new(lease);
    cache.insert(path.to_path_buf(), Arc::downgrade(&lease));
    Ok(Some(lease))
}

fn acquire(
    albums: &[&Path],
    exclusive: Option<&Path>,
    nonblocking: bool,
) -> Result<Option<TreeLease>, String> {
    let (markers, identities) = discover(albums, exclusive)?;
    let timeout = Duration::from_secs(read_settings().ffmpeg.timeout_secs);
    let started = Instant::now();
    let mut lease = TreeLease {
        _readers: Vec::new(),
        _writer: None,
        markers: markers.clone(),
    };
    // Ancestors precede descendants, and multi-library moves use one order.
    let mut locks: Vec<_> = markers.into_iter().collect();
    locks.sort_by(|a, b| {
        a.components()
            .count()
            .cmp(&b.components().count())
            .then_with(|| a.cmp(b))
    });
    for lock in locks {
        if lock.parent() == exclusive {
            let writer = FileLock::acquire(&lock, timeout, Duration::from_millis(5))?;
            lease._writer = Some((lock, writer));
        } else {
            loop {
                if let Some(shared) = try_shared(&lock)? {
                    lease._readers.push((lock, shared));
                    break;
                }
                if nonblocking {
                    return Ok(None);
                }
                if started.elapsed() >= timeout {
                    return Err("Album tree lease timed out".into());
                }
                std::thread::sleep(Duration::from_millis(5));
            }
        }
    }
    for (requested, canonical, identity) in identities {
        if requested.canonicalize().ok().as_ref() != Some(&canonical)
            || directory_id(&canonical)? != identity
        {
            return Err("Album changed while waiting for tree lease".into());
        }
    }
    Ok(Some(lease))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(unix)]
    #[test]
    fn many_nested_read_leases_share_descriptors_and_release_cache_paths() {
        if std::env::var("ROOM237_TEST_ROLE").as_deref() == Ok("shared-fd-limit") {
            let root = PathBuf::from(std::env::var_os("ROOM237_TEST_DIR").unwrap());
            initialize(&root).unwrap();
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
            let mut readers = Vec::new();
            for _ in 0..400 {
                readers.push(read(&[&root]).unwrap());
            }
            assert!(FileLock::try_acquire(&root.join(LOCK)).unwrap().is_none());
            drop(readers);
            assert!(FileLock::try_acquire(&root.join(LOCK)).unwrap().is_some());
            assert!(!READERS
                .lock()
                .unwrap()
                .contains_key(&root.canonicalize().unwrap().join(LOCK)));
            assert_eq!(unsafe { libc::setrlimit(libc::RLIMIT_NOFILE, &saved) }, 0);
            return;
        }
        let root = tempfile::tempdir().unwrap();
        let mut child = crate::test_support::spawn_test(
            "album_tree::tests::many_nested_read_leases_share_descriptors_and_release_cache_paths",
            root.path(),
            "shared-fd-limit",
        );
        assert!(child.0.wait().unwrap().success());
    }

    #[test]
    fn late_root_marker_rejects_old_leaf_admission_without_writing_metadata() {
        let root = tempfile::tempdir().unwrap();
        let album = root.path().join("album");
        fs::create_dir(&album).unwrap();
        crate::metadata::update_file_meta(&album, "a.jpg", |entry| entry.favorite = true).unwrap();
        let before = fs::read(album.join(".room237-metadata/album.sqlite")).unwrap();
        let admission = read(&[&album]).unwrap();
        let writer = write(root.path()).unwrap();
        assert!(crate::metadata::lock_metadata_for_move_with_tree(&[&album], &admission).is_err());
        assert!(
            crate::metadata::update_file_meta(&album, "a.jpg", |entry| entry.favorite = false)
                .is_err()
        );
        assert_eq!(
            fs::read(album.join(".room237-metadata/album.sqlite")).unwrap(),
            before
        );
        assert!(FileLock::try_acquire(&album.join(".room237-metadata.lock"))
            .unwrap()
            .is_some());
        drop(writer);
        crate::metadata::update_file_meta(&album, "a.jpg", |entry| entry.favorite = false).unwrap();
        assert!(
            !crate::metadata::read_album_meta_checked(&album)
                .unwrap()
                .files["a.jpg"]
                .favorite
        );
    }

    #[cfg(unix)]
    #[test]
    fn cached_reader_cannot_authorize_a_replaced_marker() {
        let root = tempfile::tempdir().unwrap();
        initialize(root.path()).unwrap();
        let reader = read(&[root.path()]).unwrap();
        fs::rename(root.path().join(LOCK), root.path().join("old-lock")).unwrap();
        fs::write(root.path().join(LOCK), b"new marker").unwrap();
        assert!(reader.ensure_current(&[root.path()]).is_err());
        let current = read(&[root.path()]).unwrap();
        assert!(FileLock::try_acquire(&root.path().join(LOCK))
            .unwrap()
            .is_none());
        drop(current);
        assert!(FileLock::try_acquire(&root.path().join(LOCK))
            .unwrap()
            .is_some());
        drop(reader);
        assert_eq!(fs::read(root.path().join(LOCK)).unwrap(), b"new marker");
    }

    #[test]
    fn reading_an_album_does_not_create_markers_in_it_or_its_ancestors() {
        let root = tempfile::tempdir().unwrap();
        let album = root.path().join("album");
        fs::create_dir(&album).unwrap();
        let _reader = read(&[&album]).unwrap();
        assert!(!root.path().join(LOCK).exists());
        assert!(fs::read_dir(&album).unwrap().next().is_none());
    }

    #[test]
    fn nested_library_writer_obeys_the_parent_library_lease_and_releases_on_death() {
        if let Ok(root) = std::env::var("ROOM237_TEST_DIR") {
            let root = Path::new(&root);
            fs::write(root.join("started"), b"started").unwrap();
            let _writer = write(&root.join(".nested")).unwrap();
            fs::write(root.join("held"), b"held").unwrap();
            loop {
                std::thread::sleep(Duration::from_millis(10));
            }
        }
        let root = tempfile::tempdir().unwrap();
        fs::create_dir(root.path().join(".nested")).unwrap();
        let writer = write(root.path()).unwrap();
        let mut child = crate::test_support::spawn_test(
            "album_tree::tests::nested_library_writer_obeys_the_parent_library_lease_and_releases_on_death",
            root.path(), "nested-writer",
        );
        crate::test_support::wait_for_file(&root.path().join("started"));
        std::thread::sleep(Duration::from_millis(200));
        assert!(!root.path().join("held").exists());
        assert!(
            !root.path().join(".nested").join(LOCK).exists(),
            "parent lease must precede child lease creation"
        );
        drop(writer);
        crate::test_support::wait_for_file(&root.path().join("held"));
        assert!(FileLock::try_acquire(&root.path().join(LOCK))
            .unwrap()
            .is_none());
        child.0.kill().unwrap();
        child.0.wait().unwrap();
        let _writer = write(root.path()).unwrap();
        assert!(
            FileLock::try_acquire(&root.path().join(".nested").join(LOCK))
                .unwrap()
                .is_some()
        );
    }

    #[cfg(unix)]
    #[test]
    fn tree_lock_symlink_does_not_lock_or_modify_an_external_file() {
        let root = tempfile::tempdir().unwrap();
        let foreign = root.path().join("foreign");
        fs::write(&foreign, b"foreign bytes").unwrap();
        std::os::unix::fs::symlink(&foreign, root.path().join(LOCK)).unwrap();
        assert!(read(&[root.path()]).is_err());
        assert!(write(root.path()).is_err());
        assert_eq!(fs::read(&foreign).unwrap(), b"foreign bytes");
        assert!(FileLock::try_acquire(&foreign).unwrap().is_some());
    }
}
