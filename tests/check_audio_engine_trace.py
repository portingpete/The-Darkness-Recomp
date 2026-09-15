"""Read-only decoder for the optional native engine audio trace.

Schema authority: runtime/native/kernel.cpp audioEvidenceVoice..traceVoiceSrc,
plus the original SDK/limiter hooks; audio_resampler_trace.h defines the file.
No game, device, build, or third-party Python dependency is needed.

Run --self-test, or supply a .bin file. JSON is written only with --output.
Samples are sparse snapshots, not a complete PCM stream. Missing snapshot
validity bits prevent proving that an all-zero sample slot is measured silence.
"""

import argparse
import collections
import json
import math
from pathlib import Path
import struct
import sys


HEADER = struct.Struct('<8sII6Q')
PREFIX = struct.Struct('<QQIIII')
CAPACITY = 262144
VOICE, BIQUAD, LIMITER = 0x8281E758, 0x828248D0, 0x82828518
SRC = {0x8281D758: ('pcm16_variable', False, 2),
       0x8281DB98: ('pcm16_unit', True, 2),
       0x8281DF58: ('float_variable', False, 1),
       0x8281E398: ('float_unit', True, 1)}
PCM16_SCALE = struct.unpack('>f', bytes.fromhex('38000100'))[0]


def u32(b, offset=0):
    return struct.unpack_from('>I', b, offset)[0]


def floats(b, offset, count):
    return struct.unpack_from('>' + str(count) + 'f', b, offset)


def f32(x):
    try:
        return struct.unpack('>f', struct.pack('>f', x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def finite(*values):
    return all(math.isfinite(x) for value in values
               for x in (value if isinstance(value, (tuple, list)) else (value,)))


def span(pointer, size):
    # Metadata bounds only; a capture cannot prove that guest pages were mapped.
    return pointer != 0 and 0 <= size <= 0x100000000 - pointer


def top(counter, limit=30):
    return {'distinct': len(counter), 'items': [dict(value=k, count=v)
            for k, v in counter.most_common(limit)],
            'omitted_count': sum(v for _, v in counter.most_common()[limit:])}


def json_safe(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {str(k): json_safe(v) for k, v in value.items()}
    if isinstance(value, (tuple, list)):
        return [json_safe(v) for v in value]
    return value


class Metric:
    def __init__(self):
        self.n, self.total, self.maximum, self.over = 0, 0.0, 0.0, 0

    def add(self, value):
        if not math.isfinite(value):
            return
        value = abs(value)
        self.n += 1
        self.total += value
        self.maximum = max(self.maximum, value)
        self.over += value > 0.0001

    def report(self):
        return dict(samples=self.n, sum=self.total, maximum=self.maximum,
                    mean=self.total / self.n if self.n else None,
                    above_1e_4=self.over)


class Seams:
    def __init__(self):
        self.boundary, self.midpoint = Metric(), Metric()

    def add(self, old_phases, phases, channels):
        for c in range(min(channels, 2)):
            self.boundary.add(phases[c] - old_phases[6 + c])
            self.midpoint.add(phases[4 + c] - phases[2 + c])

    def report(self):
        return dict(boundary=self.boundary.report(), midpoint=self.midpoint.report(),
                    boundary_over_midpoint=self.boundary.total / self.midpoint.total
                    if self.midpoint.total else None)


class Record:
    def __init__(self, data, index):
        offset = 64 + 256 * index
        self.index = index
        (self.qpc, self.sequence, self.thread, self.descriptor,
         self.tag, self.result) = PREFIX.unpack_from(data, offset)
        self.b = data[offset + 32:offset + 120]
        self.a = data[offset + 120:offset + 208]
        self.first = struct.unpack_from('<6f', data, offset + 208)
        self.last = struct.unpack_from('<6f', data, offset + 232)

    def where(self):
        return dict(index=self.index, sequence=self.sequence,
                    accepted_seconds=(self.sequence - 1) * 256 / 48000,
                    thread=self.thread, descriptor=f'{self.descriptor:08X}',
                    tag=f'{self.tag:08X}')


def validate(data):
    if len(data) < 64:
        raise ValueError('Trace is shorter than its 64-byte header (possibly not persisted yet)')
    magic, version, size, count, frequency, start, end, dropped, reserved = HEADER.unpack_from(data)
    if (magic, version, size) != (b'DRRSMP01', 1, 256):
        raise ValueError('Unsupported trace magic/version/record size')
    if count > CAPACITY or len(data) != 64 + count * size:
        raise ValueError('Trace extent/count does not match the bounded 256-byte record format')
    if frequency == 0 or start >= end or reserved != 0:
        raise ValueError('Invalid trace frequency/window/reserved header field')
    return dict(records=count, qpc_frequency=frequency, start_frame=start,
                end_frame=end, dropped=dropped, version=version, record_bytes=size)


def voice_state(b):
    return dict(buffer=struct.unpack_from('>H', b, 14)[0],
                extent=struct.unpack_from('>Q', b, 24)[0],
                cursor=struct.unpack_from('>q', b, 32)[0],
                source_hz=floats(b, 40, 1)[0], pitch=floats(b, 44, 1)[0],
                phase=floats(b, 48, 1)[0], buffer_cursor=u32(b, 52),
                format_word=u32(b, 72), buffer_word=u32(b, 76),
                format=u32(b, 72) >> 30,
                buffer_frames=(u32(b, 76) >> 7) & 0x1ffffff)


def biquad_sample(x, coefficient, history, c):
    # Original separate float SIMD operations, as in audio_biquad_tests.h.
    a1, a2, b0, b1, b2, wet = coefficient
    y = f32(b0 * x)
    for gain, old in ((b1, history[c]), (b2, history[2+c]),
                      (a1, history[4+c]), (a2, history[6+c])):
        y = f32(y + f32(gain * old))
    return f32(f32(y * wet) - f32(f32(x * wet) - x))


def source_detail(call):
    r = call['row']
    return dict(index=r.index, input=f'{call["input"]:08X}', available=call['available'],
                output=f'{call["output"]:08X}', requested=r.result, produced=call['produced'],
                phase=call['phase'], returned_phase=call['returned_phase'], rate=call['rate'],
                gain=call['gain'], delta=call['delta'], input_pair=floats(r.b, 64, 4))


def group_detail(group):
    return dict(**group['row'].where(), channels=group['channels'],
                before=group['pre'], after=group['post'],
                gain=group['gain'], target=group['target'],
                first=group['row'].first, last=group['row'].last,
                first_src=source_detail(group['src'][0]),
                last_src=source_detail(group['src'][-1]),
                chunks=[call['produced'] for call in group['src']])


def source_continuity(voices):
    """Sparse per-voice ranking, and exact pointer/phase carry where observable."""
    counts, ranking = collections.Counter(), {}
    phase_error, same_pair_error = Metric(), Metric()
    pointer_examples, seam_events, same_pair_examples = [], [], []
    for voice in voices.values():
        previous = {}
        for group in voice['groups']:
            r = group['row']
            old = previous.get(r.thread)
            previous[r.thread] = group
            if (not old or old['row'].sequence+1 != r.sequence or
                    not old['src'] or not group['src']):
                continue
            first, last = group['src'][0], old['src'][-1]
            ch = group['channels']
            old_ch = old['channels']
            if ch != old_ch or ch > 4:
                counts['channel_change_or_unsupported'] += 1
                continue
            unit = SRC[first['row'].tag][1]
            fmt = SRC[first['row'].tag][2]
            old_fmt = SRC[last['row'].tag][2]
            old_unit = SRC[last['row'].tag][1]
            stride = ch * (2 if fmt == 2 else 4)
            returned = last['returned_phase']
            # Pointer arithmetic is valid only while the old span still has
            # samples; a new/refilled buffer is intentionally not compared.
            same_span = (fmt == old_fmt and returned >= 0 and
                old['post']['buffer'] == group['pre']['buffer'] and
                last['available'] - math.floor(returned) > 1)
            if same_span:
                expected = last['input'] + math.floor(returned)*stride
                pointer_delta = first['input']-expected
                phase_delta = first['phase']-(returned-math.floor(returned))
                counts['same_span_pairs'] += 1
                counts['same_span_pointer_mismatch'] += pointer_delta != 0
                phase_error.add(phase_delta)
                if (pointer_delta or abs(phase_delta) > 1e-6) and len(pointer_examples) < 20:
                    pointer_examples.append(dict(previous=group_detail(old), current=group_detail(group),
                                                 pointer_delta=pointer_delta, phase_delta=phase_delta))
                relative_last = ((last['input']-first['input'])/stride +
                                 returned-last['rate'])
                peek = floats(first['row'].b, 64, 4)
                # The previous last and current first can share a raw sample
                # pair when rate<1. This checks buffer contents across calls,
                # using the CURRENT peek to predict the PREVIOUS output.
                if (not pointer_delta and not old_unit and not unit and
                        0 <= relative_last < 1 and first['phase']+1 < first['available'] and
                        any(peek) and last['produced'] and
                        old['row'].last[:ch] == last['row'].last[:ch]):
                    for c in range(min(ch, 2)):
                        gain = last['gain'][c] + (last['produced']-1)*last['delta'][c]
                        expected_sample = (peek[c]+(peek[2+c]-peek[c])*relative_last)*gain
                        if fmt == 2:
                            expected_sample *= PCM16_SCALE
                        error = abs(expected_sample-last['row'].last[c])
                        same_pair_error.add(error)
                        if error > 0.0001 and len(same_pair_examples) < 20:
                            same_pair_examples.append(dict(previous=group_detail(old),
                                current=group_detail(group), channel=c, error=error))
            if last['produced'] < 8:
                counts['ranking_previous_final_src_less_8'] += 1
                continue
            if (old['row'].last[:ch] != last['row'].last[:ch] or
                    r.first[:ch] != first['row'].first[:ch]):
                counts['ranking_edge_mismatch'] += 1
                continue
            if not group['gain'] == group['target'] == old['gain'] == old['target']:
                counts['ranking_changing_gain'] += 1
                continue
            relative_rate_change = abs(first['rate']-last['rate']) / max(abs(first['rate']), 1e-10)
            if relative_rate_change > 0.001:
                counts['ranking_rate_changed_over_0_1_percent'] += 1
                continue
            counts['ranking_eligible_stable_pairs'] += 1
            tail = floats(last['row'].a, 24, 16)
            for c in range(min(ch, 2)):
                boundary = abs(r.first[c]-old['row'].last[c])
                interior = sum(abs(tail[2*(n+1)+c]-tail[2*n+c]) for n in range(7))/7
                key = (r.descriptor, c, group['pre']['source_hz'])
                item = ranking.setdefault(key, dict(pairs=0, boundary=0.0, interior=0.0,
                                                   maximum=0.0, events=[]))
                item['pairs'] += 1
                item['boundary'] += boundary
                item['interior'] += interior
                item['maximum'] = max(item['maximum'], boundary)
                if boundary > 0.005 and boundary > 3*interior:
                    event = dict(boundary=boundary, interior_mean=interior,
                        ratio=boundary/interior if interior else None, channel=c,
                        rate_relative_change=relative_rate_change,
                        previous=group_detail(old), current=group_detail(group))
                    item['events'].append(event)
                    seam_events.append(event)
    ranked = []
    for (address, c, hz), item in ranking.items():
        if item['pairs'] < 16:
            continue
        ranked.append(dict(voice=f'{address:08X}', channel=c, source_hz=hz,
            pairs=item['pairs'], boundary_sum=item['boundary'], interior_mean_sum=item['interior'],
            ratio=item['boundary']/item['interior'] if item['interior'] else None,
            maximum_boundary=item['maximum'], large_seam_events=len(item['events']),
            events=sorted(item['events'], key=lambda x:-x['boundary'])[:3]))
    ranked.sort(key=lambda x:x['interior_mean_sum']-x['boundary_sum'])
    return dict(counters=dict(counts), same_span_phase_error=phase_error.report(),
        previous_output_vs_current_raw_pair_error=same_pair_error.report(),
        pointer_examples=pointer_examples, same_pair_examples=same_pair_examples,
        ranking_rule='Stable equal gains, <=0.1% rate change, >=16 pairs; ranked by total boundary excess over mean seven prior-tail differences. Ranking is not proof of a defect.',
        ranked_identities=len(ranked), ranking=ranked[:128],
        largest_events=sorted(seam_events, key=lambda x:-x['boundary'])[:25])


def analyze(data):
    header = validate(data)
    counters, tags = collections.Counter(), collections.Counter()
    shapes = collections.defaultdict(collections.Counter)
    metrics = collections.defaultdict(Metric)
    examples = collections.defaultdict(list)
    voices, previous_voice, current_voice = {}, {}, {}
    states, previous_bq, previous_limiter = {}, {}, {}
    limiter_in, limiter_out = Seams(), Seams()
    limiter_engine_in, limiter_engine_out = Seams(), Seams()
    rate_values, master_values = [], []

    def example(kind, r, **fields):
        if len(examples[kind]) < 20:
            examples[kind].append(dict(**r.where(), **fields))

    for i in range(header['records']):
        r = Record(data, i)
        b, a = r.b, r.a
        tags[f'{r.tag:08X}'] += 1
        if not r.sequence or not r.qpc:
            counters['invalid_prefix'] += 1
            continue
        if not header['start_frame'] <= (r.sequence-1)*256 < header['end_frame']:
            counters['outside_header_window'] += 1
        if r.tag in SRC or r.tag in (VOICE, BIQUAD):
            if not 2160000 <= (r.sequence-1)*256 < 2640000:
                counters['engine_tag_outside_45_55_seconds'] += 1
        if r.tag == VOICE:
            ch, frames = r.result >> 24, r.result & 0xffffff
            if not 1 <= ch <= 16 or not 1 <= frames <= 256 or not span(r.descriptor, 80):
                counters['voice_invalid_snapshot'] += 1
                continue
            pre, post = voice_state(b), voice_state(a)
            old_gain, target = floats(b, 80, 2)
            master, new_gain = floats(a, 80, 2)
            if not finite(pre['phase'], post['phase'], pre['source_hz'], pre['pitch'],
                          old_gain, target, master, new_gain, r.first, r.last):
                counters['voice_nonfinite'] += 1
                continue
            shapes['voice_format_channels'][(pre['format'], ch)] += 1
            shapes['voice_hz_pitch'][(pre['source_hz'], pre['pitch'])] += 1
            shapes['voice_gain_old_target_after'][(old_gain, target, new_gain)] += 1
            counters['voice_gain_changing'] += old_gain != target
            counters['voice_post_gain_not_target'] += new_gain != target
            counters['voice_missing_buffer_words'] += pre['buffer_word'] == pre['format_word'] == 0
            master_values.append(master)
            group = dict(row=r, pre=pre, post=post, channels=ch, frames=frames,
                         gain=old_gain, target=target, new_gain=new_gain, master=master,
                         src=[], counts=collections.Counter())
            current_voice[r.thread] = group
            v = voices.setdefault(r.descriptor, dict(count=0, formats=collections.Counter(),
                 channels=collections.Counter(), changes=collections.Counter(),
                 boundary=Metric(), src_calls=collections.Counter(), groups=[]))
            v['count'] += 1
            v['formats'][pre['format']] += 1
            v['channels'][ch] += 1
            v['groups'].append(group)
            key = (r.thread, r.descriptor)
            old = previous_voice.get(key)
            previous_voice[key] = group
            if old and old['row'].sequence + 1 == r.sequence:
                prior = old['post']
                for field in ('buffer', 'cursor', 'phase', 'buffer_cursor', 'extent'):
                    changed = prior[field] != pre[field]
                    v['changes'][field] += changed
                    counters['voice_between_calls_changed_' + field] += changed
                for c in range(min(ch, old['channels'], 6)):
                    v['boundary'].add(r.first[c] - old['row'].last[c])
                if prior['buffer'] == pre['buffer'] and prior['cursor'] != pre['cursor']:
                    example('voice_cursor_changes_same_buffer', r, previous=prior, current=pre)
            continue

        if r.tag in SRC:
            name, unit, expected_format = SRC[r.tag]
            ch, inp, available, output, count_slot, lr = struct.unpack_from('>6I', b)
            phase, rate = floats(b, 24, 2)
            gain, delta = floats(b, 32, 4), floats(b, 48, 4)
            pair, owner, fmt = floats(b, 64, 4), u32(b, 80), u32(b, 84)
            returned_phase, produced = floats(a, 0, 1)[0], u32(a, 4)
            tail = floats(a, 24, 16)
            if (not 1 <= ch <= 4 or fmt != expected_format or not 0 <= produced <= r.result <= 256
                    or not span(inp, available * ch * (2 if fmt == 2 else 4))
                    or not span(output, produced * 16) or not span(count_slot, 4)):
                counters['src_invalid_snapshot'] += 1
                example('src_invalid_snapshot', r, channels=ch, requested=r.result, produced=produced)
                continue
            if not finite(phase, rate, returned_phase, gain, delta, pair, tail, r.first, r.last):
                counters['src_nonfinite'] += 1
                continue
            counters['src_valid'] += 1
            counters['src_partial'] += produced < r.result
            counters['src_nonzero_gain_step'] += any(x != 0 for x in delta[:ch])
            counters['src_rate_equal_one'] += rate == 1
            counters['src_rate_below_one'] += rate < 1
            counters['src_rate_above_one'] += rate > 1
            shapes['src_tag_channels'][(name, ch)] += 1
            shapes['src_requested_produced'][(r.result, produced)] += 1
            shapes['src_output_mod128'][output % 128] += 1
            shapes['src_gain_and_delta'][(gain, delta)] += 1
            shapes['src_real_caller'][f'{lr:08X}'] += 1
            rate_values.append(rate)
            metrics['src_returned_phase_vs_ideal_linear_error'].add(returned_phase - (phase + produced * rate))
            # Peeks have no explicit validity bit. Require nonzero data as well
            # as the hook's logical bounds before a conditional sample oracle.
            peek_eligible = phase >= 0 and phase + 1 < available
            if produced and peek_eligible and any(x != 0 for x in pair):
                fraction = 0 if unit else phase - math.floor(phase)
                scale = PCM16_SCALE if fmt == 2 else 1
                for c in range(min(ch, 2)):
                    expected = (pair[c] + (pair[2+c]-pair[c])*fraction)*scale*gain[c]
                    error = abs(r.first[c] - expected)
                    metrics['src_first_sample_conditional_error'].add(error)
                    if error > 0.0001:
                        example('src_first_sample_error', r, channel=c, actual=r.first[c],
                                expected=expected, phase=phase, gain=gain, input_pair=pair)
            else:
                counters['src_first_sample_oracle_unavailable'] += 1
            tail_pattern = (produced >= 8 and
                all(tail[n*2+c] == 0 for n in range(7) for c in range(min(ch, 2))) and
                any(tail[14+c] != 0 for c in range(min(ch, 2))))
            if tail_pattern:
                counters['src_sparse_seven_zero_tail_pattern'] += 1
                counters['src_unit_seven_zero_tail_pattern'] += unit
                example('src_seven_zero_tail', r, produced=produced, output=f'{output:08X}',
                        unit=unit, tail=tail, gain=gain, delta=delta)
            group = current_voice.get(r.thread)
            if not group or group['row'].descriptor != owner or group['row'].sequence != r.sequence:
                counters['src_unmatched_owner'] += 1
                continue
            counters['src_matched_owner'] += 1
            calls = group['src']
            offset = sum(call['produced'] for call in calls)
            # Negative initial source cursor means leading output silence.
            # E758 advances the gain over that skipped output before first SRC.
            if group['pre']['cursor'] < 0:
                offset += group['frames'] - (calls[0]['row'].result if calls else r.result)
            expected_gain = group['gain'] + offset * (group['target']-group['gain']) / group['frames']
            expected_step = (group['target']-group['gain']) / group['frames']
            for c in range(ch):
                metrics['src_gain_vs_voice_linear_ramp_error'].add(gain[c]-expected_gain)
                metrics['src_delta_vs_voice_linear_ramp_error'].add(delta[c]-expected_step)
            expected_rate = f32(f32(group['pre']['source_hz'] * group['pre']['pitch']) * group['master'])
            metrics['src_rate_vs_voice_product_error'].add(rate - expected_rate)
            if calls:
                previous = calls[-1]
                total_prior = sum(call['produced'] for call in calls)
                for c in range(ch):
                    carried = calls[0]['gain'][c] + total_prior*calls[0]['delta'][c]
                    metrics['src_gain_vs_first_src_carried_error'].add(gain[c]-carried)
                    counters['src_delta_changed_within_voice'] += delta[c] != calls[0]['delta'][c]
                    if abs(gain[c]-carried) > 1e-6:
                        example('src_gain_carry_error', r, channel=c, expected=carried,
                                actual=gain[c], prior_produced=total_prior)
                contiguous = output == previous['output'] + previous['produced'] * 16
                counters['src_split_pairs'] += 1
                counters['src_split_noncontiguous_output'] += not contiguous
                shapes['src_split_pair'][(previous['produced'], produced, contiguous)] += 1
                if contiguous and previous['produced'] and produced:
                    for c in range(min(ch, 2)):
                        metrics['src_split_edge_delta'].add(r.first[c]-previous['row'].last[c])
            calls.append(dict(row=r, produced=produced, output=output, phase=phase,
                              returned_phase=returned_phase, rate=rate, gain=gain, delta=delta,
                              input=inp, available=available))
            continue

        if r.tag == BIQUAD:
            fmt, inp, out = struct.unpack_from('>3I', b)
            ch, frames = fmt >> 24, fmt & 0xffffff
            if (r.result != fmt or frames != 256 or not 1 <= ch <= 8
                    or not span(r.descriptor, 280) or not span(inp, 256*((ch+3)//4)*16)):
                counters['biquad_invalid_snapshot'] += 1
                continue
            kind = u32(b, 12)
            params = floats(b, 16, 4)
            old_coeff, new_coeff = floats(b, 32, 6), floats(a, 0, 6)
            inputs, outputs, history = floats(b, 56, 8), floats(a, 24, 8), floats(a, 56, 8)
            if not finite(params, old_coeff, new_coeff, inputs, outputs, history):
                counters['biquad_nonfinite'] += 1
                continue
            shape = (ch, kind, *params)
            shapes['biquad_parameters_channels_type_hz_q_gain_wet'][shape] += 1
            state = states.setdefault(r.descriptor, dict(count=0, shapes=collections.Counter(),
                       input=Seams(), output=Seams(), first_error=Metric(), x1_error=Metric(),
                       coeff_discontinuities=0))
            state['count'] += 1
            state['shapes'][shape] += 1
            for c in range(min(ch, 2)):
                state['x1_error'].add(history[c]-inputs[6+c])
            key = (r.thread, r.descriptor)
            previous = previous_bq.get(key)
            previous_bq[key] = dict(row=r, coeff=new_coeff, input=inputs, output=outputs, history=history)
            if previous and previous['row'].sequence + 1 == r.sequence:
                state['input'].add(previous['input'], inputs, ch)
                state['output'].add(previous['output'], outputs, ch)
                if old_coeff != previous['coeff']:
                    state['coeff_discontinuities'] += 1
                else:
                    for c in range(min(ch, 2)):
                        expected = biquad_sample(inputs[c], old_coeff, previous['history'], c)
                        error = abs(outputs[c]-expected)
                        state['first_error'].add(error)
                        if error > 0.0001:
                            example('biquad_first_sample_error', r, channel=c, expected=expected,
                                    actual=outputs[c], old_coeff=old_coeff, previous_history=previous['history'])
            continue

        if r.tag == LIMITER:
            fmt, inp, out, history, delay, attack = struct.unpack_from('>6I', b)
            ch, frames = fmt >> 24, fmt & 0xffffff
            levels, before, after = floats(b, 24, 3), floats(b, 36, 3), floats(a, 0, 3)
            inputs, outputs = floats(b, 48, 8), floats(a, 12, 8)
            if (not 1 <= ch <= 8 or frames != 256 or not span(history, 1)
                    or not span(inp, frames*((ch+3)//4)*16) or not span(out, frames*((ch+3)//4)*16)):
                counters['limiter_invalid_snapshot'] += 1
                continue
            if not finite(levels, before, after, inputs, outputs):
                counters['limiter_nonfinite'] += 1
                continue
            shapes['limiter_shape'][(ch, frames, delay, attack, *levels)] += 1
            counters['limiter_alias'] += inp == out or inp == history or out == history
            previous = previous_limiter.get((r.thread, history))
            previous_limiter[(r.thread, history)] = dict(row=r, input=inputs, output=outputs)
            if previous and previous['row'].sequence + 1 == r.sequence:
                limiter_in.add(previous['input'], inputs, ch)
                limiter_out.add(previous['output'], outputs, ch)
                if 2160000 <= (r.sequence-1)*256 < 2640000:
                    limiter_engine_in.add(previous['input'], inputs, ch)
                    limiter_engine_out.add(previous['output'], outputs, ch)
                limit, target, release = levels
                if delay == 128 and limit > 0:
                    n = attack if max(before[2], after[2])*before[0] > limit and attack else 256
                    def gain_at(phase):
                        return target/limit*(before[0]+min(phase, n)*(after[0]-before[0])/n)
                    for c in range(min(ch, 2)):
                        error = max(abs(previous['input'][6+c]*gain_at(127)-outputs[2+c]),
                                    abs(inputs[c]*gain_at(128)-outputs[4+c]))
                        metrics['limiter_delay_gain_oracle_error'].add(error)
                        if error > 0.0001:
                            example('limiter_oracle_error', r, channel=c, error=error)
            continue

        # The SDK record's tag is its original caller PC, not a stage marker.
        # Validate the known descriptor shape before interpreting such a record.
        ch, capacity = b[13], u32(b, 24)
        if (1 <= ch <= 6 and 0 < capacity <= 256 and
                u32(b, 28) <= u32(a, 28) <= capacity and
                4000 <= u32(b, 16) <= 192000 and 4000 <= u32(b, 32) <= 192000):
            counters['sdk_descriptor_records'] += 1
            shapes['sdk_kernel_channels'][(f'{u32(a,76):08X}', ch)] += 1
        else:
            counters['unknown_or_incomplete_tag'] += 1

    voice_reports = []
    for address, voice in voices.items():
        for group in voice['groups']:
            counts = tuple(c['produced'] for c in group['src'])
            voice['src_calls'][len(counts)] += 1
            shapes['src_calls_per_voice'][len(counts)] += 1
            shapes['src_voice_output_chunks'][counts] += 1
            counters['voice_src_total_less_than_frames'] += sum(counts) < group['frames']
            counters['voice_src_total_exceeds_frames'] += sum(counts) > group['frames']
            if group['src'] and sum(counts) == group['frames']:
                first, last = group['src'][0]['row'], group['src'][-1]['row']
                for c in range(min(group['channels'], 6)):
                    metrics['voice_vs_first_src_edge_error'].add(group['row'].first[c]-first.first[c])
                    metrics['voice_vs_last_src_edge_error'].add(group['row'].last[c]-last.last[c])
        voice_reports.append(dict(voice=f'{address:08X}', calls=voice['count'],
            formats=top(voice['formats']), channels=top(voice['channels']),
            between_call_changes=dict(voice['changes']), boundary=voice['boundary'].report(),
            src_calls_per_voice=top(voice['src_calls'])))
    state_reports = [dict(state=f'{address:08X}', calls=s['count'], params=top(s['shapes'], 8),
        input=s['input'].report(), output=s['output'].report(),
        first_sample_oracle_error=s['first_error'].report(),
        post_x1_vs_input255_error=s['x1_error'].report(),
        old_coeff_vs_previous_new_discontinuities=s['coeff_discontinuities'])
        for address, s in states.items()]
    def extent(values):
        return dict(count=len(values), minimum=min(values) if values else None,
                    maximum=max(values) if values else None)
    report = dict(header=header, tags=dict(tags), counters=dict(counters),
        src_rates=extent(rate_values), master_pitch_conversion_factor=extent(master_values),
        histograms={k:top(v) for k,v in shapes.items()},
        metrics={k:v.report() for k,v in metrics.items()},
        limiter=dict(input=limiter_in.report(), output=limiter_out.report(),
                     engine_window_input=limiter_engine_in.report(),
                     engine_window_output=limiter_engine_out.report()),
        unique_voices=len(voice_reports), unique_biquad_states=len(state_reports),
        voices=sorted(voice_reports, key=lambda r:-r['calls'])[:512],
        biquad_states=sorted(state_reports, key=lambda r:-r['calls'])[:512],
        source_continuity=source_continuity(voices),
        examples=dict(examples), notes=[
            'All metadata is BE; prefix/header and first/last floats are native LE.',
            'Snapshot zeros lack explicit validity bits: zero samples alone do not prove silence.',
            'Voice buffer format 0 may be absent/transition metadata; actual SRC tag proves decoded format.',
            'First-sample/state/ramp oracles are conditional checks, not complete waveform verification.',
            'Returned-phase ideal-linear error includes repeated float32 accumulation; it is not an instruction oracle or an automatic failure.',
            'Continuity checks pair consecutive submits on the same thread and voice/state identity.',
            'Voice identities can be reused; cursor changes are evidence, not automatically defects.',
            'Top histograms/examples and identity reports are bounded; omitted counts are explicit.',
            'BiQuad sparse phases only cover first two channels; native edges cover at most six.',
            'No whole-stream duplicate/clipping diagnosis is possible from sparse trace samples.'])
    return json_safe(report)


def synthetic_record(tag, before, after, sequence=9000, descriptor=0x10000,
                     result=0, first=(0,)*6, last=(0,)*6, index=0):
    return (PREFIX.pack(100000+index, sequence, 7, descriptor, tag, result)
            + bytes(before) + bytes(after) + struct.pack('<12f', *first, *last))


def synthetic_file(rows):
    return HEADER.pack(b'DRRSMP01', 1, 256, len(rows), 10000000,
                       960000, 3840000, 0, 0) + b''.join(rows)


def self_test():
    rows = []
    b, a = bytearray(88), bytearray(88)
    struct.pack_into('>H', b, 14, 16)
    struct.pack_into('>4f', b, 40, 48000, 1, 0, 0)
    struct.pack_into('>IIff', b, 72, 2 << 30, 2048 << 7, 0.5, 0.5)
    a[:] = b
    struct.pack_into('>ff', a, 80, 1/48000, 0.5)
    rows.append(synthetic_record(VOICE, b, a, result=(1 << 24) | 256,
                                first=(0.125,0,0,0,0,0), last=(0.125,0,0,0,0,0)))
    for tag, (_, unit, fmt) in SRC.items():
        b, a = bytearray(88), bytearray(88)
        struct.pack_into('>6I', b, 0, 1, 0x20000, 2048, 0x30000, 0x40000, 0x8281F000)
        struct.pack_into('>2f', b, 24, 0, 1)
        struct.pack_into('>4f', b, 32, 0.5,0.5,0.5,0.5)
        sample = 8192 if fmt == 2 else 0.25
        struct.pack_into('>4f', b, 64, sample,0,sample,0)
        struct.pack_into('>2I', b, 80, 0x10000, fmt)
        struct.pack_into('>fI', a, 0, 256,256)
        value = sample*(PCM16_SCALE if fmt == 2 else 1)*0.5
        for n in range(8):
            struct.pack_into('>2f', a, 24+n*8, value,0)
        rows.append(synthetic_record(tag,b,a,result=256,first=(value,0,0,0,0,0),
                                     last=(value,0,0,0,0,0),index=len(rows)))
    # Two identity BiQuads with a deliberately continuous 256-frame ramp.
    for n in range(2):
        b, a = bytearray(88), bytearray(88)
        struct.pack_into('>3I', b, 0, (1 << 24)|256, 0x50000, 0x50000)
        struct.pack_into('>I4f', b, 12, 0, 50,0.75,1,1)
        coefficient = (0,0,1,0,0,1)
        struct.pack_into('>6f', b, 32, *coefficient)
        struct.pack_into('>6f', a, 0, *coefficient)
        phase = tuple(v for f in (0,127,128,255) for v in ((n*256+f)/1024,0))
        struct.pack_into('>8f', b, 56, *phase)
        struct.pack_into('>8f', a, 24, *phase)
        struct.pack_into('>8f', a, 56, phase[6],0,0,0,phase[6],0,0,0)
        rows.append(synthetic_record(BIQUAD,b,a,sequence=9000+n,descriptor=0x60000,
                                     result=(1<<24)|256,index=len(rows)))
    # Two default delay/gain limiter records with a constant input.
    for n in range(2):
        b, a = bytearray(88), bytearray(88)
        struct.pack_into('>6I', b, 0, (2<<24)|256, 0x70000,0x80000,0x90000,128,128)
        struct.pack_into('>6f', b, 24, 1,0.9,0.075,1,0,0.25)
        struct.pack_into('>3f', a, 0, 1,0,0.25)
        struct.pack_into('>8f', b, 48, *([0.25]*8))
        struct.pack_into('>8f', a, 12, *([0.225]*8))
        rows.append(synthetic_record(LIMITER,b,a,sequence=9000+n,index=len(rows)))
    report = analyze(synthetic_file(rows))
    assert report['tags'][f'{VOICE:08X}'] == 1
    assert report['counters']['src_valid'] == 4
    assert report['counters']['src_matched_owner'] == 4
    assert report['metrics']['src_first_sample_conditional_error']['maximum'] < 1e-7
    assert report['metrics']['limiter_delay_gain_oracle_error']['maximum'] < 1e-7
    assert report['biquad_states'][0]['first_sample_oracle_error']['maximum'] == 0
    assert report['biquad_states'][0]['input']['boundary_over_midpoint'] == 1
    # All-zero/invalid tags must not masquerade as measured silence.
    bad_row = synthetic_record(VOICE, bytes(88), bytes(88))
    assert analyze(synthetic_file([bad_row]))['counters']['voice_invalid_snapshot'] == 1
    malformed = [b'', synthetic_file(rows)[:-1], synthetic_file(rows)+b'x']
    for offset, fmt, value in ((8,'<I',2),(12,'<I',128),(16,'<Q',CAPACITY+1),(24,'<Q',0)):
        wrong = bytearray(synthetic_file(rows))
        struct.pack_into(fmt,wrong,offset,value)
        malformed.append(wrong)
    for wrong in malformed:
        try:
            analyze(wrong)
        except ValueError:
            pass
        else:
            raise AssertionError('Malformed extent/header accepted')
    nonfinite = bytearray(synthetic_file(rows))
    struct.pack_into('>f', nonfinite,64+32+80,math.nan)
    assert analyze(nonfinite)['counters']['voice_nonfinite'] == 1
    json.dumps(json_safe({'nan':math.nan,'inf':math.inf,'report':report}),allow_nan=False)
    print('PASS: all six engine tags, BE/native fields, SRC oracle, BiQuad continuity, limiter oracle, invalid snapshots, extent/version/count bounds, strict JSON')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', nargs='?', type=Path)
    parser.add_argument('--output', type=Path, help='Optional JSON analysis output; input is never modified')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
    if args.trace:
        if args.output and args.output.resolve() == args.trace.resolve():
            parser.error('Output must not overwrite the trace input')
        if args.trace.stat().st_size > 64 + CAPACITY*256:
            parser.error('File exceeds the bounded trace capacity')
        report = analyze(args.trace.read_bytes())
        encoded = json.dumps(report,indent=2,allow_nan=False)
        if args.output:
            args.output.write_text(encoded+'\n',encoding='utf-8')
            print(json.dumps({k:report[k] for k in ('header','tags','counters','src_rates','metrics')},
                             indent=2,allow_nan=False))
            print('Analysis:', args.output)
        else:
            print(encoded)
    elif not args.self_test:
        parser.error('Supply a trace path or --self-test')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, struct.error) as error:
        print('Trace analysis failed:', error, file=sys.stderr)
        sys.exit(2)
