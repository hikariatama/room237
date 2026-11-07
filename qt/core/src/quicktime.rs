//! Read only the movie creation date; skip media payloads and bound atom work.
use std::{
    fs::OpenOptions,
    io::{Read, Seek, SeekFrom},
    path::Path,
};

pub(crate) const EPOCH_OFFSET: i64 = 2_082_844_800;
const MAX_ATOMS: usize = 1024;

pub(crate) fn creation_time(path: &Path) -> Option<i64> {
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NONBLOCK);
    }
    let mut file = options.open(path).ok()?;
    let info = file.metadata().ok()?;
    if !info.is_file() {
        return None;
    }
    let length = info.len();
    read_creation_time(&mut file, length)
}

fn read_creation_time(input: &mut (impl Read + Seek), length: u64) -> Option<i64> {
    let mut budget = MAX_ATOMS;
    let (start, end) = find_atom(input, 0, length, b"moov", &mut budget)?;
    let (start, end) = find_atom(input, start, end, b"mvhd", &mut budget)?;
    input.seek(SeekFrom::Start(start)).ok()?;
    let mut flags = [0; 4];
    if end.checked_sub(start)? < 4 {
        return None;
    }
    input.read_exact(&mut flags).ok()?;
    let time = match flags[0] {
        0 if end - start >= 8 => {
            let mut bytes = [0; 4];
            input.read_exact(&mut bytes).ok()?;
            u32::from_be_bytes(bytes) as u64
        }
        1 if end - start >= 12 => {
            let mut bytes = [0; 8];
            input.read_exact(&mut bytes).ok()?;
            u64::from_be_bytes(bytes)
        }
        _ => return None,
    };
    // Zero denotes an unset timestamp in the media handled by FFmpeg.
    if time == 0 {
        return None;
    }
    i64::try_from(time).ok()?.checked_sub(EPOCH_OFFSET)
}

fn find_atom(
    input: &mut (impl Read + Seek),
    mut start: u64,
    end: u64,
    wanted: &[u8; 4],
    budget: &mut usize,
) -> Option<(u64, u64)> {
    while start < end {
        *budget = budget.checked_sub(1)?;
        if end.checked_sub(start)? < 8 {
            return None;
        }
        input.seek(SeekFrom::Start(start)).ok()?;
        let mut header = [0; 8];
        input.read_exact(&mut header).ok()?;
        let mut header_size = 8;
        let size = match u32::from_be_bytes(header[..4].try_into().ok()?) {
            0 => end - start,
            1 => {
                if end - start < 16 {
                    return None;
                }
                let mut extended = [0; 8];
                input.read_exact(&mut extended).ok()?;
                header_size = 16;
                u64::from_be_bytes(extended)
            }
            size => size as u64,
        };
        if size < header_size {
            return None;
        }
        let next = start.checked_add(size)?;
        if next > end {
            return None;
        }
        if &header[4..] == wanted {
            return Some((start + header_size, next));
        }
        start = next;
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;
    fn atom(name: &[u8; 4], payload: &[u8], extended: bool) -> Vec<u8> {
        let mut result = Vec::new();
        result.extend_from_slice(
            &(if extended {
                1
            } else {
                payload.len() as u32 + 8
            })
            .to_be_bytes(),
        );
        result.extend_from_slice(name);
        if extended {
            result.extend_from_slice(&(payload.len() as u64 + 16).to_be_bytes());
        }
        result.extend_from_slice(payload);
        result
    }
    fn read(bytes: Vec<u8>) -> Option<i64> {
        let length = bytes.len() as u64;
        read_creation_time(&mut Cursor::new(bytes), length)
    }
    fn movie(time: u64, version: u8, extended: bool) -> Vec<u8> {
        let mut payload = vec![version, 0, 0, 0];
        if version == 0 {
            payload.extend_from_slice(&(time as u32).to_be_bytes());
        } else {
            payload.extend_from_slice(&time.to_be_bytes());
        }
        atom(b"moov", &atom(b"mvhd", &payload, extended), extended)
    }
    #[test]
    fn dates_preserve_1904_epoch_and_both_atom_versions() {
        for time in [
            1,
            EPOCH_OFFSET as u64 - 315619200,
            EPOCH_OFFSET as u64,
            4_000_000_000,
        ] {
            for version in [0, 1] {
                for extended in [false, true] {
                    assert_eq!(
                        read(movie(time, version, extended)),
                        Some(time as i64 - EPOCH_OFFSET)
                    );
                }
            }
        }
        assert_eq!(
            read(movie(5_000_000_000, 1, true)),
            Some(5_000_000_000 - EPOCH_OFFSET)
        );
        assert_eq!(read(movie(0, 0, false)), None);
        assert_eq!(read(movie(u64::MAX, 1, false)), None);
        assert_eq!(read(movie(100, 2, false)), None);
    }
    #[test]
    fn skips_fake_dates_in_media_payload_and_accepts_final_size_zero_atom() {
        let mut bytes = atom(b"mdat", &movie(1, 0, false), false);
        bytes.extend_from_slice(&movie(EPOCH_OFFSET as u64 - 315619200, 0, false));
        assert_eq!(read(bytes.clone()), Some(-315619200));
        let start = bytes.windows(4).rposition(|part| part == b"moov").unwrap() - 4;
        bytes[start..start + 4].fill(0);
        assert_eq!(read(bytes), Some(-315619200));
    }
    #[test]
    fn malformed_and_excessive_atoms_are_bounded() {
        for bytes in [
            vec![],
            vec![0; 7],
            [4u32.to_be_bytes().as_slice(), b"moov"].concat(),
            [
                1u32.to_be_bytes().as_slice(),
                b"moov",
                u64::MAX.to_be_bytes().as_slice(),
            ]
            .concat(),
            atom(b"moov", &atom(b"mvhd", &[0; 7], false), false),
            atom(b"moov", &[0; 9], false),
        ] {
            assert_eq!(read(bytes), None);
        }
        let mut bytes = atom(b"free", &[], false).repeat(MAX_ATOMS);
        bytes.extend_from_slice(&movie(EPOCH_OFFSET as u64, 0, false));
        assert_eq!(read(bytes), None);
    }

    #[test]
    fn reads_movie_header_after_a_large_sparse_media_atom() {
        use std::fs::File;
        use std::io::Write;
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("large.mov");
        let mut file = File::create(&path).unwrap();
        let media_size = (1u64 << 32) + 16;
        file.write_all(&1u32.to_be_bytes()).unwrap();
        file.write_all(b"mdat").unwrap();
        file.write_all(&media_size.to_be_bytes()).unwrap();
        file.seek(SeekFrom::Start(media_size)).unwrap();
        file.write_all(&movie(EPOCH_OFFSET as u64 - 315619200, 0, false))
            .unwrap();
        drop(file);
        assert_eq!(creation_time(&path), Some(-315619200));
    }

    #[test]
    #[cfg(unix)]
    fn does_not_wait_for_a_named_pipe_writer() {
        use std::os::unix::ffi::OsStrExt;
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("pipe.mov");
        let raw = std::ffi::CString::new(path.as_os_str().as_bytes()).unwrap();
        assert_eq!(unsafe { libc::mkfifo(raw.as_ptr(), 0o600) }, 0);
        assert_eq!(creation_time(&path), None);
    }
}
