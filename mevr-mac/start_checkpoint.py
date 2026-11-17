"""Binds F11 in the game's TdInput.ini to `start <Map>?LoadCheckpoint=<name>` (the engine console command), so the
launcher can press it once the menu is up. Idempotent; the first run keeps a backup (TdInput.ini.mevr-bak).
usage: start_checkpoint.py <TdInput.ini> <checkpoints.txt> <checkpoint name | --list>"""
import re, shutil, sys, os
ini, lst, want = sys.argv[1], sys.argv[2], sys.argv[3]
cmds = [l.strip() for l in open(lst, encoding="utf-8") if l.strip().startswith("start ")]
if want == "--list":
    for c in cmds:
        m = re.match(r"start (\w+)\?LoadCheckpoint=(\S+)", c)
        if m: print(f"{m.group(2):40s} ({m.group(1)})")
    sys.exit(0)
hit = [c for c in cmds if c.lower().endswith("=" + want.lower())]
if not hit:
    sys.exit(f"unknown checkpoint {want!r}; run with --list")
if not os.path.exists(ini + ".mevr-bak"):
    shutil.copy2(ini, ini + ".mevr-bak")
s = open(ini, newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
s = re.sub(r'Bindings=\(Name="F11",Command="[^"]*",Control=False,Shift=False,Alt=False\)\r?\n', "", s)
marker = "bEnableMouseSmoothing=true" + nl
if marker not in s:
    sys.exit("marker bEnableMouseSmoothing=true not found in TdInput.ini")
line = f'Bindings=(Name="F11",Command="{hit[0]}",Control=False,Shift=False,Alt=False){nl}'
open(ini, "w", newline="").write(s.replace(marker, marker + line, 1))
print(hit[0])
