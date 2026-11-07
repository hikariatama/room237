use serde::{Serialize, de::DeserializeOwned};
use serde_json::Value;
pub struct Context {
    pub app: crate::host::AppHandle,
    pub settings: crate::settings::SettingsState,
    pub startup: crate::startup::StartupState,
}
impl Context {
    pub fn new(profile: std::path::PathBuf) -> Result<Self, String> {
        let app = crate::host::AppHandle::new(profile);
        Ok(Self { settings: crate::settings::SettingsState::load(&app)?, app, startup: Default::default() })
    }
}
fn argument<T: DeserializeOwned>(params: &Value, name: &str) -> Result<T, String> {
    serde_json::from_value(params.get(name).cloned().unwrap_or(Value::Null)).map_err(|e| format!("{name}: {e}"))
}
fn value<T: Serialize>(result: T) -> Result<Value, String> {
    let mut value=serde_json::to_value(result).map_err(|e|e.to_string())?;
    enrich(&mut value); Ok(value)
}
fn enrich(value: &mut Value) {
    match value {
        Value::Array(items)=>for item in items {enrich(item)},
        Value::Object(object)=>{
            if let Some(meta)=object.get("meta").and_then(Value::as_str).and_then(|s|crate::metadata::unpack_file_meta(s).ok()) {
                object.insert("width".into(),serde_json::json!(meta.w));
                object.insert("height".into(),serde_json::json!(meta.h));
                object.insert("dateAdded".into(),serde_json::json!(meta.a));
                object.insert("dateTaken".into(),serde_json::json!(meta.s));
                object.insert("isVideo".into(),serde_json::json!(meta.v));
            }
            for value in object.values_mut(){enrich(value)}
        },_=>{}
    }
}
pub async fn dispatch(context: &Context, method: &str, params: Value) -> Result<Value, String> {
    match method {
        "get_startup_status" => value(crate::startup::get_startup_status(crate::host::State::new(&context.startup))?),
        "retry_startup" => value(crate::startup::retry_startup(context.app.clone(), crate::host::State::new(&context.startup))),
        "get_file_metadata" => value(crate::commands::get_file_metadata(argument(&params, "path")?).await?),
        "get_album_media" => value(crate::album::get_album_media(context.app.clone(), argument(&params, "dir")?).await?),
        "get_media_page" => value(crate::catalog::get_media_page(argument(&params, "query")?).await?),
        "get_inventory_print" => value(crate::catalog::get_inventory_print(argument(&params, "query")?).await?),
        "create_media_selection" => value(crate::catalog::create_media_selection(argument(&params, "query")?, argument(&params, "token")?).await?),
        "consume_media_selection" => value(crate::catalog::consume_media_selection(argument(&params, "rootDir")?, argument(&params, "token")?, argument(&params, "paths")?).await?),
        "prune_media_selection" => value(crate::catalog::prune_media_selection(argument(&params, "rootDir")?, argument(&params, "token")?).await?),
        "drop_media_selection" => value(crate::catalog::drop_media_selection(argument(&params, "rootDir")?, argument(&params, "token")?).await?),
        "media_selection_flags" => value(crate::catalog::media_selection_flags(argument(&params, "rootDir")?, argument(&params, "token")?, argument(&params, "paths")?).await?),
        "media_selection_batch" => value(crate::catalog::media_selection_batch(argument(&params, "rootDir")?, argument(&params, "token")?, argument(&params, "offset")?).await?),
        "copy_media_selection" => value(crate::catalog::copy_media_selection(argument(&params, "rootDir")?, argument(&params, "token")?, argument(&params, "excluded")?, argument(&params, "extra")?).await?),
        "get_viewer_media" => value(crate::catalog::get_viewer_media(argument(&params, "query")?, argument(&params, "index")?).await?),
        "get_media_neighbor" => value(crate::catalog::get_media_neighbor(argument(&params, "query")?, argument(&params, "currentPath")?, argument(&params, "delta")?).await?),
        "begin_gallery_scroll" => value(crate::catalog::begin_gallery_scroll(argument(&params, "rootDir")?, argument(&params, "session")?).await?),
        "save_gallery_scroll" => value(crate::catalog::save_gallery_scroll(argument(&params, "rootDir")?, argument(&params, "session")?, argument(&params, "marks")?).await?),
        "read_gallery_scroll" => value(crate::catalog::read_gallery_scroll(argument(&params, "rootDir")?, argument(&params, "session")?, argument(&params, "y")?).await?),
        "refresh_catalog_paths" => value(crate::catalog::refresh_catalog_paths(argument(&params, "rootDir")?, argument(&params, "paths")?).await?),
        "rescan_inventory" => value(crate::catalog::rescan_inventory(context.app.clone(), argument(&params, "rootDir")?, argument(&params, "albumId")?).await?),
        "reconcile_album" => value(crate::catalog::reconcile_album(context.app.clone(), argument(&params, "rootDir")?, argument(&params, "albumId")?).await?),
        "update_check" => value(crate::update::check(argument(&params, "manifest")?, argument(&params, "signature")?, argument(&params, "currentVersion")?, argument(&params, "platform")?)?),
        "update_stage" => { let plan=argument(&params,"plan")?; value(tokio::task::spawn_blocking(move||crate::update::stage(plan)).await.map_err(|e|e.to_string())??) },
        "update_launch_helper" => value(crate::update::launch_helper(argument(&params, "plan")?)?),
        "update_cleanup" => value(crate::update::cleanup(argument(&params, "install")?)?),
        "favorite_counts" => value(crate::catalog::favorite_counts(argument(&params, "rootDir")?).await?),
        "get_album_size" => value(crate::commands::get_album_size(argument(&params, "dir")?).await?),
        "get_albums_detached" => value(crate::commands::get_albums_detached(context.app.clone(), argument(&params, "rootDir")?).await?),
        "move_media" => value(crate::commands::move_media(argument(&params, "source")?, argument(&params, "target")?, argument(&params, "media")?).await?),
        "move_media_batch" => value(crate::album::move_media_batch(argument(&params, "source")?, argument(&params, "target")?, argument(&params, "media")?).await?),
        "is_preloading" => value(crate::preload::is_preloading()),
        "lock_until_preloaded" => value(crate::preload::lock_until_preloaded(context.app.clone()).await?),
        "set_allow_open" => value(crate::preload::set_allow_open(context.app.clone(), argument(&params, "allow")?)),
        "rebuild_thumbnails" => value(crate::debugging::rebuild_thumbnails(argument(&params, "rootDir")?).await?),
        "rebuild_metadata" => value(crate::debugging::rebuild_metadata(context.app.clone(), argument(&params, "rootDir")?).await?),
        "register_new_media" => value(crate::album::register_new_media(context.app.clone(), argument(&params, "albumPath")?, argument(&params, "mediaName")?).await?),
        "find_duplicates" => value(crate::duplicates::find_duplicates(argument(&params, "dir")?).await?),
        "find_duplicate_page" => value(crate::disk_duplicates::find_duplicate_page(argument(&params, "dir")?, argument(&params, "pageIndex")?).await?),
        "find_duplicate_groups" => value(crate::disk_duplicates::find_duplicate_groups(argument(&params, "dir")?, argument(&params, "offset")?, argument(&params, "signature")?).await?),
        "duplicate_group_members" => value(crate::disk_duplicates::duplicate_group_members(argument(&params, "dir")?, argument(&params, "root")?, argument(&params, "signature")?, argument(&params, "offset")?).await?),
        "mark_duplicate_group_different" => value(crate::disk_duplicates::mark_duplicate_group_different(argument(&params, "dir")?, argument(&params, "root")?, argument(&params, "signature")?, argument(&params, "offset")?).await?),
        "delete_duplicate_groups" => value(crate::disk_duplicates::delete_duplicate_groups(argument(&params, "dir")?, argument(&params, "signature")?).await?),
        "mark_non_duplicates" => value(crate::commands::mark_non_duplicates(argument(&params, "dir")?, argument(&params, "files")?).await?),
        "reset_duplicates" => value(crate::debugging::reset_duplicates(argument(&params, "rootDir")?).await?),
        "add_media_files" => value(crate::album::add_media_files(context.app.clone(), argument(&params, "dir")?, argument(&params, "files")?).await?),
        "clear_room237_artifacts" => value(crate::debugging::clear_room237_artifacts(argument(&params, "rootDir")?).await?),
        "set_media_favorite" => value(crate::commands::set_media_favorite(argument(&params, "path")?, argument(&params, "favorite")?).await?),
        "delete_media_if_unchanged" => value(crate::commands::delete_media_if_unchanged(argument(&params, "path")?, argument(&params, "revision")?, argument(&params, "observed")?).await?),
        "delete_media" => value(crate::commands::delete_media(argument(&params, "path")?).await?),
        "delete_media_batch" => value(crate::commands::delete_media_batch(argument(&params, "albumPath")?, argument(&params, "targets")?, argument(&params, "observed")?).await?),
        "validate_media_revisions" => value(crate::commands::validate_media_revisions(argument(&params, "observed")?).await?),
        "list_favorites" => value(crate::commands::list_favorites(context.app.clone(), argument(&params, "rootDir")?).await?),
        "delete_album" => { let root=argument(&params,"rootDir")?; let id=argument(&params,"albumId")?; value(tokio::task::spawn_blocking(move||crate::album::delete_album(root,id)).await.map_err(|e|e.to_string())??) },
        "rename_album" => value(crate::commands::rename_album(argument(&params, "rootDir")?, argument(&params, "albumId")?, argument(&params, "newName")?).await?),
        "move_album" => value(crate::commands::move_album(argument(&params, "rootDir")?, argument(&params, "albumId")?, argument(&params, "newParentId")?).await?),
        "set_media_timestamp" => value(crate::commands::set_media_timestamp(argument(&params, "albumPath")?, argument(&params, "names")?, argument(&params, "timestamp")?).await?),
        "get_file_manager_name" => value(crate::util::get_file_manager_name()?),
        "canonicalize_directory" => value(crate::util::canonicalize_directory(argument(&params, "directory")?)?),
        "get_settings" => value(crate::settings::get_settings(crate::host::State::new(&context.settings))?),
        "update_settings" => value(crate::settings::update_settings(crate::host::State::new(&context.settings), argument(&params, "settings")?)?),
        "reset_settings" => value(crate::settings::reset_settings(crate::host::State::new(&context.settings))?),
        "set_clipboard_files" => value(crate::clipboard::set_clipboard_files(argument(&params, "paths")?)?),
        "native_geometry" => value(crate::catalog::native_geometry(argument(&params, "query")?).await?),
        "native_default_settings" => value(crate::settings::AdvancedSettings::default().clamp()),
        "native_delete_media_batch" => { let album=argument(&params,"albumPath")?; let targets=argument(&params,"targets")?; value(tokio::task::spawn_blocking(move||crate::media_delete::native_delete_batch(album,targets)).await.map_err(|e|e.to_string())??) },
        "ping" => Ok(serde_json::json!({"protocol":1,"engine":"native","webview":false})),
        _ => Err(format!("Unknown command: {method}")),
    }
}
