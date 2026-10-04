#!/usr/bin/env python3
"""Rebuild a GP4 directory tree from its file targets before packaging."""

import argparse
import os
from pathlib import Path
import stat
import tempfile
import xml.etree.ElementTree as ET


def target_parts(target):
    """Require a canonical, relative PS4 target path."""
    if not target or target.startswith("/") or "\\" in target:
        raise ValueError(f"Unsafe GP4 target path: {target!r}")
    parts = target.split("/")
    if (
        any(part in ("", ".", "..") for part in parts)
        or ":" in parts[0]
        or any(ord(char) < 32 or ord(char) == 127 for char in target)
    ):
        raise ValueError(f"Unsafe GP4 target path: {target!r}")
    return parts


def normalize(path):
    path = Path(path)
    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))
    tree = ET.parse(path, parser=parser)
    project = tree.getroot()
    namespace = project.tag.partition("}")[0] + "}" if project.tag.startswith("{") else ""
    tag = lambda name: namespace + name
    if project.tag != tag("psproject"):
        raise ValueError("Expected a GP4 psproject root")
    file_lists = project.findall(tag("files"))
    if len(file_lists) != 1:
        raise ValueError("Expected exactly one GP4 files element")
    file_elements = file_lists[0].findall(tag("file"))
    if not file_elements:
        raise ValueError("GP4 contains no file targets")

    directories = {}
    targets = set()
    parents = set()
    for file_element in file_elements:
        target = file_element.get("targ_path")
        parts = target_parts(target)
        if target in targets:
            raise ValueError(f"Duplicate GP4 file target: {target!r}")
        targets.add(target)
        current = directories
        for depth, part in enumerate(parts[:-1], 1):
            current = current.setdefault(part, {})
            parents.add("/".join(parts[:depth]))
    if targets & parents:
        raise ValueError("A GP4 target is both a file and a directory")

    rootdir = ET.Element(tag("rootdir"))

    def append_directories(parent, children):
        for name in sorted(children):
            child = ET.SubElement(parent, tag("dir"), {"targ_name": name})
            append_directories(child, children[name])

    append_directories(rootdir, directories)
    old_rootdirs = project.findall(tag("rootdir"))
    if len(old_rootdirs) > 1:
        raise ValueError("GP4 contains multiple rootdir elements")
    if old_rootdirs:
        old = old_rootdirs[0]
        index = list(project).index(old)
        rootdir.tail = old.tail
        project.remove(old)
        project.insert(index, rootdir)
    else:
        project.append(rootdir)
    ET.indent(rootdir, space="\t", level=1)

    # Keep the input intact if validation or serialization fails.
    mode = stat.S_IMODE(path.stat().st_mode)
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as output:
            temporary_path = Path(output.name)
            tree.write(output, encoding="utf-8", xml_declaration=True)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary_path, mode)
        os.replace(temporary_path, path)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
    return len(parents)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gp4", type=Path, help="GP4 file to normalize in place")
    args = parser.parse_args()
    try:
        count = normalize(args.gp4)
    except (OSError, ValueError, ET.ParseError) as error:
        parser.exit(1, f"GP4 normalization failed: {error}\n")
    print(f"Normalized {args.gp4}: {count} directories")


if __name__ == "__main__":
    main()
