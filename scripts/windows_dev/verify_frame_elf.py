#!/usr/bin/env python3
"""Check an ARM64 ELF's direct dependencies and GNU ABI versions in a Frame sysroot."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


VERSION_PROVIDER = {
    "GLIBC_": "libc.so.6",
    "GLIBCXX_": "libstdc++.so.6",
    "CXXABI_": "libstdc++.so.6",
    "GCC_": "libgcc_s.so.1",
}
SEARCH_DIRS = (
    "lib/aarch64-linux-gnu",
    "usr/lib/aarch64-linux-gnu",
    "lib",
    "usr/lib",
)


class AbiError(RuntimeError):
    pass


def run_readelf(*args: str) -> str:
    readelf = shutil.which("readelf")
    if not readelf:
        raise AbiError("readelf is missing; install binutils")
    result = subprocess.run(
        [readelf, *args], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if result.returncode:
        raise AbiError(result.stderr.strip() or f"readelf failed: {args}")
    return result.stdout


def find_sysroot_file(sysroot: Path, name: str) -> Path:
    for directory in SEARCH_DIRS:
        candidate = sysroot / directory / name
        if candidate.exists() or candidate.is_symlink():
            return candidate
    raise AbiError(f"{name} is missing from the Frame sysroot")


def section(output: str, start: str, stop: str) -> str:
    start_index = output.find(start)
    if start_index < 0:
        return ""
    end_index = output.find(stop, start_index + len(start))
    return output[start_index:] if end_index < 0 else output[start_index:end_index]


def version_names(output: str, section_header: str, next_header: str) -> set[str]:
    block = section(output, section_header, next_header)
    return set(re.findall(r"\bName: ([A-Za-z0-9_.+-]+)", block))


def direct_dependencies(output: str) -> set[str]:
    return set(re.findall(r"Shared library: \[([^\]]+)\]", output))


def verify_elf(elf_path: Path, sysroot_path: Path) -> list[str]:
    elf = elf_path.resolve(strict=True)
    sysroot = sysroot_path.resolve(strict=True)
    if not sysroot.is_dir():
        raise AbiError(f"not a sysroot directory: {sysroot}")

    header = run_readelf("-h", str(elf))
    machine = re.search(r"^\s*Machine:\s*(.+?)\s*$", header, re.MULTILINE)
    if not machine or machine.group(1) != "AArch64":
        actual = machine.group(1) if machine else "unknown"
        raise AbiError(f"expected AArch64 ELF, found {actual}: {elf}")

    program_headers = run_readelf("-lW", str(elf))
    interpreter = re.search(
        r"Requesting program interpreter:\s*([^\]\s]+)", program_headers
    )
    if interpreter:
        interpreter_path = interpreter.group(1).lstrip("/")
        candidate = sysroot / interpreter_path
        if not candidate.exists() and not candidate.is_symlink():
            raise AbiError(f"target interpreter is missing: /{interpreter_path}")

    dynamic = run_readelf("-d", str(elf))
    dependencies = sorted(direct_dependencies(dynamic))
    if not dependencies:
        raise AbiError(f"no DT_NEEDED entries found in {elf}")
    for dependency in dependencies:
        find_sysroot_file(sysroot, dependency)

    version_info = run_readelf("-V", str(elf))
    needs = version_names(
        version_info,
        "Version needs section",
        "Version definition section",
    )
    checked: list[str] = []
    for prefix, library_name in VERSION_PROVIDER.items():
        required = sorted(
            (name for name in needs if name.startswith(prefix)),
            key=lambda value: tuple(int(part) for part in re.findall(r"\d+", value)),
        )
        if not required:
            continue
        library = find_sysroot_file(sysroot, library_name)
        definitions = version_names(
            run_readelf("-V", str(library)),
            "Version definition section",
            "Version needs section",
        )
        missing = sorted(set(required) - definitions)
        if missing:
            raise AbiError(
                f"{library_name} in the Frame sysroot does not provide: "
                + ", ".join(missing)
            )
        checked.append(f"{prefix[:-1]} through {required[-1]} from {library_name}")

    return [
        f"AArch64: {elf}",
        "DT_NEEDED found in Frame sysroot: " + ", ".join(dependencies),
        "Version requirements accepted by Frame sysroot: "
        + (", ".join(checked) if checked else "none"),
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path, help="target ELF built for Steam Frame")
    parser.add_argument(
        "--sysroot",
        type=Path,
        default=Path("/home/astro/MGSOneFrameDev/sysroot/frame"),
        help="relocated Frame root (default: %(default)s)",
    )
    args = parser.parse_args()
    try:
        lines = verify_elf(args.elf, args.sysroot)
    except (OSError, AbiError) as exc:
        print(f"Frame ABI check failed: {exc}", file=sys.stderr)
        return 1
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
