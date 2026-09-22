"""Regenerate the synthetic password fixtures (requires reportlab and pypdf).

The checked-in PDFs let the C++ tests run without either Python dependency.
RC4-128 makes regeneration deterministic; this is test data, not encryption
advice. Each document has searchable text, a red render marker, and a checkbox.
"""

from io import BytesIO
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import ArrayObject, ByteStringObject
from reportlab.pdfgen import canvas


def generate(index: int) -> None:
    source = BytesIO()
    page = canvas.Canvas(source, pagesize=(300, 400), invariant=True)
    page.setFont("Helvetica", 14)
    page.drawString(30, 350, f"Protected fixture {index}")
    page.setFillColorRGB(1, 0, 0)
    page.rect(30, 280, 50, 50, stroke=0, fill=1)
    page.acroForm.checkbox(name="FixtureCheck", x=30, y=200, size=20,
                          checked=False, buttonStyle="check")
    page.showPage()
    page.save()

    writer = PdfWriter(clone_from=PdfReader(source))
    identifier = ByteStringObject(f"TrePDF fixture {index}".encode("ascii"))
    writer._ID = ArrayObject([identifier, identifier])
    writer.encrypt(f"fixture-open-{index}", f"fixture-owner-{index}",
                   algorithm="RC4-128")
    with Path(__file__).with_name(f"password-{index}.pdf").open("wb") as output:
        writer.write(output)


if __name__ == "__main__":
    for fixture_index in (1, 2):
        generate(fixture_index)
