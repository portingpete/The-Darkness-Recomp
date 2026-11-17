//! Steam Deck desktop wizard. The existing Python installer owns file updates.
use std::env;
use std::ffi::OsString;
use std::fs::{self, File, OpenOptions};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

type Result<T> = std::result::Result<T, String>;
const TITLE: &str = "The Darkness Recomp — Steam Deck setup";
const HELP: &str = "The Darkness Recomp Steam Deck installer\n\n\
Run in Desktop Mode to choose your own extracted Xbox 360 dump and installation folder.\n\
Requires Python 3.10+, and KDialog or Zenity for the graphical wizard.\n\
The installer downloads verified UMU/GE-Proton tools. First play may download Steam Runtime.\n\
It never downloads game data or launches the game. Use a Linux filesystem such as ext4.\n\n\
Options:\n\
  --source DIR          Extracted release (default: bundled AppImage payload)\n\
  --game-dir DIR        Owned dump containing default.xex, Content and System\n\
  --install-dir DIR     Exact destination (wizard otherwise selects a parent folder)\n\
  --umu-archive FILE    Verified local UMU archive for offline setup\n\
  --proton-archive FILE Verified local GE-Proton archive for offline setup\n\
  --skip-download      Fail if pinned tool archives are unavailable\n\
  --yes                Noninteractive; requires --game-dir and --install-dir\n\
  --help               Show this help\n";

#[derive(Default, Debug)]
struct Options {
    source: Option<PathBuf>,
    game: Option<PathBuf>,
    install: Option<PathBuf>,
    umu: Option<PathBuf>,
    proton: Option<PathBuf>,
    skip_download: bool,
    yes: bool,
    help: bool,
}

fn parse(args: impl IntoIterator<Item = OsString>) -> Result<Options> {
    let mut options = Options::default();
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        let field = match arg.to_str() {
            Some("--source") => &mut options.source,
            Some("--game-dir") => &mut options.game,
            Some("--install-dir") => &mut options.install,
            Some("--umu-archive") => &mut options.umu,
            Some("--proton-archive") => &mut options.proton,
            Some("--skip-download") => {
                options.skip_download = true;
                continue;
            }
            Some("--yes") => {
                options.yes = true;
                continue;
            }
            Some("--help" | "-h") => {
                options.help = true;
                continue;
            }
            _ => {
                return Err(format!(
                    "Unknown option: {}. Use --help.",
                    arg.to_string_lossy()
                ))
            }
        };
        let value = args
            .next()
            .ok_or_else(|| format!("Missing value for {}", arg.to_string_lossy()))?;
        if value.is_empty() || value.to_string_lossy().starts_with("--") {
            return Err(format!("Missing path for {}", arg.to_string_lossy()));
        }
        if field.replace(PathBuf::from(value)).is_some() {
            return Err(format!("Repeated option: {}", arg.to_string_lossy()));
        }
    }
    Ok(options)
}

fn program(name: &str) -> Option<PathBuf> {
    env::split_paths(&env::var_os("PATH").unwrap_or_default())
        .map(|path| path.join(name))
        .find(|path| executable(path))
}

fn executable(path: &Path) -> bool {
    if !path.is_file() {
        return false;
    }
    #[cfg(unix)]
    {
        use std::ffi::CString;
        use std::os::raw::{c_char, c_int};
        use std::os::unix::ffi::OsStrExt;
        extern "C" {
            fn access(path: *const c_char, mode: c_int) -> c_int;
        }
        let Ok(path) = CString::new(path.as_os_str().as_bytes()) else {
            return false;
        };
        // This unprivileged installer must skip PATH entries its user cannot
        // execute, including ACL restrictions. X_OK is 1 on POSIX systems.
        // SAFETY: CString owns a live, NUL-terminated path for the entire call;
        // access only reads it and does not retain the pointer.
        unsafe { access(path.as_ptr(), 1) == 0 }
    }
    #[cfg(not(unix))]
    {
        true
    }
}

#[derive(Clone, Debug)]
enum Dialog {
    Kde(PathBuf),
    Zenity(PathBuf),
}

// KDialog labels can interpret rich text. Selected filenames must stay text.
fn markup(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\n', "<br/>")
}

impl Dialog {
    fn detect() -> Option<Self> {
        program("kdialog")
            .map(Self::Kde)
            .or_else(|| program("zenity").map(Self::Zenity))
    }

    fn command(&self) -> Command {
        let mut cmd = match self {
            Self::Kde(path) | Self::Zenity(path) => Command::new(path),
        };
        cmd.arg("--title").arg(TITLE);
        cmd
    }

    fn choose(&self, title: &str, initial: &Path) -> Result<Option<PathBuf>> {
        let mut cmd = self.command();
        match self {
            Self::Kde(_) => {
                cmd.arg("--getexistingdirectory")
                    .arg(initial)
                    .arg("--title")
                    .arg(title);
            }
            Self::Zenity(_) => {
                let mut initial = initial.as_os_str().to_os_string();
                initial.push("/");
                cmd.args(["--file-selection", "--directory", "--title"])
                    .arg(title)
                    .arg("--filename")
                    .arg(initial);
            }
        }
        let output = cmd
            .output()
            .map_err(|e| format!("Cannot open folder picker: {e}"))?;
        if output.status.code() == Some(1) {
            return Ok(None);
        }
        if !output.status.success() {
            return Err("The folder picker failed. Run this installer in Desktop Mode.".into());
        }
        let text = String::from_utf8(output.stdout)
            .map_err(|_| "Folder picker returned a non-UTF-8 path")?;
        let text = text.strip_suffix('\n').unwrap_or(&text);
        let text = text.strip_suffix('\r').unwrap_or(text);
        if text.is_empty() {
            return Ok(None);
        }
        Ok(Some(PathBuf::from(text)))
    }

    fn confirm(&self, text: &str) -> Result<bool> {
        let mut cmd = self.command();
        match self {
            Self::Kde(_) => {
                cmd.arg("--yesno").arg(markup(text)).args([
                    "--yes-label",
                    "Install",
                    "--no-label",
                    "Cancel",
                ]);
            }
            Self::Zenity(_) => {
                cmd.args(["--question", "--no-markup", "--text"])
                    .arg(text)
                    .args(["--ok-label=Install", "--cancel-label=Cancel"]);
            }
        }
        let status = cmd
            .status()
            .map_err(|e| format!("Cannot open confirmation: {e}"))?;
        match status.code() {
            Some(0) => Ok(true),
            Some(1) => Ok(false),
            Some(2) if matches!(self, Self::Kde(_)) => Ok(false),
            _ => Err("The confirmation dialog failed.".into()),
        }
    }

    fn message(&self, text: &str, error: bool) {
        let mut cmd = self.command();
        match self {
            Self::Kde(_) => {
                cmd.arg(if error { "--error" } else { "--msgbox" })
                    .arg(markup(text));
            }
            Self::Zenity(_) => {
                cmd.arg(if error { "--error" } else { "--info" })
                    .args(["--no-markup", "--text"])
                    .arg(text);
            }
        }
        let _ = cmd.status();
    }

    fn progress(&self, log: &Path) -> Progress {
        let text = format!("Installing The Darkness Recomp. Downloads and copying can take several minutes.\n\nLog: {}", log.display());
        // Zenity stays alive until its input closes. KDialog's progressbar instead
        // returns a D-Bus address; use qdbus to close that separate dialog.
        match self {
            Self::Zenity(_) => {
                let mut cmd = self.command();
                cmd.args([
                    "--progress",
                    "--pulsate",
                    "--no-cancel",
                    "--auto-close",
                    "--no-markup",
                    "--text",
                ])
                .arg(text)
                .stdin(Stdio::piped())
                .stdout(Stdio::null())
                .stderr(Stdio::null());
                Progress::Child(cmd.spawn().ok())
            }
            Self::Kde(_) => {
                let dbus = [
                    "qdbus",
                    "qdbus6",
                    "qdbus-qt6",
                    "/usr/lib/qt6/bin/qdbus",
                    "/usr/lib/qt5/bin/qdbus",
                ]
                .iter()
                .find_map(|name| program(name));
                if let Some(dbus) = dbus {
                    if let Ok(output) = self
                        .command()
                        .arg("--progressbar")
                        .arg(markup(&text))
                        .arg("0")
                        .output()
                    {
                        let address = String::from_utf8_lossy(&output.stdout)
                            .split_whitespace()
                            .map(String::from)
                            .collect::<Vec<_>>();
                        if output.status.success() && address.len() == 2 {
                            // The setup engine safely preserves partial work,
                            // but has no interactive cancellation protocol.
                            let hidden = Command::new(&dbus)
                                .args(&address)
                                .args(["showCancelButton", "false"])
                                .stdout(Stdio::null())
                                .stderr(Stdio::null())
                                .status();
                            if matches!(hidden, Ok(status) if status.success()) {
                                return Progress::Dbus {
                                    program: dbus,
                                    address,
                                };
                            }
                            let _ = Command::new(&dbus)
                                .args(&address)
                                .arg("close")
                                .stdout(Stdio::null())
                                .stderr(Stdio::null())
                                .spawn();
                        }
                    }
                }
                // Desktop notifications can outlive kdialog's process. Keep this
                // fallback brief; only the D-Bus progressbar is closeable below.
                let notice = format!(
                    "Setup has started. A result dialog will appear when it finishes.\n\nLog: {}",
                    log.display()
                );
                let child = self
                    .command()
                    .arg("--passivepopup")
                    .arg(markup(&notice))
                    .arg("10")
                    .stdout(Stdio::null())
                    .stderr(Stdio::null())
                    .spawn()
                    .ok();
                Progress::Child(child)
            }
        }
    }
}

enum Progress {
    Child(Option<Child>),
    Dbus {
        program: PathBuf,
        address: Vec<String>,
    },
}

impl Drop for Progress {
    fn drop(&mut self) {
        match self {
            Self::Child(Some(child)) => {
                // Do not wait indefinitely on a GUI process during shutdown.
                child.stdin.take();
                let _ = child.kill();
                let _ = child.wait();
            }
            Self::Dbus { program, address } => {
                let _ = Command::new(program)
                    .args(address)
                    .arg("close")
                    .stdout(Stdio::null())
                    .stderr(Stdio::null())
                    .spawn();
            }
            _ => {}
        }
    }
}

fn bundled_source() -> Result<PathBuf> {
    let executable = env::current_exe().map_err(|e| e.to_string())?;
    let bin = executable
        .parent()
        .ok_or("Cannot find installer directory")?;
    Ok(bin.join("../share/darkrecomp/release"))
}

fn require_absolute(path: &Path, label: &str) -> Result<()> {
    if !path.is_absolute() {
        return Err(format!(
            "{label} must be an absolute path: {}",
            path.display()
        ));
    }
    Ok(())
}

fn regular(path: &Path) -> bool {
    fs::symlink_metadata(path)
        .map(|m| m.is_file() && m.len() > 0)
        .unwrap_or(false)
}

fn check_source(source: &Path) -> Result<()> {
    for name in [
        "tools/setup_linux.py",
        "Launch.sh",
        "build_native/Release/DarkRecomp.exe",
    ] {
        if !regular(&source.join(name)) {
            return Err(format!(
                "The bundled release is incomplete: {}",
                source.join(name).display()
            ));
        }
    }
    Ok(())
}

fn check_game(game: &Path) -> Result<()> {
    // setup_linux.py handles case-insensitive dump names and checks the full tree.
    let entries =
        fs::read_dir(game).map_err(|e| format!("Cannot read game dump {}: {e}", game.display()))?;
    let mut found = [false; 3];
    for entry in entries {
        let entry = entry.map_err(|e| e.to_string())?;
        let name = entry.file_name().to_string_lossy().to_lowercase();
        let kind = entry.file_type().map_err(|e| e.to_string())?;
        match name.as_str() {
            "default.xex" => found[0] = regular(&entry.path()),
            "content" => found[1] = kind.is_dir(),
            "system" => found[2] = kind.is_dir(),
            _ => {}
        }
    }
    if !found.iter().all(|value| *value) {
        return Err("Select the complete extracted Xbox 360 dump folder containing default.xex, Content and System. An ISO alone cannot be installed.".into());
    }
    Ok(())
}

fn python() -> Result<PathBuf> {
    let path = program("python3")
        .ok_or("Python 3.10+ is required. Run in Steam Deck Desktop Mode with Python installed.")?;
    let status = Command::new(&path)
        .args([
            "-I",
            "-c",
            "import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)",
        ])
        .status()
        .map_err(|e| format!("Cannot run Python: {e}"))?;
    if !status.success() {
        return Err("Python 3.10 or newer is required.".into());
    }
    Ok(path)
}

fn log_file() -> Result<(PathBuf, File)> {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos();
    let directory =
        env::temp_dir().join(format!("darkrecomp-setup-{}-{stamp}", std::process::id()));
    #[allow(unused_mut)]
    let mut builder = fs::DirBuilder::new();
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        builder.mode(0o700);
    }
    builder
        .create(&directory)
        .map_err(|e| format!("Cannot create setup log folder: {e}"))?;
    let path = directory.join("install.log");
    let file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
        .map_err(|e| e.to_string())?;
    Ok((path, file))
}

fn setup_command(
    python: &Path,
    source: &Path,
    game: &Path,
    install: &Path,
    options: &Options,
) -> Command {
    let mut cmd = Command::new(python);
    let engine = if options.source.is_some() {
        source.join("tools/setup_linux.py")
    } else {
        source.parent().unwrap_or(source).join("setup_linux.py")
    };
    cmd.arg("-I")
        .arg(engine)
        .arg("--source")
        .arg(source)
        .arg("--game-dir")
        .arg(game)
        .arg("--install-dir")
        .arg(install);
    if let Some(path) = &options.umu {
        cmd.arg("--umu-archive").arg(path);
    }
    if let Some(path) = &options.proton {
        cmd.arg("--proton-archive").arg(path);
    }
    if options.skip_download {
        cmd.arg("--skip-download");
    }
    cmd
}

fn desktop_string(text: &str) -> String {
    text.replace('\\', "\\\\")
        .replace('\n', "\\n")
        .replace('\r', "\\r")
        .replace('\t', "\\t")
}

fn desktop_arg(path: &Path) -> String {
    let mut escaped = String::from("\"");
    for ch in path.to_string_lossy().chars() {
        if matches!(ch, '\\' | '"' | '`' | '$') {
            escaped.push('\\');
        }
        if ch == '%' {
            escaped.push('%');
        }
        escaped.push(ch);
    }
    escaped.push('"');
    // Desktop entry string escaping is a separate pass from Exec quoting.
    desktop_string(&escaped)
}

fn desktop_text(install: &Path) -> String {
    format!("[Desktop Entry]\nType=Application\nName=The Darkness (DarkRecomp)\nExec={}\nPath={}\nIcon=applications-games\nTerminal=false\nCategories=Game;\n",
        desktop_arg(&install.join("play-linux.sh")), desktop_string(&install.join("game").to_string_lossy()))
}

fn shortcut(install: &Path) -> Result<PathBuf> {
    let path = install.join("Play-The-Darkness.desktop");
    let text = desktop_text(install);
    if let Ok(metadata) = fs::symlink_metadata(&path) {
        if !metadata.is_file() || fs::read_to_string(&path).ok().as_deref() != Some(&text) {
            return Err(format!(
                "Preserved an existing different launcher: {}",
                path.display()
            ));
        }
        return Ok(path);
    }
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o755);
    }
    use std::io::Write;
    options
        .open(&path)
        .and_then(|mut file| file.write_all(text.as_bytes()))
        .map_err(|e| format!("Cannot create launcher {}: {e}", path.display()))?;
    Ok(path)
}

fn log_tail(path: &Path) -> String {
    use std::io::{Seek, SeekFrom};
    let Ok(mut file) = File::open(path) else {
        return String::new();
    };
    let _ = file
        .seek(SeekFrom::End(-4096))
        .or_else(|_| file.seek(SeekFrom::Start(0)));
    let mut bytes = Vec::new();
    let _ = file.take(4096).read_to_end(&mut bytes);
    String::from_utf8_lossy(&bytes).into_owned()
}

fn run(options: Options, gui: Option<&Dialog>) -> Result<()> {
    let source = options
        .source
        .clone()
        .map(Ok)
        .unwrap_or_else(bundled_source)?;
    require_absolute(&source, "Release folder")?;
    check_source(&source)?;
    let python = python()?;
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(env::temp_dir);
    let game = match options.game.clone() {
        Some(path) => path,
        None if options.yes => return Err("--yes requires --game-dir and --install-dir".into()),
        None => match gui
            .ok_or("KDialog or Zenity is required for the folder picker")?
            .choose(
                "Choose your extracted Xbox 360 dump (default.xex, Content, System)",
                &home,
            )? {
            Some(path) => path,
            None => return Ok(()),
        },
    };
    require_absolute(&game, "Game dump")?;
    check_game(&game)?;
    let install = match options.install.clone() {
        Some(path) => path,
        None if options.yes => return Err("--yes requires --game-dir and --install-dir".into()),
        None => match gui.ok_or("KDialog or Zenity is required for the folder picker")?
            .choose("Choose a parent folder — The-Darkness-Recomp will be created here (Linux filesystem)", &home)? {
                Some(path) => path.join("The-Darkness-Recomp"), None => return Ok(()),
            },
    };
    require_absolute(&install, "Installation folder")?;
    for path in [&options.umu, &options.proton].into_iter().flatten() {
        require_absolute(path, "Offline archive")?;
    }
    if !options.yes {
        let text = format!("Install The Darkness Recomp?\n\nYour extracted dump:\n{}\n\nInstallation folder:\n{}\n\nSetup copies your dump and downloads verified UMU and GE-Proton tools (several GB of free space and an internet connection are needed). Use ext4 or another Linux filesystem. Existing saves and settings are preserved.\n\nThe game will not start during setup. Steam Runtime may download on first play.", game.display(), install.display());
        if !gui.ok_or("KDialog or Zenity is required. Use --yes with explicit paths for noninteractive setup.")?.confirm(&text)? { return Ok(()); }
    }
    let (log, file) = log_file()?;
    println!(
        "Installing to {}\nSetup log: {}",
        install.display(),
        log.display()
    );
    let progress = gui.map(|gui| gui.progress(&log));
    let mut command = setup_command(&python, &source, &game, &install, &options);
    let status = command
        .stdout(Stdio::from(file.try_clone().map_err(|e| e.to_string())?))
        .stderr(Stdio::from(file))
        .status()
        .map_err(|e| format!("Cannot start setup: {e}\nLog: {}", log.display()))?;
    drop(progress);
    if !status.success() {
        return Err(format!("Installation failed ({}).\n\n{}\n\nFull log: {}\nResolve the error above before rerunning the installer. If the destination was rejected, choose a folder on a Linux filesystem.", status, log_tail(&log), log.display()));
    }
    if !regular(&install.join("play-linux.sh")) {
        return Err(format!(
            "Setup did not create play-linux.sh.\nFull log: {}",
            log.display()
        ));
    }
    let launcher = shortcut(&install)?;
    let text = format!("Installation complete.\n\nOpen this launcher in Desktop Mode:\n{}\n\nFor Steam Gaming Mode, add this file as a Non-Steam Game:\n{}\nSet Start In to:\n{}\nChoose an installed Proton tool under Properties > Compatibility and use Steam Input's Gamepad template.\n\nYour installed files stay in the chosen folder; the installer AppImage is no longer needed.\nSetup log: {}", launcher.display(), install.join("game/build_native/Release/DarkRecomp.exe").display(), install.join("game").display(), log.display());
    println!("{text}");
    if let Some(gui) = gui {
        gui.message(&text, false);
    }
    Ok(())
}

fn main() {
    let parsed = parse(env::args_os().skip(1));
    if matches!(&parsed, Ok(options) if options.help) {
        print!("{HELP}");
        return;
    }
    let gui = if matches!(&parsed, Ok(options) if options.yes) {
        None
    } else {
        Dialog::detect()
    };
    let result = parsed.and_then(|options| run(options, gui.as_ref()));
    if let Err(error) = result {
        eprintln!("{error}");
        if let Some(gui) = gui {
            gui.message(&error, true);
        }
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::OsStr;

    #[test]
    fn options_preserve_paths_without_shell_parsing() {
        let path = "/run/media/deck/My $Games `x` 100% & \"quoted\"";
        let options = parse(
            [
                "--game-dir",
                path,
                "--source",
                "/payload",
                "--install-dir",
                "/home/deck/game",
                "--yes",
            ]
            .map(OsString::from),
        )
        .unwrap();
        let cmd = setup_command(
            Path::new("python3"),
            Path::new("/payload"),
            options.game.as_ref().unwrap(),
            options.install.as_ref().unwrap(),
            &options,
        );
        let script = Path::new("/payload").join("tools/setup_linux.py");
        assert_eq!(
            cmd.get_args().collect::<Vec<_>>(),
            vec![
                OsStr::new("-I"),
                script.as_os_str(),
                OsStr::new("--source"),
                OsStr::new("/payload"),
                OsStr::new("--game-dir"),
                OsStr::new(path),
                OsStr::new("--install-dir"),
                OsStr::new("/home/deck/game")
            ]
        );
    }

    #[test]
    fn invalid_cli_is_rejected_before_work() {
        for args in [
            vec!["--unknown"],
            vec!["--source"],
            vec!["--source", "--yes"],
            vec!["--source", "/a", "--source", "/b"],
        ] {
            assert!(parse(args.into_iter().map(OsString::from)).is_err());
        }
    }

    #[test]
    fn desktop_escapes_field_codes_and_both_quoting_layers() {
        assert_eq!(desktop_arg(Path::new("/a b")), r#""/a b""#);
        assert_eq!(desktop_arg(Path::new("/100%")), r#""/100%%""#);
        assert_eq!(desktop_arg(Path::new("/$x")), r#""/\\$x""#);
        assert_eq!(desktop_arg(Path::new("/\"x\"")), r#""/\\"x\\"""#);
        assert_eq!(desktop_arg(Path::new(r"/a\b")), r#""/a\\\\b""#);
        assert_eq!(desktop_string("a\nb\tc\rd\\e"), "a\\nb\\tc\\rd\\\\e");
    }

    #[test]
    fn dialog_labels_escape_markup_from_filenames() {
        assert_eq!(markup("<&>\"\n"), "&lt;&amp;&gt;&quot;<br/>");
    }
}
