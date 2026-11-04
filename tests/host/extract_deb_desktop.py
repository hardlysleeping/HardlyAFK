#!/usr/bin/env python3
"""Extract .desktop files from Debian packages, resolved through a Packages index.

    extract_deb_desktop.py /path/to/Packages /out/dir qterminal konsole ...

Downloads each package's .deb from deb.debian.org, reads usr/share/applications
out of its data archive, and writes those files into /out/dir. Used to give the
launcher tests real desktop files instead of hand-written approximations.
"""

import io
import os
import sys
import tarfile
import urllib.request

MIRROR = "http://deb.debian.org/debian"


def package_filenames(index_path):
    filenames = {}
    with open(index_path, encoding="utf-8", errors="replace") as handle:
        stanza = {}
        for line in handle:
            line = line.rstrip("\n")
            if not line:
                if "Package" in stanza and "Filename" in stanza:
                    filenames[stanza["Package"]] = stanza["Filename"]
                stanza = {}
                continue
            if line.startswith(" ") or ":" not in line:
                continue
            key, _, value = line.partition(": ")
            if key in ("Package", "Filename"):
                stanza[key] = value.strip()
    if "Package" in stanza and "Filename" in stanza:
        filenames[stanza["Package"]] = stanza["Filename"]
    return filenames


def data_member(blob):
    """Yield the data.tar.* member of a .deb (which is an ar archive)."""
    if not blob.startswith(b"!<arch>\n"):
        raise ValueError("not a .deb archive")
    offset = 8
    while offset + 60 <= len(blob):
        header = blob[offset : offset + 60]
        name = header[:16].decode("ascii", "replace").strip()
        try:
            size = int(header[48:58].decode("ascii", "replace").strip())
        except ValueError:
            raise ValueError("malformed ar member header")
        body = blob[offset + 60 : offset + 60 + size]
        if name.startswith("data.tar"):
            return name, body
        offset += 60 + size + (size % 2)
    raise ValueError("no data.tar member")


def extract(deb_path_or_bytes, outdir):
    blob = deb_path_or_bytes
    name, body = data_member(blob)
    suffix = name.rsplit(".", 1)[-1]
    stream = io.BytesIO(body)
    if suffix == "xz":
        archive = tarfile.open(fileobj=stream, mode="r:xz")
    elif suffix == "gz":
        archive = tarfile.open(fileobj=stream, mode="r:gz")
    elif suffix == "bz2":
        archive = tarfile.open(fileobj=stream, mode="r:bz2")
    else:
        archive = tarfile.open(fileobj=stream)
    written = 0
    for member in archive.getmembers():
        if "/share/applications/" not in member.name or not member.name.endswith(".desktop"):
            continue
        payload = archive.extractfile(member)
        if payload is None:
            continue
        target = os.path.join(outdir, os.path.basename(member.name))
        with open(target, "wb") as handle:
            handle.write(payload.read())
        written += 1
    return written


def main(argv):
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    index, outdir, wanted = argv[1], argv[2], argv[3:]
    os.makedirs(outdir, exist_ok=True)
    filenames = package_filenames(index)
    total = 0
    for package in wanted:
        relative = filenames.get(package)
        if not relative:
            print(f"{package}: not in the index", file=sys.stderr)
            return 1
        url = f"{MIRROR}/{relative}"
        with urllib.request.urlopen(url, timeout=60) as response:
            blob = response.read()
        count = extract(blob, outdir)
        print(f"{package}: {len(blob)} bytes, {count} desktop file(s)")
        total += count
    if total == 0:
        print("no desktop files extracted", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
