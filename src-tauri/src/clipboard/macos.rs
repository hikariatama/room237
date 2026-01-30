use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2_app_kit::{NSPasteboard, NSPasteboardWriting};
use objc2_foundation::{NSArray, NSString, NSURL};
use std::path::PathBuf;

pub fn set_clipboard_files_impl(paths: &[PathBuf]) -> Result<(), String> {
    objc2::rc::autoreleasepool(|_pool| unsafe {
        let pasteboard = NSPasteboard::generalPasteboard();

        pasteboard.clearContents();

        let urls: Vec<Retained<NSURL>> = paths
            .iter()
            .map(|path| {
                let path_str = path.to_string_lossy();
                let ns_path = NSString::from_str(&path_str);
                NSURL::fileURLWithPath(&ns_path)
            })
            .collect();

        let protocol_objects: Vec<&ProtocolObject<dyn NSPasteboardWriting>> = urls
            .iter()
            .map(|url| {
                let protocol_obj: &ProtocolObject<dyn NSPasteboardWriting> =
                    ProtocolObject::from_ref(&**url);
                protocol_obj
            })
            .collect();

        let ns_array = NSArray::from_slice(&protocol_objects);

        let success = pasteboard.writeObjects(&ns_array);

        if !success {
            return Err("Failed to write file URLs to pasteboard".to_string());
        }

        Ok(())
    })
}
