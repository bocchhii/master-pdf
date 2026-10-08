"""Downloads the free fonts Master PDF comes with into fonts/ - each family's files and its
licence (they're all free to share: SIL Open Font License, Ubuntu Font Licence, or the
Liberation / DejaVu / Selawik licences, kept next to them). Run once from the project folder:
    python packaging/get_fonts.py
"""
import io, os, tarfile, urllib.request, zipfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "fonts")
GF = "https://raw.githubusercontent.com/google/fonts/main/"

# family folder -> files from Google's font repository (their licence file too)
GOOGLE = {
    "Carlito": ("ofl/carlito", ["Carlito-Regular", "Carlito-Bold", "Carlito-Italic", "Carlito-BoldItalic"]),
    "Caladea": ("ofl/caladea", ["Caladea-Regular", "Caladea-Bold", "Caladea-Italic", "Caladea-BoldItalic"]),
    "Amiri": ("ofl/amiri", ["Amiri-Regular", "Amiri-Bold", "Amiri-Italic", "Amiri-BoldItalic"]),
    "Tajawal": ("ofl/tajawal", ["Tajawal-Regular", "Tajawal-Bold"]),
    "Comic Neue": ("ofl/comicneue", ["ComicNeue-Regular", "ComicNeue-Bold", "ComicNeue-Italic", "ComicNeue-BoldItalic"]),
    "Courier Prime": ("ofl/courierprime", ["CourierPrime-Regular", "CourierPrime-Bold", "CourierPrime-Italic", "CourierPrime-BoldItalic"]),
    "Lato": ("ofl/lato", ["Lato-Regular", "Lato-Bold", "Lato-Italic", "Lato-BoldItalic"]),
    "Poppins": ("ofl/poppins", ["Poppins-Regular", "Poppins-Bold", "Poppins-Italic", "Poppins-BoldItalic"]),
    "PT Sans": ("ofl/ptsans", ["PT_Sans-Web-Regular", "PT_Sans-Web-Bold", "PT_Sans-Web-Italic", "PT_Sans-Web-BoldItalic"]),
    "PT Serif": ("ofl/ptserif", ["PT_Serif-Web-Regular", "PT_Serif-Web-Bold", "PT_Serif-Web-Italic", "PT_Serif-Web-BoldItalic"]),
    "Ubuntu": ("ufl/ubuntu", ["Ubuntu-Regular", "Ubuntu-Bold", "Ubuntu-Italic", "Ubuntu-BoldItalic"]),
}
LIBERATION = "https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz"
DEJAVU = "https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip"
SELAWIK = "https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip"


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "MasterPDF-build"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def save(folder, name, data):
    os.makedirs(os.path.join(OUT, folder), exist_ok=True)
    with open(os.path.join(OUT, folder, name), "wb") as f:
        f.write(data)


def main():
    for folder, (path, files) in GOOGLE.items():
        for f in files:
            save(folder, f + ".ttf", fetch(GF + path + "/" + f + ".ttf"))
        for lic in ("OFL.txt", "UFL.txt", "LICENCE.txt", "LICENSE.txt"):
            try:
                save(folder, lic, fetch(GF + path + "/" + lic))
                break
            except Exception:
                continue
        print("got", folder)
    with tarfile.open(fileobj=io.BytesIO(fetch(LIBERATION)), mode="r:gz") as tar:
        for m in tar.getmembers():
            base = os.path.basename(m.name)
            if base.endswith(".ttf") or base in ("LICENSE", "AUTHORS"):
                fam = ("Liberation " + base.split("-")[0][len("Liberation"):]) if base.endswith(".ttf") else None
                data = tar.extractfile(m).read()
                if fam:
                    save(fam, base, data)
                else:
                    for f in ("Liberation Sans", "Liberation Serif", "Liberation Mono"):
                        save(f, base + ".txt", data)
    print("got Liberation")
    with zipfile.ZipFile(io.BytesIO(fetch(DEJAVU))) as z:
        for n in z.namelist():
            base = os.path.basename(n)
            if base in ("DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf",
                        "DejaVuSans-BoldOblique.ttf", "DejaVuSerif.ttf", "DejaVuSerif-Bold.ttf",
                        "DejaVuSerif-Italic.ttf", "DejaVuSerif-BoldItalic.ttf", "DejaVuSansMono.ttf",
                        "DejaVuSansMono-Bold.ttf", "LICENSE"):
                save("DejaVu", base + (".txt" if base == "LICENSE" else ""), z.read(n))
    print("got DejaVu")
    with zipfile.ZipFile(io.BytesIO(fetch(SELAWIK))) as z:
        for n in z.namelist():
            base = os.path.basename(n)
            if base.lower().endswith(".ttf") or base.upper().startswith("LICENSE"):
                save("Selawik", base, z.read(n))
    save("Selawik", "LICENSE.txt", fetch("https://raw.githubusercontent.com/microsoft/Selawik/master/LICENSE.txt"))
    print("got Selawik")


if __name__ == "__main__":
    main()
