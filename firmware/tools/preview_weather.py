#!/usr/bin/env python3
"""Build the host-side firmware renderer and export its framebuffer as PNG."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
HOST_PROJECT = ROOT / "firmware" / "tools" / "weather_renderer_host"
BUILD_DIR = ROOT / "firmware" / "build-s3" / "weather-preview"
DEFAULT_OUTPUT = ROOT / "firmware" / "build-s3" / "preview" / "weather.png"


def find_tool(name: str, patterns: list[tuple[Path, str]]) -> str:
    found = shutil.which(name)
    if found:
        return found
    for base, pattern in patterns:
        matches = sorted(base.glob(pattern))
        for match in matches:
            if match.is_file():
                return str(match)
    raise RuntimeError(f"找不到 {name}；请安装 CMake/Ninja 或配置 ESP-IDF 工具链。")


def compiler() -> Path:
    configured = os.environ.get("LLVM_MINGW_ROOT")
    candidates = [Path(configured) / "bin" / "clang++.exe"] if configured else []
    local = os.environ.get("LOCALAPPDATA")
    if local:
        candidates += sorted((Path(local) / "codex-tools").glob("llvm-mingw-*/*/bin/clang++.exe"), reverse=True)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError("找不到 LLVM-MinGW clang++。请设置 LLVM_MINGW_ROOT 指向其安装目录。")


def ppm_to_png(source: Path, target: Path) -> None:
    data = source.read_bytes()
    header, pixels = data.split(b"\n255\n", 1)
    fields = header.split()
    if fields[0] != b"P6":
        raise RuntimeError("Renderer 输出不是 P6 PPM。")
    width, height = int(fields[1]), int(fields[2])
    if (width, height) != (400, 300) or len(pixels) != width * height * 3:
        raise RuntimeError(f"意外的 framebuffer 尺寸或数据长度：{width}x{height}")

    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(png)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", "-o", type=Path, default=DEFAULT_OUTPUT, help="PNG 输出路径")
    args = parser.parse_args()
    try:
        tools = Path(r"C:\Espressif\tools")
        cmake = find_tool("cmake", [(tools, "cmake/*/bin/cmake.exe")])
        ninja = find_tool("ninja", [(tools, "ninja/*/ninja.exe")])
        clang = compiler()
        env = os.environ.copy()
        env["PATH"] = str(clang.parent) + os.pathsep + str(Path(ninja).parent) + os.pathsep + env.get("PATH", "")
        configure = [cmake, "-S", str(HOST_PROJECT), "-B", str(BUILD_DIR), "-G", "Ninja",
                     f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_C_COMPILER={clang.parent / 'clang.exe'}",
                     f"-DCMAKE_CXX_COMPILER={clang}"]
        subprocess.run(configure, check=True, env=env)
        subprocess.run([cmake, "--build", str(BUILD_DIR), "--parallel", "8"], check=True, env=env)
        ppm = ROOT / "firmware" / "build-s3" / "preview" / "weather-renderer.ppm"
        ppm.parent.mkdir(parents=True, exist_ok=True)
        exe = BUILD_DIR / "weather_renderer_preview.exe"
        subprocess.run([str(exe), str(ppm)], check=True, env=env)
        output = args.output if args.output.is_absolute() else ROOT / args.output
        ppm_to_png(ppm, output)
        print(f"天气页预览已生成：{output}")
        return 0
    except (RuntimeError, subprocess.CalledProcessError, OSError, ValueError) as exc:
        print(f"预览失败：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
