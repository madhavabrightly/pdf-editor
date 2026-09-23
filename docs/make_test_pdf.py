"""Generates a small multi-page PDF so Rpfg can be exercised end to end.

Deliberately hand-rolled: no dependencies beyond the standard library, and it
emits a byte-accurate xref table so the viewer parses a well-formed file rather
than falling back to MuPDF's repair path.

Object layout (N pages):
    1             catalog
    2             page tree
    3             font
    4 + 2i        page i
    5 + 2i        content stream for page i
"""

import sys
from pathlib import Path

PAGE_COUNT = 3

CATALOG = 1
PAGES = 2
FONT = 3
FIRST_PAGE = 4


def page_object(index: int) -> int:
    return FIRST_PAGE + index * 2


def content_object(index: int) -> int:
    return FIRST_PAGE + index * 2 + 1


def content_stream(index: int) -> bytes:
    return (
        "q\n"
        "1 0 0 RG 4 w\n"
        "72 120 468 540 re S\n"
        "0 0 1 rg\n"
        f"72 620 {120 + index * 60} 60 re f\n"
        "0 0 0 rg\n"
        f"BT /F1 36 Tf 92 700 Td (Rpfg test - page {index + 1}) Tj ET\n"
        "BT /F1 14 Tf 92 660 Td (rasterised by MuPDF on a worker thread) Tj ET\n"
        "Q\n"
    ).encode("latin-1")


objects = {
    CATALOG: f"<< /Type /Catalog /Pages {PAGES} 0 R >>".encode(),
    PAGES: (
        "<< /Type /Pages /Kids ["
        + " ".join(f"{page_object(i)} 0 R" for i in range(PAGE_COUNT))
        + f"] /Count {PAGE_COUNT} >>"
    ).encode(),
    FONT: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
}

for index in range(PAGE_COUNT):
    objects[page_object(index)] = (
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        f"/Contents {content_object(index)} 0 R "
        f"/Resources << /Font << /F1 {FONT} 0 R >> >> >>"
    ).encode()
    body = content_stream(index)
    objects[content_object(index)] = (
        f"<< /Length {len(body)} >>\nstream\n".encode() + body + b"endstream"
    )

pdf = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
offsets = {}
for number in sorted(objects):
    offsets[number] = len(pdf)
    pdf += f"{number} 0 obj\n".encode() + objects[number] + b"\nendobj\n"

xref_offset = len(pdf)
size = max(objects) + 1
pdf += f"xref\n0 {size}\n".encode() + b"0000000000 65535 f \n"
for number in range(1, size):
    pdf += b"%010d 00000 n \n" % offsets[number]
pdf += (
    f"trailer\n<< /Size {size} /Root {CATALOG} 0 R >>\n"
    f"startxref\n{xref_offset}\n%%EOF\n"
).encode()

destination = Path(sys.argv[1])
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_bytes(bytes(pdf))
print(f"wrote {destination} ({len(pdf)} bytes, {PAGE_COUNT} pages)")
