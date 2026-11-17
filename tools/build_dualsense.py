"""Build the Rust HID backend using a content-keyed root-crate invocation.

Cargo's dependency cache remains reusable, but restoring older source mtimes
cannot leave a stale native static library behind a new C++ input fingerprint.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,tempfile

p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--cargo',required=True);args=p.parse_args()
root=args.root.resolve();crate=root/'runtime/dualsense';output=args.output.resolve();output.parent.mkdir(parents=True,exist_ok=True)
files=[crate/'Cargo.toml',crate/'Cargo.lock',Path(__file__).resolve(),*sorted((crate/'src').rglob('*.rs'))]
hashes={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
digest=hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest()
subprocess.run([args.cargo,'rustc','--manifest-path',str(crate/'Cargo.toml'),'--locked','--lib','--release',
    '--target','x86_64-pc-windows-msvc','--target-dir',str(output.parent/'cargo'),'--','--cfg',f'dark_dualsense_source="{digest}"'],check=True)
library=output.parent/'cargo/x86_64-pc-windows-msvc/release/dark_dualsense.lib'
if not output.is_file() or hashlib.sha256(output.read_bytes()).digest()!=hashlib.sha256(library.read_bytes()).digest():
    with tempfile.NamedTemporaryFile(dir=output.parent,delete=False) as f:temporary=Path(f.name)
    try:shutil.copyfile(library,temporary);os.replace(temporary,output)
    finally:temporary.unlink(missing_ok=True)
(output.parent/'inputs.json').write_text(json.dumps({'fingerprint':digest,'inputs':hashes},indent=2)+'\n')
