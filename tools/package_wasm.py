#!/usr/bin/env python3
"""Package the browser build and its README as a distributable ZIP."""

import argparse
import hashlib
from pathlib import Path
import zipfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=root / "build" / "wasm")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or args.build_dir / "sf-wasm.zip"
    files = {
        "index.html": args.build_dir / "sf_wasm_shell.html",
        "sf_wasm_shell.js": args.build_dir / "sf_wasm_shell.js",
        "sf_wasm_shell.wasm": args.build_dir / "sf_wasm_shell.wasm",
        "sf_wasm_shell.data": args.build_dir / "sf_wasm_shell.data",
        "README.md": root / "apps" / "sf_wasm_shell" / "README.md",
        "FFMPEG-NOTICE.txt": root / "apps" / "sf_wasm_shell" / "FFMPEG-NOTICE.txt",
    }
    for source in files.values():
        if not source.is_file():
            parser.error(f"Missing file: {source}; build wasm-shell first")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        for name, source in files.items():
            bundle.write(source, name)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    checksum = output.with_suffix(output.suffix + ".sha256")
    checksum.write_text(f"{digest}  {output.name}\n", encoding="utf-8")
    print(output)
    print(checksum)


if __name__ == "__main__":
    main()
