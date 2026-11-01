"""What scripts/test/release/windows.sh reads in the Windows packages.

    windows_files.py pe <file> [file-version] [product-version]
        The file is a 64-bit x86 PE (PE32+, AMD64); with a version given, its
        version resource's FileVersion (and ProductVersion) are those.
    windows_files.py msix <package> <version>
        The package is an MSIX: a ZIP holding its manifest, block map, content
        types and both binaries, its Identity at <version>.

Prints what it found; exits 1 on the first difference. Needs pefile
(apt: python3-pefile).
"""
import re
import sys
import zipfile


def pe(path, file_version=None, product_version=None):
    import pefile
    p = pefile.PE(path, fast_load=True)
    p.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    if p.OPTIONAL_HEADER.Magic != 0x20B or p.FILE_HEADER.Machine != pefile.MACHINE_TYPE["IMAGE_FILE_MACHINE_AMD64"]:
        sys.exit(f"{path} is not a 64-bit x86 PE (PE32+, AMD64)")
    strings = {}
    for info in getattr(p, "FileInfo", None) or []:
        for entry in info:
            for table in getattr(entry, "StringTable", []):
                strings.update({k.decode(): v.decode() for k, v in table.entries.items()})
    print(f"{path}: PE32+ AMD64, FileVersion {strings.get('FileVersion')!r}, ProductVersion {strings.get('ProductVersion')!r}")
    if file_version and strings.get("FileVersion") != file_version:
        sys.exit(f"{path} reports FileVersion {strings.get('FileVersion')!r}, expected {file_version!r}")
    if product_version and strings.get("ProductVersion") != product_version:
        sys.exit(f"{path} reports ProductVersion {strings.get('ProductVersion')!r}, expected {product_version!r}")


def msix(path, version):
    names = set(zipfile.ZipFile(path).namelist())
    want = {"AppxManifest.xml", "AppxBlockMap.xml", "[Content_Types].xml", "bin/openchime.exe", "bin/openchime-tui.exe"}
    missing = want - names
    if missing:
        sys.exit(f"{path} lacks {sorted(missing)}")
    manifest = zipfile.ZipFile(path).read("AppxManifest.xml").decode("utf-8", "replace")
    identity = re.search(r"<Identity\b[^>]*>", manifest, re.S)
    got = re.search(r'\bVersion="([^"]+)"', identity.group(0)).group(1) if identity else None
    print(f"{path}: {len(names)} entries, Identity Version {got!r}")
    if got != version:
        sys.exit(f"{path}'s Identity Version is {got!r}, expected {version!r}")


if __name__ == "__main__":
    kind, *args = sys.argv[1:]
    {"pe": pe, "msix": msix}[kind](*args)
