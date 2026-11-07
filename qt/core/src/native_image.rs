//! In-process image work on macOS (ImageIO + libwebp). Every entry point returns None when it
//! cannot produce the same artifact, and the caller then uses the FFmpeg path unchanged.
#![cfg(target_os = "macos")]
use std::{ffi::c_void, os::raw::c_int, path::Path, ptr, sync::OnceLock};

type CFTypeRef = *const c_void;
type CFIndex = isize;
#[repr(C)]
#[derive(Clone, Copy)]
struct CGPoint { x: f64, y: f64 }
#[repr(C)]
#[derive(Clone, Copy)]
struct CGSize { width: f64, height: f64 }
#[repr(C)]
#[derive(Clone, Copy)]
struct CGRect { origin: CGPoint, size: CGSize }

#[link(name = "CoreFoundation", kind = "framework")]
extern "C" {
    static kCFAllocatorDefault: CFTypeRef;
    static kCFBooleanTrue: CFTypeRef;
    static kCFBooleanFalse: CFTypeRef;
    static kCFTypeDictionaryKeyCallBacks: c_void;
    static kCFTypeDictionaryValueCallBacks: c_void;
    fn CFRelease(value: CFTypeRef);
    fn CFURLCreateFromFileSystemRepresentation(a: CFTypeRef, bytes: *const u8, len: CFIndex, dir: u8) -> CFTypeRef;
    fn CFDictionaryCreate(a: CFTypeRef, keys: *const CFTypeRef, values: *const CFTypeRef, n: CFIndex, k: *const c_void, v: *const c_void) -> CFTypeRef;
    fn CFDictionaryGetValue(dict: CFTypeRef, key: CFTypeRef) -> CFTypeRef;
    fn CFNumberCreate(a: CFTypeRef, kind: CFIndex, value: *const c_void) -> CFTypeRef;
    fn CFNumberGetValue(number: CFTypeRef, kind: CFIndex, out: *mut c_void) -> u8;
}
#[link(name = "ImageIO", kind = "framework")]
extern "C" {
    static kCGImageSourceCreateThumbnailFromImageAlways: CFTypeRef;
    static kCGImageSourceThumbnailMaxPixelSize: CFTypeRef;
    static kCGImageSourceCreateThumbnailWithTransform: CFTypeRef;
    static kCGImageSourceShouldCache: CFTypeRef;
    static kCGImagePropertyPixelWidth: CFTypeRef;
    static kCGImagePropertyPixelHeight: CFTypeRef;
    static kCGImagePropertyOrientation: CFTypeRef;
    fn CGImageSourceCreateWithURL(url: CFTypeRef, options: CFTypeRef) -> CFTypeRef;
    fn CGImageSourceCreateThumbnailAtIndex(source: CFTypeRef, index: usize, options: CFTypeRef) -> CFTypeRef;
    fn CGImageSourceCopyPropertiesAtIndex(source: CFTypeRef, index: usize, options: CFTypeRef) -> CFTypeRef;
}
#[link(name = "CoreGraphics", kind = "framework")]
extern "C" {
    static kCGColorSpaceSRGB: CFTypeRef;
    fn CGColorSpaceCreateWithName(name: CFTypeRef) -> CFTypeRef;
    fn CGColorSpaceRelease(space: CFTypeRef);
    fn CGImageGetWidth(image: CFTypeRef) -> usize;
    fn CGImageGetHeight(image: CFTypeRef) -> usize;
    fn CGImageRelease(image: CFTypeRef);
    fn CGBitmapContextCreate(data: *mut c_void, w: usize, h: usize, bits: usize, row: usize, space: CFTypeRef, info: u32) -> CFTypeRef;
    fn CGContextDrawImage(context: CFTypeRef, rect: CGRect, image: CFTypeRef);
    fn CGContextSetInterpolationQuality(context: CFTypeRef, quality: c_int);
    fn CGContextSetRGBFillColor(context: CFTypeRef, r: f64, g: f64, b: f64, a: f64);
    fn CGContextFillRect(context: CFTypeRef, rect: CGRect);
    fn CGContextRelease(context: CFTypeRef);
}

/// Each switch defaults to on; `ROOM237_NATIVE_<X>=0` selects the FFmpeg path.
fn enabled(cell: &'static OnceLock<bool>, name: &str) -> bool {
    *cell.get_or_init(|| std::env::var(name).map(|v| v != "0").unwrap_or(true))
}
pub fn metadata_enabled() -> bool { static C: OnceLock<bool> = OnceLock::new(); enabled(&C, "ROOM237_NATIVE_META") }
pub fn thumbnails_enabled() -> bool { static C: OnceLock<bool> = OnceLock::new(); enabled(&C, "ROOM237_NATIVE_THUMB") }
pub fn hash_decode_enabled() -> bool { static C: OnceLock<bool> = OnceLock::new(); enabled(&C, "ROOM237_NATIVE_HASH") }

struct Owned(CFTypeRef);
impl Drop for Owned {
    fn drop(&mut self) { unsafe { CFRelease(self.0) } }
}

fn open_source(path: &Path) -> Option<Owned> {
    use std::os::unix::ffi::OsStrExt;
    let bytes = path.as_os_str().as_bytes();
    unsafe {
        let url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, bytes.as_ptr(), bytes.len() as CFIndex, 0);
        if url.is_null() { return None; }
        let url = Owned(url);
        let source = CGImageSourceCreateWithURL(url.0, ptr::null());
        (!source.is_null()).then(|| Owned(source))
    }
}

/// Stored pixel dimensions from the file header, without decoding pixels.
pub fn dimensions(path: &Path) -> Option<(u32, u32)> {
    let source = open_source(path)?;
    unsafe {
        let keys = [kCGImageSourceShouldCache];
        let values = [kCFBooleanFalse];
        let options = Owned(CFDictionaryCreate(kCFAllocatorDefault, keys.as_ptr(), values.as_ptr(), 1, &kCFTypeDictionaryKeyCallBacks as *const _ as *const c_void, &kCFTypeDictionaryValueCallBacks as *const _ as *const c_void));
        let props = CGImageSourceCopyPropertiesAtIndex(source.0, 0, options.0);
        if props.is_null() { return None; }
        let props = Owned(props);
        let read = |key: CFTypeRef| -> Option<u32> {
            let number = CFDictionaryGetValue(props.0, key);
            if number.is_null() { return None; }
            let mut out: i64 = 0;
            (CFNumberGetValue(number, 4, &mut out as *mut i64 as *mut c_void) != 0 && out > 0).then_some(out as u32)
        };
        let (width, height) = (read(kCGImagePropertyPixelWidth)?, read(kCGImagePropertyPixelHeight)?);
        // Orientations 5-8 turn the picture by 90 degrees; everything downstream works in display dimensions.
        Some(if read(kCGImagePropertyOrientation).unwrap_or(1) >= 5 { (height, width) } else { (width, height) })
    }
}

fn draw_rgb(image: CFTypeRef, w: usize, h: usize, quality: c_int) -> Option<Vec<u8>> {
    let mut rgba = vec![0u8; w * h * 4];
    unsafe {
        let space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        // kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big
        let context = CGBitmapContextCreate(rgba.as_mut_ptr() as *mut c_void, w, h, 8, w * 4, space, 1 | (4 << 12));
        CGColorSpaceRelease(space);
        if context.is_null() { return None; }
        let rect = CGRect { origin: CGPoint { x: 0.0, y: 0.0 }, size: CGSize { width: w as f64, height: h as f64 } };
        CGContextSetRGBFillColor(context, 0.0, 0.0, 0.0, 1.0);
        CGContextFillRect(context, rect);
        CGContextSetInterpolationQuality(context, quality);
        CGContextDrawImage(context, rect, image);
        CGContextRelease(context);
    }
    let mut rgb = Vec::with_capacity(w * h * 3);
    for px in rgba.chunks_exact(4) { rgb.extend_from_slice(&px[..3]); }
    Some(rgb)
}

/// Straight-alpha RGBA when the image has any transparency, like the FFmpeg path keeps it.
fn draw_rgba(image: CFTypeRef, w: usize, h: usize) -> Option<Vec<u8>> {
    let mut rgba = vec![0u8; w * h * 4];
    unsafe {
        let space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        let context = CGBitmapContextCreate(rgba.as_mut_ptr() as *mut c_void, w, h, 8, w * 4, space, 1 | (4 << 12));
        CGColorSpaceRelease(space);
        if context.is_null() { return None; }
        CGContextDrawImage(context, CGRect { origin: CGPoint { x: 0.0, y: 0.0 }, size: CGSize { width: w as f64, height: h as f64 } }, image);
        CGContextRelease(context);
    }
    if rgba.chunks_exact(4).all(|px| px[3] == 255) { return None; }
    for px in rgba.chunks_exact_mut(4) {
        let alpha = px[3] as u32;
        if alpha > 0 && alpha < 255 {
            for channel in &mut px[..3] { *channel = ((*channel as u32 * 255 + alpha / 2) / alpha).min(255) as u8; }
        }
    }
    Some(rgba)
}

fn encode_webp(rgb: &[u8], w: usize, h: usize, quality: f32, method: c_int) -> Option<Vec<u8>> {
    use libwebp_sys::*;
    unsafe {
        let mut config: WebPConfig = std::mem::zeroed();
        if WebPConfigInitInternal(&mut config, WebPPreset::WEBP_PRESET_DEFAULT, 75.0, WEBP_ENCODER_ABI_VERSION as c_int) == 0 { return None; }
        config.quality = quality;
        config.method = method;
        let mut picture: WebPPicture = std::mem::zeroed();
        if WebPPictureInitInternal(&mut picture, WEBP_ENCODER_ABI_VERSION as c_int) == 0 { return None; }
        picture.width = w as c_int;
        picture.height = h as c_int;
        picture.use_argb = 1;
        let imported = if rgb.len() == w * h * 4 { WebPPictureImportRGBA(&mut picture, rgb.as_ptr(), (w * 4) as c_int) } else { WebPPictureImportRGB(&mut picture, rgb.as_ptr(), (w * 3) as c_int) };
        if imported == 0 { return None; }
        let mut writer: WebPMemoryWriter = std::mem::zeroed();
        WebPMemoryWriterInit(&mut writer);
        picture.writer = Some(WebPMemoryWrite);
        picture.custom_ptr = &mut writer as *mut WebPMemoryWriter as *mut c_void;
        let ok = WebPEncode(&config, &mut picture);
        WebPPictureFree(&mut picture);
        let bytes = (ok != 0).then(|| std::slice::from_raw_parts(writer.mem, writer.size).to_vec());
        WebPMemoryWriterClear(&mut writer);
        bytes
    }
}

/// Oriented preview bounded by `max_dim` plus a 16x16 preview, both encoded as WebP.
pub fn thumbnail(path: &Path, max_dim: u32, quality: f32, method: u8) -> Option<(Vec<u8>, Vec<u8>)> {
    let source = open_source(path)?;
    unsafe {
        let max = max_dim as i64;
        let number = Owned(CFNumberCreate(kCFAllocatorDefault, 4, &max as *const i64 as *const c_void));
        let keys = [kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform, kCGImageSourceThumbnailMaxPixelSize, kCGImageSourceShouldCache];
        let values = [kCFBooleanTrue, kCFBooleanTrue, number.0, kCFBooleanFalse];
        let options = Owned(CFDictionaryCreate(kCFAllocatorDefault, keys.as_ptr(), values.as_ptr(), 4, &kCFTypeDictionaryKeyCallBacks as *const _ as *const c_void, &kCFTypeDictionaryValueCallBacks as *const _ as *const c_void));
        let image = CGImageSourceCreateThumbnailAtIndex(source.0, 0, options.0);
        if image.is_null() { return None; }
        let (w, h) = (CGImageGetWidth(image), CGImageGetHeight(image));
        let preview = if w > 0 && h > 0 { draw_rgba(image, w, h).or_else(|| draw_rgb(image, w, h, 0)) } else { None };
        let blur = draw_rgb(image, 16, 16, 3);
        CGImageRelease(image);
        let main = encode_webp(&preview?, w, h, quality, method as c_int)?;
        let small = encode_webp(&blur?, 16, 16, 20.0, 0)?;
        Some((main, small))
    }
}
