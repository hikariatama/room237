#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod album;
mod album_db;
mod album_tree;
mod catalog;
mod clipboard;
mod commands;
mod constants;
mod debugging;
mod disk_duplicates;
mod duplicates;

mod media_delete;
mod media_move;
#[cfg(test)]
mod media_regressions;
mod metadata;
#[cfg(target_os = "macos")]
mod native_image;
mod preload;
mod preload_queue;
mod quicktime;
mod settings;
mod startup;
#[cfg(test)]
mod test_support;
mod thumb;
mod util;
pub mod update;
pub mod migrate;




pub use album::{
    add_media_files, get_album_media, get_album_size, get_albums_detached, list_favorites,
    move_album, move_media, move_media_batch, register_new_media, rename_album,
};
pub use clipboard::set_clipboard_files;
pub use debugging::{
    clear_room237_artifacts, rebuild_metadata, rebuild_thumbnails, reset_duplicates,
};
pub use duplicates::{find_duplicates, mark_non_duplicates};
pub use metadata::{
    delete_media_if_unchanged, get_file_metadata, set_media_favorite, set_media_timestamp,
    validate_media_revisions,
};
pub use preload::{is_preloading, lock_until_preloaded, set_allow_open};
pub use settings::{get_settings, reset_settings, update_settings, SettingsState};
pub use util::get_file_manager_name;


pub mod host;
pub mod protocol;
