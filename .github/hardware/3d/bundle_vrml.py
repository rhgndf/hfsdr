#!/usr/bin/env python3
"""Bundle KiCad VRML Inline references into one browser-viewable WRL file."""

import argparse
import re
import shutil
from pathlib import Path
from typing import Dict, List, Optional, Tuple


BACKGROUND = "Background {\n  skyColor [ 0.933 0.949 0.969 ]\n}\n"
STRIP_TOP_LEVEL_NODES = {"WorldInfo", "NavigationInfo", "Background", "Viewpoint"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="KiCad-exported board WRL")
    parser.add_argument("output", type=Path, help="Bundled output WRL")
    parser.add_argument(
        "--copy-shapes",
        type=Path,
        help="Optional destination for the source shapes3D directory",
    )
    return parser.parse_args()


def find_matching_brace(text: str, open_index: int) -> int:
    depth = 0
    index = open_index
    in_string = False
    escaped = False
    while index < len(text):
        char = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
        else:
            if char == '"':
                in_string = True
            elif char == "#":
                newline = text.find("\n", index)
                if newline == -1:
                    return len(text) - 1
                index = newline
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return index
        index += 1
    raise ValueError("unclosed brace in VRML file")


def iter_inline_blocks(text: str):
    for match in re.finditer(r"\bInline\s*\{", text):
        open_index = text.find("{", match.start())
        close_index = find_matching_brace(text, open_index)
        yield match.start(), close_index + 1, text[match.start() : close_index + 1]


def extract_url(inline_block: str) -> Optional[str]:
    match = re.search(r"\burl\s+(?:\[\s*)?\"([^\"]+)\"", inline_block)
    if not match:
        return None
    return match.group(1)


def strip_header(text: str) -> str:
    text = re.sub(r"^\s*#VRML\s+V2\.0\s+utf8\s*", "", text, count=1)
    return text.lstrip()


def strip_top_level_nodes(text: str) -> str:
    output: List[str] = []
    index = 0
    node_pattern = re.compile(r"\b(" + "|".join(sorted(STRIP_TOP_LEVEL_NODES)) + r")\s*\{")
    while index < len(text):
        match = node_pattern.search(text, index)
        if not match:
            output.append(text[index:])
            break

        output.append(text[index : match.start()])
        open_index = text.find("{", match.start())
        close_index = find_matching_brace(text, open_index)
        index = close_index + 1
    return "".join(output).strip()


def normalize_url(url: str) -> str:
    return url.replace("\\", "/")


def resolve_inline_path(base_dir: Path, url: str) -> Path:
    normalized = normalize_url(url)
    if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", normalized):
        raise ValueError(f"remote VRML Inline URLs are not supported: {url}")
    return (base_dir / normalized).resolve()


def bundle_body(path: Path, stack: Tuple[Path, ...], cache: Dict[Path, str]) -> str:
    path = path.resolve()
    if path in stack:
        chain = " -> ".join(str(item) for item in (*stack, path))
        raise ValueError(f"recursive VRML Inline loop: {chain}")
    if path in cache:
        return f"USE {cache[path]}"
    if not path.exists():
        raise FileNotFoundError(f"Inline VRML file not found: {path}")

    source = path.read_text(encoding="utf-8", errors="replace")
    body = strip_top_level_nodes(strip_header(source))
    body = bundle_inlines(body, path.parent, (*stack, path), cache)

    def_name = f"BUNDLED_{len(cache) + 1}"
    cache[path] = def_name
    return f"DEF {def_name} Group {{\n  children [\n{indent(body, 4)}\n  ]\n}}"


def bundle_inlines(text: str, base_dir: Path, stack: Tuple[Path, ...], cache: Dict[Path, str]) -> str:
    pieces: List[str] = []
    cursor = 0
    for start, end, block in iter_inline_blocks(text):
        pieces.append(text[cursor:start])
        url = extract_url(block)
        if not url:
            raise ValueError(f"Inline block is missing a URL: {block[:120]}")
        pieces.append(bundle_body(resolve_inline_path(base_dir, url), stack, cache))
        cursor = end
    pieces.append(text[cursor:])
    return "".join(pieces)


def indent(text: str, spaces: int) -> str:
    prefix = " " * spaces
    return "\n".join(prefix + line if line.strip() else line for line in text.splitlines())


def add_background(text: str) -> str:
    body = strip_header(text)
    body = re.sub(r"\bBackground\s*\{[^{}]*\}\s*", "", body, count=1)
    return "#VRML V2.0 utf8\n" + BACKGROUND + body.lstrip()


def copy_shapes_dir(input_path: Path, destination: Optional[Path]) -> None:
    if destination is None:
        return
    source = input_path.parent / "shapes3D"
    if not source.is_dir():
        return
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source, destination)


def main() -> int:
    args = parse_args()
    input_path = args.input.resolve()
    output_path = args.output.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    copy_shapes_dir(input_path, args.copy_shapes)

    source = input_path.read_text(encoding="utf-8", errors="replace")
    bundled = bundle_inlines(strip_header(source), input_path.parent, (input_path,), {})
    with output_path.open("w", encoding="utf-8", newline="\n") as output_file:
        output_file.write(add_background(bundled))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
