#!/usr/bin/env python3
"""Check clipping when a partly offscreen view changes workspaces.

Requires kitty, grim, Pillow, and a GPU-backed wlroots headless renderer.
The nested compositor and clients use a private runtime directory.
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


IPC = runpy.run_path(str(Path(__file__).with_name("profile-vswitch.py")))["IPC"]


def stop(process):
    if process and process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2)


def green_pixels(path):
    with Image.open(path) as image:
        return sum(1 for red, green, blue in image.convert("RGB").get_flattened_data()
                   if green > 100 and red < 100 and blue < 100)


def check(wayfire_binary, scale, axis):
    with tempfile.TemporaryDirectory(prefix="wayfire-isolation-") as temp:
        base = Path(temp)
        runtime = base / "runtime"
        runtime.mkdir(mode=0o700)
        vertical = axis == "vertical"
        target = {"x": 0 if vertical else 1, "y": 1 if vertical else 0}
        config = base / "wayfire.ini"
        config.write_text(
            "[core]\nplugins = ipc ipc-rules vswitch place\n"
            f"vwidth = {1 if vertical else 2}\n"
            f"vheight = {2 if vertical else 1}\n"
            "isolate_workspaces = true\n"
            "[output:HEADLESS-1]\nmode = 800x600@120000\n"
            f"scale = {scale}\nposition = 0,0\n"
            "[vswitch]\nduration = 300ms\n")
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base),
            XDG_DATA_HOME=str(base / "data"), WLR_BACKENDS="headless",
            WLR_HEADLESS_OUTPUTS="1", WLR_RENDER_DRM_DEVICE="/dev/dri/renderD128",
            WLR_RENDERER="gles2", WAYFIRE_PLUGIN_PATH="/usr/lib/wayfire")
        log = (base / "wayfire.log").open("w")
        compositor = subprocess.Popen([str(wayfire_binary), "-c", str(config)],
            env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        kitty = None
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
            env["WAYLAND_DISPLAY"] = socket.name.removeprefix("wayfire-").removesuffix("-.socket")
            ipc = IPC(str(socket))
            output = ipc.call("window-rules/list-outputs")[0]
            output_id = output["id"]
            width = int(output["geometry"]["width"])
            height = int(output["geometry"]["height"])

            kitty = subprocess.Popen(["kitty", "--class", "isolation-green",
                "--title", "Isolation Green", "--override", "background=#00ff00",
                "--override", "foreground=#00ff00", "sh", "-c", "sleep 30"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)
            view = None
            for _ in range(100):
                view = next((v for v in ipc.call("window-rules/list-views")
                    if v.get("app-id") == "isolation-green" and v.get("mapped")), None)
                if view:
                    break
                time.sleep(.05)
            if view is None:
                raise RuntimeError("Kitty did not map")

            geometry = {"x": width - 120 if not vertical else 80,
                "y": height - 120 if vertical else 80, "width": 400, "height": 240}
            ipc.call("window-rules/configure-view", {"id": view["id"], "geometry": geometry})
            time.sleep(.4)

            def capture(name):
                path = base / f"{name}.png"
                subprocess.run(["grim", str(path)], env=env, check=True,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return green_pixels(path)

            home_before = capture("home-before")
            ipc.call("vswitch/set-workspace", {**target, "output-id": output_id})
            time.sleep(.6)
            neighbor_before = capture("neighbor-before")
            ipc.call("vswitch/send-view", {**target, "view-id": view["id"]})
            time.sleep(.4)
            neighbor_after = capture("neighbor-after")
            ipc.call("vswitch/set-workspace", {"x": 0, "y": 0, "output-id": output_id})
            time.sleep(.6)
            home_after = capture("home-after")
            counts = (home_before, neighbor_before, neighbor_after, home_after)
            print(f"{axis} scale={scale}: green pixels {counts}")
            if home_before < 1000 or neighbor_before != 0 or neighbor_after < 1000 or home_after != 0:
                raise RuntimeError("Workspace isolation regression")
        finally:
            if ipc:
                ipc.close()
            stop(kitty)
            stop(compositor)
            log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wayfire", type=Path,
        default=Path(__file__).resolve().parents[1] / "build/src/wayfire")
    args = parser.parse_args()
    for axis in ("vertical", "horizontal"):
        for scale in (1, 1.25):
            check(args.wayfire, scale, axis)


if __name__ == "__main__":
    main()
