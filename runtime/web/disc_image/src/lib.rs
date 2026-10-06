//! The player's disc image, read in place by the web build. nod opens the image (ISO, WBFS, RVZ,
//! WIA, CISO, GCZ, ...) and decrypts the game partition; this file gives disc_worker.js a C ABI to
//! list the partition's files and read byte ranges of them. The image itself is read through
//! `mkw_disc_source_read`, which disc_worker.js implements on the picked File (FileReaderSync).
//!
//! Everything runs on the worker's one thread, so the state is a plain global behind a Mutex.

use std::fmt::Write as _;
use std::io::{self, Read, Seek, SeekFrom};
use std::sync::Mutex;

use nod::common::PartitionKind;
use nod::read::{DiscOptions, DiscReader, DiscStream, PartitionMeta, PartitionOptions, PartitionReader};

extern "C" {
    /// Reads up to `len` bytes at `offset` of the image into `out`. Returns the byte count, -1 on
    /// error. Offsets are doubles (exact below 2^53) to keep 64-bit integers out of the JS ABI.
    fn mkw_disc_source_read(offset: f64, out: *mut u8, len: usize) -> i32;
    /// The image's size in bytes.
    fn mkw_disc_source_size() -> f64;
}

#[derive(Clone)]
struct SourceStream;

impl DiscStream for SourceStream {
    fn read_exact_at(&mut self, buf: &mut [u8], offset: u64) -> io::Result<()> {
        let mut done = 0;
        while done < buf.len() {
            let rest = &mut buf[done..];
            let read = unsafe { mkw_disc_source_read((offset + done as u64) as f64, rest.as_mut_ptr(), rest.len()) };
            if read < 0 {
                return Err(io::Error::other("reading the disc image failed"));
            }
            if read == 0 {
                return Err(io::Error::from(io::ErrorKind::UnexpectedEof));
            }
            done += read as usize;
        }
        Ok(())
    }

    fn stream_len(&mut self) -> io::Result<u64> { Ok(unsafe { mkw_disc_source_size() } as u64) }
}

struct File {
    offset: u64,
    length: u64,
}

struct Disc {
    partition: Box<dyn PartitionReader>,
    meta: PartitionMeta,
    files: Vec<File>,
    listing: String,
}

static DISC: Mutex<Option<Disc>> = Mutex::new(None);
static ERROR: Mutex<String> = Mutex::new(String::new());

fn json_string(out: &mut String, text: &str) {
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn open() -> Result<Disc, String> {
    let disc = DiscReader::new_stream(Box::new(SourceStream), &DiscOptions::default())
        .map_err(|e| format!("not a disc image nod can read: {e}"))?;
    let disc_meta = disc.meta();
    let game_id = disc.header().game_id_str().to_string();
    let title = disc.header().game_title_str().to_string();
    let mut partition = disc
        .open_partition_kind(PartitionKind::Data, &PartitionOptions::default())
        .map_err(|e| format!("no game partition: {e}"))?;
    let meta = partition.meta().map_err(|e| format!("reading the game partition failed: {e}"))?;
    let is_wii = partition.is_wii();
    let fst = meta.fst().map_err(|e| format!("bad file table: {e}"))?;

    let mut files = Vec::new();
    let mut listing = String::from("{\"gameId\":");
    json_string(&mut listing, &game_id);
    listing.push_str(",\"title\":");
    json_string(&mut listing, &title);
    listing.push_str(",\"format\":");
    json_string(&mut listing, &disc_meta.format.to_string());
    listing.push_str(",\"files\":[");
    for (_, node, path) in fst.iter() {
        if !node.is_file() {
            continue;
        }
        if !files.is_empty() {
            listing.push(',');
        }
        listing.push('[');
        json_string(&mut listing, &format!("files/{path}"));
        let _ = write!(listing, ",{}]", node.length());
        files.push(File { offset: node.offset(is_wii), length: node.length() as u64 });
    }
    listing.push_str("]}");
    drop(fst);
    Ok(Disc { partition, meta, files, listing })
}

fn set_error(message: String) { *ERROR.lock().unwrap() = message; }

/// Opens the image. 0 on success; otherwise mkw_disc_error says why.
#[no_mangle]
pub extern "C" fn mkw_disc_open() -> i32 {
    match open() {
        Ok(disc) => {
            *DISC.lock().unwrap() = Some(disc);
            0
        }
        Err(message) => {
            set_error(message);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn mkw_disc_error(len: *mut usize) -> *const u8 {
    let error = ERROR.lock().unwrap();
    unsafe { *len = error.len() };
    error.as_ptr()
}

/// JSON: {"gameId", "title", "format", "files": [[path, size], ...]}; a file's id is its index.
#[no_mangle]
pub extern "C" fn mkw_disc_listing(len: *mut usize) -> *const u8 {
    let disc = DISC.lock().unwrap();
    let Some(disc) = disc.as_ref() else { return std::ptr::null() };
    unsafe { *len = disc.listing.len() };
    disc.listing.as_ptr()
}

/// The partition's system files, as an extracted DATA/sys folder holds them:
/// 0 boot.bin, 1 bi2.bin, 2 apploader.img, 3 main.dol, 4 fst.bin.
#[no_mangle]
pub extern "C" fn mkw_disc_sys(which: u32, len: *mut usize) -> *const u8 {
    let disc = DISC.lock().unwrap();
    let Some(disc) = disc.as_ref() else { return std::ptr::null() };
    let bytes: &[u8] = match which {
        0 => disc.meta.raw_boot.as_slice(),
        1 => disc.meta.raw_bi2.as_slice(),
        2 => &disc.meta.raw_apploader,
        3 => &disc.meta.raw_dol,
        4 => &disc.meta.raw_fst,
        _ => return std::ptr::null(),
    };
    unsafe { *len = bytes.len() };
    bytes.as_ptr()
}

/// Reads `len` bytes at `offset` of file `id` (an index into the listing) into `out`. Returns the
/// byte count (short at the end of the file), or -1 with mkw_disc_error set.
#[no_mangle]
pub extern "C" fn mkw_disc_read(id: u32, offset: f64, out: *mut u8, len: usize) -> i32 {
    let mut guard = DISC.lock().unwrap();
    let Some(disc) = guard.as_mut() else { return -1 };
    let Some(file) = disc.files.get(id as usize) else { return -1 };
    let offset = offset as u64;
    if offset >= file.length {
        return 0;
    }
    let len = len.min((file.length - offset) as usize);
    let buf = unsafe { std::slice::from_raw_parts_mut(out, len) };
    let result = disc
        .partition
        .seek(SeekFrom::Start(file.offset + offset))
        .and_then(|_| disc.partition.read_exact(buf));
    match result {
        Ok(()) => len as i32,
        Err(e) => {
            drop(guard);
            set_error(format!("reading file {id} failed: {e}"));
            -1
        }
    }
}
