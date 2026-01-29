#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod album;
mod clipboard;
mod constants;
mod debugging;
mod duplicates;
mod metadata;
mod preload;
mod settings;
mod thumb;
mod util;

use anyhow::anyhow;
use tauri::Manager;

pub use album::{
    add_media_files, get_album_media, get_album_size, get_albums_detached, list_favorites,
    move_album, move_media, move_media_batch, register_new_media, rename_album,
};
pub use clipboard::set_clipboard_files;
pub use debugging::{
    clear_room237_artifacts, rebuild_metadata, rebuild_thumbnails, reset_duplicates,
};
pub use duplicates::{find_duplicates, mark_non_duplicates};
pub use metadata::{get_file_metadata, set_media_favorite, set_media_timestamp};
pub use preload::{is_preloading, lock_until_preloaded, set_allow_open};
pub use settings::{get_settings, reset_settings, update_settings, SettingsState};
pub use util::get_file_manager_name;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .plugin(tauri_plugin_process::init())
        .invoke_handler(tauri::generate_handler![
            get_file_metadata,
            get_album_media,
            get_album_size,
            get_albums_detached,
            move_media,
            move_media_batch,
            is_preloading,
            lock_until_preloaded,
            set_allow_open,
            rebuild_thumbnails,
            rebuild_metadata,
            register_new_media,
            find_duplicates,
            mark_non_duplicates,
            reset_duplicates,
            add_media_files,
            clear_room237_artifacts,
            set_media_favorite,
            list_favorites,
            rename_album,
            move_album,
            set_media_timestamp,
            get_file_manager_name,
            get_settings,
            update_settings,
            reset_settings,
            set_clipboard_files,
        ])
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_store::Builder::new().build())
        .plugin(
            tauri_plugin_log::Builder::default()
                .level(log::LevelFilter::Info)
                .targets([tauri_plugin_log::Target::new(
                    tauri_plugin_log::TargetKind::Stdout,
                )])
                .build(),
        )
        .setup(|app| {
            let settings_state = SettingsState::load(&app.handle())
                .map_err(|e| anyhow!("Failed to load settings: {e}"))?;
            app.manage(settings_state);
            ffmpeg_sidecar::download::auto_download().unwrap();
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("tauri run failed");
}
