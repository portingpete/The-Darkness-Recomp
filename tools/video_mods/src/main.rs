use darkrecomp_video_pack::{
    encode, media, parse, target_path, Container, Entry, MAX_ENTRIES, MAX_MANIFEST_BYTES,
    MAX_VIDEO_BYTES,
};
use std::{
    collections::HashSet,
    fs::{self, File, OpenOptions},
    io::{Read, Seek, SeekFrom, Write},
    path::{Component, Path, PathBuf},
    process::{Command, Stdio},
    time::{Duration, Instant},
};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
fn absolute(path: &Path) -> Result<PathBuf> {
    if path.components().any(|c| c == Component::ParentDir) { return Err("parent path segments are unsupported".into()); }
    Ok(if path.is_absolute() { path.to_path_buf() } else { std::env::current_dir()?.join(path) })
}
fn ordinary(path: &Path) -> Result<()> {
    for ancestor in path.ancestors() {
        let meta = fs::symlink_metadata(ancestor)?;
        if meta.file_type().is_symlink() { return Err("symlink paths are unsupported".into()); }
        #[cfg(windows)] {
            use std::os::windows::fs::MetadataExt;
            if meta.file_attributes() & 0x400 != 0 { return Err("reparse paths are unsupported".into()); }
        }
        if ancestor != path && !meta.is_dir() { return Err("ancestor is not a directory".into()); }
    }
    Ok(())
}
fn source(path: &Path) -> Result<File> {
    ordinary(path)?;
    let mut options = OpenOptions::new(); options.read(true);
    #[cfg(windows)] {
        use std::os::windows::fs::OpenOptionsExt;
        options.share_mode(1).custom_flags(0x0020_0000);
    }
    let file = options.open(path)?;
    let meta = file.metadata()?;
    #[cfg(windows)] {
        use std::os::windows::fs::MetadataExt;
        if meta.file_attributes() & 0x400 != 0 { return Err("opened source is a reparse point".into()); }
    }
    if !meta.is_file() || meta.len() == 0 || meta.len() > MAX_VIDEO_BYTES { return Err("video must be an ordinary file of 1 byte through 2 GiB".into()); }
    Ok(file)
}
fn probe(path: &Path) -> Result<(media::Video, String)> {
    let program = std::env::var_os("DARK_FFPROBE").unwrap_or_else(|| "ffprobe".into());
    let mut command = Command::new(program);
    command.args(["-v", "error", "-protocol_whitelist", "file", "-probesize", "8388608", "-analyzeduration", "3000000", "-show_entries",
        "stream=codec_type,codec_name,pix_fmt,width,height,r_frame_rate,start_time,duration,sample_rate,channels:format=duration", "-of", "default"])
        .arg(path).stdout(Stdio::piped()).stderr(Stdio::null());
    #[cfg(windows)] {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x0800_0000);
    }
    let mut child = command.spawn().map_err(|e| format!("cannot start ffprobe (install FFmpeg or set DARK_FFPROBE): {e}"))?;
    let stdout = child.stdout.take().ok_or("ffprobe stdout unavailable")?;
    let reader = std::thread::spawn(move || {
        let mut bytes = Vec::new(); stdout.take(65537).read_to_end(&mut bytes).map(|_| bytes)
    });
    let deadline = Instant::now() + Duration::from_secs(30);
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) => (),
            Err(error) => {
                let _ = child.kill(); let _ = child.wait(); let _ = reader.join(); return Err(error.into());
            }
        }
        if Instant::now() >= deadline { let _ = child.kill(); let _ = child.wait(); let _ = reader.join(); return Err("ffprobe exceeded 30 seconds".into()); }
        std::thread::sleep(Duration::from_millis(25));
    };
    let bytes = reader.join().map_err(|_| "ffprobe output reader failed")??;
    if bytes.len() > 65536 || !status.success() { return Err("ffprobe rejected the media or exceeded its output bound".into()); }
    let text = String::from_utf8(bytes)?;
    Ok((media::parse_probe(&text)?, text))
}
fn container(file: &File, kind: Container) -> Result<()> {
    let mut reader = file.try_clone()?;
    let mut magic = [0; 16]; reader.read_exact(&mut magic)?;
    let valid = match kind {
        Container::Theora => &magic[..5] == b"OggS\0",
        Container::Asf => magic == [0x30,0x26,0xb2,0x75,0x8e,0x66,0xcf,0x11,0xa6,0xd9,0,0xaa,0,0x62,0xce,0x6c],
    };
    if !valid { return Err("media container differs from its target".into()); }
    Ok(())
}
struct Clip {
    relative: String,
    replacement: PathBuf,
}

fn read_batch(mapping: &Path) -> Result<Vec<Clip>> {
    ordinary(mapping)?;
    let file = File::open(mapping)?;
    if !file.metadata()?.is_file() {
        return Err("batch mapping must be an ordinary file".into());
    }
    let mut text = String::new();
    file.take(MAX_MANIFEST_BYTES as u64 + 1).read_to_string(&mut text)?;
    if text.len() > MAX_MANIFEST_BYTES {
        return Err("batch mapping exceeds 64 KiB".into());
    }
    let parent = mapping.parent().ok_or("batch mapping parent missing")?;
    let mut clips = Vec::new();
    let mut seen = HashSet::new();
    for (index, line) in text.trim_start_matches('\u{feff}').lines().enumerate() {
        if clips.len() == MAX_ENTRIES {
            return Err("batch mapping exceeds 128 clips".into());
        }
        let row = index + 1;
        let (relative, replacement) = line
            .split_once('\t')
            .ok_or_else(|| format!("batch line {row}: expected video target and replacement path"))?;
        if replacement.is_empty() || replacement.contains(['\t', '\0']) {
            return Err(format!("batch line {row}: invalid replacement path").into());
        }
        let (key, _) = target_path(relative).map_err(|error| format!("batch line {row}: {error}"))?;
        if !seen.insert(key) {
            return Err(format!("batch line {row}: duplicate video target").into());
        }
        clips.push(Clip {
            relative: relative.into(),
            replacement: absolute(&parent.join(replacement))?,
        });
    }
    if clips.is_empty() {
        return Err("batch mapping is empty".into());
    }
    Ok(clips)
}

struct PreparedClip {
    entry: Entry,
    replacement: File,
    stock: media::Video,
    authored: media::Video,
    stock_probe: String,
    authored_probe: String,
}

fn prepare_clip(
    game: &Path,
    clip: &Clip,
    basename: &str,
    allow_aspect_change: bool,
) -> Result<PreparedClip> {
    let relative = &clip.relative;
    let (key, kind) = target_path(relative)?;
    let original = game.join(relative.replace('\\', "/"));
    let original_lock = source(&original)?;
    let replacement = source(&clip.replacement)?;
    container(&original_lock, kind)?;
    container(&replacement, kind)?;
    let (stock, stock_probe) = probe(&original)?;
    let (authored, authored_probe) = probe(&clip.replacement)?;
    media::compatible(&stock, &authored, allow_aspect_change)?;
    let extension = match kind {
        Container::Theora => "ogg",
        Container::Asf => "wmv",
    };
    let entry = Entry {
        target: key,
        payload: format!("{basename}.{extension}"),
        bytes: replacement.metadata()?.len(),
    };
    Ok(PreparedClip {
        entry,
        replacement,
        stock,
        authored,
        stock_probe,
        authored_probe,
    })
}

fn create(args: &[std::ffi::OsString], batch: bool) -> Result<()> {
    let required = if batch { 3 } else { 4 };
    let allow_aspect_change =
        args.len() == required + 1 && args[required] == "--allow-aspect-change";
    if args.len() != required && !allow_aspect_change {
        return Err(USAGE.into());
    }
    let game = absolute(Path::new(&args[0]))?;
    ordinary(&game)?;
    let output = absolute(Path::new(&args[required - 1]))?;
    ordinary(output.parent().ok_or("pack parent missing")?)?;
    if output.try_exists()? {
        return Err("new pack directory already exists; nothing overwritten".into());
    }
    let clips = if batch {
        read_batch(&absolute(Path::new(&args[1]))?)?
    } else {
        vec![Clip {
            relative: args[1]
                .to_str()
                .ok_or("video target must be ASCII")?
                .into(),
            replacement: absolute(Path::new(&args[2]))?,
        }]
    };
    let mut prepared = Vec::with_capacity(clips.len());
    for (index, clip) in clips.iter().enumerate() {
        let basename = if batch {
            format!("video-{}", index + 1)
        } else {
            "video".into()
        };
        let clip = prepare_clip(&game, clip, &basename, allow_aspect_change)
            .map_err(|error| format!("{}: {error}", clip.relative))?;
        prepared.push(clip);
    }
    let entries: Vec<_> = prepared.iter().map(|clip| clip.entry.clone()).collect();
    let manifest = encode(&entries)?;

    fs::create_dir(&output)?;
    ordinary(&output)?;
    // Only the invocation that created this directory writes the pack.
    // Keep the manifest out of sight until every payload is complete.
    let mut report = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(output.join("media.txt"))?;
    for clip in &mut prepared {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(output.join(&clip.entry.payload))?;
        clip.replacement.seek(SeekFrom::Start(0))?;
        let mut payload = (&clip.replacement).take(clip.entry.bytes + 1);
        if std::io::copy(&mut payload, &mut file)? != clip.entry.bytes {
            return Err(format!("{} changed during import; incomplete pack retained", clip.entry.target).into());
        }
        file.sync_all()?;
        if batch {
            writeln!(report, "{}\nPayload: {}", clip.entry.target, clip.entry.payload)?;
        }
        writeln!(
            report,
            "Original ffprobe\n{}\nReplacement ffprobe\n{}\nAspect change allowed: {allow_aspect_change}",
            clip.stock_probe, clip.authored_probe,
        )?;
    }
    report.sync_all()?;
    let staged_manifest = output.join("videos.tsv.tmp");
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&staged_manifest)?;
    file.write_all(&manifest)?;
    file.sync_all()?;
    drop(file);
    fs::rename(staged_manifest, output.join("videos.tsv"))?;
    if batch {
        println!(
            "Created a {}-video pack. Set DARK_VIDEO_MOD_DIR to {} before launch.",
            prepared.len(),
            output.display(),
        );
    } else {
        let clip = &prepared[0];
        println!("Created {} video pack: {}x{} -> {}x{}, {:.6}s. Set DARK_VIDEO_MOD_DIR to {} before launch.",
            clip.authored.codec, clip.stock.width, clip.stock.height, clip.authored.width,
            clip.authored.height, clip.authored.duration, output.display());
    }
    Ok(())
}

const USAGE: &str = "usage: darkrecomp-video-pack create GAME_DIR RELATIVE_VIDEO REPLACEMENT NEW_PACK_DIR [--allow-aspect-change]\n       darkrecomp-video-pack create-batch GAME_DIR MAP.tsv NEW_PACK_DIR [--allow-aspect-change]\n       darkrecomp-video-pack check MANIFEST";

fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let result = match args.first().and_then(|x| x.to_str()) {
        Some("create") => create(&args[1..], false),
        Some("create-batch") => create(&args[1..], true),
        Some("check") if args.len() == 2 => (|| -> Result<()> {
            let path = absolute(Path::new(&args[1]))?; ordinary(&path)?;
            let mut bytes = Vec::new(); File::open(path)?.take(65537).read_to_end(&mut bytes)?;
            let entries = parse(&bytes)?; println!("{} video entries; manifest syntax is valid.", entries.len()); Ok(())
        })(),
        _ => Err(USAGE.into()),
    };
    if let Err(error) = result { eprintln!("Video pack: {error}"); std::process::exit(1); }
}
