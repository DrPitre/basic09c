#!/usr/bin/env python3

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run a BASIC09 source file under CoCo 3 BASIC09 in MAME."
    )
    parser.add_argument("--mame", required=True)
    parser.add_argument("--os9", required=True)
    parser.add_argument("--basic09c", required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--disk", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--source-name", default="codextest")
    parser.add_argument("--output-name", default="coco.out")
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--seconds", type=int, default=150)
    parser.add_argument(
        "--basic09-shutdown-delay",
        type=int,
        default=60,
        help="emulated seconds to wait before typing bye at the BASIC09 prompt",
    )
    return parser.parse_args()


def run(cmd, **kwargs):
    return subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        **kwargs,
    )


def normalize_output(text):
    lines = []
    for raw_line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = raw_line.rstrip()
        if line:
            lines.append(line)
    return "\n".join(lines) + "\n"


def run_checked(cmd, description):
    result = run(cmd)
    if result.returncode != 0:
        sys.stderr.write(f"{description} failed.\n")
        sys.stderr.write("Command: " + " ".join(str(part) for part in cmd) + "\n")
        sys.stderr.write(result.stdout)
        return None
    return result.stdout


def run_basic09c(args, work_dir):
    ir = work_dir / "basic09c.ll"
    wrapper = work_dir / "main.c"
    executable = work_dir / "basic09c-host"

    emitted_ir = run_checked(
        [args.basic09c, "--emit-llvm", args.source],
        "basic09c IR emission",
    )
    if emitted_ir is None:
        return None
    # Newer LLVM prints some exactly-represented floating constants as f0x...
    # in textual IR. Apple clang on the test host still expects 0x...
    # when compiling .ll files, so normalize the spelling before handoff.
    emitted_ir = re.sub(r"(?<![A-Za-z0-9_.])f0x", "0x", emitted_ir)
    ir.write_text(emitted_ir, encoding="utf-8")

    compile_cmd = [args.cc, str(ir), "-o", str(executable)]
    if "define i32 @main(" not in emitted_ir:
        wrapper.write_text(
            f"int {args.source_name}(void);\n"
            "int main(void) {\n"
            f"  return {args.source_name}();\n"
            "}\n",
            encoding="utf-8",
        )
        compile_cmd.insert(2, str(wrapper))

    compile_result = run_checked(compile_cmd, "host compilation of basic09c IR")
    if compile_result is None:
        return None

    host = run([str(executable)])
    if host.returncode != 0:
        sys.stderr.write("compiled basic09c program failed.\n")
        sys.stderr.write(host.stdout)
        return None
    return normalize_output(host.stdout)


def copy_text_to_disk(os9, host_path, disk_path):
    result = run([os9, "copy", "-l", str(host_path), disk_path])
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        return False
    return True


def ensure_dir_on_disk(os9, disk_path):
    result = run([os9, "dir", disk_path])
    if result.returncode == 0:
        return True
    result = run([os9, "makdir", disk_path])
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        return False
    return True


def replace_on_disk(os9, host_path, disk_path):
    run([os9, "attr", disk_path, "-w"])
    run([os9, "del", disk_path])
    return copy_text_to_disk(os9, host_path, disk_path)


def main():
    args = parse_args()
    work_dir = Path(args.work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)

    disk = work_dir / "basic09.dsk"
    startup = work_dir / "startup"
    runtests = work_dir / "runtests"
    script = work_dir / "autoboot.lua"
    mame_stdout = work_dir / "mame.stdout"
    shutil.copyfile(args.disk, disk)

    source_disk_path = f"{disk},/BASIC09/{args.source_name}"
    output_disk_path = f"{disk},/{args.output_name}"
    output_host_path = work_dir / args.output_name

    if not ensure_dir_on_disk(args.os9, f"{disk},/BASIC09"):
        return 1
    if not copy_text_to_disk(args.os9, args.source, source_disk_path):
        return 1

    startup.write_text(
        "link shell\n"
        "load utilpak1\n"
        "date -t\n"
        "runtests\n",
        encoding="utf-8",
    )
    if not replace_on_disk(args.os9, startup, f"{disk},/startup"):
        return 1

    # Keep the BASIC09 invocation in a root-level OS-9 procedure so startup
    # only boots support modules and delegates the actual test command.
    runtests.write_text(
        "chd basic09\n"
        f"basic09 {args.source_name} #32k </1 >-../{args.output_name}\n",
        encoding="utf-8",
    )
    if not replace_on_disk(args.os9, runtests, f"{disk},/runtests"):
        return 1

    rompath = os.environ.get("MAME_ROM_PATH") or os.environ.get("MAME_ROMPATH")
    video = os.environ.get("MAME_VIDEO", "none")

    script.write_text(
        f"""
manager.machine.natkeyboard.in_use = true
emu.wait(4)
manager.machine.natkeyboard:post_coded("DOS{{ENTER}}")
emu.wait({args.basic09_shutdown_delay})
manager.machine.natkeyboard:post_coded("bye{{ENTER}}")
emu.wait(5)
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

    with mame_stdout.open("w", encoding="utf-8") as output:
        result = subprocess.run(cmd, stdout=output, stderr=subprocess.STDOUT, text=True)

    if result.returncode != 0:
        sys.stderr.write(mame_stdout.read_text(encoding="utf-8", errors="replace"))
        return result.returncode

    coco_copy = run([args.os9, "copy", "-l", output_disk_path, str(output_host_path)])
    if coco_copy.returncode != 0:
        sys.stderr.write("CoCo BASIC09 did not produce the expected output file.\n")
        sys.stderr.write(f"Startup script:\n{startup.read_text(encoding='utf-8')}\n")
        sys.stderr.write(f"Output path: {output_disk_path}\n")
        sys.stderr.write(coco_copy.stdout)
        sys.stderr.write("\nMAME output:\n")
        sys.stderr.write(mame_stdout.read_text(encoding="utf-8", errors="replace"))
        return 1

    coco = run([args.os9, "list", output_disk_path])
    if coco.returncode != 0:
        sys.stderr.write("CoCo BASIC09 did not produce the expected output file.\n")
        sys.stderr.write(f"Startup script:\n{startup.read_text(encoding='utf-8')}\n")
        sys.stderr.write(f"Output path: {output_disk_path}\n")
        sys.stderr.write(coco.stdout)
        sys.stderr.write("\nMAME output:\n")
        sys.stderr.write(mame_stdout.read_text(encoding="utf-8", errors="replace"))
        return 1

    coco_output = normalize_output(coco.stdout)
    basic09c_output = run_basic09c(args, work_dir)
    if basic09c_output is None:
        return 1

    if coco_output != basic09c_output:
        sys.stderr.write("basic09c output differs from CoCo BASIC09 output.\n")
        sys.stderr.write("\nCoCo BASIC09 output:\n")
        sys.stderr.write(coco_output)
        sys.stderr.write("\nbasic09c output:\n")
        sys.stderr.write(basic09c_output)
        return 1

    sys.stdout.write(coco_output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
