# -*- Python -*-

import os

import lit.formats

config.name = "BASIC09C"
config.test_format = lit.formats.ShTest()
config.suffixes = [".test"]
config.excludes = ["Inputs", "CMakeLists.txt"]

config.test_source_root = os.path.join(config.basic09c_src_root, "test")
config.test_exec_root = os.path.join(config.basic09c_obj_root, "test")

path_entries = [
    os.path.join(config.basic09c_obj_root, "bin"),
    config.llvm_tools_dir,
]
path_entries = [entry for entry in path_entries if entry]
if path_entries:
    current_path = config.environment.get("PATH", "")
    config.environment["PATH"] = os.pathsep.join(path_entries + [current_path])

config.substitutions.append(("%python", config.python_executable))
