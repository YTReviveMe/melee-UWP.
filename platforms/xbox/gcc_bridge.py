#!/usr/bin/env python3
"""Compile the game C sources with MinGW GCC for Xbox UWP."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def environment(gcc: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PATH"] = str(gcc.parent) + os.pathsep + env.get("PATH", "")
    return env


def probe(gcc: Path) -> int:
    source = 'struct __attribute__((scalar_storage_order("big-endian"))) S { unsigned v; };\n'
    source += 'int read_value(const struct S *s) { return s->v == 0x12345678u; }\n'
    with tempfile.TemporaryDirectory(prefix="melee-gcc-uwp-") as directory:
        path = Path(directory)
        (path / "probe.c").write_text(source)
        result = subprocess.run(
            [str(gcc), "-Werror", "-O2", "-c", str(path / "probe.c"),
             "-o", str(path / "probe.obj")],
            env=environment(gcc), cwd=path, capture_output=True, text=True,
        )
        if result.returncode or not (path / "probe.obj").is_file():
            print(f"GCC probe exit={result.returncode} object={(path / 'probe.obj').is_file()}"
                  f" stdout={result.stdout!r} stderr={result.stderr!r}", file=sys.stderr)
            return result.returncode or 1
    return 0


def compile_game(gcc: Path, command: list[str]) -> int:
    if not command:
        raise ValueError("missing CMake compiler command")
    args = command[1:]
    output = None
    source = None
    converted = ["-std=gnu11", "-fexec-charset=CP932", "-Wno-scalar-storage-order"]
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--":
            i += 1
            continue
        if arg.startswith("/Fo"):
            output = arg[3:]
        elif arg == "-o" and i + 1 < len(args):
            i += 1
            output = args[i]
        elif arg.startswith("/D") and len(arg) > 2:
            converted.append("-D" + arg[2:])
        elif arg.startswith("/I") and len(arg) > 2:
            converted.append("-I" + arg[2:])
        elif arg.startswith("/external:I"):
            converted.extend(["-isystem", arg[len("/external:I"):]])
        elif arg.startswith("-imsvc"):
            converted.extend(["-isystem", arg[len("-imsvc"):]])
        elif arg in ("-I", "-D", "-include", "-isystem") and i + 1 < len(args):
            converted.extend([arg, args[i + 1]])
            i += 1
        elif arg in ("/O2", "/Ox"):
            converted.append("-O2")
        elif arg == "/Od":
            converted.append("-O0")
        elif arg.endswith(".c") and Path(arg).is_file():
            source = arg
        elif arg.startswith("-") and arg not in ("-c", "-MD", "-MDd", "-Zi", "-Z7", "-fcolor-diagnostics",
                                                "-Wno-unknown-warning-option", "-Wno-unknown-attributes"):
            converted.append(arg)
        i += 1
    if not source or not output:
        raise ValueError("expected one C source and an explicit output object")
    output = str(Path(output).resolve())
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(gcc), *converted, "-c", source, "-o", output], env=environment(gcc),
        cwd=Path(__file__).resolve().parents[2], capture_output=True, text=True,
    )
    if result.returncode:
        print(f"GCC bridge failed ({result.returncode}) compiling {source}", file=sys.stderr)
        print(result.stderr or result.stdout or "GCC produced no diagnostics", file=sys.stderr)
    elif result.stderr:
        print(result.stderr, file=sys.stderr)
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gcc", type=Path, required=True)
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.gcc.is_file():
        parser.error(f"GCC does not exist: {args.gcc}")
    if args.probe:
        return probe(args.gcc)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    try:
        return compile_game(args.gcc, command)
    except (OSError, ValueError) as error:
        print(f"melee GCC UWP bridge: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
