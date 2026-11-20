#!/usr/bin/env python3
"""Check that capped blur buffers still draw the whole translucent window.

Runs an isolated, GPU-backed headless Wayfire session. Requires kitty, grim,
Pillow, and the Wayfire IPC and blur plugins.
"""

import argparse
import os
from pathlib import Path
import runpy
import signal
import subprocess
import tempfile
import time

from PIL import Image


REPO = Path(__file__).resolve().parents[1]
IPC = runpy.run_path(str(REPO / 'scripts/profile-vswitch.py'))['IPC']


def stop(process):
    if process and process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=3)


def wait_for_socket(runtime, compositor):
    for _ in range(100):
        sockets = list(runtime.glob('wayfire-wayland-*.socket'))
        if sockets:
            return sockets[0]
        if compositor.poll() is not None:
            raise RuntimeError(f'Wayfire exited: {compositor.returncode}')
        time.sleep(.05)
    raise RuntimeError('Wayfire IPC socket did not appear')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wayfire', default=str(REPO / 'build/src/wayfire'))
    parser.add_argument('--plugin-path', default=':'.join((
        str(REPO / 'build/plugins/blur'), str(REPO / 'build/plugins/ipc-rules'),
        '/usr/lib/wayfire')))
    parser.add_argument('--image', type=Path,
        default=Path('/tmp/wayfire-blur-buffer-cap.png'))
    parser.add_argument('--scale', type=float, default=1.0,
        help='Output scale; keeps the logical test area at 1280x720')
    parser.add_argument('--buffer-limit', type=int, default=128,
        help='Runtime max buffer size for the test scene')
    args = parser.parse_args()
    if args.scale <= 0:
        parser.error('--scale must be positive')
    if args.buffer_limit <= 0:
        parser.error('--buffer-limit must be positive')

    def pixel(x, y):
        return round(x * args.scale), round(y * args.scale)

    with tempfile.TemporaryDirectory(prefix='wayfire-blur-cap-') as temp:
        base = Path(temp)
        runtime = base / 'runtime'
        runtime.mkdir(mode=0o700)
        config = base / 'wayfire.ini'
        config.write_text(
            '[core]\nplugins = ipc ipc-rules window-rules blur\n'
            f'[output:HEADLESS-1]\nmode = {round(1280 * args.scale)}x'
            f'{round(720 * args.scale)}@120000\nscale = {args.scale}\n'
            '[blur]\nmethod = kawase\n'
            'blur_by_default = (type is "toplevel")\n'
            '[workarounds]\nmax_buffer_size = 16384\n')
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base),
                   WAYFIRE_PLUGIN_XML_PATH=str(REPO / 'metadata'),
                   WAYFIRE_PLUGIN_PATH=args.plugin_path, WLR_BACKENDS='headless',
                   WLR_HEADLESS_OUTPUTS='1', WLR_RENDERER='gles2',
                   WLR_RENDER_DRM_DEVICE='/dev/dri/renderD128')
        log = (base / 'wayfire.log').open('w')
        compositor = subprocess.Popen([args.wayfire, '-c', str(config)], env=env,
                                      stdout=log, stderr=subprocess.STDOUT,
                                      start_new_session=True)
        clients = []
        ipc = None
        try:
            socket = wait_for_socket(runtime, compositor)
            ipc = IPC(str(socket))
            env['WAYFIRE_SOCKET'] = str(socket)
            env['WAYLAND_DISPLAY'] = socket.name.removeprefix('wayfire-').removesuffix('-.socket')

            pattern = ('import time\n'
                       'for row in range(55):\n'
                       ' print("".join(f"\\x1b[48;2;{(row*31+col*17)%256};'
                       '{(row*11+col*41)%256};{(row*71+col*13)%256}m " '
                       'for col in range(110))+"\\x1b[0m",flush=True)\n'
                       'time.sleep(30)\n')
            clients.append(subprocess.Popen(
                ['kitty', '--config', 'NONE', '--class', 'cap-bottom',
                 'python', '-c', pattern], env=env, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, start_new_session=True))
            fifo = base / 'update'
            os.mkfifo(fifo)
            top = ('import sys,time\n'
                   'with open(sys.argv[1]) as pipe: pipe.readline()\n'
                   'sys.stdout.write("\\x1b[20;40H\\x1b[41m  \\x1b[0m")\n'
                   'sys.stdout.flush()\n'
                   'time.sleep(30)\n')
            clients.append(subprocess.Popen(
                ['kitty', '--config', 'NONE', '--class', 'cap-top',
                 '-o', 'background_opacity=0.65', '-o', 'background=#303030',
                 'python', '-c', top, str(fifo)], env=env,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True))

            for _ in range(100):
                views = {view.get('app-id'): view
                         for view in ipc.call('window-rules/list-views')
                         if view.get('mapped') and view.get('app-id') in
                         ('cap-bottom', 'cap-top')}
                if len(views) == 2:
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('Test windows did not map')

            for app, x, y in (('cap-bottom', 70, 50), ('cap-top', 280, 160)):
                reply = ipc.call('window-rules/configure-view', {
                    'id': views[app]['id'],
                    'geometry': {'x': x, 'y': y, 'width': 640, 'height': 400}})
                if reply.get('result') != 'ok':
                    raise RuntimeError(reply)
            ipc.call('window-rules/focus-view', {'id': views['cap-top']['id']})
            for _ in range(80):
                configured = [view for view in ipc.call('window-rules/list-views')
                              if view.get('app-id') in ('cap-bottom', 'cap-top')]
                if len(configured) == 2 and all(
                        view.get('bbox', {}).get('width') == 640 and
                        view.get('bbox', {}).get('height') == 400
                        for view in configured):
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('Test windows did not reach requested size')
            time.sleep(.1)

            reply = ipc.call('wayfire/set-config-options',
                             {'workarounds/max_buffer_size': str(args.buffer_limit)})
            if reply.get('result') != 'ok':
                raise RuntimeError(reply)
            with fifo.open('w') as pipe:
                pipe.write('draw\n')
            time.sleep(.3)
            subprocess.run(['grim', str(args.image)], env=env, check=True,
                           stdout=subprocess.DEVNULL)
            image = Image.open(args.image).convert('RGB')
            background = image.getpixel(pixel(1100, 600))
            window = image.getpixel(pixel(600, 250))
            old_position = image.getpixel(pixel(300, 500))
            background_mean = sum(background) / 3
            window_mean = sum(window) / 3
            print(f'background={background} far-window={window} image={args.image}')
            if window_mean <= background_mean + 8:
                raise RuntimeError('Capped blur erased the right side of the window')
            if sum(old_position) / 3 <= background_mean + 8:
                raise RuntimeError('The original window position is not visible')

            reply = ipc.call('window-rules/configure-view', {
                'id': views['cap-top']['id'],
                'geometry': {'x': 400, 'y': 160, 'width': 640, 'height': 400}})
            if reply.get('result') != 'ok':
                raise RuntimeError(reply)
            time.sleep(.3)
            moved_image = args.image.with_name(args.image.stem + '-moved' + args.image.suffix)
            subprocess.run(['grim', str(moved_image)], env=env, check=True,
                           stdout=subprocess.DEVNULL)
            moved = Image.open(moved_image).convert('RGB')
            moved_window = moved.getpixel(pixel(720, 250))
            cleared = moved.getpixel(pixel(300, 500))
            print(f'moved-window={moved_window} cleared={cleared} image={moved_image}')
            if sum(moved_window) / 3 <= background_mean + 8:
                raise RuntimeError('Capped blur erased the moved window')
            if any(abs(cleared[i] - background[i]) > 2 for i in range(3)):
                raise RuntimeError('Old window pixels remained after the move')
        finally:
            if ipc:
                ipc.close()
            for process in clients:
                stop(process)
            stop(compositor)
            log.close()


if __name__ == '__main__':
    main()
