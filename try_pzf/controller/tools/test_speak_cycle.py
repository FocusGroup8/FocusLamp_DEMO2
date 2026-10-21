"""Compile and run the real speak_cycle.h C fixture; no ESP-IDF or board needed."""

import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def compiler_candidates(explicit):
    if explicit:
        yield shutil.which(explicit) or explicit
        return
    if os.environ.get("CC"):
        yield shutil.which(os.environ["CC"]) or os.environ["CC"]
    for name in ("clang", "gcc", "cc"):
        found = shutil.which(name)
        if found:
            yield found
    if os.name == "nt":
        for candidate in (
            "C:/Program Files/LLVM/bin/clang.exe",
            "C:/msys64/ucrt64/bin/gcc.exe",
            "C:/msys64/mingw64/bin/gcc.exe",
            "C:/Espressif/tools/esp-clang/esp-19.1.2_20250312/esp-clang/bin/clang.exe",
        ):
            if Path(candidate).is_file():
                yield candidate


def compiler_kind(compiler):
    version = subprocess.run(
        [compiler, "--version"], capture_output=True, text=True, check=True
    ).stdout.lower()
    return "clang" if "clang" in version else "gcc"


def select_compiler(parser, args):
    rejected = []
    for compiler in dict.fromkeys(compiler_candidates(args.cc)):
        kind = compiler_kind(compiler)
        if args.compile_only:
            return compiler, kind
        if os.name == "nt":
            if kind == "clang":
                targets = subprocess.run(
                    [compiler, "--print-targets"], capture_output=True, text=True,
                    check=True,
                ).stdout
                if "x86-64" not in targets and "x86_64" not in targets:
                    rejected.append(f"{compiler}: no x86-64 host backend")
                    continue
                linker = Path(compiler).resolve().with_name("ld.lld.exe")
                if not linker.is_file() and not shutil.which("ld.lld"):
                    rejected.append(f"{compiler}: ld.lld not found")
                    continue
            else:
                target = subprocess.run(
                    [compiler, "-dumpmachine"], capture_output=True, text=True,
                    check=True,
                ).stdout.strip()
                if "x86_64" not in target or "mingw" not in target:
                    rejected.append(f"{compiler}: not an x86-64 Windows host compiler")
                    continue
        return compiler, kind
    details = "; ".join(rejected)
    parser.error(
        "No suitable host C compiler found. Pass --cc PATH; use --compile-only "
        "with a cross compiler for syntax validation only. No C tests were run."
        + (f" Checked: {details}" if details else "")
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="C compiler executable")
    parser.add_argument(
        "--compile-only", action="store_true",
        help="Compile fixture only (also supports RISC-V); does not run C tests",
    )
    args = parser.parse_args()
    compiler, kind = select_compiler(parser, args)
    fixture = Path(__file__).with_suffix(".c").resolve()
    common = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]

    with tempfile.TemporaryDirectory(prefix="speak-cycle-test-") as temporary:
        directory = Path(temporary)
        if args.compile_only:
            subprocess.run(
                [compiler, *common, "-c", str(fixture), "-o", str(directory / "test.o")],
                check=True,
            )
            print("PASS: C fixture compiled. Runtime C assertions were NOT executed.")
            return

        if os.name == "nt" and kind == "clang":
            linker = Path(compiler).resolve().with_name("ld.lld.exe")
            if not linker.is_file():
                linker = Path(shutil.which("ld.lld"))
            output = directory / "speak_cycle_test.dll"
            obj = directory / "speak_cycle_test.obj"
            subprocess.run([
                compiler, "--target=x86_64-pc-windows-msvc", *common,
                "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
                "-mno-stack-arg-probe", "-DSPEAK_TEST_FREESTANDING",
                "-c", str(fixture), "-o", str(obj),
            ], check=True)
            subprocess.run([
                str(linker), "-flavor", "link", "/dll", "/noentry", "/nodefaultlib",
                "/export:speak_cycle_test", f"/out:{output}", str(obj),
            ], check=True)
        else:
            output = directory / ("speak_cycle_test.dll" if os.name == "nt" else "speak_cycle_test.so")
            subprocess.run([
                compiler, *common, "-shared", *([] if os.name == "nt" else ["-fPIC"]),
                str(fixture), "-o", str(output),
            ], check=True)

        library = ctypes.CDLL(str(output))
        try:
            test = library.speak_cycle_test
            test.argtypes = []
            test.restype = ctypes.c_int
            failure_line = test()
        finally:
            # Unlock the DLL before TemporaryDirectory removes it on Windows.
            if os.name == "nt":
                import _ctypes
                _ctypes.FreeLibrary(library._handle)
        if failure_line:
            raise SystemExit(f"FAIL: {fixture}:{failure_line}: C assertion failed")
    print(
        "PASS: actual C speak cycle tests (rapid START, generation isolation, "
        "first-response timeout, no success before START, first outcome, recovery)"
    )


if __name__ == "__main__":
    main()
