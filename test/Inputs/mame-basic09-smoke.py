#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


MARKER = "MAME-BASIC09-SMOKE"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Boot the BASIC09 CoCo 3 disk under MAME and check printer output."
    )
    parser.add_argument("--mame", required=True)
    parser.add_argument("--os9", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--seconds", type=int, default=60)
    return parser.parse_args()


def main():
    args = parse_args()
    work_dir = Path(args.work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)

    disk = work_dir / "basic09.dsk"
    stdout = work_dir / "mame.stdout"
    startup = work_dir / "startup"
    script = work_dir / "autoboot.lua"
    shutil.copyfile(args.disk, disk)

    startup.write_text(
        "link shell\n"
        "load utilpak1\n"
        f"echo {MARKER} >/dd/mame.out\n",
        encoding="utf-8",
    )
    for cmd, required in (
        ([args.os9, "attr", f"{disk},/startup", "-w"], False),
        ([args.os9, "del", f"{disk},/startup"], False),
        ([args.os9, "copy", "-l", str(startup), f"{disk},/startup"], True),
    ):
        result = subprocess.run(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        if required and result.returncode != 0:
            sys.stderr.write(result.stdout)
            return result.returncode

    rompath = os.environ.get("MAME_ROM_PATH") or os.environ.get("MAME_ROMPATH")
    video = os.environ.get("MAME_VIDEO", "none")

    script.write_text(
        """
manager.machine.natkeyboard.in_use = true
emu.wait(8)
manager.machine.natkeyboard:post_coded("DOS{ENTER}")
""".lstrip(),
        encoding="utf-8",
    )

    cmd = [
        args.mame,
        "coco3",
        "-skip_gameinfo",
        "-noautosave",
        "-nothrottle",
        "-window",
        "-nomaximize",
        "-sound",
        "none",
        "-natural",
        "-video",
        video,
        "-ext:fdc:wd17xx:0",
        "525qd",
        "-flop1",
        str(disk),
        "-autoboot_delay",
        "1",
        "-autoboot_script",
        str(script),
        "-seconds_to_run",
        str(args.seconds),
    ]
    if rompath:
        cmd.extend(["-rompath", rompath])

    with stdout.open("w", encoding="utf-8") as output:
        result = subprocess.run(
            cmd, stdout=output, stderr=subprocess.STDOUT, text=True
        )

    if result.returncode != 0:
        sys.stderr.write(stdout.read_text(encoding="utf-8", errors="replace"))
        return result.returncode

    marker = subprocess.run(
        [args.os9, "list", f"{disk},/mame.out"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if marker.returncode != 0:
        sys.stderr.write("MAME did not produce the expected disk marker.\n")
        sys.stderr.write(f"Marker output path: {disk},/mame.out\n")
        sys.stderr.write(marker.stdout)
        sys.stderr.write("\nMAME output:\n")
        sys.stderr.write(stdout.read_text(encoding="utf-8", errors="replace"))
        return 1

    print(f"{MARKER}-PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
