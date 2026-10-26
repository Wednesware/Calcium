import zipfile
from pathlib import Path

with zipfile.ZipFile("sample.zip", "w", compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr("folder/hé.txt", "hé😀")
    archive.writestr("empty", b"")
    Path("input").write_bytes(b"binary\x00data")
    archive.write("input", "data")
print(zipfile.is_zipfile("sample.zip"), zipfile.is_zipfile("input"))
with zipfile.ZipFile("sample.zip") as archive:
    print(archive.namelist())
    print(archive.read("folder/hé.txt"))
    print(archive.getinfo("data").file_size)
    print([(i.filename, i.file_size) for i in archive.infolist()])
    print(archive.testzip())
    archive.extractall("output")
    print(Path("output/folder/hé.txt").read_text())
    print(Path("output/data").read_bytes())
    try:
        archive.read("missing")
    except KeyError:
        print("missing")
try:
    archive.read("empty")
except ValueError:
    print("closed")
try:
    zipfile.ZipFile("input")
except zipfile.BadZipFile:
    print("bad ZIP")
