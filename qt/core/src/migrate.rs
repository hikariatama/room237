//! One-way import of the Tauri profile. The old profile is only read, so the old app keeps working.
//! Every step can repeat after a crash: files appear through atomic renames and the marker comes last.
use std::{fs, path::Path};

const MARKER: &str = "migration-tauri.json";
const STORE_KEY: &str = "room237-storage";

#[derive(Debug, PartialEq)]
pub enum Outcome {
    AlreadyDone,
    NoLegacyProfile,
    Imported { settings: bool, state: bool },
}

fn write_atomic(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let temporary = path.with_extension("migrating");
    fs::write(&temporary, bytes).map_err(|e| e.to_string())?;
    fs::File::open(&temporary).and_then(|file| file.sync_all()).map_err(|e| e.to_string())?;
    fs::rename(&temporary, path).map_err(|e| e.to_string())
}

/// `state.json` of tauri-plugin-store keeps the UI state as a JSON string under one key.
fn legacy_ui_state(legacy: &Path) -> Option<serde_json::Value> {
    let store: serde_json::Value = serde_json::from_slice(&fs::read(legacy.join("state.json")).ok()?).ok()?;
    let inner = store.get(STORE_KEY)?;
    let state: serde_json::Value = match inner {
        serde_json::Value::String(text) => serde_json::from_str(text).ok()?,
        other => other.clone(),
    };
    state.is_object().then_some(state)
}

pub fn migrate_profile(profile: &Path, legacy: &Path) -> Outcome {
    let marker = profile.join(MARKER);
    if marker.is_file() {
        return Outcome::AlreadyDone;
    }
    if !legacy.is_dir() || legacy == profile {
        return Outcome::NoLegacyProfile;
    }
    let _ = fs::create_dir_all(profile);
    let mut imported_settings = false;
    let settings_target = profile.join("settings.json");
    if !settings_target.exists() {
        // Corrupt settings stay behind: the core reports them instead of silently losing privacy flags.
        if let Ok(bytes) = fs::read(legacy.join("settings.json")) {
            if serde_json::from_slice::<serde_json::Value>(&bytes).is_ok_and(|value| value.is_object())
                && write_atomic(&settings_target, &bytes).is_ok()
            {
                imported_settings = true;
            }
        }
    }
    let mut imported_state = false;
    let state_target = profile.join("ui-state.json");
    if !state_target.exists() {
        if let Some(state) = legacy_ui_state(legacy) {
            if let Ok(bytes) = serde_json::to_vec_pretty(&state) {
                imported_state = write_atomic(&state_target, &bytes).is_ok();
            }
        }
    }
    let record = serde_json::json!({
        "source": legacy.to_string_lossy(),
        "settings": imported_settings,
        "state": imported_state,
    });
    let _ = write_atomic(&marker, record.to_string().as_bytes());
    Outcome::Imported { settings: imported_settings, state: imported_state }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn legacy_profile(state: &str) -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        fs::write(dir.path().join("state.json"), state).unwrap();
        fs::write(dir.path().join("settings.json"), r#"{"privacy":{"enabled":true}}"#).unwrap();
        dir
    }

    #[test]
    fn imports_store_string_settings_and_leaves_the_legacy_profile_untouched() {
        let inner = serde_json::json!({"rootDir":"/lib","decoyRoot":"/decoy","columns":5,"language":"ru","layout":"masonry","sortKey":"added","sortDir":"asc","contentProtected":true});
        let legacy = legacy_profile(&serde_json::json!({ "room237-storage": inner.to_string() }).to_string());
        let before = fs::read(legacy.path().join("state.json")).unwrap();
        let profile = tempfile::tempdir().unwrap();
        assert_eq!(migrate_profile(profile.path(), legacy.path()), Outcome::Imported { settings: true, state: true });
        let state: serde_json::Value = serde_json::from_slice(&fs::read(profile.path().join("ui-state.json")).unwrap()).unwrap();
        assert_eq!(state, inner);
        assert_eq!(fs::read(legacy.path().join("state.json")).unwrap(), before);
        assert_eq!(fs::read_to_string(profile.path().join("settings.json")).unwrap(), r#"{"privacy":{"enabled":true}}"#);
    }

    #[test]
    fn second_run_changes_nothing_and_never_overwrites_new_state() {
        let legacy = legacy_profile(&serde_json::json!({ "room237-storage": "{\"columns\":5}" }).to_string());
        let profile = tempfile::tempdir().unwrap();
        migrate_profile(profile.path(), legacy.path());
        fs::write(profile.path().join("ui-state.json"), "{\"columns\":9}").unwrap();
        assert_eq!(migrate_profile(profile.path(), legacy.path()), Outcome::AlreadyDone);
        assert_eq!(fs::read_to_string(profile.path().join("ui-state.json")).unwrap(), "{\"columns\":9}");
    }

    #[test]
    fn interrupted_migration_resumes_without_a_marker() {
        let legacy = legacy_profile(&serde_json::json!({ "room237-storage": "{\"columns\":5}" }).to_string());
        let profile = tempfile::tempdir().unwrap();
        // A crash after the settings step: the file exists and the marker does not.
        fs::write(profile.path().join("settings.json"), "{\"kept\":true}").unwrap();
        assert_eq!(migrate_profile(profile.path(), legacy.path()), Outcome::Imported { settings: false, state: true });
        assert_eq!(fs::read_to_string(profile.path().join("settings.json")).unwrap(), "{\"kept\":true}");
        assert!(profile.path().join(MARKER).is_file());
    }

    #[test]
    fn corrupt_legacy_files_are_skipped_without_partial_output() {
        let legacy = legacy_profile("{not json");
        fs::write(legacy.path().join("settings.json"), "[1,2").unwrap();
        let profile = tempfile::tempdir().unwrap();
        assert_eq!(migrate_profile(profile.path(), legacy.path()), Outcome::Imported { settings: false, state: false });
        assert!(!profile.path().join("settings.json").exists());
        assert!(!profile.path().join("ui-state.json").exists());
    }
}
