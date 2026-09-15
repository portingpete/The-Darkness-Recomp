"""Translate a bounded subset of the original VP.xrg into build-time HLSL.

Conditions are explicit native inputs, NOT a guessed retail cache-key mapping.
No Xbox shader bytecode or GPU packets are interpreted. Original assets are
read-only; their hashes pin the source language and reviewed branch selection.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re

ASSETS = {
    'System/Gl/VP.xrg': '7b14a9d6d54833715c70e7dd8dcfdbdf7b3cad467987af999c7b845aa9143095',
    'System/Xenon/VertexProgram/VPDefines_HLSL.xrg': '8f0aa62b00b7ad5f97a06b542156c809641dc986209979f01c2986cc7a90c51c',
    'System/Xenon/VPInclude_Xenon.xrg': 'cb1fd983597d46745468772a93eed2d72b85bc7754e4808a6fd1a7c3cbc9d611',
}
# Unquoted values stop at comment openers but may contain ordinary path slashes.
TOKEN = re.compile(r'\s+|//[^\n]*|/\*[\s\S]*?\*/|"[^"]*"|\*\w+|[{}]|(?:/(?![/*])|[^\s{}"*/])+')


def parse(source):
    tokens = []
    cursor = 0
    for match in TOKEN.finditer(source):
        if match.start() != cursor:
            raise ValueError('Unrecognized template syntax')
        cursor = match.end()
        value = match.group()
        if not (value.isspace() or value.startswith(('//', '/*'))):
            tokens.append((value, source.count('\n', 0, match.start()) + 1))
    if cursor != len(source):
        raise ValueError('Truncated template syntax')
    index = 0

    def nodes(nested=False):
        nonlocal index
        result = []
        while index < len(tokens):
            name, line = tokens[index]
            index += 1
            if name == '}':
                if not nested:
                    raise ValueError('Unexpected closing brace')
                return result
            if not name.startswith('*') or index == len(tokens):
                raise ValueError(f'Expected named node at line {line}')
            value, _ = tokens[index]
            index += 1
            if value == '{':
                value = nodes(True)
            elif value.startswith('"'):
                value = value[1:-1]
            elif value.startswith('*') or value == '}':
                raise ValueError(f'Missing value at line {line}')
            result.append((name[1:], value, line))
        if nested:
            raise ValueError('Unclosed template block')
        return result
    return nodes()


def condition(name):
    # Selected: ordinary position/palette, no normals/lighting/clip/fog,
    # diffuse output and texture-coordinate input/output zero only.
    if re.fullmatch(r'MWComp[0-8]', name):
        return f'MWCOMP == {name[-1]}'
    return {'PosTrans': 'POSITION_TRANS', 'TextureTrans0': 'TEXTURE_TRANS',
            'texmatrix0': 'TEXTURE_MATRIX', 'colorvertex': 'VERTEX_COLOR',
            'coloroutput': True, 'texcoordin0': True, 'texcoordout0': True,
            'texgen0_texcoord': True,
            **{f'texgen{i}_void': True for i in range(1, 8)}}.get(name, False)


def translate(game):
    sources = {}
    for name, expected in ASSETS.items():
        data = (game / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Original asset changed; review before translating: {name}')
        sources[name] = data.decode('latin-1')
    root = parse(sources['System/Gl/VP.xrg'])
    if len(root) != 1 or root[0][0] != 'VertexProgram':
        raise ValueError('Unexpected vertex template root')
    definitions = dict((name, value) for name, value, _ in parse(
        sources['System/Xenon/VertexProgram/VPDefines_HLSL.xrg'])[0][1])
    constants = {'BASE': '0', 'MP': 'paletteRef', 'POSTRANS': 'positionRef',
                 'CONSTANTCOLOR': 'colorRef', 'TEXTURETRANS0': 'textureRef',
                 'TEXMATRIX0': 'matrixRef', 'TEXINPUT0': '_vT0'}
    retained = []

    def instruction(text, line):
        # The original assembly mixes r4/R4 in MWComp1/2; registers are case-insensitive.
        text = re.sub(r'\br(\d+)\b', r'R\1', text)
        text = re.sub(r'@([A-Za-z]\w*)', lambda m: definitions[m[1]], text)
        text = re.sub(r'\$([A-Za-z]\w*)', lambda m: constants[m[1]], text)
        if not text.strip():
            return []
        result = []
        for operation in text.split(';'):
            operation = operation.strip()
            if not operation:
                continue
            match = re.fullmatch(r'(MOV|ADD|MUL|MAD|DP4|ARL)\s+(.*)', operation)
            if not match:
                raise ValueError(f'Unsupported selected instruction at {line}: {operation}')
            opcode, arguments = match.groups()
            arguments = [a.strip() for a in arguments.split(',')]
            if len(arguments) != {'MOV': 2, 'ARL': 2, 'MAD': 4}.get(opcode, 3):
                raise ValueError(f'Wrong operand count at {line}')
            destination, *operands = arguments
            if not re.fullmatch(r'(?:R\d+|_oPos|_oC0|_oT0|A0)(?:\.[xyzw]{1,4})?', destination):
                raise ValueError(f'Unsupported destination: {destination}')
            for operand in operands:
                if not re.fullmatch(r'(?:R\d+|_v(?:Pos|C0|T0|MI2?|MW2?)|c\[[\w\s+.]+\])(?:\.[xyzw]{1,4})?', operand):
                    raise ValueError(f'Unsupported operand: {operand}')
            retained.append({'line': line, 'operation': operation})
            if opcode == 'ARL':
                result.append(f'{destination} = (int)ARL({operands[0]});')
            else:
                mask = '.' + destination.split('.')[1] if '.' in destination else ''
                result.append(f'{destination} = {opcode}({", ".join(operands)}){mask};')
        return result

    def emit(nodes):
        result = []
        for name, value, line in nodes:
            test = True
            if name.startswith(('if_', 'ifnot_')):
                inverse = name.startswith('ifnot_')
                test = condition(name.split('_', 1)[1])
                if isinstance(test, bool):
                    test = test != inverse
                elif inverse:
                    test = f'!({test})'
            if test is False:
                continue
            if isinstance(test, str):
                result.append(f'#if {test}')
            if isinstance(value, list):
                result.extend(emit(value))
            else:
                # Assembly comments end at newline, including inside strings.
                body = re.sub(r'#[^\n]*', '', value)
                result.extend(instruction(body, line))
            if isinstance(test, str):
                result.append('#endif')
        return result

    body = '\n'.join(emit(root[0][1]))
    helpers = sources['System/Xenon/VPInclude_Xenon.xrg']
    hlsl = '''// Generated from original VP.xrg; explicit native conditions only.
cbuffer Constants : register(b0) { float4 c[256]; }
cbuffer References : register(b1) {
    uint paletteRef, positionRef, colorRef, textureRef;
    uint matrixRef; uint3 referencePadding;
}
struct VertexInput {
    float4 position : POSITION; float4 uv : TEXCOORD0; float4 color : COLOR0;
    float4 indices : BLENDINDICES0; float4 weights : BLENDWEIGHT0;
    float4 indices2 : BLENDINDICES1; float4 weights2 : BLENDWEIGHT1;
};
struct VertexOutput { float4 position : SV_Position; float4 uv : TEXCOORD0; float4 color : COLOR0; };
''' + helpers + '''
VertexOutput vertexMain(VertexInput input) {
    float4 _vPos = input.position, _vT0 = input.uv, _vC0 = input.color;
    float4 _vMI = input.indices, _vMW = input.weights, _vMI2 = input.indices2, _vMW2 = input.weights2;
    precise float4 R0, R1, R2, R3, R4, R5, R8, R10;
    int4 A0;
    precise float4 _oPos, _oT0, _oC0;
''' + body + '''
    VertexOutput output; output.position = _oPos; output.uv = _oT0; output.color = _oC0;
    return output;
}
'''
    return hlsl, {'assets': ASSETS, 'instructions': retained,
                  'contract': 'Explicit conditions only; no retail program-key mapping or live world draw.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    hlsl, manifest = translate(args.game_dir)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    outputs = {'engine_vertex_template.hlsl': hlsl,
               'engine_vertex_template.generated.h': '#pragma once\nnamespace DarkRecomp {\ninline constexpr char engineVertexTemplateSource[] = R"DARKVP(' + hlsl + ')DARKVP";\n}\n',
               'engine_vertex_template.source.json': json.dumps(manifest, indent=2) + '\n'}
    for name, contents in outputs.items():
        path = args.output_dir / name
        if not path.exists() or path.read_text(encoding='utf-8') != contents:
            path.write_text(contents, encoding='utf-8', newline='\n')
    print(f'Original vertex template: {len(manifest["instructions"])} selected source instructions, 3 verified assets')


if __name__ == '__main__':
    main()
