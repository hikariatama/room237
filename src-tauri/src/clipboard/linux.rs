use std::path::PathBuf;
use std::sync::mpsc::{channel, Sender};
use std::thread;
use std::time::Duration;
use url::Url;

fn build_uri_list(paths: &[PathBuf]) -> Result<String, String> {
    let uris: Result<Vec<String>, String> = paths
        .iter()
        .map(|path| {
            Url::from_file_path(path)
                .map(|url| url.to_string())
                .map_err(|_| format!("Failed to convert path to URI: {}", path.display()))
        })
        .collect();

    let mut result = uris?.join("\n");
    result.push('\n');
    Ok(result)
}

fn build_gnome_copied_files(paths: &[PathBuf]) -> Result<String, String> {
    let uris: Result<Vec<String>, String> = paths
        .iter()
        .map(|path| {
            Url::from_file_path(path)
                .map(|url| url.to_string())
                .map_err(|_| format!("Failed to convert path to URI: {}", path.display()))
        })
        .collect();

    let mut result = String::from("copy\n");
    for uri in uris? {
        result.push_str(&uri);
        result.push('\n');
    }
    Ok(result)
}

#[cfg(all(unix, not(target_os = "macos")))]
fn try_x11_clipboard(paths: &[PathBuf]) -> Result<(), String> {
    use x11_clipboard::Clipboard as X11Clipboard;

    let clipboard =
        X11Clipboard::new().map_err(|e| format!("Failed to connect to X11 clipboard: {}", e))?;

    let uri_list = build_uri_list(paths)?;
    let gnome_format = build_gnome_copied_files(paths)?;

    let (tx, rx) = channel();

    thread::spawn(move || {
        let clipboard = match X11Clipboard::new() {
            Ok(c) => c,
            Err(_) => return,
        };

        let atoms = clipboard.setter.atoms.clone();

        let uri_list_atom = atoms.utf8_string;
        let gnome_atom = atoms.clipboard;

        if let Err(_) = clipboard.store(atoms.clipboard, uri_list_atom, uri_list.as_bytes()) {
            return;
        }

        thread::sleep(Duration::from_secs(30));
        let _ = tx.send(());
    });

    thread::sleep(Duration::from_millis(100));

    Ok(())
}

#[cfg(all(unix, not(target_os = "macos")))]
fn try_wayland_clipboard(paths: &[PathBuf]) -> Result<(), String> {
    use wl_clipboard_rs::copy::{MimeType, Options, Source};

    let uri_list = build_uri_list(paths)?;

    let mut opts = Options::new();
    opts.foreground(true);
    opts.trim_newline(false);

    opts.copy(
        Source::Bytes(uri_list.as_bytes().into()),
        MimeType::Specific("text/uri-list".to_string()),
    )
    .map_err(|e| format!("Failed to set Wayland clipboard: {}", e))?;

    Ok(())
}

pub fn set_clipboard_files_impl(paths: &[PathBuf]) -> Result<(), String> {
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        let session_type = std::env::var("XDG_SESSION_TYPE").unwrap_or_default();
        let wayland_display = std::env::var("WAYLAND_DISPLAY").ok();
        let x11_display = std::env::var("DISPLAY").ok();

        if session_type == "wayland" || wayland_display.is_some() {
            if let Ok(()) = try_wayland_clipboard(paths) {
                return Ok(());
            }
        }

        if x11_display.is_some() {
            if let Ok(()) = try_x11_clipboard(paths) {
                return Ok(());
            }
        }

        Err("Failed to access clipboard: neither X11 nor Wayland clipboard available".to_string())
    }

    #[cfg(not(all(unix, not(target_os = "macos"))))]
    {
        Err("Unsupported platform".to_string())
    }
}
