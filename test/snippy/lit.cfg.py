import os
import lit.formats
import lit.util

from pathlib import Path as _Path

config.name = "bleach snippy-based random tests"
config.test_format = lit.formats.ShTest(0)
config.excludes = ["inputs", "CMakeLists.txt"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = config.obj_root

substitutions = [
    ("%bin", str((_Path(config.obj_root) / "bin").resolve())),
    ("%config-gen-path", config.src_root + "/tools/config-gen"),
]
config.substitutions.extend(substitutions)
