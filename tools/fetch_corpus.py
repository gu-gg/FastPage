#!/usr/bin/env python3
"""Download the official Silesia corpus, or validate an existing local copy.

MD5 values are the official corpus integrity checks, not authentication tokens.
Importing this module never downloads or creates files.
"""
import argparse
import hashlib
from pathlib import Path
import tempfile
import urllib.request
import zipfile

URL = "https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip"
EXPECTED = {
    "dickens": "88334708559f6db57d79096bc0aca07e",
    "mozilla": "c7789a2097f1ff944b0c737430a339b3",
    "mr": "38e623e3093b7bf2003ca4b1bbc19927",
    "nci": "31f85bc8706f3c921104e7c169e2e2e1",
    "ooffice": "573c4ae915e36631d8f2dcffb9b9b66d",
    "osdb": "e734b0c48e6a982adfb5802da3032ecd",
    "reymont": "d8f54d78105079775f32d76dc55fc671",
    "samba": "154eaea7ea70e89f6339ff0abf4112ca",
    "sao": "79e95a22e18cd82b7e42bf91b380d30b",
    "webster": "474931ad907ac27bf962c75ded46c069",
    "xml": "9b09c0c80104adb8aae910b7d7db003e",
    "x-ray": "9baec32ad14ec3eff487d254382cb91c",
}
SIZES = dict(zip(EXPECTED, (
    10192446, 51220480, 9970564, 33553445, 6152192, 10085684,
    6627202, 21606400, 7251944, 41458703, 5345280, 8474240,
)))


def verify_file(path, name):
    """Return a checksum record, or raise on missing/wrong-sized/wrong content."""
    path = Path(path)
    if not path.is_file():
        raise ValueError("Missing corpus file: {}".format(path))
    size = path.stat().st_size
    if size != SIZES[name]:
        raise ValueError("Size mismatch for {}: {} != {}".format(name, size, SIZES[name]))
    digest = hashlib.md5(usedforsecurity=False)
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    actual = digest.hexdigest()
    if actual != EXPECTED[name]:
        raise ValueError("MD5 mismatch for {}: {} != {}".format(name, actual, EXPECTED[name]))
    return {"name": name, "bytes": size, "md5": actual}


def verify_corpus(directory):
    """Validate all 12 official files in a directory, in the fixed corpus order."""
    directory = Path(directory)
    return [verify_file(directory / name, name) for name in EXPECTED]


def fetch_corpus(directory):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    missing = []
    for name in EXPECTED:
        try:
            verify_file(directory / name, name)
        except (OSError, ValueError):
            missing.append(name)
    if not missing:
        return verify_corpus(directory)
    # Temporary archive and extracted files are removed on any failure. Only
    # explicitly named, verified corpus files are moved into the destination.
    with tempfile.TemporaryDirectory(prefix=".silesia-", dir=directory) as temporary:
        temporary = Path(temporary)
        archive = temporary / "download.zip"
        with urllib.request.urlopen(URL, timeout=60) as response, archive.open("wb") as stream:
            for chunk in iter(lambda: response.read(1024 * 1024), b""):
                stream.write(chunk)
        with zipfile.ZipFile(archive) as package:
            entries = {}
            for entry in package.infolist():
                name = Path(entry.filename).name
                if entry.is_dir() or name not in EXPECTED:
                    continue
                if name in entries:
                    raise ValueError("Duplicate archive member: {}".format(name))
                entries[name] = entry
            for name in missing:
                if name not in entries or entries[name].file_size != SIZES[name]:
                    raise ValueError("Missing or wrong-sized archive member: {}".format(name))
                target = temporary / name
                with package.open(entries[name]) as source, target.open("wb") as output:
                    for chunk in iter(lambda: source.read(1024 * 1024), b""):
                        output.write(chunk)
                verify_file(target, name)
                target.replace(directory / name)
    return verify_corpus(directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=Path("corpus"))
    parser.add_argument("--verify-only", action="store_true", help="check local files without downloading")
    args = parser.parse_args()
    try:
        records = verify_corpus(args.directory) if args.verify_only else fetch_corpus(args.directory)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, "Corpus check failed: {}\n".format(error))
    print("Verified {} official Silesia files ({} bytes).".format(
        len(records), sum(record["bytes"] for record in records)))


if __name__ == "__main__":
    main()
