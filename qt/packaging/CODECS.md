# Decoding in the packages

| Format | macOS | Linux / Windows |
|---|---|---|
| WebP with alpha, animated WebP | Qt `qwebp`; source alpha kept in ARGB32 | Qt `qwebp` |
| GIF | Qt `qgif` | Qt `qgif` |
| JPEG | Qt `qjpeg` | Qt `qjpeg` |
| AVIF, HEIC, HEIF | ImageIO; bundled `ffmpeg` if ImageIO fails | bundled `ffmpeg`, RGBA PNG over a pipe; no `qheif` needed |
| MOV, AVI, WebM and other video | Qt Multimedia: FFmpeg plugin and bundled libav*; available native backends stay in the package | Qt Multimedia: FFmpeg plugin and bundled libav* |
| AV1 video | Thumbnail: bundled `ffmpeg` (dav1d). The viewer shows no picture and no error: Qt Multimedia's libav* is built without dav1d | Linux: same; Windows: not tested |
| Thumbnails | Core: ImageIO/libwebp where supported, otherwise bundled `ffmpeg` | Core: bundled `ffmpeg` |

The table describes the code paths (`thumbs.cpp`, `platform_mac.mm`, `core/src/native_image_mac.mm`), not test results.
`verify_packages.py --fixtures …` decodes the synthetic corpus with the packaged `ffmpeg` CLI, including the
transparency of `alpha.webp`. `test_formats.py` checks thumbnails, sizes, viewing and frame motion on a given build.
A library being present in a package does not count as decoding. Results under Wine do not replace a check on
Windows with Direct3D.

On Linux the Fusion style is built into QtWidgets; the Qt SDK used has no separate style plugin. macOS and Windows
packages must contain the platform style plugin. The Qt WebP, GIF and JPEG image plugins and the FFmpeg multimedia
plugin are required on all three platforms.
