import re, sys, os

def clean_arg(arg):
    return arg.strip().rstrip(";")

def _eval_xrg_cond(cond):
    # PC reference build: platform_pc=true, xenon=false.
    cond = cond.strip().lower()
    if cond == "xenon":
        return False
    if cond == "platform_pc":
        return True
    return False

def _preprocess_xrg(arb_text, base_dir=None, _depth=0):
    """Expand Starbreeze XRG wrappers into flat ARB lines.

    Handles: *INCLUDE "file.fph" inlining, @FOGCONST-style *defines
    substitution, and @if/@ifnot/@else/@endif for platform_pc/xenon.
    """
    if _depth > 5:
        return arb_text.splitlines()
    raw_lines = arb_text.splitlines()
    # 1. Collect numeric *defines for @NAME substitution.
    defines = {}
    for ln in raw_lines:
        s = ln.strip().lstrip("@")
        m = re.match(r"\*([A-Za-z0-9_]+)\s+(-?\d+)\s*$", s)
        if m:
            defines[m.group(1)] = m.group(2)
    # Also collect defines without * inside *defines blocks (e.g. "*FOGCONST0 0").
    out_lines = []
    # Conditional stack: list of (outer_active, cond_result); active = outer and cond.
    # Start with single True.
    active_stack = [True]
    cond_stack = []  # parallel: cond result for current level (for @else)
    for ln in raw_lines:
        s = ln.strip()
        # Resolve *INCLUDE "file"
        if "*INCLUDE" in s:
            m = re.search(r'\*INCLUDE\s+"([^"]+)"', s)
            if m and base_dir:
                inc_path = os.path.join(base_dir, m.group(1))
                if os.path.exists(inc_path):
                    try:
                        with open(inc_path, "r", encoding="utf-8", errors="ignore") as f:
                            inc_text = f.read()
                        for il in _preprocess_xrg(inc_text, base_dir, _depth + 1):
                            # Apply defines substitution to included lines too.
                            for k, v in defines.items():
                                il = il.replace("@" + k, v)
                            out_lines.append(il)
                    except Exception:
                        pass
            continue
        # Substitute @DEFINES (e.g. [@FOGTEXCOORD0] -> [0]).
        if "@" in s and defines:
            for k, v in defines.items():
                s = s.replace("@" + k, v)
        # Handle @if/@ifnot/@else/@endif (strip leading @ for matching).
        t = s.lstrip("@").strip()
        if t.startswith("ifnot ") or t.startswith("ifnot\t"):
            cond = t[6:].strip().split()[0] if t[6:].strip() else ""
            outer = all(active_stack)
            res = not _eval_xrg_cond(cond)
            active_stack.append(outer and res)
            cond_stack.append(res)
            continue
        if t.startswith("if ") or t.startswith("if\t"):
            cond = t[3:].strip().split()[0] if t[3:].strip() else ""
            outer = all(active_stack)
            res = _eval_xrg_cond(cond)
            active_stack.append(outer and res)
            cond_stack.append(res)
            continue
        if t == "else" or t.startswith("else ") or t.startswith("else\t"):
            if len(active_stack) > 1 and cond_stack:
                old_cond = cond_stack.pop()
                active_stack.pop()
                outer = all(active_stack)
                new_cond = not old_cond
                active_stack.append(outer and new_cond)
                cond_stack.append(new_cond)
            continue
        if t == "endif" or t.startswith("endif"):
            if len(active_stack) > 1:
                active_stack.pop()
                if cond_stack:
                    cond_stack.pop()
            continue
        if not all(active_stack):
            continue
        # Strip a single leading @ from PARAM/TEMP etc. (@PARAM16 -> PARAM16).
        if s.startswith("@"):
            s = s[1:]
        out_lines.append(s)
    return out_lines

def transpile_arb(arb_text, entry_name="main", base_dir=None):
    lines = _preprocess_xrg(arb_text, base_dir)
    attribs = {}
    temps = set()
    params = {}
    outputs = {}
    env_params = {}

    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("ATTRIB"):
            m = re.match(r"ATTRIB\s+([A-Za-z0-9_]+)\s*=\s*(?:fragment\.)?texcoord\[(\d+)\]", line)
            if m:
                attribs[m.group(1)] = ("TEXCOORD", int(m.group(2)))
            else:
                m_col = re.match(r"ATTRIB\s+([A-Za-z0-9_]+)\s*=\s*(?:fragment\.)?color(?:\[(\d+)\])?", line)
                if m_col:
                    idx = int(m_col.group(2)) if m_col.group(2) else 0
                    attribs[m_col.group(1)] = ("COLOR", idx)
        elif line.startswith("PARAM16") or line.startswith("PARAM"):
            m = re.match(r"PARAM(?:16)?\s+([A-Za-z0-9_]+)\s*=\s*\{\s*([^}]+)\s*\}", line)
            if m:
                pname, vals = m.group(1), m.group(2)
                params[pname] = f"static const float4 {pname} = float4({vals});"
            elif "program.env" in line:
                m2 = re.match(r"PARAM(?:16)?\s+([A-Za-z0-9_]+)\s*=\s*program\.env\[(\d+)\]", line)
                if m2:
                    pname, idx = m2.group(1), int(m2.group(2))
                    env_params[pname] = idx
        elif line.startswith("TEMP16") or line.startswith("TEMP"):
            m = re.match(r"TEMP(?:16)?\s+([^;]+);", line)
            if m:
                for t in m.group(1).split(","):
                    t = t.strip()
                    if t: temps.add(t)
        elif line.startswith("OUTPUT"):
            m = re.match(r"OUTPUT\s+([A-Za-z0-9_]+)\s*=\s*result\.color(?:\[(\d+)\])?", line)
            if m:
                oname = m.group(1)
                oidx = int(m.group(2)) if m.group(2) else 0
                outputs[oname] = oidx

    out = []
    out.append("// Transpiled from Starbreeze P5 ARB fragment program")
    out.append("struct PS_INPUT {")
    out.append("    float4 pos : SV_POSITION;")
    for name, (sem, idx) in sorted(attribs.items(), key=lambda x: (x[1][0], x[1][1])):
        out.append(f"    float4 {name} : {sem}{idx};")
    out.append("};")
    out.append("")

    out.append("struct PS_OUTPUT {")
    if not outputs:
        outputs["oCol"] = 0
    for oname, oidx in sorted(outputs.items(), key=lambda x: x[1]):
        out.append(f"    float4 {oname} : SV_Target{oidx};")
    out.append("};")
    out.append("")

    if env_params:
        out.append("cbuffer EnvConsts : register(b0) {")
        for pname, idx in sorted(env_params.items(), key=lambda x: x[1]):
            out.append(f"    float4 {pname};")
        out.append("};")
        out.append("")

    for pdef in params.values():
        out.append(pdef)
    out.append("")

    # Declare only samplers actually referenced (ps_5_0 has 16 sampler slots).
    # Scan for TEX ..., texture[N], <kind> to decide Sample vs SampleCmp.
    _used = {}  # idx -> set("2D","SHADOW","CUBE")
    for _ln in lines:
        _s = _ln.strip()
        if not _s or _s.startswith("#"):
            continue
        _m = re.search(r"\bTEX\w*\s+[^,]+,\s*[^,]+,\s*texture\[(\d+)\]\s*,\s*([A-Za-z0-9_]+)", _s)
        if _m:
            _idx, _kind = int(_m.group(1)), _m.group(2).upper()
            _used.setdefault(_idx, set()).add(_kind)
    for _idx in sorted(_used):
        _kinds = _used[_idx]
        out.append(f"Texture2D texture{_idx} : register(t{_idx});")
        if any("SHADOW" in k for k in _kinds):
            out.append(f"SamplerComparisonState samplerCmp{_idx} : register(s{_idx});")
        else:
            out.append(f"SamplerState sampler{_idx} : register(s{_idx});")
        # If a texture is used both ways (rare), alias normal sampler too.
        if any("SHADOW" in k for k in _kinds) and any("SHADOW" not in k and "CUBE" not in k for k in _kinds):
            out.append(f"SamplerState sampler{_idx} : register(s{_idx}); // also sampled normally")
    # Cube sampler (fog) at high slot; only if CUBE referenced.
    _needs_cube = any("CUBE" in k for v in _used.values() for k in v)
    if _needs_cube or True:
        # Always declare cube (cheap, one slot) but avoid clobbering 2D-15:
        # use t14/s14 if 2D-14 unused else t15/s15.
        _cube_slot = 14 if 14 not in _used else 15
        out.append(f"TextureCube textureCube2 : register(t{_cube_slot});")
        out.append(f"SamplerState samplerCube2 : register(s{_cube_slot});")
    out.append("")

    out.append(f"PS_OUTPUT {entry_name}(PS_INPUT input) : SV_Target {{")
    out.append("    PS_OUTPUT output = (PS_OUTPUT)0;")
    
    for t in sorted(temps):
        out.append(f"    float4 {t} = float4(0, 0, 0, 0);")

    def map_operand(opnd):
        opnd = opnd.strip().rstrip(";")
        if not opnd: return opnd
        neg = ""
        if opnd.startswith("-"):
            neg = "-"
            opnd = opnd[1:]
        # Inline constant vector {a, b, c, d} -> float4(a, b, c, d).
        if opnd.startswith("{"):
            inner = opnd.strip("{} ")
            return f"{neg}float4({inner})"
        if opnd in attribs:
            return f"{neg}input.{opnd}"
        if opnd in outputs:
            return f"{neg}output.{opnd}"
        # swizzle on attrib
        base = opnd.split(".")[0]
        swz = ("." + opnd.split(".")[1]) if "." in opnd else ""
        if base in attribs:
            return f"{neg}input.{base}{swz}"
        if base in outputs:
            return f"{neg}output.{base}{swz}"
        return f"{neg}{opnd}"

    def map_dest(dst):
        dst = dst.strip().rstrip(";")
        base = dst.split(".")[0]
        swz = ("." + dst.split(".")[1]) if "." in dst else ""
        if base in outputs:
            return f"output.{base}{swz}"
        return dst

    for line in lines:
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("OPTION") or line.startswith("ATTRIB") or line.startswith("PARAM") or line.startswith("TEMP") or line.startswith("OUTPUT") or line == "END":
            continue
        # Skip XRG structural lines (already expanded; residual *directives).
        if line.startswith("*") or line.startswith('"') or line == "{" or line == "}":
            continue
        if line.startswith("@if") or line.startswith("@else") or line.startswith("@endif"):
            continue
        if "#" in line:
            line = line[:line.index("#")].strip()
        if not line: continue

        parts = line.split(None, 1)
        op = parts[0]
        # Split args on commas, but not inside {...} (inline constant vectors).
        def _split_args(s):
            res, cur, depth = [], "", 0
            for ch in s:
                if ch == "{":
                    depth += 1
                    cur += ch
                elif ch == "}":
                    depth = max(0, depth - 1)
                    cur += ch
                elif ch == "," and depth == 0:
                    res.append(cur)
                    cur = ""
                else:
                    cur += ch
            res.append(cur)
            return res
        raw_args = [clean_arg(a) for a in _split_args(parts[1])] if len(parts) > 1 else []
        args = [map_operand(a) for a in raw_args]
        dst = map_dest(raw_args[0]) if raw_args else ""

        is_sat = "_SAT" in op
        base_op = op.replace("_SAT", "")

        def emit_expr(expr):
            if is_sat:
                out.append(f"    {dst} = saturate({expr});")
            else:
                out.append(f"    {dst} = {expr};")

        _num_re = re.compile(r"^[\+\-]?\d+(\.\d*)?([eE][\+\-]?\d+)?$")
        def _is_scalar_lit(a):
            return bool(_num_re.match(a.strip()))

        if base_op == "MOV":
            emit_expr(f"{args[1]}")
        elif base_op == "MAD":
            emit_expr(f"({args[1]}) * ({args[2]}) + ({args[3]})")
        elif base_op == "MUL":
            emit_expr(f"({args[1]}) * ({args[2]})")
        elif base_op == "ADD":
            emit_expr(f"({args[1]}) + ({args[2]})")
        elif base_op == "SUB":
            emit_expr(f"({args[1]}) - ({args[2]})")
        elif base_op == "DP3":
            b = args[2] if not _is_scalar_lit(raw_args[2]) else f"float3({raw_args[2].strip()}, {raw_args[2].strip()}, {raw_args[2].strip()})"
            emit_expr(f"dot(({args[1]}).xyz, ({b}).xyz)")
        elif base_op == "DP4":
            b = args[2] if not _is_scalar_lit(raw_args[2]) else f"float4({raw_args[2].strip()}, {raw_args[2].strip()}, {raw_args[2].strip()}, {raw_args[2].strip()})"
            emit_expr(f"dot({args[1]}, {b})")
        elif base_op == "MIN":
            emit_expr(f"min({args[1]}, {args[2]})")
        elif base_op == "MAX":
            emit_expr(f"max({args[1]}, {args[2]})")
        elif base_op == "FLR":
            emit_expr(f"floor({args[1]})")
        elif base_op == "FRC":
            emit_expr(f"frac({args[1]})")
        elif base_op == "LRP":
            # ARB LRP dst, s0, s1, s2  =>  s0*s2 + s1*(1-s2)
            emit_expr(f"(({args[1]}) * ({args[3]}) + ({args[2]}) * (1.0f - ({args[3]})))")
        elif base_op == "RSQ":
            emit_expr(f"rsqrt(max(1e-12f, abs({args[1]})))")
        elif base_op == "RCP":
            emit_expr(f"1.0f / ({args[1]})")
        elif base_op == "POW":
            emit_expr(f"pow(max(1e-12f, abs({args[1]})), {args[2]})")
        elif base_op == "ABS":
            emit_expr(f"abs({args[1]})")
        elif base_op == "KIL":
            out.append(f"    clip({args[0]});")
        elif base_op in ["TEX", "TEXDYN"]:
            tc, tex = raw_args[1], raw_args[2]
            idx = re.search(r"\d+", tex).group(0) if re.search(r"\d+", tex) else "0"
            is_cube = (len(raw_args) > 3 and "CUBE" in raw_args[3])
            is_shadow = (len(raw_args) > 3 and "SHADOW" in raw_args[3])
            if is_cube:
                emit_expr(f"textureCube2.Sample(samplerCube2, ({args[1]}).xyz)")
            elif is_shadow:
                # PC shadow compare: use SampleCmpLevelZero with comparison
                # sampler; reference depth is .z (or .w for projected).
                emit_expr(f"texture{idx}.SampleCmpLevelZero(samplerCmp{idx}, ({args[1]}).xy, ({args[1]}).z)")
            else:
                emit_expr(f"texture{idx}.Sample(sampler{idx}, ({args[1]}).xy)")
        elif base_op == "SWZ":
            src = args[1]
            comp_map = lambda c: "0.0f" if c == "0" else ("1.0f" if c == "1" else f"{src}.{c}")
            swz_expr = f"float4({comp_map(raw_args[2])}, {comp_map(raw_args[3])}, {comp_map(raw_args[4])}, {comp_map(raw_args[5])})"
            if "." in dst:
                mask = dst.split(".")[1]
                var = dst.split(".")[0]
                out.append(f"    {var}.{mask} = ({swz_expr}).{mask};")
            else:
                out.append(f"    {dst} = {swz_expr};")

    out.append("    return output;")
    out.append("}")
    return "\n".join(out)
