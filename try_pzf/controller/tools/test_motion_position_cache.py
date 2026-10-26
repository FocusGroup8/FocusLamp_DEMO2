"""Compile and execute the actual motion_position_cache.h C tests, without a board."""

import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="Path to a host C compiler (Clang on Windows)")
    args = parser.parse_args()
    compiler = args.cc or os.environ.get("CC") or shutil.which("clang")
    if not compiler and os.name == "nt":
        bundled = Path("C:/Espressif/tools/esp-clang/esp-19.1.2_20250312/esp-clang/bin/clang.exe")
        if bundled.is_file():
            compiler = str(bundled)
    if not compiler and os.name != "nt":
        compiler = shutil.which("cc") or shutil.which("gcc")
    if not compiler:
        parser.error("No host C compiler found; pass --cc PATH. No C tests were run.")
    compiler = shutil.which(compiler) or compiler

    if os.name == "nt":
        targets = subprocess.run(
            [compiler, "--print-targets"], capture_output=True, text=True, check=True
        ).stdout
        if "x86-64" not in targets and "x86_64" not in targets:
            parser.error(
                "The selected Clang has no x86-64 host backend; pass --cc PATH "
                "to a host Clang. No C tests were run."
            )

    fixture = Path(__file__).with_suffix(".c").resolve()
    with tempfile.TemporaryDirectory(prefix="motion-position-cache-test-") as temporary:
        directory = Path(temporary)
        if os.name == "nt":
            linker = Path(compiler).resolve().with_name("ld.lld.exe")
            if not linker.is_file():
                found = shutil.which("ld.lld")
                if not found:
                    parser.error("Windows tests require Clang and ld.lld; no C tests were run.")
                linker = Path(found)
            output = directory / "motion_position_cache_test.dll"
            obj = directory / "motion_position_cache_test.obj"
            subprocess.run([
                compiler, "--target=x86_64-pc-windows-msvc", "-std=c11", "-O2",
                "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin",
                "-fno-stack-protector", "-mno-stack-arg-probe",
                "-DMOTION_POSITION_TEST_FREESTANDING", "-c", str(fixture), "-o", str(obj),
            ], check=True)
            subprocess.run([
                str(linker), "-flavor", "link", "/dll", "/noentry", "/nodefaultlib",
                "/export:motion_position_cache_test", f"/out:{output}", str(obj),
            ], check=True)
        else:
            output = directory / "motion_position_cache_test.so"
            subprocess.run([
                compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-shared", "-fPIC", str(fixture), "-o", str(output),
            ], check=True)

        library = ctypes.CDLL(str(output))
        test = library.motion_position_cache_test
        test.argtypes = []
        test.restype = ctypes.c_int
        failure_line = test()
        # Release the Windows DLL before TemporaryDirectory removes its files.
        if os.name == "nt":
            import _ctypes
            _ctypes.FreeLibrary(library._handle)
        if failure_line:
            raise SystemExit(f"FAIL: {fixture}:{failure_line}: C assertion failed")
    print("PASS: actual C motion position cache tests "
          "(unknown, zero, failure retention, axis isolation, freshness, recovery, bounds, saturation)")


if __name__ == "__main__":
    main()
