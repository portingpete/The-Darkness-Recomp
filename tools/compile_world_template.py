"""Build native world vertex programs from the original engine template.

The original files are read-only and hash pinned. Unsupported template branches
are excluded explicitly; the runtime rejects materials requesting those branches.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
from compile_vertex_template import ASSETS, parse

MODES = {'texcoord': 0, 'linear': 1, 'void': 4, 'constant': 7, 'mspos': 8,
         'wspos': 10, 'env': 13, 'LightField': 17, 'bumpcubeenv': 18, 'tslv': 20, 'depthoffset': 22, 'Lighting_Nonormal': 16}
WORLD_ASSETS = ASSETS | {'System/Xenon/ProgramCache.xpc':
    'e5f5d6a29761cf884a11655911d3affcbb6024e59a5f7fb2c592a675a081966b'}


def condition(name):
    if re.fullmatch(r'MWComp[0-8]', name):
        return f'MWCOMP == {name[-1]}'
    if m := re.fullmatch(r'texgen([0-7])_(\w+)', name):
        # Original cache records 9/13/23/26/240/283/419 establish these
        # stage-specific branches; other stages remain unsupported.
        if m[2] == 'bumpcubeenv' and m[1] not in ('1', '5'):
            return False
        if m[2] == 'env' and m[1] != '0':
            return False
        if m[2] == 'LightField' and m[1] != '5':
            return False
        # Cache 18/250/253/423: stage1 transforms the post-skin position by
        # c12..14 with three DP4s and supplies W from c8.y (0/4/8 weights).
        if m[2] == 'wspos' and m[1] != '1':
            return False
        # Original cache37/50/75/144/145/164/165/368/369 establish
        # stage0 depthoffset, including conversion and texture-matrix forms.
        if m[2] == 'depthoffset' and m[1] != '0':
            return False
        # Cache 324/326/328 prove Lighting_Nonormal stages 3/4/5 (distance-only,
        # all-lane color multiply); other stages remain unsupported.
        if m[2] == 'Lighting_Nonormal' and m[1] not in ('3', '4', '5'):
            return False
        # Cache record 23 exports c12/c13 unchanged for mode 9 in stages 3/4;
        # rec324 E3=c14/E4=c15 and rec326 E4=c16 extend the same constant
        # passthrough to stage 5.
        if m[2] == 'constant' and m[1] in ('3', '4', '5'):
            return f'(MODE_{m[1]} == 7 || MODE_{m[1]} == 9)'
        return f'MODE_{m[1]} == {MODES[m[2]]}' if m[2] in MODES else False
    if m := re.fullmatch(r'TextureTrans([0-7])', name):
        return f'CONVERT_{m[1]}'
    if m := re.fullmatch(r'texmatrix([0-7])', name):
        return f'MATRIX_{m[1]}'
    if re.fullmatch(r'texcoord(?:in|out)[0-7]', name):
        return True
    return {'PosTrans': 'POSITION_TRANS', 'usenormal': 'USE_NORMAL',
            'usetangents': 'USE_TANGENTS', 'normalizenormal': 'NORMALIZE_NORMAL',
            'coloroutput': True, 'colorvertex': 'VERTEX_COLOR'}.get(name, False)


def translate(game):
    sources = {}
    for name, expected in WORLD_ASSETS.items():
        data = (game / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Original asset changed: {name}')
        sources[name] = data.decode('latin-1')
    definitions = dict((n, v) for n, v, _ in parse(sources[
        'System/Xenon/VertexProgram/VPDefines_HLSL.xrg'])[0][1])
    refs = {'BASE': '0', 'MP': 'refs[0].x', 'POSTRANS': 'refs[0].y', 'CONSTANTCOLOR': 'refs[0].z'}
    for s in range(8):
        refs.update({f'TEXINPUT{s}': f'_t{s}', f'TEXTURETRANS{s}': f'refs[{s+1}].x',
                     f'TEXMATRIX{s}': f'refs[{s+1}].y', f'TEXPARAM{s}': f'refs[{s+1}].z'})
    retained = []

    def instruction(source, line):
        source = re.sub(r'\br(\d+)\b', r'R\1', source)
        source = re.sub(r'@([A-Za-z]\w*)', lambda m: definitions[m[1]], source)
        source = re.sub(r'\$([A-Za-z]\w*)', lambda m: refs[m[1]], source)
        result = []
        for operation in source.split(';'):
            operation = operation.strip()
            if not operation:
                continue
            match = re.fullmatch(r'(MOV|ADD|SUB|MUL|MAD|DP3|DP4|RSQ|RCP|ARL|MAX)\s+(.*)', operation)
            if not match:
                raise ValueError(f'Unsupported instruction at {line}: {operation}')
            op, operands = match.groups()
            dest, *args = [a.strip() for a in operands.split(',')]
            if len(args) != {'MOV': 1, 'ARL': 1, 'RSQ': 1, 'RCP': 1, 'MAD': 3}.get(op, 2):
                raise ValueError(f'Wrong arity at {line}: {operation}')
            if not re.fullmatch(r'(?:R\d+|_oPos|_oC0|_oT[0-7]|A0)(?:\.[xyzw]{1,4})?', dest):
                raise ValueError(f'Invalid destination: {dest}')
            retained.append({'line': line, 'operation': operation})
            mask = '.' + dest.split('.')[1] if '.' in dest else ''
            result.append(f'{dest} = (int)ARL({args[0]});' if op == 'ARL' else
                          f'{dest} = {op}({", ".join(args)}){mask};')
        return result

    def emit(nodes):
        result = []
        for name, value, line in nodes:
            test = True
            if name.startswith(('if_', 'ifnot_')):
                inverse = name.startswith('ifnot_')
                test = condition(name.split('_', 1)[1])
                test = test != inverse if isinstance(test, bool) else f'!({test})' if inverse else test
            if test is False:
                continue
            if isinstance(test, str):
                result.append(f'#if {test}')
            result += emit(value) if isinstance(value, list) else instruction(re.sub(r'#[^\n]*', '', value), line)
            if isinstance(test, str):
                result.append('#endif')
        return result

    body = '\n'.join(emit(parse(sources['System/Gl/VP.xrg'])[0][1]))
    hlsl = '''// Generated from the hash-pinned original VP.xrg and Xenon helpers.
cbuffer Constants : register(b0) { float4 c[256]; }
cbuffer References : register(b1) { uint4 refs[9]; }
struct VertexInput {
    float4 position : POSITION; float4 normal : NORMAL;
    float4 tex[8] : TEXCOORD0; float4 color : COLOR0;
    float4 indices : BLENDINDICES0; float4 weights : BLENDWEIGHT0;
    float4 indices2 : BLENDINDICES1; float4 weights2 : BLENDWEIGHT1;
};
struct VertexOutput { float4 position : SV_Position; float4 tex[8] : TEXCOORD0; float4 color : COLOR0; };
''' + sources['System/Xenon/VPInclude_Xenon.xrg'] + '''
VertexOutput vertexMain(VertexInput input) {
    float4 _vPos = input.position, _vNrm = input.normal, _vC0 = input.color;
    float4 _vMI = input.indices, _vMW = input.weights, _vMI2 = input.indices2, _vMW2 = input.weights2;
'''
    hlsl += '\n'.join(f'    float4 _t{s} = input.tex[COORD_{s}];' for s in range(8))
    hlsl += '\n    precise float4 ' + ', '.join(f'R{i} = 0' for i in range(12)) + ';\n    int4 A0 = 0;\n'
    hlsl += '    precise float4 _oPos = 0, _oC0 = 0, ' + ', '.join(f'_oT{s} = 0' for s in range(8)) + ';\n'
    hlsl += body + '\n    VertexOutput output; output.position = _oPos; output.color = _oC0;\n'
    hlsl += '\n'.join(f'    output.tex[{s}] = _oT{s};' for s in range(8))
    hlsl += '\n    return output;\n}\n'
    return hlsl, {'assets': WORLD_ASSETS, 'modes': MODES,
                  'constant_mode9_stages': [3, 4, 5],
                  'wspos_mode10_stages': [1],
                  'cache_evidence_records': [9, 13, 18, 23, 26, 37, 50, 75, 144, 145, 164, 165, 240, 250, 253, 283, 323, 324, 325, 326, 327, 328, 368, 369, 419, 423],
                  'instructions': retained}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    source, manifest = translate(args.game_dir)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, content in {
        'engine_world_template.hlsl': source,
        'engine_world_template.generated.h': '#pragma once\nnamespace DarkRecomp {\ninline constexpr char engineWorldTemplateSource[] = R"DARKWORLD(' + source + ')DARKWORLD";\n}\n',
        'engine_world_template.source.json': json.dumps(manifest, indent=2) + '\n',
    }.items():
        path = args.output_dir / name
        if not path.exists() or path.read_text(encoding='utf-8') != content:
            path.write_text(content, encoding='utf-8', newline='\n')
    print(f'Original world vertex template: {len(manifest["instructions"])} source instructions')


if __name__ == '__main__':
    main()
