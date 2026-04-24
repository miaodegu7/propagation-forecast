#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


SYSTEM_DLLS = {
    "advapi32.dll",
    "authz.dll",
    "bcrypt.dll",
    "bcryptprimitives.dll",
    "cfgmgr32.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "cryptui.dll",
    "dhcpcsvc.dll",
    "dnsapi.dll",
    "d3d11.dll",
    "d3d12.dll",
    "gdi32.dll",
    "gdiplus.dll",
    "dwmapi.dll",
    "dwrite.dll",
    "dxgi.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "mpr.dll",
    "msvcrt.dll",
    "ntdll.dll",
    "ncrypt.dll",
    "netapi32.dll",
    "normaliz.dll",
    "ole32.dll",
    "oleaut32.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "setupapi.dll",
    "shell32.dll",
    "shlwapi.dll",
    "usp10.dll",
    "uxtheme.dll",
    "version.dll",
    "winhttp.dll",
    "ucrtbase.dll",
    "user32.dll",
    "userenv.dll",
    "winmm.dll",
    "wldap32.dll",
    "wsock32.dll",
    "ws2_32.dll",
}


def is_system_dll(name: str) -> bool:
    key = name.lower()
    return key in SYSTEM_DLLS or key.startswith("api-ms-win-") or key.startswith("ext-ms-win-")


def find_objdump(search_dir: Path) -> str | None:
    candidates = [
        shutil.which("objdump"),
        str(search_dir / "objdump.exe"),
        str(search_dir.parent / "x86_64-w64-mingw32" / "bin" / "objdump.exe"),
        str(search_dir.parent.parent / "usr" / "bin" / "objdump.exe"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


def list_dlls(binary: Path, objdump: str) -> list[str]:
    result = subprocess.run(
        [objdump, "-p", str(binary)],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )
    dlls: list[str] = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("DLL Name:"):
            dlls.append(line.split(":", 1)[1].strip())
    return dlls


def copy_tree_dependencies(root_binary: Path, out_dir: Path, search_dir: Path, objdump: str) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root_binary, out_dir / root_binary.name)

    available = {item.name.lower(): item for item in search_dir.glob("*.dll")}
    queue = [out_dir / root_binary.name]
    visited = set()
    copied = set()
    missing = 0

    while queue:
        binary = queue.pop()
        if binary.name.lower() in visited:
            continue
        visited.add(binary.name.lower())

        for dll_name in list_dlls(binary, objdump):
            key = dll_name.lower()
            if is_system_dll(key) or key in copied:
                continue
            source = available.get(key)
            if source is None:
                print(f"warning: dependency not found in {search_dir}: {dll_name}", file=sys.stderr)
                missing += 1
                continue
            destination = out_dir / source.name
            shutil.copy2(source, destination)
            copied.add(key)
            queue.append(destination)

    return missing


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: package_windows.py <exe> <out_dir> <dll_search_dir>", file=sys.stderr)
        return 2

    root_binary = Path(sys.argv[1]).resolve()
    out_dir = Path(sys.argv[2]).resolve()
    search_dir = Path(sys.argv[3]).resolve()

    if not root_binary.is_file():
        print(f"missing binary: {root_binary}", file=sys.stderr)
        return 2
    if not search_dir.is_dir():
        print(f"missing dll directory: {search_dir}", file=sys.stderr)
        return 2

    objdump = find_objdump(search_dir)
    if objdump is None:
        print("missing objdump: install MSYS2 binutils or add objdump.exe to PATH", file=sys.stderr)
        return 2

    missing = copy_tree_dependencies(root_binary, out_dir, search_dir, objdump)
    if missing:
        print(f"packaged with {missing} unresolved dependencies", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
