#!/usr/bin/env python3
"""Check that selecting the current workspace does not start an animation.

Runs a private Wayland-backed Wayfire nested inside the current session.
"""

import argparse
import os
from pathlib import Path
import runpy
import signal
import subprocess
import tempfile
import time


IPC = runpy.run_path(str(Path(__file__).with_name("profile-vswitch.py")))["IPC"]


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wayfire", type=Path,
        default=Path(__file__).resolve().parents[1] / "build/src/wayfire")
    parser.add_argument("--plugin-path", default=None)
    args = parser.parse_args()
    parent_display = str(Path(os.environ["XDG_RUNTIME_DIR"]) / os.environ["WAYLAND_DISPLAY"])

    with tempfile.TemporaryDirectory(prefix="wayfire-vswitch-noop-") as temp:
        base = Path(temp)
        runtime = base / "runtime"
        runtime.mkdir(mode=0o700)
        config = base / "wayfire.ini"
        config.write_text("[core]\nplugins = ipc ipc-rules vswitch\n"
            "vwidth = 1\nvheight = 2\n"
            "[output:WL-1]\nposition = 0,0\n"
            "[vswitch]\nduration = 300ms\n")
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base),
            WAYLAND_DISPLAY=parent_display, WLR_BACKENDS="wayland",
            WLR_WL_OUTPUTS="1", WLR_RENDERER="gles2")
        if args.plugin_path:
            env["WAYFIRE_PLUGIN_PATH"] = args.plugin_path

        log_path = base / "wayfire.log"
        log = log_path.open("w")
        compositor = subprocess.Popen([str(args.wayfire), "-c", str(config)],
            env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        ipc = None
        try:
            socket = None
            for _ in range(100):
                sockets = list(runtime.glob("wayfire-wayland-*.socket"))
                if sockets:
                    socket = sockets[0]
                    break
                if compositor.poll() is not None:
                    raise RuntimeError(f"Wayfire exited: {compositor.returncode}")
                time.sleep(.05)
            if socket is None:
                raise RuntimeError("Wayfire IPC socket did not appear")
            ipc = IPC(str(socket))
            output = ipc.call("window-rules/list-outputs")[0]
            output_id = output["id"]

            def count_presents(seconds):
                deadline = time.monotonic() + seconds
                timestamps = []
                while time.monotonic() < deadline:
                    timestamp = ipc.call("wayfire/render-metrics",
                        {"output-id": output_id})["last-presentation-ns"]
                    if timestamp and (not timestamps or timestamp != timestamps[-1]):
                        timestamps.append(timestamp)
                    time.sleep(.003)
                return len(timestamps)

            time.sleep(.5)
            idle = count_presents(.4)
            ipc.call("vswitch/set-workspace", {"x": 0, "y": 0, "output-id": output_id})
            same = count_presents(.45)
            ipc.call("vswitch/set-workspace", {"x": 0, "y": 1, "output-id": output_id})
            forward = count_presents(.45)
            ipc.call("vswitch/set-workspace", {"x": 0, "y": 0, "output-id": output_id})
            backward = count_presents(.45)
            print(f"presented frames: idle={idle} same={same} forward={forward} backward={backward}")
            if same > idle + 2 or forward < 20 or backward < 20:
                raise RuntimeError("vswitch no-op or real-switch regression")
        finally:
            if ipc:
                ipc.close()
            stop(compositor)
            log.close()


if __name__ == "__main__":
    main()
