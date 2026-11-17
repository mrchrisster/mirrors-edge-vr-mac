"""Generates vr_defaults.inl and build_identity.inl for the mod's source (mac equivalent of build.ps1)."""
import hashlib, os, re, sys
root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "vr-mod"))
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen")
ini = open(os.path.join(root, "mevr.ini.example"), encoding="utf-8").read()
rx = r"(?m)^; Rename this file to  mevr\.ini  and leave it beside d3d9\.dll.*$"
assert re.search(rx, ini), "shipped defaults no longer match mevr.ini.example"
ini = re.sub(rx, "; This IS mevr.ini. Keep it beside libmevr.dylib.", ini)
for notice in ("LICENSE", "THIRD-PARTY-NOTICES.txt", "third_party/minhook/LICENSE.txt"):
    ini += f"\r\n; ---- {notice} ----\r\n"
    for line in re.split(r"\r?\n", open(os.path.join(root, notice), encoding="utf-8").read()):
        ini += f"; {line}\r\n"
h = "static const char kVrShippedDefaults[] =\r\n"
for i in range(0, len(ini), 4096):
    h += 'R"MEVRDEFAULT(' + ini[i:i + 4096] + ')MEVRDEFAULT"\r\n'
h += ";"
open(os.path.join(out, "vr_defaults.inl"), "w", encoding="utf-8", newline="").write(h)
open(os.path.join(out, "mevr.ini"), "w", encoding="utf-8", newline="").write(ini)
src = os.path.join(root, "src")
ident = "\n".join(f"/src/{f}:" + hashlib.sha256(open(os.path.join(src, f), "rb").read()).hexdigest().upper()
                  for f in sorted(os.listdir(src)) if f.endswith((".cpp", ".inl", ".h", ".def")) and f != "build_identity.inl")
bid = "mac-" + hashlib.sha256(ident.encode()).hexdigest().upper()[:32]
open(os.path.join(out, "build_identity.inl"), "w").write(f'static const char kMevrBuildId[] = "{bid}";')
print("generated", out)
