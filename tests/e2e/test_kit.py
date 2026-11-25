"""End-to-end tests for the installed kit. Windows only.
Needs ZODIAC_KIT_DIR (the kit installed by the CTest fixture zodiac_kit_install, see tests/CMakeLists.txt), ZODIAC_MODULE_DIR (the build tree's shipped modules)
and ZODIAC_TEST_MODULE_DIR (the build tree's fixture modules, which must never reach the kit) in the environment."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

KIT = Path(os.environ.get("ZODIAC_KIT_DIR", ""))
MODULE_DIR = Path(os.environ.get("ZODIAC_MODULE_DIR", ""))
TEST_MODULE_DIR = Path(os.environ.get("ZODIAC_TEST_MODULE_DIR", ""))
TOP_LEVEL = {"bin", "docs", "examples", "include", "lib", "modules", "LICENSE", "THIRD_PARTY_LICENSES"}


def system_path():
    """Only the Windows system folders: no toolchain, no build tree, nothing the kit could lean on."""
    root = os.environ.get("SystemRoot", r"C:\Windows")
    return os.pathsep.join([os.path.join(root, "System32"), root, os.path.join(root, "System32", "Wbem")])


def run(args, cwd=None, env=None):
    result = subprocess.run([str(a) for a in args], capture_output=True, cwd=cwd, env=env, timeout=300)
    return result.returncode, result.stdout.decode("utf-8", "replace"), result.stderr.decode("utf-8", "replace")


def names(folder, pattern):
    return sorted(p.name for p in Path(folder).glob(pattern) if p.is_file())


def restore_environment(key, value):
    if value is None:
        os.environ.pop(key, None)
    else:
        os.environ[key] = value


def isolate_user_data(test):
    """Points APPDATA and XDG_DATA_HOME, where a game's data:/ folder lives, into a temporary folder of the test for every subprocess it
    starts (each one inherits os.environ, and the env dicts built here copy it), so neither the kit nor a built game writes player data
    outside the test's temporary folders. The old values come back, and the folder goes, when the test ends."""
    folder = tempfile.TemporaryDirectory()
    test.addCleanup(folder.cleanup)
    for key in ("APPDATA", "XDG_DATA_HOME"):
        test.addCleanup(restore_environment, key, os.environ.get(key))
        os.environ[key] = folder.name


@unittest.skipUnless(os.name == "nt", "the kit is Windows only for now")
class KitTest(unittest.TestCase):
    def setUp(self):
        isolate_user_data(self)

    def test_layout(self):
        self.assertTrue(KIT.is_dir(), "ZODIAC_KIT_DIR is not set or not installed: %s" % KIT)
        self.assertEqual({p.name for p in KIT.iterdir()}, TOP_LEVEL)
        for exe in ("zodiac.exe", "zodiacw.exe", "ld.lld.exe"):
            self.assertTrue((KIT / "bin" / exe).is_file(), exe)
        # The DLLs ld.lld.exe loads from its own folder.
        dlls = names(KIT / "bin", "*.dll")
        self.assertTrue(any(d.startswith("libLLVM") for d in dlls), dlls)
        for dll in ("libc++.dll", "libunwind.dll"):
            self.assertIn(dll, dlls)
        self.assertEqual(sorted(p.name for p in (KIT / "bin").iterdir()), sorted(["zodiac.exe", "zodiacw.exe", "ld.lld.exe"] + dlls))
        release = KIT / "lib" / "release"
        for name in ("libzodiac_release_stub.a", "libzodiac_core_release.a", "libangelscript.a", "zodiac_manifest.obj", "zodiac_manifest_admin.obj", "link.txt", "toolchain.txt"):
            self.assertTrue((release / name).is_file(), name)
        # Every toolchain file the link line names is in crt/, and nothing else.
        link = (release / "link.txt").read_text(encoding="utf-8").splitlines()
        wanted = sorted({line.rsplit("/", 1)[1] for line in link if line.startswith("{release_lib}/crt/")})
        self.assertTrue(wanted)
        self.assertEqual(sorted(p.name for p in (release / "crt").iterdir()), wanted)
        self.assertEqual(sorted(p.name for p in release.iterdir()), sorted(["crt", "libzodiac_release_stub.a", "libzodiac_core_release.a", "libangelscript.a", "zodiac_manifest.obj", "zodiac_manifest_admin.obj", "link.txt", "toolchain.txt"]))
        # The shipped application manifest (the Segment Heap, asInvoker) is in zodiac.exe, zodiacw.exe and the object every game links; the
        # administrator object differs only in its execution level.
        heap_type = b'<heapType xmlns="http://schemas.microsoft.com/SMI/2020/WindowsSettings">SegmentHeap</heapType>'
        as_invoker = b'<requestedExecutionLevel level="asInvoker" uiAccess="false"/>'
        as_admin = b'<requestedExecutionLevel level="requireAdministrator" uiAccess="false"/>'
        for path in (KIT / "bin" / "zodiac.exe", KIT / "bin" / "zodiacw.exe", release / "zodiac_manifest.obj"):
            data = path.read_bytes()
            self.assertIn(heap_type, data, path)
            self.assertIn(as_invoker, data, path)
            self.assertNotIn(as_admin, data, path)
        admin = (release / "zodiac_manifest_admin.obj").read_bytes()
        self.assertIn(heap_type, admin)
        self.assertIn(as_admin, admin)
        self.assertNotIn(as_invoker, admin)
        for header in ("plugin.h", "plugin.hpp"):
            self.assertTrue((KIT / "include" / "zodiac" / header).is_file(), header)
        self.assertEqual(sorted(p.name for p in (KIT / "include").iterdir()), ["zodiac"])
        self.assertEqual(sorted(p.name for p in (KIT / "include" / "zodiac").iterdir()), ["plugin.h", "plugin.hpp"])
        self.assertTrue((KIT / "docs").is_dir())
        self.assertTrue((KIT / "LICENSE").is_file())
        licenses = (KIT / "THIRD_PARTY_LICENSES").read_text(encoding="utf-8")
        for text in ("AngelCode Scripting Library", "Mark Gillard", "Apache License", "LLVM Exceptions", "mingw-w64 project", "Henry Spencer", "Todd C. Miller", "Yann Collet", "UNICODE, INC. LICENSE AGREEMENT", "Copyright © 1991-2015 Unicode, Inc.", "UNICODE LICENSE V3", "Unicode Character Database 16.0.0", "derived from AngelScript's scriptarray add-on", "an altered version of AngelScript 2.39.0", "its number literal scanner and its compiler"):
            self.assertIn(text, licenses)

    def test_modules_are_the_shipped_ones(self):
        shipped = names(MODULE_DIR, "*.dll")
        self.assertIn("zodiac_mod_example.dll", shipped)
        self.assertEqual(names(KIT / "modules", "*.dll"), shipped)
        fixtures = names(TEST_MODULE_DIR, "*.dll")
        self.assertTrue(fixtures)
        self.assertEqual(set(fixtures) & set(p.name for p in (KIT / "modules").rglob("*")), set())
        self.assertEqual(sorted(p.name for p in (KIT / "modules").iterdir() if p.is_file()), shipped)
        static = sorted(p.name for p in (KIT / "modules" / "static").iterdir())
        self.assertIn("zodiac_mod_example.a", static)
        self.assertIn("zodiac_mod_example.stamp", static)
        for name in static:
            self.assertRegex(name, r"^zodiac_mod_[a-z0-9_]+\.(a|stamp)$")
            self.assertNotIn(name.rsplit(".", 1)[0] + ".dll", fixtures)
        toolchain = (KIT / "lib" / "release" / "toolchain.txt").read_text(encoding="utf-8")
        self.assertEqual((KIT / "modules" / "static" / "zodiac_mod_example.stamp").read_text(encoding="utf-8"), toolchain)

    def test_example_matches_new(self):
        example = KIT / "examples" / "hello"
        self.assertEqual(sorted(p.name for p in (KIT / "examples").iterdir()), ["hello"])
        self.assertEqual(sorted(p.name for p in example.iterdir()), ["main.zs", "zodiac.toml"])
        with tempfile.TemporaryDirectory() as temp:
            generated = Path(temp) / "hello"
            code, _, err = run([KIT / "bin" / "zodiac.exe", "new", generated])
            self.assertEqual((code, err), (0, ""))
            for name in ("zodiac.toml", "main.zs"):
                self.assertEqual((example / name).read_bytes(), (generated / name).read_bytes(), name)

    def test_works_from_any_folder_without_the_toolchain(self):
        env = dict(os.environ)
        env["PATH"] = system_path()
        for key in [k for k in env if k.startswith("ZODIAC_")]:
            del env[key]
        with tempfile.TemporaryDirectory() as temp:
            # A copy in a folder with a space and a non-ASCII letter: the kit finds its files from its own location.
            kit = Path(temp) / "Zodiac Kit \u015f"
            shutil.copytree(KIT, kit)
            exe = kit / "bin" / "zodiac.exe"
            for name, modules, script, expected in (
                ("static_game", 'static = ["example"]', 'void main()\n{\nexample_print("kit static");\n}\n', (0, ["kit static"])),
                ("dynamic_game", 'dynamic = ["example"]', 'int main()\n{\nexample_print("kit dynamic");\nreturn example_add(2, 3);\n}\n', (5, ["kit dynamic"])),
            ):
                with self.subTest(game=name):
                    game = Path(temp) / name
                    code, out, err = run([exe, "new", game], env=env)
                    self.assertEqual((code, err), (0, ""), out)
                    toml = game / "zodiac.toml"
                    kind = modules.split(" ")[0]
                    text = toml.read_text(encoding="utf-8").replace("%s = []" % kind, modules)
                    toml.write_bytes(text.encode("utf-8"))
                    (game / "main.zs").write_bytes(script.encode("utf-8"))
                    code, out, err = run([exe, "build", game, "--no-window"], env=env)
                    self.assertEqual((code, err), (0, ""), out)
                    release = game / "release"
                    built = release / (name + ".exe")
                    self.assertTrue(built.is_file(), out)
                    # A dynamic module is shipped next to the executable, from the kit's modules folder.
                    self.assertEqual((release / "zodiac_mod_example.dll").is_file(), kind == "dynamic", sorted(p.name for p in release.iterdir()))
                    code, out, err = run([built], env=env)
                    self.assertEqual((code, out.splitlines(), err), (expected[0], expected[1], ""))


if __name__ == "__main__":
    unittest.main()
