import os
import shlex
import shutil
import subprocess


config.suffixes = [".test"]

_mame_rompath = os.environ.get("MAME_ROM_PATH") or os.environ.get("MAME_ROMPATH")
if _mame_rompath:
    config.environment["MAME_ROM_PATH"] = _mame_rompath


def _find_mame():
    mame = os.environ.get("MAME")
    if mame:
        return mame
    return shutil.which("mame")


def _mame_can_launch_coco3(mame):
    cmd = [
        mame,
        "coco3",
        "-skip_gameinfo",
        "-noautosave",
        "-nothrottle",
        "-sound",
        "none",
        "-video",
        "none",
        "-seconds_to_run",
        "1",
    ]
    if _mame_rompath:
        cmd.extend(["-rompath", _mame_rompath])

    try:
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return False

    return result.returncode == 0


_mame = _find_mame()
if _mame:
    config.substitutions.append(("%mame", shlex.quote(_mame)))
    if _mame_can_launch_coco3(_mame):
        config.available_features.add("mame-coco3")

_os9 = shutil.which("os9")
if _os9:
    config.substitutions.append(("%os9", shlex.quote(_os9)))
    config.available_features.add("toolshed-os9")
