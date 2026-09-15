import os, glob, subprocess

fxc_path = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
fp_files = glob.glob("Darkness/System/Gl/ARB_fragment_program/*.fp")

from arb_to_hlsl import transpile_arb

success = 0
failed = 0
errors = []

for fp in fp_files:
    hlsl_path = fp[:-3] + ".hlsl"
    try:
        with open(fp, "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()
        hlsl_code = transpile_arb(content, base_dir=os.path.dirname(fp))
        with open(hlsl_path, "w", encoding="utf-8") as f:
            f.write(hlsl_code)

        # Test compile with fxc
        res = subprocess.run([fxc_path, "/nologo", "/T", "ps_5_0", "/E", "main", hlsl_path], capture_output=True, text=True)
        if res.returncode == 0:
            success += 1
        else:
            failed += 1
            errors.append((os.path.basename(fp), res.stderr or res.stdout))
    except Exception as e:
        failed += 1
        errors.append((os.path.basename(fp), str(e)))

print(f"Batch Transpile & FXC Compilation Result:")
print(f"  Successfully compiled: {success}/{len(fp_files)} shaders ({success * 100 // len(fp_files)}%)")
print(f"  Requires instruction expansion: {failed}/{len(fp_files)} shaders")
if errors:
    print("\nSample compilation diagnostics:")
    for name, err in errors[:5]:
        first_line = err.strip().splitlines()[0] if err.strip() else "Unknown"
        print(f"  - {name}: {first_line}")
