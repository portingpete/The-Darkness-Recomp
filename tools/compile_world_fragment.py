"""Translate hash-pinned original world fragment sources to native HLSL.

Preserves register numbers, texture dimensionality/slots and masked lanes.
Unknown syntax/opcodes fail the build. Assets are never rewritten.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
from compile_vertex_template import parse

ASSETS = {
    'System/Gl/ARB_fragment_program/WClientMod_DV5_0.fp': '0b44b474d46f01228f657175c2b817b6f7246ed65a6bfe4fb99af963b7201647',
    'System/Gl/ARB_fragment_program/WClientMod_DV5_1.fp': 'a99c1ce457f9d493975a9d05fffa3b138d714a1ffed6d78df18d97b69e2bdc9c',
    'System/Gl/ARB_fragment_program/XRShader_FP20_NDS.fp': '4d756e598bbb515c443e5175dbd4cf6967ccff1682dbbb398bc0a00f94e698b5',
    'System/Gl/ARB_fragment_program/VBOp_Fresnel.fp': 'c2eed837a9c13166970d8b03bda9a942a5e009610e7e9c135b52ae374c2361ea',
    'System/Gl/ARB_fragment_program/XRShader_FP20_LF.fp': '0113cb0529fc2eb5c0efe170c0d6ea6ee715a8b1060423e6939badda455b9b43',
    'System/Gl/ARB_fragment_program/XRShader_FP20_NDSEATP.fp': '2b58402b5aa091451e39a10138f06a2133003081244d9ccb0ad389cf9f217eee',
    'System/Gl/ARB_fragment_program/VBOp_GenEnv2.fp': 'da206300c85ce01bcfeb7ce2ffd8a40606fe67fcce384348c6c4e756d44a8780',
    'System/Xenon/FragmentProgram/MRenderXenon_Attrib_TexEnvMode00.fp': 'dc3932e232974b2ed52e5de9585f2efe387f0651d603091ad122dcc2be7909d6',
    'System/Xenon/FragmentProgram/MRenderXenon_Attrib_TexEnvMode01.fp': 'c2a86bc30a96d765ea15213cb7c7a8acd26917bffb748e7d493cd7c91dd81b20',
    'System/Xenon/FragmentProgram/MRenderXenon_Attrib_TexEnvMode02.fp': '5be3c91c1411428d3bd7b264e5dd62990d0544608883aae98bbb53bfd8d8823e',
    'System/Gl/ARB_fragment_program/XRShader_FP20_NDSP.fp': 'cae15be2c970a5d3a8096324348f8516f5f2bdfa6d78951af06592b7bbad6f33',
    'System/Gl/ARB_fragment_program/XRShader_FP20_LFM.fp': '8e917a3a901a029c78824a2e504a2011040cffdde7c2a83c60d0a2d5f17b413f',
    'System/Gl/ARB_fragment_program/XREngine_DepthFog.fp': '3a0b9ec36e559989356394150dd109381f796fd7b6416715902bf88705e48f24',
    'System/Gl/ARB_fragment_program/XREngine_GaussClampedHurt.fp': '5b6c05a4c62f9a7a0ef747b331776e939d3dc8b00900be6a2013bab074fd07f2',
    'System/Gl/ARB_fragment_program/XRShader_MotionMap.fp': '4636540b97619cfae896cabab9624ab66ea07c976e0af18474fcbafc977e988e',
    'System/Xenon/FPInclude_Xenon.xrg': '965a3e6842c05fab44c028ece6d13f73cc267285ab8b719b637097d6e680cf7b',
    'System/Gl/ARB_fragment_program/GUIFadeToWhite.fp': '739043c787ac7bb0ab3feace0a73dc645c3da3cdba636e588bf656d7afff1b04',
    'System/Gl/ARB_fragment_program/XREngine_Histogram.fp': '87f176503dba1a0403c56e8cd40756e782f303321cb161394abda18de4e5bc07',
    'System/Gl/ARB_fragment_program/XRUtil_RenderSurface.fp': '03e1acee983482c12abf6d6e1dae8bcf6679cd44e53b118a0a286c0a8ebdc812',
    'System/Gl/ARB_fragment_program/Include_XREngine_Fog.fph': '75e520d9a3f8825e6321f5a4c8b8e9ab77d601b7985542805b3c66d2f51bdaf0',
    'System/Gl/ARB_fragment_program/XREngine_GaussClamped.fp': 'c174225cb929b194c2036cd98b7c7e28fd3d86f1f56fe58f025324a9117d621a',
    'System/Gl/ARB_fragment_program/XREngine_RadialBlur.fp': '0e8ad060b99db378b57aa946e8fa2bc5729c38c24bde378a424e46886001f2a3',
    'System/Gl/ARB_fragment_program/XREngine_RadialBlurHurt.fp': 'a0fc820e5ce2143c24a6f167ad914933524a7f67d2d89b792c7337a95c6b0644',
    'System/Gl/ARB_fragment_program/XREngine_RadialBlurInvert.fp': 'ee18787fd088b03ca231a3d54c4db62abe726013de52df41b0434ae525738024',
    'System/Gl/ARB_fragment_program/XRUtil_ShrinkTexture8.fp': '0380695ef8eb18e3fe65cf8eb0016e589f6ecd2423ed2dec8d18010a37b8e495',
    'System/Gl/ARB_fragment_program/XREngine_CCFuser.fp': '2dc30d146fa599ea0f40a0be2e228bc905d15b5eb48b6f3eb883f64b6fd7805c',
    'System/Gl/ARB_fragment_program/XREngine_Final5.fp': '04f4ac016bcb2d05ed6f8c59b0e388d09fd3360c12a2a2fd4d1d56abe1e02b8c',
    'System/Gl/ARB_fragment_program/XREngine_MulFilter.fp': '78e83bb6678b53ee9c2e79f79dc0b79d85a652289dbf7606e02c2e8758d0e3e3',
    'System/Gl/ARB_fragment_program/XREngine_ShadowProj.fp': 'f128ded198aa6c5313f8197d92537e80d05727e29ca4f2c5d25ae2731d49064f',
}
LANES = str.maketrans('rgba', 'xyzw')
VARIANTS = {'XRShader_FP20_NDSP': [0], 'XRShader_FP20_NDS': [0], 'XRShader_MotionMap': [0], 'GUIFadeToWhite': [0],
            'WClientMod_DV5_0': [0, 1, 2], 'WClientMod_DV5_1': [0, 1, 2],
            'XRShader_FP20_LF': [0, 1], 'XRShader_FP20_NDSEATP': list(range(32)),
            'VBOp_GenEnv2': [0, 1], 'VBOp_Fresnel': [0],
            'XREngine_GaussClamped': [0], 'XREngine_RadialBlur': [0], 'XREngine_RadialBlurHurt': [0],
            'XREngine_RadialBlurInvert': [0], 'XRUtil_ShrinkTexture8': [0],
            'XREngine_CCFuser': [0, 1, 2, 4], 'XREngine_Final5': list(range(16)), 'XREngine_Histogram': [0],
            'XREngine_ShadowProj': [8],
            # Original RenderSurface's lighting/projector, fog and second
            # texture branches, including the two alpha-only fog variants.
            'XRUtil_RenderSurface': sorted({0, 64, 65, 67, 71, 128, 264, 328} |
                {8 | fog | material for fog in (0, 16, 32, 48)
                 for material in (0, 64, 65, 67, 71, 128)}),
            'XRShader_FP20_LFM': [0],
            'XREngine_DepthFog': list(range(4)), 'XREngine_GaussClampedHurt': [0],
            'XREngine_MulFilter': [0]}


def select_template(source, flags, includes=None):
    if '*program' not in source:
        if flags:
            raise ValueError('Plain program has no permutations')
        return source
    flags_block = re.search(r'\*flags\s*\{([^{}]*)\}', source)[0]
    names = {n: int(v, 0) for n, v, _ in parse(flags_block)[0][1]}
    if flags & ~sum(names.values()):
        raise ValueError('Unknown fragment permutation')
    # The *generate block has a different grammar; only *program contains code.
    nodes = parse(source[source.index('*program'):])[0][1]
    defines_block = re.search(r'\*defines\s*\{([^{}]*)\}', source)
    defines = {n: v for n, v, _ in parse(defines_block[0])[0][1]} if defines_block else {}
    def emit(nodes):
        output = []
        for name, value, _ in nodes:
            if name == 'INCLUDE':
                if not includes or value not in includes:
                    raise ValueError(f'Unpinned fragment include: {value}')
                output.extend(emit(parse(includes[value])))
                continue
            if name.startswith(('if_', 'ifnot_')):
                inverse = name.startswith('ifnot_')
                test = bool(flags & names.get(name.split('_', 1)[1], 0))
                if test == inverse:
                    continue
            if isinstance(value, list):
                output.extend(emit(value))
            else:
                value = value.replace('@@', '@')
                for key, replacement in defines.items():
                    value = re.sub(r'@' + re.escape(key) + r'\b', replacement, value)
                output.append(value)
        return output
    return '\n'.join(emit(nodes))


def compile_source(source):
    # Original Xenon preprocessing822441E8 installs precision aliases.
    source = re.sub(r'@TEMP16\b', 'TEMP', source)
    source = re.sub(r'@PARAM16\b', 'PARAM', source)
    selected, stack = [], [True]
    else_seen = [False]
    for line in source.splitlines():
        line = line.split('#', 1)[0].strip()
        if line.startswith(('@if ', '@ifnot ')):
            inverse = line.startswith('@ifnot ')
            name = line[7:].strip() if inverse else line[4:].strip()
            if name not in ('dynmip', 'support_normalize', 'platform_pc', 'xenon'):
                raise ValueError(f'Unknown condition: {name}')
            stack.append((name == 'xenon') != inverse)
            else_seen.append(False)
        elif line == '@else':
            if len(stack) == 1:
                raise ValueError('Unmatched else')
            if else_seen[-1]:
                raise ValueError('Duplicate else')
            else_seen[-1] = True
            stack[-1] = not stack[-1]
        elif line == '@endif':
            if len(stack) == 1:
                raise ValueError('Unmatched endif')
            stack.pop()
            else_seen.pop()
        elif line and all(stack) and not line.startswith('!!') and set(line) != {'-'}:
            selected.append(line)
    if len(stack) != 1:
        raise ValueError('Unterminated condition')
    declarations, body, textures, symbols = [], [], {}, set()
    uses_pcf4x4 = False
    output = None

    def operand(value):
        if value.startswith('{') and value.endswith('}'):
            lanes = [s.strip() for s in value[1:-1].split(',')]
            if len(lanes) != 4 or not all(re.fullmatch(r'-?\d+(?:\.\d*)?(?:e[-+]?\d+)?', v, re.I) for v in lanes):
                raise ValueError(f'Invalid literal operand: {value}')
            return 'float4(' + ', '.join(lanes) + ')'
        if re.fullmatch(r'-?\d+(?:\.\d*)?(?:e[-+]?\d+)?', value, re.I):
            return f'((float4)({value}))'
        m = re.fullmatch(r'(-?)(\w+)(?:\.([xyzwrgba]{1,4}))?', value)
        if not m or m[2] not in symbols:
            raise ValueError(f'Unknown operand: {value}')
        sign, name, swizzle = m.groups()
        if swizzle:
            swizzle = swizzle.translate(LANES)
            if len(swizzle) == 1:
                swizzle *= 4
            if len(swizzle) != 4:
                raise ValueError(f'Unsupported source lane count: {value}')
            name += '.' + swizzle
        return f'({sign}{name})'

    for statement in ' '.join(selected).split(';'):
        statement = statement.strip()
        if not statement or statement == 'END':
            continue
        if statement in ('OPTION ARB_precision_hint_fastest', 'OPTION ARB_fragment_program_shadow'):
            continue
        match = re.fullmatch(r'(OUTPUT|ATTRIB|PARAM)\s+(\w+)\s*=\s*(.+)', statement)
        if match:
            kind, name, value = match.groups()
            if name in symbols:
                raise ValueError(f'Duplicate declaration: {name}')
            symbols.add(name)
            if kind == 'OUTPUT' and value == 'result.color':
                output = name
                declarations.append(f'precise float4 {name} = 0;')
            elif kind == 'ATTRIB' and value == 'fragment.color':
                declarations.append(f'float4 {name} = input.color;')
            elif kind == 'ATTRIB' and (m := re.fullmatch(r'fragment.texcoord\[([0-7])\]', value)):
                declarations.append(f'float4 {name} = input.tex[{m[1]}];')
            elif kind == 'PARAM' and (m := re.fullmatch(r'program.env\[(\d+)\]', value)):
                if int(m[1]) >= 256:
                    raise ValueError('Constant register overflow')
                declarations.append(f'float4 {name} = env[{m[1]}];')
            elif kind == 'PARAM' and value.startswith('{') and value.endswith('}'):
                lanes = [s.strip() for s in value[1:-1].split(',')]
                if len(lanes) != 4 or not all(re.fullmatch(r'-?\d+(?:\.\d*)?', v) for v in lanes):
                    raise ValueError(f'Invalid literal vector: {value}')
                declarations.append(f'float4 {name} = float4({", ".join(lanes)});')
            else:
                raise ValueError(f'Unsupported declaration: {statement}')
            continue
        if statement.startswith('TEMP '):
            for name in statement[5:].split(','):
                name = name.strip()
                if not re.fullmatch(r'\w+', name) or name in symbols:
                    raise ValueError(f'Invalid temporary: {name}')
                symbols.add(name)
                declarations.append(f'precise float4 {name} = 0;')
            continue
        if statement.startswith('KIL '):
            # ARB discards if any source lane is negative. A scalar swizzle
            # replicates that lane; clip preserves zero as a surviving pixel.
            body.append(f'clip({operand(statement[4:].strip())});')
            continue
        match = re.fullmatch(r'(\w+)\s+(\w+)(?:\.([xyzwrgba]{1,4}))?,\s*(.+)', statement)
        if not match or match[2] not in symbols:
            raise ValueError(f'Unrecognized instruction: {statement}')
        opcode, dest, mask, arguments = match.groups()
        mask = '.' + mask.translate(LANES) if mask else ''
        args = [a.strip() for a in re.split(r',\s*(?![^{}]*\})', arguments)]
        saturate = opcode.endswith('_SAT')
        op = opcode.removesuffix('_SAT')
        if op == 'TEX':
            if len(args) != 3 or not (m := re.fullmatch(r'texture\[(\d+)\]', args[1])) or args[2] not in ('2D', 'CUBE', 'PCF4X42D'):
                raise ValueError('Invalid texture instruction')
            slot = int(m[1])
            dimension = '2D' if args[2] == 'PCF4X42D' else args[2]
            if slot >= 16 or (slot in textures and textures[slot] != dimension):
                raise ValueError('Inconsistent texture slot')
            textures[slot] = dimension
            if args[2] == 'PCF4X42D':
                if slot != 0:
                    raise ValueError('Unsupported shadow map slot')
                uses_pcf4x4 = True
                expr = f'((float4)nativeShadow4x4({operand(args[0])}))'
            else:
                coord = 'xy' if dimension == '2D' else 'xyz'
                expr = f'(texture{slot}.Sample(sampler{slot}, {operand(args[0])}.{coord}) * sampleScale[{slot}])'
        elif op == 'SWZ':
            if len(args) != 5:
                raise ValueError('Invalid SWZ arity')
            lanes = []
            for lane in args[1:]:
                if lane in ('0', '1'):
                    lanes.append(lane)
                elif re.fullmatch(r'[xyzwrgba]', lane):
                    lanes.append(operand(args[0]) + '.' + lane.translate(LANES))
                else:
                    raise ValueError('Invalid SWZ selector')
            expr = 'float4(' + ', '.join(lanes) + ')'
        else:
            counts = {'MOV': 1, 'RCP': 1, 'RSQ': 1, 'FRC': 1, 'ADD': 2, 'SUB': 2, 'MUL': 2,
                      'DP3': 2, 'DP4': 2, 'XPD': 2, 'POW': 2, 'MAD': 3, 'LRP': 3, 'MIN': 2, 'MAX': 2}
            if op not in counts or len(args) != counts[op]:
                raise ValueError(f'Unsupported opcode or arity: {statement}')
            a = [operand(x) for x in args]
            if op == 'MOV': expr = a[0]
            elif op == 'FRC': expr = f'frac({a[0]})'
            elif op in ('ADD', 'SUB', 'MUL'): expr = f'{a[0]} ' + {'ADD': '+', 'SUB': '-', 'MUL': '*'}[op] + f' {a[1]}'
            elif op in ('DP3', 'DP4'):
                sw = 'xyz' if op == 'DP3' else 'xyzw'
                expr = f'((float4)dot({a[0]}.{sw}, {a[1]}.{sw}))'
            elif op == 'XPD':
                if not mask or 'w' in mask:
                    raise ValueError('XPD W result is undefined')
                expr = f'float4(cross({a[0]}.xyz, {a[1]}.xyz), 0)'
            elif op == 'MAD': expr = f'{a[0]} * {a[1]} + {a[2]}'
            elif op == 'LRP': expr = f'{a[1]} * {a[0]} + {a[2]} * (1 - {a[0]})'
            elif op == 'RCP': expr = f'((float4)(1.0 / {a[0]}.x))'
            elif op == 'RSQ': expr = f'((float4)(1.0 / sqrt(abs({a[0]}.x))))'
            # ARB leaves negative POW bases undefined; abs also matches PS2 pow.
            elif op == 'POW': expr = f'((float4)pow(abs({a[0]}.x), {a[1]}.x))'
            else: expr = f'{op.lower()}({a[0]}, {a[1]})'
        if saturate:
            expr = f'saturate({expr})'
        body.append(f'{dest}{mask} = ({expr}){mask};')
    if not output:
        raise ValueError('Missing color output')
    source = 'cbuffer FragmentConstants : register(b0) { float4 env[256]; }\ncbuffer TextureScales : register(b1) { float4 sampleScale[16]; }\n'
    for slot, dimension in sorted(textures.items()):
        source += f'Texture{"Cube" if dimension == "CUBE" else "2D"}<float4> texture{slot} : register(t{slot});\nSamplerState sampler{slot} : register(s{slot});\n'
    if uses_pcf4x4:
        # The guest's four-by-four taps are spaced in logical shadow texels.
        # env[9] retains that pitch while the native depth map grows by 2x/3x.
        source += '''float nativeShadow4x4(float4 position) {
    float total = 0;
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            float2 uv = position.xy + env[9].xy * float2(x - 1.5, y - 1.5);
            float depth = texture0.SampleLevel(sampler0, uv, 0).r * sampleScale[0].x;
            total += position.z >= depth ? 1.0 : 0.0;
        }
    }
    return total * (1.0 / 16.0);
}
'''
    source += 'struct Fragment { float4 position : SV_Position; float4 tex[8] : TEXCOORD0; float4 color : COLOR0; };\n'
    source += 'float4 pixelMain(Fragment input) : SV_Target {\n' + '\n'.join(declarations + body) + f'\nreturn {output};\n}}\n'
    return source, {'textures': textures, 'instruction_count': len(body), 'conditions': {'dynmip': False, 'support_normalize': False, 'platform_pc': False, 'xenon': True}}


def compile_fixed(source):
    # Translate the shipped PS2 assembly through the same masked ARB arithmetic.
    source = re.sub(r'/\*.*?\*/', '', source, flags=re.S)
    lines = [x.strip() for x in source.splitlines() if x.strip()]
    if lines.pop(0) != 'ps_2_0':
        raise ValueError('Unknown fixed fragment profile')
    code = ['OUTPUT outColor = result.color;']
    registers = set(re.findall(r'\b(?:r|c)\d+\b', '\n'.join(lines)))
    for r in sorted(registers):
        code.append(f'TEMP {r};' if r[0] == 'r' else f'PARAM {r} = program.env[{r[1:]}];')
    for line in lines:
        if line == 'dcl v0': code.append('ATTRIB v0 = fragment.color;')
        elif re.fullmatch(r'dcl t[0-7]', line): code.append(f'ATTRIB {line[4:]} = fragment.texcoord[{line[-1]}];')
        elif re.fullmatch(r'dcl_2d s[0-7]', line): pass
        elif m := re.fullmatch(r'texld (r\d+), (t[0-7]), s([0-7])', line):
            code.append(f'TEX {m[1]}, {m[2]}, texture[{m[3]}], 2D;')
        elif m := re.fullmatch(r'(mov|lrp|mul) (.+)', line):
            code.append(m[1].upper() + ' ' + m[2].replace('oC0', 'outColor') + ';')
        else: raise ValueError(f'Unknown fixed instruction: {line}')
    return compile_source('\n'.join(code))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    sources = {}
    for name, expected in ASSETS.items():
        data = (args.game_dir / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Original shader asset changed: {name}')
        sources[name] = data.decode('latin-1')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    header, manifests = '#pragma once\nnamespace DarkRecomp {\n', {}
    records, bindings = [], []
    variants_by_name = VARIANTS | {f'MRenderXenon_Attrib_TexEnvMode0{i}': [0] for i in range(3)}
    for name, variants in variants_by_name.items():
      for flags in variants:
        if name.startswith('MRenderXenon_Attrib_'):
            shader, manifest = compile_fixed(sources[f'System/Xenon/FragmentProgram/{name}.fp'])
        else:
            shader, manifest = compile_source(select_template(sources[f'System/Gl/ARB_fragment_program/{name}.fp'], flags,
                {Path(p).name: text for p, text in sources.items() if p.endswith('.fph')}))
        key = name if flags == 0 else f'{name}_{flags}'
        (args.output_dir / f'{key}.native.hlsl').write_text(shader, encoding='utf-8', newline='\n')
        header += f'inline constexpr char {key}Source[] = R"DARKFP({shader})DARKFP";\n'
        manifests[key] = manifest
        mask = sum(1 << s for s in manifest['textures'])
        cube = sum(1 << s for s, dim in manifest['textures'].items() if dim == 'CUBE')
        records.append(f'{{"{name}", {flags}, {mask}, {cube}, {key}Source}}')
        bindings.append(f'{{"{name}", {flags}, {mask}, {cube}}}')
    header += 'struct WorldFragmentSource {const char* name; unsigned flags, textures, cubes; const char* source;};\n'
    header += 'inline constexpr WorldFragmentSource worldFragmentSources[]{' + ',\n'.join(records) + '};\n'
    outputs = {'engine_world_fragments.generated.h': header + '}\n',
               'engine_world_fragment_bindings.generated.h': '#pragma once\nnamespace DarkRecomp {\n'
                   'struct WorldFragmentBinding {const char* name; unsigned flags, textures, cubes;};\n'
                   'inline constexpr WorldFragmentBinding worldFragmentBindings[]{' + ',\n'.join(bindings) + '};\n}\n',
               'engine_world_fragments.source.json': json.dumps({'assets': ASSETS, 'programs': manifests}, indent=2) + '\n'}
    for name, content in outputs.items():
        path = args.output_dir / name
        if not path.exists() or path.read_text(encoding='utf-8') != content:
            path.write_text(content, encoding='utf-8', newline='\n')
    print(json.dumps(manifests))


if __name__ == '__main__':
    main()
