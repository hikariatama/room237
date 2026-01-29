use std::path::PathBuf;

#[cfg(target_os = "windows")]
mod windows;

#[cfg(target_os = "macos")]
mod macos;

#[cfg(target_os = "linux")]
mod linux;

#[tauri::command]
pub fn set_clipboard_files(paths: Vec<String>) -> Result<(), String> {
    if paths.is_empty() {
        return Err("No paths provided".to_string());
    }

    let path_bufs: Result<Vec<PathBuf>, String> = paths
        .iter()
        .map(|path_str| {
            let path = PathBuf::from(path_str);

            if !path.is_absolute() {
                return Err(format!("Path is not absolute: {}", path_str));
            }

            if !path.exists() {
                return Err(format!("Path does not exist: {}", path_str));
            }

            if !path.is_file() {
                return Err(format!("Path is not a regular file: {}", path_str));
            }

            Ok(path)
        })
        .collect();

    let path_bufs = path_bufs?;

    #[cfg(target_os = "windows")]
    return windows::set_clipboard_files_impl(&path_bufs);

    #[cfg(target_os = "macos")]
    return macos::set_clipboard_files_impl(&path_bufs);

    #[cfg(target_os = "linux")]
    return linux::set_clipboard_files_impl(&path_bufs);

    #[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
    return Err("Unsupported platform".to_string());
}
