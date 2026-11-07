use crate::{metadata, util};
use serde::{Deserialize, Serialize};
use std::{
    fs::{self, File, OpenOptions},
    io::{BufRead, BufReader, Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
};

const PREFIX: &str = ".room237-move-";
const HEADER: &str = "header.json";
const EVENTS: &str = "events.jsonl";
const HEADER_LIMIT: u64 = 16 * 1024;
const RECORD_LIMIT: u64 = 16 * 1024 * 1024;
const JOURNAL_LIMIT: u64 = 128 * 1024 * 1024;

#[derive(Serialize, Deserialize, PartialEq, Eq)]
struct Header {
    version: u8,
    source: PathBuf,
    target: PathBuf,
    source_id: String,
    target_id: String,
}

#[derive(Serialize, Deserialize)]
enum Event {
    File {
        source: String,
        target: String,
        revision: String,
    },
    Artifact(Artifact),
    Metadata(metadata::MoveMetadataUndo),
    Committed,
    RolledBack,
}

#[derive(Serialize, Deserialize, Clone, Copy, PartialEq, Eq, Hash)]
enum ArtifactKind {
    Preview,
    LegacyMetadata,
}

impl ArtifactKind {
    fn directory(self) -> &'static str {
        match self {
            Self::Preview => ".room237-thumb",
            Self::LegacyMetadata => ".room237-metadata",
        }
    }

    fn path(self, album: &Path, name: &str) -> Result<PathBuf, String> {
        let dir = album.join(self.directory());
        match self {
            Self::Preview => crate::thumb::thumb_path(&album.join(name), &dir),
            Self::LegacyMetadata => Ok(dir.join(format!("{name}.meta"))),
        }
    }

    fn valid_name(self, media: &str, file: &str) -> bool {
        match self {
            Self::Preview => crate::thumb::deleted_thumbnail_paths(media, Path::new(""))
                .iter()
                .any(|path| path.file_name().is_some_and(|name| name == file)),
            Self::LegacyMetadata => file == format!("{media}.meta"),
        }
    }
}

#[derive(Serialize, Deserialize)]
struct Artifact {
    kind: ArtifactKind,
    source: String,
    target: String,
    media_revision: String,
    source_file: String,
    target_file: String,
    revision: String,
    source_directory_id: String,
}

impl Artifact {
    fn validate(&self) -> Result<(), String> {
        for name in [
            &self.source,
            &self.target,
            &self.source_file,
            &self.target_file,
        ] {
            util::validate_file_name(name)?;
        }
        if self.media_revision.is_empty()
            || self.revision.is_empty()
            || self.source_directory_id.is_empty()
            || !self.kind.valid_name(&self.source, &self.source_file)
            || !self.kind.valid_name(&self.target, &self.target_file)
        {
            return Err("Invalid move artifact".into());
        }
        Ok(())
    }
}

fn regular_revision(path: &Path) -> Option<String> {
    let info = fs::symlink_metadata(path).ok()?;
    info.is_file()
        .then(|| metadata::source_revision_from_info(&info))
        .flatten()
}

pub(crate) fn directory_id(path: &Path) -> Result<String, String> {
    let info = fs::symlink_metadata(path).map_err(|e| e.to_string())?;
    if !info.is_dir() {
        return Err("Move album is not a regular directory".into());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        Ok(format!("{}-{}", info.dev(), info.ino()))
    }
    #[cfg(windows)]
    {
        use std::os::windows::{fs::OpenOptionsExt, io::AsRawHandle};
        #[repr(C)]
        #[derive(Default)]
        struct FileInformation {
            attributes: u32,
            created: [u32; 2],
            accessed: [u32; 2],
            written: [u32; 2],
            volume: u32,
            size_high: u32,
            size_low: u32,
            links: u32,
            index_high: u32,
            index_low: u32,
        }
        extern "system" {
            fn GetFileInformationByHandle(handle: *mut std::ffi::c_void, information: *mut FileInformation) -> i32;
        }
        const BACKUP_SEMANTICS: u32 = 0x0200_0000; // Required to open a directory.
        let file = fs::OpenOptions::new().read(true).custom_flags(BACKUP_SEMANTICS).open(path).map_err(|e| e.to_string())?;
        let mut information = FileInformation::default();
        if unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut information) } == 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
        Ok(format!("{}-{}-{}", information.volume, information.index_high, information.index_low))
    }
}

fn sync_dir(path: &Path) -> Result<(), String> {
    #[cfg(unix)]
    File::open(path)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    Ok(())
}

fn sync_albums(header: &Header) -> Result<(), String> {
    for album in [&header.source, &header.target] {
        // Metadata files are synced at atomic publication; their directory
        // renames and the original-media renames must precede the commit record.
        let meta = album.join(".room237-metadata");
        if fs::symlink_metadata(&meta).is_ok_and(|info| info.is_dir()) {
            sync_dir(&meta)?;
        }
        sync_dir(album)?;
    }
    Ok(())
}

fn regular_file(path: &Path) -> Result<File, String> {
    if !fs::symlink_metadata(path).is_ok_and(|info| info.is_file()) {
        return Err(format!(
            "Move journal is not a regular file: {}",
            path.display()
        ));
    }
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW);
    }
    options.open(path).map_err(|e| e.to_string())
}

fn read_header(staging: &Path) -> Result<Header, String> {
    if !fs::symlink_metadata(staging).is_ok_and(|info| info.is_dir()) {
        return Err("Move staging is not a regular directory".into());
    }
    let mut bytes = Vec::new();
    regular_file(&staging.join(HEADER))?
        .take(HEADER_LIMIT + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() as u64 > HEADER_LIMIT {
        return Err("Move header too large".into());
    }
    serde_json::from_slice(&bytes).map_err(|e| format!("Invalid move header: {e}"))
}

impl Header {
    fn validate(&self, album: &Path) -> Result<(), String> {
        if self.version != 1
            || self.source == self.target
            || (album != self.source && album != self.target)
            || self.source.canonicalize().ok().as_ref() != Some(&self.source)
            || self.target.canonicalize().ok().as_ref() != Some(&self.target)
            || directory_id(&self.source)? != self.source_id
            || directory_id(&self.target)? != self.target_id
        {
            return Err("Move album identity changed; preserving journal".into());
        }
        Ok(())
    }
}

// Each group is appended and synced before its first rename. Appending keeps
// a large batch linear in journal size rather than rewriting all prior moves.
pub(crate) struct MoveJournal {
    header: Header,
    staging: PathBuf,
    peer: PathBuf,
    events: File,
    failed: bool,
}

impl MoveJournal {
    pub(crate) fn create(source: &Path, target: &Path) -> Result<Self, String> {
        let header = Header {
            version: 1,
            source: source.into(),
            target: target.into(),
            source_id: directory_id(source)?,
            target_id: directory_id(target)?,
        };
        let bytes = serde_json::to_vec(&header).map_err(|e| e.to_string())?;
        if bytes.len() as u64 > HEADER_LIMIT {
            return Err("Move header too large".into());
        }
        let staging = tempfile::Builder::new()
            .prefix(PREFIX)
            .tempdir_in(source)
            .map_err(|e| e.to_string())?
            .keep();
        let peer = target.join(staging.file_name().ok_or("Missing journal name")?);
        // The peer is a locator, not another independently committed journal.
        util::atomic_write_owned(&staging.join(HEADER), &bytes)?;
        let events = OpenOptions::new()
            .create_new(true)
            .read(true)
            .write(true)
            .open(staging.join(EVENTS))
            .map_err(|e| e.to_string())?;
        fs::create_dir(&peer).map_err(|e| e.to_string())?;
        util::atomic_write_owned(&peer.join(HEADER), &bytes)?;
        for dir in [&staging, &peer, source, target] {
            sync_dir(dir)?;
        }
        Ok(Self {
            header,
            staging,
            peer,
            events,
            failed: false,
        })
    }

    fn append(&mut self, event: &Event) -> Result<(), String> {
        if self.failed {
            return Err("Move journal write failed".into());
        }
        let mut bytes = serde_json::to_vec(event).map_err(|e| e.to_string())?;
        bytes.push(b'\n');
        let current = self.events.metadata().map_err(|e| e.to_string())?.len();
        let limit = if matches!(event, Event::Committed | Event::RolledBack) {
            JOURNAL_LIMIT
        } else {
            JOURNAL_LIMIT - 32
        };
        if bytes.len() as u64 > RECORD_LIMIT || current + bytes.len() as u64 > limit {
            return Err("Move journal is too large".into());
        }
        if let Err(error) = self.events.write_all(&bytes) {
            self.failed = true;
            let _ = self.events.set_len(current);
            let _ = self.events.seek(SeekFrom::End(0));
            return Err(error.to_string());
        }
        Ok(())
    }

    fn sync(&mut self) -> Result<(), String> {
        self.events.sync_all().map_err(|error| {
            self.failed = true;
            error.to_string()
        })
    }

    #[cfg(test)]
    pub(crate) fn record_files(
        &mut self,
        files: &[(String, String, String)],
    ) -> Result<(), String> {
        self.record_files_with_artifacts(files, false)
    }

    pub(crate) fn record_files_with_artifacts(
        &mut self,
        files: &[(String, String, String)],
        move_artifacts: bool,
    ) -> Result<(), String> {
        for (source, target, revision) in files {
            self.append(&Event::File {
                source: source.clone(),
                target: target.clone(),
                revision: revision.clone(),
            })?;
            if move_artifacts {
                for kind in [ArtifactKind::Preview, ArtifactKind::LegacyMetadata] {
                    let source_dir = self.header.source.join(kind.directory());
                    // Never follow a derived directory or file outside the album.
                    let Ok(source_directory_id) = directory_id(&source_dir) else {
                        continue;
                    };
                    let path = kind.path(&self.header.source, source)?;
                    let Some(artifact_revision) = regular_revision(&path) else {
                        continue;
                    };
                    let destination = kind.path(&self.header.target, target)?;
                    self.append(&Event::Artifact(Artifact {
                        kind,
                        source: source.clone(),
                        target: target.clone(),
                        media_revision: revision.clone(),
                        source_file: path
                            .file_name()
                            .ok_or("Missing artifact name")?
                            .to_string_lossy()
                            .into_owned(),
                        target_file: destination
                            .file_name()
                            .ok_or("Missing artifact name")?
                            .to_string_lossy()
                            .into_owned(),
                        revision: artifact_revision,
                        source_directory_id,
                    }))?;
                }
            }
        }
        self.sync()
    }

    pub(crate) fn record_metadata(
        &mut self,
        undo: metadata::MoveMetadataUndo,
    ) -> Result<(), String> {
        self.append(&Event::Metadata(undo))?;
        self.sync()
    }

    pub(crate) fn commit(
        &mut self,
        checkpoint: impl FnOnce() -> Result<(), String>,
    ) -> Result<(), String> {
        sync_albums(&self.header)?;
        self.append(&Event::Committed)?;
        self.sync()?;
        checkpoint()?;
        Ok(())
    }

    pub(crate) fn finish(
        &self,
        checkpoint: impl Fn(&str) -> Result<(), String>,
    ) -> Result<(), String> {
        let (events, _) = read_events(&self.staging)?;
        if !matches!(events.last(), Some(Event::Committed)) {
            return Err("Move is not committed".into());
        }
        finish_artifacts(&self.header, &events, checkpoint)?;
        cleanup(&self.staging, &self.peer)
    }

    pub(crate) fn rollback(
        &mut self,
        guard: Option<&metadata::MetadataWriteGuard>,
    ) -> Result<(), String> {
        let (events, valid_len) = read_events(&self.staging)?;
        validate_undo(&self.header, &events, guard)?;
        rollback_files(&self.header, &events)?;
        for event in events.iter().rev() {
            if let Event::Metadata(undo) = event {
                metadata::restore_move_metadata_with_guard(
                    &self.header.source,
                    &self.header.target,
                    undo,
                    guard.ok_or("Missing metadata lease")?,
                )?;
            }
        }
        self.events.set_len(valid_len).map_err(|e| e.to_string())?;
        self.events
            .seek(SeekFrom::End(0))
            .map_err(|e| e.to_string())?;
        self.failed = false;
        sync_albums(&self.header)?;
        self.append(&Event::RolledBack)?;
        self.sync()?;
        cleanup(&self.staging, &self.peer)
    }
}

fn remove_known_directory(path: &Path, names: &[&str]) -> Result<(), String> {
    if !path.exists() {
        return Ok(());
    }
    if !fs::symlink_metadata(path).is_ok_and(|info| info.is_dir()) {
        return Err("Move staging changed; preserving it".into());
    }
    for entry in fs::read_dir(path).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if !names.iter().any(|name| entry.file_name() == *name)
            || !entry.file_type().map_err(|e| e.to_string())?.is_file()
        {
            return Err("Unknown files in move journal; preserving them".into());
        }
    }
    for name in names {
        match fs::remove_file(path.join(name)) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(e.to_string()),
        }
    }
    fs::remove_dir(path).map_err(|e| e.to_string())?;
    sync_dir(path.parent().ok_or("Missing journal parent")?)
}

fn cleanup(staging: &Path, peer: &Path) -> Result<(), String> {
    // Remove the locator first, retaining the authoritative commit record until
    // there is no peer that could request recovery through it.
    remove_known_directory(peer, &[HEADER])?;
    remove_known_directory(staging, &[EVENTS, HEADER])
}

fn quarantine_incomplete(path: &Path) -> Result<(), String> {
    let name = path
        .file_name()
        .ok_or("Missing journal name")?
        .to_string_lossy();
    let quarantine = path.with_file_name(format!(".room237-unfinished-{name}"));
    util::rename_noclobber(path, &quarantine).map_err(|e| e.to_string())?;
    sync_dir(path.parent().ok_or("Missing journal parent")?)?;
    log::warn!(
        "Preserved incomplete move staging in {}",
        quarantine.display()
    );
    Ok(())
}

fn read_events(staging: &Path) -> Result<(Vec<Event>, u64), String> {
    let path = staging.join(EVENTS);
    if matches!(fs::symlink_metadata(&path), Err(e) if e.kind() == std::io::ErrorKind::NotFound) {
        return Ok((Vec::new(), 0));
    }
    let file = regular_file(&path)?;
    if file.metadata().map_err(|e| e.to_string())?.len() > JOURNAL_LIMIT {
        return Err("Move journal is too large".into());
    }
    let mut reader = BufReader::new(file);
    let mut events = Vec::new();
    let mut valid_len = 0;
    loop {
        let mut line = Vec::new();
        Read::take(&mut reader, RECORD_LIMIT + 1)
            .read_until(b'\n', &mut line)
            .map_err(|e| e.to_string())?;
        if line.len() as u64 > RECORD_LIMIT {
            return Err("Move journal record too large".into());
        }
        if line.is_empty() {
            break;
        }
        // A process can die during append. That incomplete final record was
        // never synced, so the operations it describes were not started.
        if line.last() != Some(&b'\n') {
            break;
        }
        let event: Event =
            serde_json::from_slice(&line).map_err(|e| format!("Invalid move journal: {e}"))?;
        match &event {
            Event::File {
                source,
                target,
                revision,
            } => {
                util::validate_file_name(source)?;
                util::validate_file_name(target)?;
                if revision.is_empty() {
                    return Err("Empty move revision".into());
                }
            }
            Event::Metadata(undo) => undo.validate()?,
            Event::Artifact(artifact) => artifact.validate()?,
            Event::Committed | Event::RolledBack => {}
        }
        if events
            .last()
            .is_some_and(|event| matches!(event, Event::Committed | Event::RolledBack))
        {
            return Err("Events after move commit".into());
        }
        valid_len += line.len() as u64;
        events.push(event);
    }
    Ok((events, valid_len))
}

fn finish_artifacts(
    header: &Header,
    events: &[Event],
    checkpoint: impl Fn(&str) -> Result<(), String>,
) -> Result<(), String> {
    let mut seen = std::collections::HashSet::new();
    // A retry can pick another collision suffix; only its latest intent counts.
    for event in events.iter().rev() {
        let Event::Artifact(artifact) = event else {
            continue;
        };
        if !seen.insert((artifact.kind, &artifact.source)) {
            continue;
        }
        // Initial plans include failed groups. Only committed original renames
        // authorize their derived artifacts; preserve external replacements.
        if !matches!(fs::symlink_metadata(header.source.join(&artifact.source)),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound)
            || regular_revision(&header.target.join(&artifact.target)).as_deref()
                != Some(&artifact.media_revision)
        {
            continue;
        }
        let source_dir = header.source.join(artifact.kind.directory());
        if directory_id(&source_dir).as_deref() != Ok(&artifact.source_directory_id) {
            continue;
        }
        let source = source_dir.join(&artifact.source_file);
        let target_dir = header.target.join(artifact.kind.directory());
        let target = target_dir.join(&artifact.target_file);
        // Absence also covers a previous recovery interrupted after this rename.
        if regular_revision(&source).as_deref() != Some(&artifact.revision) {
            continue;
        }
        match fs::create_dir(&target_dir) {
            Ok(()) => sync_dir(&header.target)?,
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {}
            Err(error) => return Err(error.to_string()),
        }
        if directory_id(&target_dir).is_err() {
            log::warn!("Preserving move artifact: target directory changed");
            continue;
        }
        match util::rename_noclobber(&source, &target) {
            Ok(()) => checkpoint("move_artifact_renamed")?,
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {
                log::warn!("Preserving conflicting move artifact {}", source.display());
            }
            Err(error) => return Err(error.to_string()),
        }
    }
    // Also sync after resuming an already-performed rename, before removing the
    // only remaining journal that knows it needs durable directory entries.
    for album in [&header.source, &header.target] {
        for kind in [ArtifactKind::Preview, ArtifactKind::LegacyMetadata] {
            let dir = album.join(kind.directory());
            if directory_id(&dir).is_ok() {
                sync_dir(&dir)?;
            }
        }
        sync_dir(album)?;
    }
    Ok(())
}

fn rollback_files(header: &Header, events: &[Event]) -> Result<(), String> {
    // A failed group can be retried through its other pair member. Its latest
    // intent supersedes earlier destinations that were already rolled back.
    let mut seen = std::collections::HashSet::new();
    let files: Vec<_> = events
        .iter()
        .rev()
        .filter_map(|event| {
            let Event::File {
                source,
                target,
                revision,
            } = event
            else {
                return None;
            };
            seen.insert(source).then_some((source, target, revision))
        })
        .collect();
    let mut errors = Vec::new();
    for (source, target, revision) in &files {
        let original = header.source.join(source);
        let moved = header.target.join(target);
        if fs::symlink_metadata(&original).is_ok_and(|info| {
            info.is_file()
                && metadata::source_revision_from_info(&info).as_deref() == Some(revision)
        }) {
            continue;
        }
        if fs::symlink_metadata(&original).is_ok()
            || !fs::symlink_metadata(&moved).is_ok_and(|info| {
                info.is_file()
                    && metadata::source_revision_from_info(&info).as_deref() == Some(revision)
            })
        {
            errors.push(format!(
                "Move originals changed; preserving {source} and {target}"
            ));
        }
    }
    if !errors.is_empty() {
        return Err(errors.join("; "));
    }
    for (source, target, revision) in files {
        let original = header.source.join(source);
        let moved = header.target.join(target);
        if fs::symlink_metadata(&original).is_ok_and(|info| {
            info.is_file()
                && metadata::source_revision_from_info(&info).as_deref() == Some(revision)
        }) {
            continue;
        }
        if !fs::symlink_metadata(&moved).is_ok_and(|info| {
            info.is_file()
                && metadata::source_revision_from_info(&info).as_deref() == Some(revision)
        }) {
            errors.push(format!(
                "Moved original changed or missing; preserving {target}"
            ));
            continue;
        }
        if let Err(e) = util::rename_noclobber(&moved, &original) {
            errors.push(format!("Cannot restore {source}: {e}"));
        }
    }
    for dir in [&header.source, &header.target] {
        sync_dir(dir)?;
    }
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors.join("; "))
    }
}

fn validate_undo(
    header: &Header,
    events: &[Event],
    guard: Option<&metadata::MetadataWriteGuard>,
) -> Result<(), String> {
    for event in events {
        if let Event::Metadata(undo) = event {
            metadata::validate_move_metadata_undo_with_guard(
                &header.source,
                &header.target,
                undo,
                guard.ok_or("Missing metadata lease")?,
            )?;
        }
    }
    Ok(())
}

pub(crate) fn recover_album(album: &Path) -> Result<(), String> {
    recover_album_with_hook(album, |_| Ok(()))
}

pub(crate) fn has_journal(album: &Path) -> Result<bool, String> {
    for entry in fs::read_dir(album).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if entry.file_name().to_string_lossy().starts_with(PREFIX)
            && entry.file_type().map_err(|e| e.to_string())?.is_dir()
        {
            return Ok(true);
        }
    }
    Ok(false)
}

pub(crate) fn recover_album_with_hook(
    album: &Path,
    checkpoint: impl Fn(&str) -> Result<(), String>,
) -> Result<(), String> {
    let album = album.canonicalize().map_err(|e| e.to_string())?;
    let local_tree = crate::album_tree::read(&[&album])?;
    let mut staging_dirs = Vec::new();
    for entry in fs::read_dir(&album).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if entry.file_name().to_string_lossy().starts_with(PREFIX)
            && entry.file_type().map_err(|e| e.to_string())?.is_dir()
        {
            staging_dirs.push(entry.path());
        }
    }
    for locator in staging_dirs {
        // A live constructor also holds this lease. Do not inspect or remove
        // its temporarily empty locator while it is still creating the header.
        let header = {
            let _local = crate::media_delete::lock_maintenance(&album)?;
            local_tree.ensure_current(&[&album])?;
            if !locator.exists() {
                continue;
            }
            if fs::read_dir(&locator)
                .map_err(|e| e.to_string())?
                .next()
                .is_none()
            {
                fs::remove_dir(&locator).map_err(|e| e.to_string())?;
                sync_dir(&album)?;
                continue;
            }
            if !locator.join(HEADER).exists() {
                quarantine_incomplete(&locator)?;
                continue;
            }
            read_header(&locator)?
        };
        checkpoint("move_recovery_header_read")?;
        if !locator.exists() {
            continue;
        }
        if let Err(error) = header.validate(&album) {
            if !locator.exists() {
                continue;
            }
            return Err(error);
        }
        let name = locator.file_name().ok_or("Missing journal name")?;
        let staging = header.source.join(name);
        let peer = header.target.join(name);
        let mut dirs = [header.source.as_path(), header.target.as_path()];
        dirs.sort();
        let tree = match crate::album_tree::read(&dirs) {
            Ok(tree) => tree,
            Err(_) if !locator.exists() => continue,
            Err(error) => return Err(error),
        };
        let _maintenance = dirs
            .iter()
            .map(|dir| crate::media_delete::lock_maintenance(dir))
            .collect::<Result<Vec<_>, _>>()?;
        // The originating operation can finish while these leases are awaited.
        if !locator.exists() {
            continue;
        }
        header.validate(&album)?;
        tree.ensure_current(&dirs)?;
        if read_header(&staging)? != header {
            return Err("Move journal header mismatch".into());
        }
        let (events, valid_len) = read_events(&staging)?;
        let settled = events
            .last()
            .is_some_and(|event| matches!(event, Event::Committed | Event::RolledBack));
        if peer.exists() {
            match read_header(&peer) {
                Ok(peer_header) if peer_header == header => {
                    if !staging.join(EVENTS).exists() {
                        return Err("Move events missing".into());
                    }
                }
                Ok(_) if events.is_empty() => {
                    remove_known_directory(&staging, &[EVENTS, HEADER])?;
                    continue;
                }
                Err(_) if events.is_empty() => {
                    quarantine_incomplete(&peer)?;
                    remove_known_directory(&staging, &[EVENTS, HEADER])?;
                    continue;
                }
                _ => return Err("Move peer header mismatch".into()),
            }
        } else if !settled && !events.is_empty() {
            return Err("Move locator missing; preserving originals".into());
        }
        let has_metadata = !settled
            && events
                .iter()
                .any(|event| matches!(event, Event::Metadata(_)));
        let (_move, guard, _store) = if has_metadata {
            let (moving, guard) = metadata::lock_metadata_for_move_with_tree(&dirs, &tree)?;
            (moving, Some(guard), None)
        } else {
            (
                util::MEDIA_MOVE_LOCK.lock().unwrap(),
                None,
                Some(util::STORE_WRITE_LOCK.lock().unwrap()),
            )
        };
        if !settled {
            validate_undo(&header, &events, guard.as_ref())?;
            rollback_files(&header, &events)?;
            checkpoint("move_recovery_files_restored")?;
            for event in events.iter().rev() {
                if let Event::Metadata(undo) = event {
                    metadata::restore_move_metadata_with_hooks(
                        &header.source,
                        &header.target,
                        undo,
                        guard.as_ref().ok_or("Missing metadata lease")?,
                        |phase| checkpoint(phase),
                    )?;
                }
            }
            sync_albums(&header)?;
            if staging.join(EVENTS).exists() {
                let mut options = OpenOptions::new();
                options.write(true);
                #[cfg(unix)]
                {
                    use std::os::unix::fs::OpenOptionsExt;
                    options.custom_flags(libc::O_NOFOLLOW);
                }
                let mut file = options
                    .open(staging.join(EVENTS))
                    .map_err(|e| e.to_string())?;
                file.set_len(valid_len).map_err(|e| e.to_string())?;
                file.seek(SeekFrom::End(0)).map_err(|e| e.to_string())?;
                file.write_all(b"\"RolledBack\"\n")
                    .map_err(|e| e.to_string())?;
                file.sync_all().map_err(|e| e.to_string())?;
                checkpoint("move_recovery_settled")?;
            }
        }
        if matches!(events.last(), Some(Event::Committed)) {
            finish_artifacts(&header, &events, |phase| checkpoint(phase))?;
        }
        remove_known_directory(&peer, &[HEADER])?;
        checkpoint("move_recovery_peer_removed")?;
        remove_known_directory(&staging, &[EVENTS, HEADER])?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> (tempfile::TempDir, PathBuf, PathBuf, MoveJournal) {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("source");
        let target = root.path().join("target");
        fs::create_dir(&source).unwrap();
        fs::create_dir(&target).unwrap();
        let source = source.canonicalize().unwrap();
        let target = target.canonicalize().unwrap();
        fs::write(source.join("a.jpg"), b"original").unwrap();
        let mut journal = MoveJournal::create(&source, &target).unwrap();
        journal
            .record_files(&[(
                "a.jpg".into(),
                "b.jpg".into(),
                metadata::source_revision(&source.join("a.jpg")).unwrap(),
            )])
            .unwrap();
        util::rename_noclobber(&source.join("a.jpg"), &target.join("b.jpg")).unwrap();
        (root, source, target, journal)
    }

    #[test]
    fn obsolete_locator_header_does_not_fail_after_another_recovery_and_tree_rename() {
        let root = tempfile::tempdir().unwrap();
        let a = root.path().join("first-library");
        let b = root.path().join("second-library");
        let source = a.join("source");
        let target = b.join("target");
        fs::create_dir_all(&source).unwrap();
        fs::create_dir_all(&target).unwrap();
        crate::album_tree::initialize(&a).unwrap();
        crate::album_tree::initialize(&b).unwrap();
        let source = source.canonicalize().unwrap();
        let target = target.canonicalize().unwrap();
        fs::write(source.join("a.jpg"), b"original").unwrap();
        let mut journal = MoveJournal::create(&source, &target).unwrap();
        journal
            .record_files(&[(
                "a.jpg".into(),
                "b.jpg".into(),
                regular_revision(&source.join("a.jpg")).unwrap(),
            )])
            .unwrap();
        util::rename_noclobber(&source.join("a.jpg"), &target.join("b.jpg")).unwrap();
        drop(journal);
        recover_album_with_hook(&target, |phase| {
            if phase == "move_recovery_header_read" {
                recover_album(&source)?;
                crate::album::rename_album(
                    a.to_string_lossy().into(),
                    "source".into(),
                    "renamed".into(),
                )?;
            }
            Ok(())
        })
        .unwrap();
        assert_eq!(fs::read(a.join("renamed/a.jpg")).unwrap(), b"original");
        assert!(!target.join("b.jpg").exists());
        assert!(!has_journal(&target).unwrap());
    }

    fn committed_artifact_fixture() -> (tempfile::TempDir, PathBuf, PathBuf) {
        let (root, source, target, mut journal) = fixture();
        fs::create_dir(source.join(".room237-thumb")).unwrap();
        fs::write(source.join(".room237-thumb/a.jpg.webp"), b"preview").unwrap();
        journal
            .record_files_with_artifacts(
                &[(
                    "a.jpg".into(),
                    "b.jpg".into(),
                    regular_revision(&target.join("b.jpg")).unwrap(),
                )],
                true,
            )
            .unwrap();
        assert!(journal
            .commit(|| Err("simulated interruption".into()))
            .is_err());
        (root, source, target)
    }

    #[test]
    fn committed_artifact_recovery_preserves_foreign_target_cache() {
        let (_root, source, target) = committed_artifact_fixture();
        fs::create_dir(target.join(".room237-thumb")).unwrap();
        fs::write(target.join(".room237-thumb/b.jpg.webp"), b"foreign preview").unwrap();
        recover_album(&target).unwrap();
        assert_eq!(
            fs::read(target.join(".room237-thumb/b.jpg.webp")).unwrap(),
            b"foreign preview"
        );
        assert_eq!(
            fs::read(source.join(".room237-thumb/a.jpg.webp")).unwrap(),
            b"preview"
        );
        assert!(!has_journal(&source).unwrap());
    }

    #[test]
    fn committed_artifact_recovery_preserves_changed_cache_and_originals() {
        for change in ["cache", "source", "target", "directory"] {
            let (root, source, target) = committed_artifact_fixture();
            match change {
                "cache" => fs::write(
                    source.join(".room237-thumb/a.jpg.webp"),
                    b"external cache edit",
                )
                .unwrap(),
                "source" => fs::write(source.join("a.jpg"), b"external source").unwrap(),
                "target" => fs::write(target.join("b.jpg"), b"external target").unwrap(),
                "directory" => {
                    fs::rename(
                        source.join(".room237-thumb"),
                        root.path().join("previous-thumbs"),
                    )
                    .unwrap();
                    fs::create_dir(source.join(".room237-thumb")).unwrap();
                    fs::write(
                        source.join(".room237-thumb/a.jpg.webp"),
                        b"foreign directory cache",
                    )
                    .unwrap();
                }
                _ => unreachable!(),
            }
            let before = fs::read(source.join(".room237-thumb/a.jpg.webp")).unwrap();
            recover_album(&target).unwrap();
            assert_eq!(
                fs::read(source.join(".room237-thumb/a.jpg.webp")).unwrap(),
                before
            );
            assert!(!target.join(".room237-thumb/b.jpg.webp").exists());
            if change == "source" {
                assert_eq!(fs::read(source.join("a.jpg")).unwrap(), b"external source");
            }
            if change == "target" {
                assert_eq!(fs::read(target.join("b.jpg")).unwrap(), b"external target");
            }
        }
    }

    #[cfg(unix)]
    #[test]
    fn committed_artifact_recovery_does_not_follow_target_cache_directory_symlink() {
        let (root, source, target) = committed_artifact_fixture();
        let outside = root.path().join("outside");
        fs::create_dir(&outside).unwrap();
        fs::write(outside.join("foreign.webp"), b"foreign").unwrap();
        std::os::unix::fs::symlink(&outside, target.join(".room237-thumb")).unwrap();
        recover_album(&target).unwrap();
        assert_eq!(fs::read(outside.join("foreign.webp")).unwrap(), b"foreign");
        assert!(!outside.join("b.jpg.webp").exists());
        assert_eq!(
            fs::read(source.join(".room237-thumb/a.jpg.webp")).unwrap(),
            b"preview"
        );
    }

    #[test]
    fn committed_artifact_recovery_can_resume_after_one_rename() {
        let (_root, source, target) = committed_artifact_fixture();
        assert!(recover_album_with_hook(&target, |phase| {
            if phase == "move_artifact_renamed" {
                Err("simulated interruption".into())
            } else {
                Ok(())
            }
        })
        .is_err());
        assert!(has_journal(&target).unwrap());
        recover_album(&target).unwrap();
        recover_album(&source).unwrap();
        assert_eq!(
            fs::read(target.join(".room237-thumb/b.jpg.webp")).unwrap(),
            b"preview"
        );
        assert!(!has_journal(&target).unwrap());
        assert!(!has_journal(&source).unwrap());
    }

    #[test]
    fn foreign_source_replacement_blocks_rollback_without_changing_either_file() {
        let (_root, source, target, journal) = fixture();
        fs::write(source.join("a.jpg"), b"foreign source").unwrap();
        drop(journal);
        assert!(recover_album(&target).is_err());
        assert_eq!(fs::read(source.join("a.jpg")).unwrap(), b"foreign source");
        assert_eq!(fs::read(target.join("b.jpg")).unwrap(), b"original");
    }

    #[test]
    fn foreign_target_replacement_is_preserved() {
        let (_root, source, target, journal) = fixture();
        fs::write(target.join("b.jpg"), b"foreign target").unwrap();
        drop(journal);
        assert!(recover_album(&source).is_err());
        assert!(!source.join("a.jpg").exists());
        assert_eq!(fs::read(target.join("b.jpg")).unwrap(), b"foreign target");
    }

    #[test]
    fn truncated_append_preserves_previous_intents_and_is_cleaned_after_rollback() {
        let (_root, source, target, mut journal) = fixture();
        journal.events.write_all(b"{\"File\":{\"source\":").unwrap();
        drop(journal);
        recover_album(&target).unwrap();
        recover_album(&source).unwrap();
        assert_eq!(fs::read(source.join("a.jpg")).unwrap(), b"original");
        assert!(!target.join("b.jpg").exists());
    }

    #[test]
    fn unknown_staging_data_is_preserved_during_cleanup() {
        let (_root, source, target, journal) = fixture();
        let extra = journal.staging.join("foreign.txt");
        fs::write(&extra, b"foreign").unwrap();
        drop(journal);
        assert!(recover_album(&target).is_err());
        assert_eq!(fs::read(extra).unwrap(), b"foreign");
        assert_eq!(fs::read(source.join("a.jpg")).unwrap(), b"original");
    }

    #[test]
    fn metadata_rollback_preserves_rows_unrelated_to_the_move() {
        let (_root, source, target, mut journal) = fixture();
        metadata::update_file_meta(&source, "a.jpg", |entry| entry.favorite = true).unwrap();
        {
            let (_moving, guard) = metadata::lock_metadata_for_move(&[&source, &target]).unwrap();
            metadata::transfer_media_state_journaled(
                &source,
                &target,
                &[("a.jpg".into(), "b.jpg".into())],
                &guard,
                true,
                &mut journal,
                |_| Ok(()),
            )
            .unwrap();
        }
        metadata::update_file_meta(&target, "other.jpg", |entry| {
            entry.meta = Some("foreign metadata".into())
        })
        .unwrap();
        drop(journal);
        recover_album(&target).unwrap();
        assert!(metadata::read_album_meta_checked(&source).unwrap().files["a.jpg"].favorite);
        assert_eq!(
            metadata::read_album_meta_checked(&target).unwrap().files["other.jpg"]
                .meta
                .as_deref(),
            Some("foreign metadata")
        );
        assert!(!metadata::read_album_meta_checked(&target)
            .unwrap()
            .files
            .contains_key("b.jpg"));
    }

    #[test]
    fn changed_metadata_blocks_recovery_before_any_file_is_moved() {
        let (_root, source, target, mut journal) = fixture();
        metadata::update_file_meta(&source, "a.jpg", |entry| entry.favorite = true).unwrap();
        {
            let (_moving, guard) = metadata::lock_metadata_for_move(&[&source, &target]).unwrap();
            metadata::transfer_media_state_journaled(
                &source,
                &target,
                &[("a.jpg".into(), "b.jpg".into())],
                &guard,
                true,
                &mut journal,
                |_| Ok(()),
            )
            .unwrap();
        }
        metadata::update_file_meta(&target, "b.jpg", |entry| {
            entry.meta = Some("external edit".into())
        })
        .unwrap();
        drop(journal);
        assert!(recover_album(&source).is_err());
        assert!(!source.join("a.jpg").exists());
        assert_eq!(fs::read(target.join("b.jpg")).unwrap(), b"original");
        assert_eq!(
            metadata::read_album_meta_checked(&target).unwrap().files["b.jpg"]
                .meta
                .as_deref(),
            Some("external edit")
        );
    }

    #[test]
    fn path_traversal_record_does_not_move_files_outside_the_albums() {
        let (root, source, _target, mut journal) = fixture();
        fs::write(root.path().join("foreign.jpg"), b"foreign").unwrap();
        journal
            .append(&Event::File {
                source: "../foreign.jpg".into(),
                target: "b.jpg".into(),
                revision: "forged".into(),
            })
            .unwrap();
        drop(journal);
        assert!(recover_album(&source).is_err());
        assert_eq!(
            fs::read(root.path().join("foreign.jpg")).unwrap(),
            b"foreign"
        );
    }

    #[test]
    fn incomplete_constructor_preserves_unknown_bytes_and_does_not_block_album_reads() {
        let root = tempfile::tempdir().unwrap();
        let staging = root.path().join(".room237-move-incomplete");
        fs::create_dir(&staging).unwrap();
        fs::write(staging.join("partial.tmp"), b"incomplete").unwrap();
        recover_album(root.path()).unwrap();
        assert_eq!(
            fs::read(
                root.path()
                    .join(".room237-unfinished-.room237-move-incomplete/partial.tmp")
            )
            .unwrap(),
            b"incomplete"
        );
    }

    #[test]
    fn replaced_album_directory_is_not_used_for_recovery() {
        let (root, source, target, journal) = fixture();
        drop(journal);
        let old = root.path().join("old-source");
        fs::rename(&source, &old).unwrap();
        fs::create_dir(&source).unwrap();
        fs::write(source.join("a.jpg"), b"foreign source").unwrap();
        assert!(recover_album(&target).is_err());
        assert_eq!(fs::read(source.join("a.jpg")).unwrap(), b"foreign source");
        assert_eq!(fs::read(target.join("b.jpg")).unwrap(), b"original");
    }

    #[cfg(unix)]
    #[test]
    fn symlinked_events_are_rejected_without_changing_originals() {
        let (root, source, target, journal) = fixture();
        let events = journal.staging.join(EVENTS);
        drop(journal);
        let outside = root.path().join("foreign-events");
        fs::rename(&events, &outside).unwrap();
        let bytes = fs::read(&outside).unwrap();
        std::os::unix::fs::symlink(&outside, &events).unwrap();
        assert!(recover_album(&source).is_err());
        assert!(!source.join("a.jpg").exists());
        assert_eq!(fs::read(target.join("b.jpg")).unwrap(), b"original");
        assert_eq!(fs::read(outside).unwrap(), bytes);
    }
}
