#!/usr/bin/env python3
"""Measure presentation intervals during Wayfire workspace switches.

Switches between the current workspace and a neighbor, then restores the
original workspace. Run inside a Wayfire session. WAYFIRE_SOCKET can select a
specific compositor when more than one Wayfire instance is running. A monotonic
presentation counter distinguishes a missed display refresh from an IPC sample
that arrived too late to observe an intermediate frame.
"""

import argparse
import json
import os
from pathlib import Path
import re
import socket
import statistics
import struct
import threading
import time


def socket_path():
    explicit = os.environ.get('WAYFIRE_SOCKET')
    if explicit:
        return explicit
    runtime = Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}'))
    candidates = sorted(runtime.glob('wayfire-wayland-*.socket'))
    if len(candidates) != 1:
        raise RuntimeError(f'Expected one Wayfire socket, found {len(candidates)}; '
                           'set WAYFIRE_SOCKET to select one')
    return str(candidates[0])


class IPC:
    def __init__(self, path):
        self.connection = socket.socket(socket.AF_UNIX)
        self.connection.settimeout(2)
        self.connection.connect(path)

    def read(self, length):
        result = bytearray()
        while len(result) < length:
            chunk = self.connection.recv(length - len(result))
            if not chunk:
                raise RuntimeError('Wayfire closed the IPC connection')
            result.extend(chunk)
        return result

    def call(self, method, data=None):
        payload = json.dumps({'method': method, 'data': data or {}}).encode()
        self.connection.sendall(struct.pack('<I', len(payload)) + payload)
        length = struct.unpack('<I', self.read(4))[0]
        if length > 1024 * 1024:
            raise RuntimeError('Unexpectedly large IPC reply')
        response = json.loads(self.read(length))
        if isinstance(response, dict) and 'error' in response:
            raise RuntimeError(response['error'])
        return response

    def close(self):
        self.connection.close()


def neighbor(workspace):
    if workspace['grid_height'] > 1:
        return (workspace['x'], workspace['y'] + 1
                if workspace['y'] + 1 < workspace['grid_height'] else workspace['y'] - 1)
    if workspace['grid_width'] > 1:
        return (workspace['x'] + 1 if workspace['x'] + 1 < workspace['grid_width']
                else workspace['x'] - 1, workspace['y'])
    raise RuntimeError('The output has only one workspace')


def sample_metrics(path, output_id, interval, stop, records, errors):
    ipc = None
    try:
        ipc = IPC(path)
        while not stop.is_set():
            started = time.monotonic()
            metrics = ipc.call('wayfire/render-metrics', {'output-id': output_id})
            completed = time.monotonic()
            records.append((completed, metrics['last-presentation-ns'],
                            (completed - started) * 1000, metrics))
            stop.wait(max(0, interval - (time.monotonic() - started)))
    except (OSError, KeyError, RuntimeError) as error:
        errors.append(error)
    finally:
        if ipc:
            ipc.close()


def interval_summary(records, start, end, show_budget=False):
    timestamps = []
    for sampled_at, presentation, _, metrics in records:
        # A query just after the switch can still report the last idle frame.
        # Its presentation happened before this measurement window and the
        # following idle-to-active gap is not an animation frame interval.
        if start <= sampled_at <= end and presentation >= int(start * 1_000_000_000) and (
            not timestamps or presentation != timestamps[-1][1]
        ):
            timestamps.append((sampled_at, presentation, metrics.get('presented-frames')))
    intervals = [((b[1] - a[1]) / 1_000_000,
                  b[2] - a[2] if a[2] is not None and b[2] is not None else None)
                 for a, b in zip(timestamps, timestamps[1:])]
    if not intervals:
        return {'frames': len(timestamps), 'intervals': 0}
    verified = [value for value, count_delta in intervals if count_delta == 1]
    observed = [value for value, _ in intervals]
    ambiguous = [(index, value) for index, (value, count_delta) in enumerate(intervals)
                 if count_delta != 1]
    result = {
        'frames': len(timestamps),
        'intervals': len(intervals),
        'verified_intervals': len(verified),
        'median_ms': round(statistics.median(verified), 2) if verified else None,
        'max_ms': round(max(verified), 2) if verified else None,
        'observed_max_ms': round(max(observed), 2),
        'over_12_5_ms': sum(value > 12.5 for value in verified),
        'over_16_ms': sum(value > 16 for value in verified),
        'unverified_over_16_ms': sum(value > 16 for _, value in ambiguous),
        'sampled_frames_skipped': sum(max(0, count_delta - 1)
                                      for _, count_delta in intervals if count_delta is not None),
        'long_gaps_at_ms': [round((timestamps[index + 1][0] - start) * 1000)
            for index, (value, count_delta) in enumerate(intervals)
            if count_delta == 1 and value > 12.5],
        'unverified_gaps_at_ms': [round((timestamps[index + 1][0] - start) * 1000)
            for index, value in ambiguous if value > 12.5],
        'slow_queries': [(round((sampled_at - start) * 1000), round(latency, 1))
            for sampled_at, _, latency, _ in records if start <= sampled_at <= end and latency > 1.5],
    }
    if show_budget:
        metrics = [entry[3] for entry in records if start <= entry[0] <= end]
        if metrics:
            budgets = [item['composed']['paint-budget-ns'] / 1_000_000 for item in metrics]
            result['paint_budget_ms_min_max'] = (round(min(budgets), 2),
                                                  round(max(budgets), 2))
            result['rejected_timer_samples_delta'] = (
                metrics[-1].get('rejected-render-timer-samples', 0) -
                metrics[0].get('rejected-render-timer-samples', 0))
            result['render_timer_support'] = metrics[-1]['render-timer-support']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--switches', type=int, default=4,
                        help='number of switches; must be positive and even (default: 4)')
    parser.add_argument('--sample-ms', type=float, default=3.0,
                        help='IPC sample period in milliseconds (default: 3)')
    parser.add_argument('--output', help='output name (default: first output)')
    parser.add_argument('--show-budget', action='store_true',
                        help='include adaptive paint budget and rejected GPU timer samples')
    parser.add_argument('--trace-output', type=Path,
                        help='write raw IPC samples and switch times as JSON for diagnosis')
    args = parser.parse_args()
    if args.switches < 2 or args.switches % 2 or args.sample_ms <= 0:
        parser.error('--switches must be positive and even; --sample-ms must be positive')

    path = socket_path()
    control = IPC(path)
    stop = threading.Event()
    records = []
    errors = []
    sampler = None
    original = None
    output_id = None
    duration = .3
    try:
        outputs = control.call('window-rules/list-outputs')
        output = next((item for item in outputs if item['name'] == args.output), None) \
            if args.output else (outputs[0] if outputs else None)
        if output is None:
            raise RuntimeError('Output not found')
        output_id = output['id']
        initial_metrics = control.call('wayfire/render-metrics', {'output-id': output_id})
        refresh_ns = initial_metrics['refresh-ns']
        counter_available = 'presented-frames' in initial_metrics
        workspace = output['workspace']
        original = (workspace['x'], workspace['y'])
        adjacent = neighbor(workspace)
        duration_text = str(control.call('wayfire/get-config-option',
            {'option': 'vswitch/duration'}).get('value', '300ms'))
        match = re.match(r'(\d+(?:\.\d+)?)ms', duration_text)
        duration = float(match.group(1)) / 1000 if match else .3
        sampler = threading.Thread(target=sample_metrics,
            args=(path, output_id, args.sample_ms / 1000, stop, records, errors), daemon=True)
        sampler.start()

        switches = []
        for index in range(args.switches):
            destination = adjacent if index % 2 == 0 else original
            started = time.monotonic()
            control.call('vswitch/set-workspace', {'x': destination[0],
                'y': destination[1], 'output-id': output_id})
            switches.append((started, started + duration))
            time.sleep(duration + .12)
        if errors:
            raise RuntimeError(f'Sampler failed: {errors[0]}')
        refresh_hz = 1_000_000_000 / refresh_ns if refresh_ns else 0
        print(f'output={output["name"]} refresh={refresh_hz:.1f}Hz duration={duration_text} '
              f'sample={args.sample_ms:g}ms '
              f'presentation-counter={"available" if counter_available else "unavailable; gaps unverified"}')
        for index, (started, ended) in enumerate(switches, 1):
            print(f'switch {index}: {interval_summary(records, started + .02, ended, args.show_budget)}')
        latencies = [item[2] for item in records]
        if latencies:
            print(f'IPC query: median={statistics.median(latencies):.2f}ms '
                  f'max={max(latencies):.2f}ms samples={len(latencies)}')
        if args.trace_output:
            stop.set()
            sampler.join(timeout=3)
            args.trace_output.write_text(json.dumps({
                'output': output['name'],
                'switches': switches,
                'records': records,
            }))
    finally:
        stop.set()
        if sampler:
            sampler.join(timeout=3)
        if original is not None and output_id is not None:
            try:
                current = next(item['workspace'] for item in control.call('window-rules/list-outputs')
                    if item['id'] == output_id)
                if (current['x'], current['y']) != original:
                    control.call('vswitch/set-workspace', {'x': original[0],
                        'y': original[1], 'output-id': output_id})
                    time.sleep(duration + .12)
            except (OSError, RuntimeError, StopIteration):
                pass
        control.close()


if __name__ == '__main__':
    main()
