#!/usr/bin/env python3
"""Roda o dist/install-linux.sh numa Steam de mentira e confere: hard links, mod na cópia, o original intacto, o atalho
no shortcuts.vdf uma vez só (mesmo instalando duas vezes, com e sem arquivo de atalhos) e a remoção voltando ao
arquivo original. Precisa de build/dist (make dist)."""
import os, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SH = os.path.join(ROOT, "build", "dist", "install-linux.sh")
NAME = "Call of Juarez: Gunslinger VR"


def vdf(b):  # leitor independente do binário VDF do Steam
    i = 0

    def cstr():
        nonlocal i
        j = b.index(b"\0", i)
        s, i = b[i:j].decode(), j + 1
        return s

    def node():
        nonlocal i
        d = {}
        while True:
            t = b[i]
            i += 1
            if t == 8:
                return d
            k = cstr()
            if t == 0:
                d[k] = node()
            elif t == 1:
                d[k] = cstr()
            else:
                d[k], i = int.from_bytes(b[i:i + 4], "little", signed=True), i + 4

    return node()


def run(steam, *args):
    env = dict(os.environ, COJVR_STEAM_ROOT=steam, COJVR_TEST="1")
    subprocess.run(["bash", SH, *args], env=env, check=True, stdout=subprocess.DEVNULL)


with tempfile.TemporaryDirectory() as steam:
    common = os.path.join(steam, "steamapps", "common")
    game, dest = os.path.join(common, "CoJ Gunslinger"), os.path.join(common, "CoJ Gunslinger VR")
    for d in (game + "/coj4", game + "/cojvr", common + "/Proton - Experimental", common + "/SteamLinuxRuntime_4",
              steam + "/userdata/123/config", steam + "/userdata/456/config"):
        os.makedirs(d)
    open(steam + "/steamapps/libraryfolders.vdf", "w").write('"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' % steam)
    open(steam + "/steamapps/appmanifest_204450.acf", "w").write('"AppState"\n{\n\t"installdir"\t\t"CoJ Gunslinger"\n}\n')
    for f, t in (("CoJGunslinger.exe", "exe"), ("coj4/Data0.pak", "pak"), ("d3d9.dll", "old mod")):
        open(os.path.join(game, f), "w").write(t)
    other = (b"\0shortcuts\0\x000\0\x02appid\0\xfb\xff\xff\xff\x01appname\0Other\0\x02OpenVR\0\0\0\0\0"
             b"\0tags\0\x010\0fav\0\x08\x08\x08\x08")
    vdf1 = steam + "/userdata/123/config/shortcuts.vdf"
    open(vdf1, "wb").write(other)

    run(steam)
    run(steam)  # atualizar
    assert os.stat(dest + "/coj4/Data0.pak").st_nlink == 2 and open(dest + "/d3d9.dll", "rb").read() != b"old mod"
    assert open(game + "/d3d9.dll").read() == "old mod" and not os.path.exists(dest + "/cojvr/old.txt")
    for u in ("123", "456"):
        sc = vdf(open(steam + "/userdata/%s/config/shortcuts.vdf" % u, "rb").read())["shortcuts"]
        names = [e["appname"] for e in sc.values()]
        assert names == (["Other", NAME] if u == "123" else [NAME]), names
        assert sc[str(len(sc) - 1)]["OpenVR"] == 1 and sc[str(len(sc) - 1)]["AllowOverlay"] == 0 and sc[str(len(sc) - 1)]["Exe"] == '"%s/cojvr-launch.sh"' % dest
    run(steam, "--uninstall")
    assert open(vdf1, "rb").read() == other and not os.path.exists(dest) and open(game + "/coj4/Data0.pak").read() == "pak"
print("ok")
