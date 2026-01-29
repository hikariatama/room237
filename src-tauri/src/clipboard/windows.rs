use std::ffi::OsStr;
use std::iter::once;
use std::mem;
use std::os::windows::ffi::OsStrExt;
use std::path::PathBuf;
use std::ptr;
use windows::Win32::Foundation::{HANDLE, HWND};
use windows::Win32::System::DataExchange::{
    CloseClipboard, EmptyClipboard, OpenClipboard, SetClipboardData,
};
use windows::Win32::System::Memory::{
    GlobalAlloc, GlobalFree, GlobalLock, GlobalUnlock, GMEM_MOVEABLE, HGLOBAL,
};
use windows::Win32::System::SystemServices::CF_HDROP;
use windows::Win32::UI::Shell::{DROPEFFECT_COPY, DROPFILES};

const CFSTR_PREFERREDDROPEFFECT: &str = "Preferred DropEffect";

struct ClipboardGuard {
    opened: bool,
}

impl ClipboardGuard {
    fn new() -> Result<Self, String> {
        unsafe {
            OpenClipboard(HWND::default())
                .map_err(|e| format!("Failed to open clipboard: {}", e))?;
        }
        Ok(ClipboardGuard { opened: true })
    }
}

impl Drop for ClipboardGuard {
    fn drop(&mut self) {
        if self.opened {
            unsafe {
                let _ = CloseClipboard();
            }
        }
    }
}

fn to_wide_string(s: &str) -> Vec<u16> {
    OsStr::new(s).encode_wide().chain(once(0)).collect()
}

fn build_hdrop_data(paths: &[PathBuf]) -> Result<Vec<u8>, String> {
    let dropfiles_size = mem::size_of::<DROPFILES>();

    let wide_paths: Vec<Vec<u16>> = paths
        .iter()
        .map(|p| to_wide_string(&p.to_string_lossy()))
        .collect();

    let paths_size: usize = wide_paths.iter().map(|p| p.len() * 2).sum();

    let total_size = dropfiles_size + paths_size + 2;

    let mut data = vec![0u8; total_size];

    let dropfiles = DROPFILES {
        pFiles: dropfiles_size as u32,
        pt: Default::default(),
        fNC: false.into(),
        fWide: true.into(),
    };

    unsafe {
        ptr::copy_nonoverlapping(
            &dropfiles as *const DROPFILES as *const u8,
            data.as_mut_ptr(),
            dropfiles_size,
        );
    }

    let mut offset = dropfiles_size;
    for wide_path in wide_paths {
        let path_bytes = wide_path.len() * 2;
        unsafe {
            ptr::copy_nonoverlapping(
                wide_path.as_ptr() as *const u8,
                data.as_mut_ptr().add(offset),
                path_bytes,
            );
        }
        offset += path_bytes;
    }

    data[total_size - 2] = 0;
    data[total_size - 1] = 0;

    Ok(data)
}

fn set_preferred_drop_effect(clipboard_guard: &ClipboardGuard) -> Result<(), String> {
    use windows::Win32::System::DataExchange::RegisterClipboardFormatW;

    let format_name = to_wide_string(CFSTR_PREFERREDDROPEFFECT);
    let format = unsafe { RegisterClipboardFormatW(windows::core::PCWSTR(format_name.as_ptr())) };

    if format == 0 {
        return Err("Failed to register Preferred DropEffect format".to_string());
    }

    let size = mem::size_of::<u32>();
    let hmem = unsafe { GlobalAlloc(GMEM_MOVEABLE, size) }
        .map_err(|e| format!("Failed to allocate memory for drop effect: {}", e))?;

    unsafe {
        let ptr = GlobalLock(hmem);
        if ptr.is_null() {
            GlobalFree(hmem).ok();
            return Err("Failed to lock memory for drop effect".to_string());
        }

        *(ptr as *mut u32) = DROPEFFECT_COPY;

        GlobalUnlock(hmem).ok();

        if SetClipboardData(format, HANDLE(hmem.0 as isize)).is_err() {
            GlobalFree(hmem).ok();
            return Err("Failed to set Preferred DropEffect".to_string());
        }
    }

    Ok(())
}

pub fn set_clipboard_files_impl(paths: &[PathBuf]) -> Result<(), String> {
    let hdrop_data = build_hdrop_data(paths)?;

    let _clipboard_guard = ClipboardGuard::new()?;

    unsafe {
        EmptyClipboard().map_err(|e| format!("Failed to empty clipboard: {}", e))?;
    }

    let hmem = unsafe { GlobalAlloc(GMEM_MOVEABLE, hdrop_data.len()) }
        .map_err(|e| format!("Failed to allocate memory: {}", e))?;

    unsafe {
        let ptr = GlobalLock(hmem);
        if ptr.is_null() {
            GlobalFree(hmem).ok();
            return Err("Failed to lock memory".to_string());
        }

        ptr::copy_nonoverlapping(hdrop_data.as_ptr(), ptr as *mut u8, hdrop_data.len());

        GlobalUnlock(hmem).ok();

        if SetClipboardData(CF_HDROP.0 as u32, HANDLE(hmem.0 as isize)).is_err() {
            GlobalFree(hmem).ok();
            return Err("Failed to set clipboard data".to_string());
        }
    }

    let _ = set_preferred_drop_effect(&_clipboard_guard);

    Ok(())
}
