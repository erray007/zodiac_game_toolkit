"""End-to-end tests for zodiac build. Windows only: release builds link with the kit's ld.lld and llvm-mingw CRT subset.
Needs ZODIAC_EXE, ZODIACW_EXE (the GUI-subsystem twin), ZODIAC_MODULE_DIR, ZODIAC_TEST_MODULE_DIR, ZODIAC_ECHO_ARGS and ZODIAC_VENDOR_MODULE_DIR
(vendor_dep with its third-party DLLs) in the environment (CTest sets them, see tests/CMakeLists.txt)."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

EXE = os.environ.get("ZODIAC_EXE", "")
GUI_EXE = os.environ.get("ZODIACW_EXE", "")
MODULE_DIR = os.environ.get("ZODIAC_MODULE_DIR", "")
TEST_MODULE_DIR = os.environ.get("ZODIAC_TEST_MODULE_DIR", "")
ECHO_ARGS = os.environ.get("ZODIAC_ECHO_ARGS", "")
VENDOR_MODULE_DIR = os.environ.get("ZODIAC_VENDOR_MODULE_DIR", "")
# The build configuration of the tree under test (CTest passes $<CONFIG>); the size gate measures Release builds only.
BUILD_CONFIG = os.environ.get("ZODIAC_BUILD_CONFIG", "")
SCRIPT_FIXTURES = Path(__file__).resolve().parent.parent / "fixtures" / "scripts"
# Fixtures that need test-only modules from ZODIAC_TEST_MODULE_DIR: a release loads its modules from the kit (static) or its own folder (dynamic),
# and the kit ships no test modules, so these cannot be built. Every other fixture takes part in the identity test.
FIXTURES_NEEDING_TEST_MODULES = {"holds_plugin_object"}
# The fixtures that build; the identity test fails if this set changes, so a fixture can never drop out of it silently.
BUILDABLE_FIXTURES = {"blood_bath_style", "int_main", "non_ascii_include", "runtime_error", "uses_module", "void_main"}
SIZE_LIMIT = 5242880
KEY_CREATED = "created .zodiac/pack.key; back it up, later builds and patches need it"
TRAILER_MAGIC = b"ZDTRAIL1"
LONE_FILE_WARNING = "warning: no zodiac.toml; only the core is available (create a project file to add modules)"
LONE_FILE_NOTE = "note: no zodiac.toml; only the core is available, so module functions are not declared (create a project file to add modules)"


def zodiac(*args, cwd=None, errors="strict", exe=None, env=None):
    result = subprocess.run([exe or EXE, *args], capture_output=True, cwd=cwd, env=env, timeout=300)
    return result.returncode, result.stdout.decode("utf-8", errors), result.stderr.decode("utf-8", errors)


def no_desktop_env():
    """The environment with the documented test hook set: has_interactive_desktop() reports no desktop, so a build without --no-window
    reports on the console instead of opening the result window (whether a CI runner has a visible window station is not guaranteed)."""
    env = dict(os.environ)
    env["ZODIAC_FORCE_NO_DESKTOP"] = "1"
    return env


def pe_subsystem(path):
    """The Subsystem field of a PE image's optional header (2 GUI, 3 console)."""
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    return struct.unpack_from("<H", data, pe + 24 + 68)[0]


def run_exe(path, cwd=None, errors="strict"):
    result = subprocess.run([str(path)], capture_output=True, cwd=cwd, timeout=60)
    return result.returncode, result.stdout.decode("utf-8", errors), result.stderr.decode("utf-8", errors)


def write(path, text):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(text.encode("utf-8"))


def restore_environment(key, value):
    if value is None:
        os.environ.pop(key, None)
    else:
        os.environ[key] = value


def isolate_user_data(test):
    """Points APPDATA and XDG_DATA_HOME, where a game's data:/ folder lives, into a temporary folder of the test for every subprocess it
    starts (each one inherits os.environ, and the env dicts built here copy it), so neither zodiac nor a built game writes player data
    outside the test's temporary folders. The old values come back, and the folder goes, when the test ends."""
    folder = tempfile.TemporaryDirectory()
    test.addCleanup(folder.cleanup)
    for key in ("APPDATA", "XDG_DATA_HOME"):
        test.addCleanup(restore_environment, key, os.environ.get(key))
        os.environ[key] = folder.name


def toml_literal(text):
    """A TOML literal string: no escapes, so Windows paths stay as they are."""
    assert "'" not in text
    return "'" + text + "'"


def pe_imports(path):
    """The DLL names a PE image imports (import and delay-import directories), lower-case."""
    data = Path(path).read_bytes()
    if data[:2] != b"MZ":
        raise ValueError("not a PE image: %s" % path)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE signature: %s" % path)
    sections_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    sections = []
    table = optional + optional_size
    for index in range(sections_count):
        entry = table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, entry + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

    def offset(rva):
        for virtual_address, size, raw_pointer in sections:
            if virtual_address <= rva < virtual_address + size:
                return rva - virtual_address + raw_pointer
        raise ValueError("RVA 0x%x is outside every section" % rva)

    def c_string(rva):
        start = offset(rva)
        return data[start:data.index(b"\0", start)].decode("ascii").lower()

    names = []
    import_rva = struct.unpack_from("<I", data, directories + 8)[0]
    if import_rva:
        at = offset(import_rva)
        while True:
            fields = struct.unpack_from("<IIIII", data, at)
            if not any(fields):
                break
            names.append(c_string(fields[3]))
            at += 20
    delay_rva = struct.unpack_from("<I", data, directories + 13 * 8)[0]
    if delay_rva:
        at = offset(delay_rva)
        while True:
            fields = struct.unpack_from("<IIIIIIII", data, at)
            if not any(fields):
                break
            names.append(c_string(fields[1]))
            at += 32
    return names


@unittest.skipUnless(os.name == "nt", "release builds are Windows only for now")
class BuildTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        isolate_user_data(self)

    def new_project(self, directory, static=(), dynamic=(), script=None):
        code, _, err = zodiac("new", str(directory))
        self.assertEqual(code, 0, err)
        toml = directory / "zodiac.toml"
        text = toml.read_text(encoding="utf-8")
        text = text.replace("static = []", "static = [%s]" % ", ".join('"%s"' % m for m in static))
        text = text.replace("dynamic = []", "dynamic = [%s]" % ", ".join('"%s"' % m for m in dynamic))
        write(toml, text)
        if script is not None:
            write(directory / "main.zs", script)
        return directory

    def append_toml(self, directory, text):
        toml = directory / "zodiac.toml"
        write(toml, toml.read_text(encoding="utf-8") + text)

    def build(self, *args, cwd=None, errors="strict"):
        return zodiac("build", *args, "--no-window", "--module-dir", MODULE_DIR, cwd=cwd, errors=errors)

    def assert_built(self, result, exe):
        code, out, err = result
        self.assertEqual((code, err), (0, ""), out)
        self.assertEqual(out.splitlines()[-1], "Build succeeded: %s" % exe)
        self.assertTrue(exe.is_file(), exe)

    def build_hello(self):
        project = self.new_project(self.root / "hello")
        exe = project / "release" / "hello.exe"
        self.assert_built(self.build(str(project)), exe)
        return exe

    def test_build_hello_runs(self):
        exe = self.build_hello()
        self.assertEqual(run_exe(exe), (0, "", ""))
        # Only the executable: no temporary object, response file or .tmp is left behind.
        self.assertEqual(sorted(p.name for p in exe.parent.iterdir()), ["hello.exe"])

    def test_build_run_as_admin_selects_the_manifest(self):
        # The manifest's bytes as the kit's objects hold them (test_kit checks the objects themselves). The built games are never run:
        # an elevated one would need a UAC prompt, which CI runners (elevated, UAC off) cannot show anyway.
        as_invoker = b'<requestedExecutionLevel level="asInvoker" uiAccess="false"/>'
        as_admin = b'<requestedExecutionLevel level="requireAdministrator" uiAccess="false"/>'
        for setting, wanted, other in (("", as_invoker, as_admin), ("\n[release]\nrun_as_admin = false\n", as_invoker, as_admin), ("\n[release]\nrun_as_admin = true\n", as_admin, as_invoker)):
            with self.subTest(setting=setting):
                project = self.new_project(self.root / ("game%d" % len(setting)))
                self.append_toml(project, setting)
                exe = project / "release" / (project.name + ".exe")
                self.assert_built(self.build(str(project)), exe)
                image = exe.read_bytes()
                # Exactly one manifest, at the level the setting asks for.
                self.assertEqual(image.count(wanted), 1)
                self.assertEqual(image.count(other), 0)
                self.assertEqual(image.count(b"<requestedExecutionLevel"), 1)
        project = self.new_project(self.root / "bad")
        self.append_toml(project, '\n[release]\nrun_as_admin = "yes"\n')
        code, out, err = self.build(str(project))
        self.assertEqual(code, 2, err)
        self.assertIn("zodiac.toml: error: [release] run_as_admin must be a boolean", err.splitlines())

    def test_build_static_example_module_prints(self):
        hello = self.build_hello()
        project = self.new_project(self.root / "game", static=["example"], script='void main()\n{\nexample_print(example_echo("merhaba kanka"));\n}\n')
        exe = project / "release" / "game.exe"
        self.assert_built(self.build(str(project)), exe)
        # Started with piped output, the GUI-subsystem release delivers the module's text on stdout, exactly.
        code, out, err = run_exe(exe)
        self.assertEqual((code, out.splitlines(), err), (0, ["merhaba kanka"], ""))
        self.assertEqual(sorted(p.name for p in exe.parent.iterdir()), ["game.exe"])
        # Undeclared modules stay out of the executable.
        self.assertNotIn(b"example_echo", hello.read_bytes())
        self.assertIn(b"example_echo", exe.read_bytes())

    def test_build_dynamic_module_ships_dll(self):
        project = self.new_project(self.root / "game", dynamic=["example"], script='int main()\n{\nexample_print("dinamik");\nreturn example_add(2,3);\n}\n')
        exe = project / "release" / "game.exe"
        self.assert_built(self.build(str(project)), exe)
        dll = exe.parent / "zodiac_mod_example.dll"
        self.assertTrue(dll.is_file())
        self.assertEqual(dll.read_bytes(), (Path(MODULE_DIR) / "zodiac_mod_example.dll").read_bytes())
        code, out, err = run_exe(exe)
        self.assertEqual((code, out.splitlines(), err), (5, ["dinamik"], ""))
        dll.unlink()
        code, out, err = run_exe(exe)
        self.assertEqual(code, 2)
        self.assertIn('module "example"', err)

    def test_identity_with_run(self):
        built = set()
        for source in sorted(p for p in SCRIPT_FIXTURES.iterdir() if p.is_dir()):
            if source.name in FIXTURES_NEEDING_TEST_MODULES:
                continue
            with self.subTest(fixture=source.name):
                # Run and built from a copy (times kept), so the build's .zodiac folder (a packing fixture's key) never lands in the source tree.
                fixture = self.root / "fixtures" / source.name
                shutil.copytree(source, fixture)
                # The system's reason in some messages (missing_include) is UTF-8 like the rest, so both sides decode strictly.
                run_code, run_out, run_err = zodiac("run", str(fixture), "--no-window", "--module-dir", MODULE_DIR)
                output = self.root / fixture.name
                build_code, build_out, build_err = zodiac("build", str(fixture), "--no-window", "--module-dir", MODULE_DIR, "--output", str(output))
                if build_code != 0:
                    # A fixture run refuses (compile, include, main or module errors) fails the build the same way.
                    self.assertEqual(build_code, run_code, build_err)
                    self.assertEqual(build_err.splitlines(), run_err.splitlines())
                    self.assertFalse((output / (fixture.name + ".exe")).exists())
                    continue
                built.add(fixture.name)
                exe_code, exe_out, exe_err = run_exe(output / (fixture.name + ".exe"))
                self.assertEqual(exe_code, run_code)
                self.assertEqual(exe_err.splitlines(), run_err.splitlines())
                self.assertEqual(exe_out, run_out)
        self.assertEqual(built, BUILDABLE_FIXTURES)

    def test_run_error_summary_only_in_zodiac_run(self):
        # Without --no-window and with no desktop, zodiac run sums its engine errors up on the console; the built game never does.
        fixture = SCRIPT_FIXTURES / "runtime_error"
        run_code, run_out, run_err = zodiac("run", str(fixture), "--module-dir", MODULE_DIR, env=no_desktop_env())
        self.assertEqual(run_code, 1)
        self.assertIn("runtime error", run_err)
        self.assertEqual(run_out.splitlines(), ["Run failed: 1 error"])
        output = self.root / "out"
        code, out, err = self.build(str(fixture), "--output", str(output))
        self.assertEqual(code, 0, err)
        exe_code, exe_out, exe_err = run_exe(output / "runtime_error.exe")
        self.assertEqual((exe_code, exe_out, exe_err.splitlines()), (1, "", run_err.splitlines()))

    def test_build_paths_with_space_and_unicode(self):
        project = self.new_project(self.root / "Kanka şövalye oyunu", static=["example"], script='int main()\n{\nexample_print("şövalye ğüşiöç");\nreturn 3;\n}\n')
        output = self.root / "çıktı klasörü"
        exe = output / "Kanka şövalye oyunu.exe"
        self.assert_built(self.build(str(project), "--output", str(output)), exe)
        code, out, err = run_exe(exe)
        self.assertEqual((code, out.splitlines(), err), (3, ["şövalye ğüşiöç"], ""))

    def test_build_from_script_file_uses_its_project(self):
        project = self.new_project(self.root / "game")
        write(project / "levels" / "one" / "level.zs", "void level()\n{\n}\n")
        exe = project / "release" / "game.exe"
        self.assert_built(self.build(str(project / "levels" / "one" / "level.zs")), exe)
        self.assertEqual(run_exe(exe), (0, "", ""))

    def test_build_from_script_file_with_upper_case_extension(self):
        project = self.new_project(self.root / "game")
        write(project / "menus" / "Menu.ZS", "void menu()\n{\n}\n")
        exe = project / "release" / "game.exe"
        self.assert_built(self.build(str(project / "menus" / "Menu.ZS")), exe)
        self.assertEqual(run_exe(exe), (0, "", ""))

    def test_build_locked_output(self):
        import ctypes
        from ctypes import wintypes
        exe = self.build_hello()
        before = exe.read_bytes()
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        generic_read = 0x80000000
        open_existing = 3
        handle = kernel32.CreateFileW(str(exe), generic_read, 0, None, open_existing, 0, None)
        self.assertNotEqual(handle, wintypes.HANDLE(-1).value, ctypes.get_last_error())
        try:
            code, out, err = self.build(str(exe.parent.parent))
        finally:
            kernel32.CloseHandle(handle)
        self.assertEqual(code, 2, err)
        self.assertIn('error: cannot replace "%s": it is in use (is the game still running?)' % exe, err.splitlines())
        self.assertEqual(out.splitlines()[-1], "Build failed: 1 error")
        self.assertEqual(sorted(p.name for p in exe.parent.iterdir()), ["hello.exe"])
        self.assertEqual(exe.read_bytes(), before)

    def test_build_compile_error_exit_1(self):
        project = self.new_project(self.root / "game", script="void main()\n{\nint(zz);\n}\n")
        code, out, err = self.build(str(project))
        self.assertEqual(code, 1)
        self.assertIn("main.zs:3:", err)
        errors = [line for line in err.splitlines() if not line.startswith("  ") and ("error: " in line)]
        self.assertTrue(errors, err)
        self.assertEqual(out.splitlines()[-1], "Build failed: %d %s" % (len(errors), "error" if len(errors) == 1 else "errors"))
        self.assertFalse((project / "release" / "game.exe").exists())

    def release_files(self, release):
        return sorted(p.relative_to(release).as_posix() for p in release.rglob("*") if p.is_file())

    # A game that copies what it reads into data:/ (APPDATA/<game>/, isolate_user_data's folder), so a test compares the bytes exactly.
    COPYING_SCRIPT = 'int main()\n{\nstring a=read_file("sounds/a.ogg");\nif(!write_file("data:/a.ogg",a))return 3;\nreturn file_exists("sounds/x/b.ogg")?0:4;\n}\n'

    def copied(self, game, name="a.ogg"):
        return (Path(os.environ["APPDATA"]) / game / name).read_bytes()

    def packing_project(self, toml="", script=None):
        project = self.new_project(self.root / "game", script=script or self.COPYING_SCRIPT)
        self.append_toml(project, '\n[build]\nassets = ["sounds/**/*.ogg"]\n' + toml)
        # Binary bytes, every value, so the pack carries them exactly and none shows through its encryption.
        self.sound = bytes(range(256)) * 64 + os.urandom(4096)
        (project / "sounds" / "x").mkdir(parents=True)
        (project / "sounds" / "a.ogg").write_bytes(self.sound)
        write(project / "sounds" / "x" / "b.ogg", "b")
        write(project / "sounds" / "c.wav", "c")
        write(project / "music" / "d.ogg", "d")
        return project

    def build_packed(self, project):
        code, out, err = self.build(str(project))
        self.assertEqual((code, err), (0, ""), out)
        self.assertEqual(out.splitlines()[-1], "Build succeeded: %s" % (project / "release" / "game.exe"))
        return out

    def test_build_assets_packed(self):
        project = self.packing_project()
        release = project / "release"
        out = self.build_packed(project)
        # The key is made on the first build, announced on stdout before the summary, and kept out of git.
        self.assertEqual(out.splitlines(), [KEY_CREATED, "Build succeeded: %s" % (release / "game.exe")])
        key_file = project / ".zodiac" / "pack.key"
        key = key_file.read_bytes()
        self.assertRegex(key.decode("ascii"), r"\A[0-9a-f]{64}\n\Z")
        # Everything in .zodiac/ is ignored by git: the key, a crash's leftover temporary name of it, the payload object, the manifest.
        self.assertIn("*", (project / ".zodiac" / ".gitignore").read_text(encoding="utf-8").splitlines())
        self.assertEqual(sorted(p.name for p in (project / ".zodiac").iterdir()), [".gitignore", "pack.key", "release.manifest"])
        # One file: the assets are inside the executable, encrypted, after the linked image and before the trailer at the end.
        self.assertEqual(self.release_files(release), ["game.exe"])
        image = (release / "game.exe").read_bytes()
        self.assertEqual(image[-8:], TRAILER_MAGIC)
        self.assertEqual(len(image) % 8, 0)
        self.assertNotIn(self.sound[:4096], image)
        # Until the protection plan the raw key is in the payload in clear, by design (pack/release_pack.hpp: obfuscation, not
        # protection), so this proves only that it is there once (the payload's copy) and that the key file's text is not shipped.
        self.assertEqual(image.count(bytes.fromhex(key.decode("ascii").strip())), 1)
        self.assertNotIn(key.strip(), image)
        self.assertEqual(run_exe(release / "game.exe"), (0, "", ""))
        self.assertEqual(self.copied("game"), self.sound)
        # The manifest of the build: the magic, the build id, the next patch, then one line per packed asset.
        manifest = (project / ".zodiac" / "release.manifest").read_text(encoding="utf-8").splitlines()
        self.assertEqual(manifest[0], "zodiac-release-manifest 1")
        self.assertRegex(manifest[1], r"\Abuild [0-9a-f]{32}\Z")
        self.assertEqual(manifest[2], "next_patch 1")
        self.assertEqual([line.split(" ", 2)[1:] for line in manifest[3:]], [[str(len(self.sound)), "sounds/a.ogg"], ["1", "sounds/x/b.ogg"]])
        self.assertTrue(all(len(line.split(" ")[0]) == 64 for line in manifest[3:]))
        # A second build reuses the key: the file is unchanged and nothing is announced. A temporary name of the key that a crash
        # left behind goes.
        (project / ".zodiac" / "pack.key.tmp").write_bytes(key)
        out = self.build_packed(project)
        self.assertFalse((project / ".zodiac" / "pack.key.tmp").exists())
        self.assertEqual(out.splitlines(), ["Build succeeded: %s" % (release / "game.exe")])
        self.assertEqual(key_file.read_bytes(), key)
        self.assertEqual(run_exe(release / "game.exe"), (0, "", ""))
        # A damaged key is refused, never replaced.
        key_file.write_bytes(b"not a key\n")
        code, out, err = self.build(str(project))
        self.assertEqual((code, err.splitlines()), (2, ["error: .zodiac/pack.key is damaged (expected 64 hex digits); restore it from your backup"]))
        self.assertEqual(key_file.read_bytes(), b"not a key\n")

    def test_build_pack_disabled_copies_assets(self):
        project = self.packing_project('\n[pack]\nenabled = false\n')
        release = project / "release"
        out = self.build_packed(project)
        self.assertNotIn(KEY_CREATED, out)
        self.assertEqual(self.release_files(release), ["game.exe", "sounds/a.ogg", "sounds/x/b.ogg"])
        self.assertEqual((release / "sounds" / "x" / "b.ogg").read_text(encoding="utf-8"), "b")
        self.assertNotEqual((release / "game.exe").read_bytes()[-8:], TRAILER_MAGIC)
        self.assertFalse((project / ".zodiac").exists())
        self.assertEqual(run_exe(release / "game.exe"), (0, "", ""))
        self.assertEqual(self.copied("game"), self.sound)

    def test_build_pack_external(self):
        project = self.packing_project('\n[pack]\nexternal = true\n')
        release = project / "release"
        self.build_packed(project)
        self.assertEqual(self.release_files(release), ["game.exe", "game.zpk"])
        self.assertNotEqual((release / "game.exe").read_bytes()[-8:], TRAILER_MAGIC)
        self.assertNotIn(self.sound[:4096], (release / "game.zpk").read_bytes())
        self.assertEqual(run_exe(release / "game.exe"), (0, "", ""))
        self.assertEqual(self.copied("game"), self.sound)
        stale = ['error: the game data is damaged (pack "game.zpk" is from another build)']
        # An old game.zpk put back over a newer build's is refused, never served.
        old = (release / "game.zpk").read_bytes()
        self.build_packed(project)
        (release / "game.zpk").write_bytes(old)
        code, out, err = run_exe(release / "game.exe")
        self.assertEqual((code, out, err.splitlines()), (2, "", stale))
        # A build whose game.zpk cannot be replaced (held open without sharing) has replaced the executable first: the new
        # executable refuses the old pack rather than run on stale assets.
        self.build_packed(project)
        held = (release / "game.zpk").read_bytes()
        import ctypes
        from ctypes import wintypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel32.CreateFileW(str(release / "game.zpk"), 0x80000000, 0, None, 3, 0, None)
        self.assertNotEqual(handle, wintypes.HANDLE(-1).value, ctypes.get_last_error())
        try:
            code, out, err = self.build(str(project))
        finally:
            kernel32.CloseHandle(handle)
        self.assertEqual(code, 2, err)
        self.assertIn('error: cannot replace "%s": it is in use (is the game still running?)' % (release / "game.zpk"), err.splitlines())
        self.assertEqual((release / "game.zpk").read_bytes(), held)
        self.assertEqual(self.release_files(release), ["game.exe", "game.zpk"])
        code, out, err = run_exe(release / "game.exe")
        self.assertEqual((code, out, err.splitlines()), (2, "", stale))
        (release / "game.zpk").unlink()
        code, out, err = run_exe(release / "game.exe")
        self.assertEqual((code, out, err.splitlines()), (2, "", ['error: the game data is damaged (pack "game.zpk" is missing)']))

    def test_build_pack_exclude_keeps_plain_files(self):
        script = 'int main()\n{\nif(!write_file("data:/a.ogg",read_file("sounds/a.ogg")))return 3;\nreturn read_file("sounds/x/b.ogg")=="b"?0:4;\n}\n'
        project = self.packing_project('\n[pack]\nexclude = ["sounds/x/**"]\n', script)
        write(project / "sounds" / "bundled.zpk", "a pack the game opens itself")
        toml = project / "zodiac.toml"
        write(toml, toml.read_text(encoding="utf-8").replace('assets = ["sounds/**/*.ogg"]', 'assets = ["sounds/**/*.ogg", "sounds/*.zpk"]'))
        release = project / "release"
        self.build_packed(project)
        # The excluded asset and every *.zpk stay plain files beside the executable; the rest is packed.
        self.assertEqual(self.release_files(release), ["game.exe", "sounds/bundled.zpk", "sounds/x/b.ogg"])
        self.assertEqual((release / "game.exe").read_bytes()[-8:], TRAILER_MAGIC)
        self.assertEqual(run_exe(release / "game.exe"), (0, "", ""))
        self.assertEqual(self.copied("game"), self.sound)
        manifest = (project / ".zodiac" / "release.manifest").read_text(encoding="utf-8").splitlines()
        self.assertEqual([line.split(" ", 2)[2] for line in manifest[3:]], ["sounds/a.ogg"])

    def test_build_writable_pack_is_external(self):
        # writable forces external (a running executable is locked); a save outside data:/ goes into the pack and survives a restart.
        # The save is read by its literal name: the script writes that name too, so the build does not warn about it.
        script = 'int main()\n{\nif(file_exists("save.txt"))return read_file("save.txt")=="level 2"?10:11;\nif(read_file("sounds/x/b.ogg")!="b")return 12;\nreturn write_file("save.txt","level 2")?0:13;\n}\n'
        project = self.packing_project('\n[pack]\nwritable = true\n', script)
        release = project / "release"
        self.build_packed(project)
        self.assertEqual(self.release_files(release), ["game.exe", "game.zpk"])
        before = (release / "game.zpk").read_bytes()
        self.assertEqual(run_exe(release / "game.exe"), (0, "", ""))
        self.assertEqual(self.release_files(release), ["game.exe", "game.zpk"])
        self.assertNotEqual((release / "game.zpk").read_bytes(), before)
        self.assertEqual(run_exe(release / "game.exe")[0], 10)

    def test_build_signed_pack_still_found(self):
        # The sign command appends an attribute certificate table (one empty WIN_CERTIFICATE, 8-aligned) and points the security
        # directory at it, as signtool does: the trailer is then no longer at the end of the file, and the game still finds its pack.
        signer = self.root / "sign.py"
        write(signer, (
            "import struct, sys\n"
            "path = sys.argv[1]\n"
            "data = bytearray(open(path, 'rb').read())\n"
            "pe = struct.unpack_from('<I', data, 0x3C)[0]\n"
            "optional = pe + 24\n"
            "directories = optional + (112 if struct.unpack_from('<H', data, optional)[0] == 0x20B else 96)\n"
            "while len(data) % 8:\n"
            "    data.append(0)\n"
            "offset = len(data)\n"
            "data += struct.pack('<IHH', 16, 0x0200, 2) + bytes(8)\n"
            "struct.pack_into('<II', data, directories + 4 * 8, offset, 16)\n"
            "open(path, 'wb').write(data)\n"))
        project = self.packing_project("\n[build.sign]\ncommand = [%s, %s, \"{file}\"]\n" % (toml_literal(sys.executable), toml_literal(str(signer))))
        release = project / "release"
        self.build_packed(project)
        image = (release / "game.exe").read_bytes()
        self.assertEqual(image[-16:-12], struct.pack("<I", 16))
        self.assertEqual(image[-24:-16], TRAILER_MAGIC)
        self.assertEqual(run_exe(release / "game.exe"), (0, "", ""))
        self.assertEqual(self.copied("game"), self.sound)

    def test_build_with_a_pack_that_fails_leaves_no_temporary_file(self):
        # A failed signing (echo_args without --marker exits 1), then a failed link (a stamped static plugin whose archive is no
        # archive): no .zpk.tmp, no manifest or its .tmp, no payload object (which holds the key) anywhere.
        self.assertTrue(ECHO_ARGS and Path(ECHO_ARGS).is_file(), ECHO_ARGS)
        project = self.packing_project("\n[build.sign]\ncommand = [%s, \"{file}\"]\n" % toml_literal(ECHO_ARGS))
        code, out, err = self.build(str(project))
        self.assertEqual(code, 2, err)
        self.assertIn('error: signing "%s" failed' % (project / "release" / "game.exe"), err.splitlines())
        self.assertEqual(list((project / "release").iterdir()), [])
        self.assertEqual(sorted(p.name for p in (project / ".zodiac").iterdir()), [".gitignore", "pack.key"])
        linking = self.new_project(self.root / "linking", static=["gpl_static"], script=self.COPYING_SCRIPT)
        self.append_toml(linking, '\n[build]\nassets = ["sounds/**"]\n')
        write(linking / "sounds" / "a.ogg", "a")
        static_dir = linking / "plugins" / "static"
        static_dir.mkdir(parents=True)
        write(static_dir / "zodiac_mod_gpl_static.a", "not an archive\n")
        toolchain = Path(EXE).resolve().parent.parent / "lib" / "release" / "toolchain.txt"
        (static_dir / "zodiac_mod_gpl_static.stamp").write_bytes(toolchain.read_bytes())
        code, out, err = self.build(str(linking), "--module-dir", TEST_MODULE_DIR)
        self.assertEqual(code, 2, err)
        self.assertIn("error: linking failed", err.splitlines())
        self.assertEqual(list((linking / "release").iterdir()), [])
        self.assertEqual(sorted(p.name for p in (linking / ".zodiac").iterdir()), [".gitignore", "pack.key"])

    def test_build_warns_for_undeclared_literal_path(self):
        script = ('void main()\n{\nread_file("sounds/a.ogg");\nfile_exists("notes.txt");\nwrite_file("log.txt","x");\nread_file("data:/save.txt");\n'
                  'list_files("sounds/x");\nread_file("log.txt");\nread_file("maps/1.txt");\n}\n'
                  'pack p;\nvoid mount()\n{\nmount_pack(p,"maps");\n}\n')
        project = self.packing_project(script=script)
        code, out, err = self.build(str(project))
        self.assertEqual(code, 0, err)
        # Only the read of an undeclared file: the declared one, the write, data:/, a folder of assets, a name the script writes itself
        # and one below a folder it mounts a pack at are fine. Never an error.
        self.assertEqual(err.splitlines(), ['warning: main.zs:4: "notes.txt" is not among the declared assets, so the release cannot open it'])
        self.assertEqual(out.splitlines()[-1], "Build succeeded: %s" % (project / "release" / "game.exe"))
        # A pack mounted at the root folder may hold any name: no warning at all.
        write(project / "main.zs", script.replace('mount_pack(p,"maps")', "mount_pack(p)"))
        code, out, err = self.build(str(project))
        self.assertEqual((code, err), (0, ""))

    def test_run_warns_writing_a_packed_path(self):
        script = 'void main()\n{\nwrite_file("sounds/a.ogg","x");\nwrite_file("sounds/a.ogg","y");\nwrite_file("data:/a.ogg","z");\nwrite_file("notes.txt","n");\n}\n'
        project = self.packing_project(script=script)
        code, out, err = zodiac("run", str(project), "--no-window", "--module-dir", MODULE_DIR)
        self.assertEqual(code, 0, err)
        self.assertEqual(err.splitlines(), ['warning: "sounds/a.ogg" is written in a folder here, but the release puts it in a read-only pack, where writing fails; save files in data:/ instead'])
        # Not with a writable release pack, where the release can write it too.
        self.append_toml(project, "\n[pack]\nwritable = true\n")
        code, out, err = zodiac("run", str(project), "--no-window", "--module-dir", MODULE_DIR)
        self.assertEqual((code, err), (0, ""))

    def test_hello_has_no_pack(self):
        exe = self.build_hello()
        self.assertFalse((exe.parent.parent / ".zodiac").exists())
        self.assertNotEqual(exe.read_bytes()[-8:], TRAILER_MAGIC)
        self.assertEqual(sorted(p.name for p in exe.parent.iterdir()), ["hello.exe"])

    def test_build_sign_command_runs(self):
        self.assertTrue(ECHO_ARGS and Path(ECHO_ARGS).is_file(), ECHO_ARGS)
        project = self.new_project(self.root / "game", dynamic=["example"])
        marker = self.root / "signed.txt"
        self.append_toml(project, "\n[build.sign]\ncommand = [%s, \"--marker\", %s, \"{file}\"]\n" % (toml_literal(ECHO_ARGS), toml_literal(str(marker))))
        release = project / "release"
        self.assert_built(self.build(str(project)), release / "game.exe")
        # The files are signed under their temporary names, before they replace the earlier build's.
        signed = marker.read_text(encoding="utf-8").splitlines()
        self.assertEqual(signed, [str(release / "game.exe.tmp"), str(release / "zodiac_mod_example.dll.tmp")])
        self.assertEqual(sorted(p.name for p in release.iterdir()), ["game.exe", "zodiac_mod_example.dll"])
        # echo_args without --marker exits with its argument count (1): the build fails naming the file, and leaves nothing behind.
        failing = self.new_project(self.root / "failing")
        self.append_toml(failing, "\n[build.sign]\ncommand = [%s, \"{file}\"]\n" % toml_literal(ECHO_ARGS))
        code, out, err = self.build(str(failing))
        self.assertEqual(code, 2, err)
        self.assertIn('error: signing "%s" failed' % (failing / "release" / "failing.exe"), err.splitlines())
        self.assertEqual(out.splitlines()[-1], "Build failed: 1 error")
        self.assertEqual(list((failing / "release").iterdir()), [])

    def test_build_failed_signing_keeps_the_earlier_release(self):
        project = self.new_project(self.root / "game", dynamic=["example"])
        release = project / "release"
        exe = release / "game.exe"
        dll = release / "zodiac_mod_example.dll"
        self.assert_built(self.build(str(project)), exe)
        before = exe.read_bytes()
        dll.write_bytes(b"earlier build")
        # The marker of the DLL's temporary copy is a folder, so signing the executable works and signing the DLL fails.
        (release / "zodiac_mod_example.dll.tmp.m").mkdir()
        self.append_toml(project, "\n[build.sign]\ncommand = [%s, \"--marker\", \"{file}.m\", \"{file}\"]\n" % toml_literal(ECHO_ARGS))
        code, out, err = self.build(str(project))
        self.assertEqual(code, 2, err)
        self.assertIn('error: signing "%s" failed' % dll, err.splitlines())
        self.assertEqual(exe.read_bytes(), before)
        self.assertEqual(dll.read_bytes(), b"earlier build")
        self.assertEqual(sorted(p.name for p in release.iterdir()), ["game.exe", "game.exe.tmp.m", "zodiac_mod_example.dll", "zodiac_mod_example.dll.tmp.m"])
        # Signing the executable itself fails: still nothing replaced.
        (release / "game.exe.tmp.m").unlink()
        (release / "game.exe.tmp.m").mkdir()
        code, out, err = self.build(str(project))
        self.assertEqual(code, 2, err)
        self.assertIn('error: signing "%s" failed' % exe, err.splitlines())
        self.assertEqual(exe.read_bytes(), before)
        self.assertEqual(dll.read_bytes(), b"earlier build")
        self.assertEqual(sorted(p.name for p in release.iterdir()), ["game.exe", "game.exe.tmp.m", "zodiac_mod_example.dll", "zodiac_mod_example.dll.tmp.m"])

    def test_build_folder_without_project(self):
        alone = self.root / "alone"
        write(alone / "game.zs", "void main()\n{\n}\n")
        code, out, err = self.build(str(alone))
        self.assertEqual((code, err.splitlines()), (2, ["error: zodiac.toml not found; create a project with zodiac new"]))
        self.assertFalse((alone / "release").exists())

    def test_build_lone_script_links_only_the_core(self):
        folder = self.root / "tek klasör"
        write(folder / "oyun.zs", "int main()\n{\nreturn 4;\n}\n")
        exe = folder / "release" / "oyun.exe"
        code, out, err = self.build(str(folder / "oyun.zs"))
        self.assertEqual((code, err.splitlines()), (0, [LONE_FILE_WARNING]), out)
        self.assertEqual(out.splitlines()[-1], "Build succeeded: %s" % exe)
        self.assertFalse((folder / "zodiac.toml").exists())
        self.assertEqual(run_exe(exe), (4, "", ""))
        # The kit's example module is installed but not linked in.
        self.assertNotIn(b"example_echo", exe.read_bytes())
        self.assertEqual(sorted(p.name for p in exe.parent.iterdir()), ["oyun.exe"])
        # Without --no-window and with no desktop, the warning still reaches the console, before the summary.
        code, out, err = zodiac("build", str(folder / "oyun.zs"), "--module-dir", MODULE_DIR, env=no_desktop_env())
        self.assertEqual((code, err.splitlines()), (0, [LONE_FILE_WARNING]), out)
        self.assertEqual(out.splitlines()[-1], "Build succeeded: %s" % exe)

    def test_build_lone_script_loads_no_installed_module(self):
        # lgpl_dynamic logs "lgpl_dynamic register" when it loads and allows only dynamic linking; cwd_probe has no static library. Neither
        # is loaded, linked or shipped for a lone script.
        folder = self.root / "lone"
        write(folder / "oyun.zs", "int main()\n{\nreturn 3;\n}\n")
        release = folder / "release"
        exe = release / "oyun.exe"
        code, out, err = self.build(str(folder / "oyun.zs"), "--module-dir", TEST_MODULE_DIR)
        self.assertEqual((code, err.splitlines()), (0, [LONE_FILE_WARNING]), out)
        self.assertEqual(out.splitlines()[-1], "Build succeeded: %s" % exe)
        self.assertEqual(sorted(p.name for p in release.iterdir()), ["oyun.exe"])
        self.assertEqual(run_exe(exe), (3, "", ""))

    def test_build_lone_script_calling_a_module_function_fails_with_the_note(self):
        folder = self.root / "lone"
        write(folder / "oyun.zs", 'void main()\n{\nexample_print("tek");\n}\n')
        code, out, err = self.build(str(folder / "oyun.zs"))
        self.assertEqual(code, 1, err)
        lines = err.splitlines()
        self.assertEqual(lines[0], LONE_FILE_WARNING)
        self.assertTrue(any("oyun.zs:3:" in line and "example_print" in line for line in lines), err)
        # The note follows the compiler's errors and is no error itself.
        self.assertEqual(lines[-1], LONE_FILE_NOTE)
        self.assertEqual(lines.count(LONE_FILE_NOTE), 1)
        self.assertEqual(out.splitlines()[-1], "Build failed: 1 error")
        self.assertFalse((folder / "release" / "oyun.exe").exists())

    def test_build_compile_error_in_a_project_has_no_note(self):
        project = self.new_project(self.root / "game", script='void main()\n{\nexample_print("x");\n}\n')
        code, out, err = self.build(str(project))
        self.assertEqual(code, 1, err)
        self.assertNotIn("note:", err)

    def test_build_missing_lone_script(self):
        missing = self.root / "yok.zs"
        code, out, err = self.build(str(missing))
        self.assertEqual((code, err.splitlines()), (2, ['error: script "%s" does not exist' % missing]))
        project = self.new_project(self.root / "game")
        code, out, err = self.build(str(project / "yok.zs"))
        self.assertEqual((code, err.splitlines()), (2, ['error: script "%s" does not exist' % (project / "yok.zs")]))
        self.assertFalse((project / "release").exists())

    def test_build_lone_script_relative_from_its_folder(self):
        folder = self.root / "lone"
        write(folder / "Oyun.ZS", "void main()\n{\n}\n")
        code, out, err = self.build("Oyun.ZS", cwd=folder)
        self.assertEqual((code, err.splitlines()), (0, [LONE_FILE_WARNING]), out)
        self.assertEqual(out.splitlines()[-1], "Build succeeded: %s" % (folder / "release" / "Oyun.exe"))

    def test_build_lone_script_under_an_unrelated_project_uses_that_project(self):
        project = self.new_project(self.root / "parent")
        write(project / "tools" / "lone.zs", 'void main()\n{\nexample_print("lone");\n}\n')
        # The upward search finds the parent's zodiac.toml: its project is built (its main.zs, no modules), not the lone file.
        exe = project / "release" / "parent.exe"
        self.assert_built(self.build(str(project / "tools" / "lone.zs")), exe)
        self.assertFalse((project / "tools" / "release").exists())
        self.assertEqual(run_exe(exe), (0, "", ""))

    def test_build_config_file_of_any_name(self):
        project = self.root / "ayar klasörü"
        write(project / "sahne" / "menü.zs", 'int main()\n{\nexample_print("ayar");\nreturn 6;\n}\n')
        # Packing off, so the assets are files the test can list.
        write(project / "ayar.txt", '[game]\nname = "ayarlı"\nmain = "sahne/menü.zs"\n[modules]\nstatic = ["example"]\n[build]\nassets = ["**/*.txt"]\n[pack]\nenabled = false\n')
        write(project / "notlar.txt", "not")
        release = project / "release"
        exe = release / "ayarlı.exe"
        # Started from another folder: the config file is absolute here, and every path in it resolves against its own folder.
        self.assert_built(self.build("--config=%s" % (project / "ayar.txt"), cwd=self.root), exe)
        code, out, err = run_exe(exe)
        self.assertEqual((code, out.splitlines(), err), (6, ["ayar"], ""))
        # The project file itself is never an asset.
        self.assertEqual(sorted(p.name for p in release.iterdir()), ["ayarlı.exe", "notlar.txt"])
        # A relative config path, resolved against the folder zodiac started in.
        other = self.root / "out"
        self.assert_built(self.build("--config", os.path.join("ayar klasörü", "ayar.txt"), "--output", str(other), cwd=self.root), other / "ayarlı.exe")

    def test_build_config_named_in_another_case_is_not_an_asset(self):
        project = self.root / "game"
        write(project / "main.zs", "void main()\n{\n}\n")
        write(project / "ayar.txt", '[game]\nname = "oyun"\nmain = "main.zs"\n[build]\nassets = ["**"]\n[pack]\nenabled = false\n')
        release = project / "release"
        self.assert_built(self.build("--config", str(project / "AYAR.TXT")), release / "oyun.exe")
        self.assertEqual(sorted(p.name for p in release.iterdir()), ["main.zs", "oyun.exe"])

    def test_build_config_with_script_overrides_main(self):
        project = self.root / "game"
        write(project / "main.zs", "int main()\n{\nreturn 1;\n}\n")
        write(project / "levels" / "two.zs", "int main()\n{\nreturn 2;\n}\n")
        write(project / "settings.toml", '[game]\nmain = "main.zs"\n')
        exe = project / "release" / "game.exe"
        self.assert_built(self.build(str(project / "levels" / "two.zs"), "--config", str(project / "settings.toml")), exe)
        self.assertEqual(run_exe(exe)[0], 2)

    def test_build_config_errors(self):
        project = self.root / "game"
        write(project / "main.zs", "void main()\n{\n}\n")
        write(project / "ayar.txt", '[game]\nmain = "main.zs"\n')
        code, out, err = self.build(str(project), "--config", str(project / "ayar.txt"))
        self.assertEqual((code, out, err.splitlines()), (2, "", ["error: --config cannot be combined with a project folder"]))
        missing = project / "yok.txt"
        code, out, err = self.build("--config", str(missing))
        self.assertEqual((code, err.splitlines()), (2, ['error: config file "%s" does not exist' % missing]))
        self.assertEqual(out.splitlines()[-1], "Build failed: 1 error")
        code, out, err = self.build(str(project / "yok.zs"), "--config", str(project / "ayar.txt"))
        self.assertEqual((code, err.splitlines()), (2, ['error: script "%s" does not exist' % (project / "yok.zs")]))
        write(self.root / "outside.zs", "void main()\n{\n}\n")
        code, out, err = self.build(str(self.root / "outside.zs"), "--config", str(project / "ayar.txt"))
        self.assertEqual((code, err.splitlines()), (2, ['error: script "%s" is outside the project folder "%s"' % (self.root / "outside.zs", project)]))
        code, out, err = zodiac("build", "--config")
        self.assertEqual((code, err.splitlines()), (2, ["error: --config needs a value"]))
        code, out, err = zodiac("build", "--config=")
        self.assertEqual((code, err.splitlines()), (2, ["error: --config needs a value"]))
        code, out, err = zodiac("build", "--config", "--no-window")
        self.assertEqual((code, err.splitlines()), (2, ["error: --config needs a value"]))
        code, out, err = zodiac("build", "--config=--no-window")
        self.assertEqual((code, err.splitlines()), (2, ["error: --config needs a value"]))
        code, out, err = zodiac("build", "--config", str(project / "ayar.txt"), "--config=" + str(project / "ayar.txt"))
        self.assertEqual((code, out, err.splitlines()), (2, "", ["error: --config given twice"]))
        self.assertFalse((project / "release").exists())

    def test_build_refuses_module_that_forbids_static_linking(self):
        project = self.new_project(self.root / "game", static=["lgpl_dynamic"])
        code, out, err = self.build(str(project), "--module-dir", TEST_MODULE_DIR)
        self.assertEqual(code, 2, err)
        self.assertEqual(err.splitlines(), ['error: module "lgpl_dynamic" (LGPL-2.1-only) cannot be linked statically; list it under dynamic'])
        self.assertEqual(out.splitlines()[-1], "Build failed: 1 error")
        self.assertFalse((project / "release").exists())

    def test_build_ships_dynamic_module_from_module_dir(self):
        project = self.new_project(self.root / "game", dynamic=["lgpl_dynamic"])
        code, out, err = self.build(str(project), "--module-dir", TEST_MODULE_DIR)
        self.assertEqual(code, 0, err)
        release = project / "release"
        self.assertTrue((release / "game.exe").is_file())
        self.assertEqual((release / "zodiac_mod_lgpl_dynamic.dll").read_bytes(), (Path(TEST_MODULE_DIR) / "zodiac_mod_lgpl_dynamic.dll").read_bytes())

    def vendor_folder(self, folder, modules=("vendor_dep",)):
        """A copy of the given vendor fixture modules and their third-party DLLs: vendor_lib.dll spelled Vendor_Lib.DLL (imports match
        names case-insensitively), vendor_base.dll, and a DLL nothing imports."""
        self.assertTrue(VENDOR_MODULE_DIR and Path(VENDOR_MODULE_DIR).is_dir(), VENDOR_MODULE_DIR)
        folder.mkdir(parents=True)
        source = Path(VENDOR_MODULE_DIR)
        for module in modules:
            shutil.copyfile(source / ("zodiac_mod_%s.dll" % module), folder / ("zodiac_mod_%s.dll" % module))
        shutil.copyfile(source / "vendor_lib.dll", folder / "Vendor_Lib.DLL")
        shutil.copyfile(source / "vendor_base.dll", folder / "vendor_base.dll")
        shutil.copyfile(source / "vendor_base.dll", folder / "unused.dll")
        return folder

    # vendor_dep delay-imports vendor_lib.dll, vendor_direct imports it normally; both functions return value*7.
    VENDOR_SCRIPTS = {"vendor_dep": "int main()\n{\nreturn vendor_value(3);\n}\n", "vendor_direct": "int main()\n{\nreturn vendor_direct_value(3);\n}\n"}
    VENDOR_SCRIPT = VENDOR_SCRIPTS["vendor_dep"]

    def assert_vendor_release(self, release, folder, module="vendor_dep"):
        expected = sorted(["Vendor_Lib.DLL", "game.exe", "vendor_base.dll", "zodiac_mod_%s.dll" % module])
        self.assertEqual(sorted(p.name for p in release.iterdir()), expected)
        for name in expected:
            if name != "game.exe":
                self.assertEqual((release / name).read_bytes(), (folder / name).read_bytes(), name)
        # vendor_lib.dll imports vendor_base.dll: the release runs only with both next to it.
        self.assertEqual(run_exe(release / "game.exe"), (21, "", ""))

    def test_build_ships_third_party_dlls_of_dynamic_modules(self):
        self.assertIn("vendor_lib.dll", pe_imports(Path(VENDOR_MODULE_DIR) / "zodiac_mod_vendor_dep.dll"))
        self.assertIn("vendor_base.dll", pe_imports(Path(VENDOR_MODULE_DIR) / "vendor_lib.dll"))
        mods = self.vendor_folder(self.root / "mods")
        project = self.new_project(self.root / "game", dynamic=["vendor_dep"], script=self.VENDOR_SCRIPT)
        marker = self.root / "signed.txt"
        self.append_toml(project, "\n[build.sign]\ncommand = [%s, \"--marker\", %s, \"{file}\"]\n" % (toml_literal(ECHO_ARGS), toml_literal(str(marker))))
        release = project / "release"
        self.assert_built(self.build(str(project), "--module-dir", str(mods)), release / "game.exe")
        self.assert_vendor_release(release, mods)
        # The third-party DLLs are signed like the module, under their temporary names, in the order they were found.
        signed = marker.read_text(encoding="utf-8").splitlines()
        self.assertEqual(signed, [str(release / name) for name in ("game.exe.tmp", "zodiac_mod_vendor_dep.dll.tmp", "Vendor_Lib.DLL.tmp", "vendor_base.dll.tmp")])

    def test_build_ships_third_party_dlls_of_plugins(self):
        project = self.new_project(self.root / "game", dynamic=["vendor_dep"], script=self.VENDOR_SCRIPT)
        plugins = self.vendor_folder(project / "plugins")
        release = project / "release"
        self.assert_built(self.build(str(project)), release / "game.exe")
        self.assert_vendor_release(release, plugins)

    def test_build_ships_third_party_dlls_of_a_normally_importing_module(self):
        self.assertIn("vendor_lib.dll", pe_imports(Path(VENDOR_MODULE_DIR) / "zodiac_mod_vendor_direct.dll"))
        mods = self.vendor_folder(self.root / "mods", modules=("vendor_direct",))
        project = self.new_project(self.root / "game", dynamic=["vendor_direct"], script=self.VENDOR_SCRIPTS["vendor_direct"])
        release = project / "release"
        self.assert_built(self.build(str(project), "--module-dir", str(mods)), release / "game.exe")
        self.assert_vendor_release(release, mods, module="vendor_direct")

    def test_build_refuses_a_module_whose_third_party_dll_is_missing(self):
        # A normal import (the BASS/FMOD case: the module could not even load) and a delay import; a missing DLL the module imports, then
        # one that DLL imports (found recursively). The build names it before loading anything and keeps the earlier release.
        for module in ("vendor_direct", "vendor_dep"):
            mods = self.vendor_folder(self.root / module / "mods", modules=(module,))
            project = self.new_project(self.root / module / "game", dynamic=[module], script=self.VENDOR_SCRIPTS[module])
            release = project / "release"
            self.assert_built(self.build(str(project), "--module-dir", str(mods)), release / "game.exe")
            before = {p.name: p.read_bytes() for p in release.iterdir()}
            library = mods / ("zodiac_mod_%s.dll" % module)
            for missing, needed in (("Vendor_Lib.DLL", "vendor_lib.dll"), ("vendor_base.dll", "vendor_base.dll")):
                with self.subTest(module=module, missing=missing):
                    held = self.root / module / missing
                    (mods / missing).rename(held)
                    code, out, err = self.build(str(project), "--module-dir", str(mods))
                    held.rename(mods / missing)
                    self.assertEqual((code, err.splitlines()), (2, ['error: module "%s" needs "%s", which is not next to "%s"' % (module, needed, library)]), out)
                    self.assertEqual(out.splitlines()[-1], "Build failed: 1 error")
                    self.assertEqual({p.name: p.read_bytes() for p in release.iterdir()}, before)

    def test_build_refuses_a_third_party_dll_that_is_not_x64(self):
        mods = self.vendor_folder(self.root / "mods")
        # vendor_base.dll with the x86 machine (0x14C) in its file header.
        base = mods / "vendor_base.dll"
        data = bytearray(base.read_bytes())
        struct.pack_into("<H", data, struct.unpack_from("<I", data, 0x3C)[0] + 4, 0x14C)
        base.write_bytes(bytes(data))
        project = self.new_project(self.root / "game", dynamic=["vendor_dep"], script=self.VENDOR_SCRIPT)
        code, out, err = self.build(str(project), "--module-dir", str(mods))
        self.assertEqual((code, err.splitlines()), (2, ['error: "%s" is not a 64-bit (x64) Windows DLL' % base]), out)
        self.assertEqual(out.splitlines()[-1], "Build failed: 1 error")
        self.assertFalse((project / "release").exists())

    def test_build_ships_a_dll_two_modules_need_once(self):
        # vendor_dep and vendor_direct each come with their own copy of vendor_lib.dll and vendor_base.dll, in folders of their own.
        first = self.vendor_folder(self.root / "first", modules=("vendor_dep",))
        second = self.vendor_folder(self.root / "second", modules=("vendor_direct",))
        project = self.new_project(self.root / "game", dynamic=["vendor_dep", "vendor_direct"], script="int main()\n{\nreturn vendor_value(1)+vendor_direct_value(2);\n}\n")
        release = project / "release"
        self.assert_built(self.build(str(project), "--module-dir", str(first), "--module-dir", str(second)), release / "game.exe")
        self.assertEqual(sorted(p.name for p in release.iterdir()), ["Vendor_Lib.DLL", "game.exe", "vendor_base.dll", "zodiac_mod_vendor_dep.dll", "zodiac_mod_vendor_direct.dll"])
        self.assertEqual(run_exe(release / "game.exe"), (21, "", ""))
        before = {p.name: p.read_bytes() for p in release.iterdir()}
        # The same name with other bytes: one of the modules would get the wrong DLL, so the build stops.
        with open(second / "vendor_base.dll", "ab") as extra:
            extra.write(b"\0")
        code, out, err = self.build(str(project), "--module-dir", str(first), "--module-dir", str(second))
        self.assertEqual((code, err.splitlines()), (2, ['error: "vendor_base.dll" is shipped by "vendor_dep" and "vendor_direct" with different contents']), out)
        self.assertEqual(out.splitlines()[-1], "Build failed: 1 error")
        self.assertEqual({p.name: p.read_bytes() for p in release.iterdir()}, before)

    def test_build_static_plugin_needs_library_and_toolchain_stamp(self):
        project = self.new_project(self.root / "game", static=["gpl_static"])
        warning = 'warning: module "gpl_static" is linked statically under license GPL-3.0-only; check its terms'
        code, out, err = self.build(str(project), "--module-dir", TEST_MODULE_DIR)
        self.assertEqual(code, 2, err)
        self.assertEqual(err.splitlines(), [warning, 'error: module "gpl_static" has no static library in the kit or in plugins/static; list it under dynamic'])
        static_dir = project / "plugins" / "static"
        static_dir.mkdir(parents=True)
        write(static_dir / "zodiac_mod_gpl_static.a", "!<arch>\n")
        stamp_error = 'error: plugin "gpl_static" was not built with this kit\'s toolchain; list it under dynamic'
        code, out, err = self.build(str(project), "--module-dir", TEST_MODULE_DIR)
        self.assertEqual((code, err.splitlines()), (2, [warning, stamp_error]))
        write(static_dir / "zodiac_mod_gpl_static.stamp", "some other toolchain")
        code, out, err = self.build(str(project), "--module-dir", TEST_MODULE_DIR)
        self.assertEqual((code, err.splitlines()), (2, [warning, stamp_error]))
        self.assertFalse((project / "release").exists())

    def test_build_link_failure_counts_as_one_error(self):
        project = self.new_project(self.root / "game", static=["gpl_static"])
        static_dir = project / "plugins" / "static"
        static_dir.mkdir(parents=True)
        # A stamped plugin whose archive is not an archive: the stamp check passes and ld.lld refuses the input.
        write(static_dir / "zodiac_mod_gpl_static.a", "not an archive\n")
        toolchain = Path(EXE).resolve().parent.parent / "lib" / "release" / "toolchain.txt"
        (static_dir / "zodiac_mod_gpl_static.stamp").write_bytes(toolchain.read_bytes())
        code, out, err = self.build(str(project), "--module-dir", TEST_MODULE_DIR)
        self.assertEqual(code, 2, err)
        # The fixture module logs "gpl_static register" and "gpl_static shutdown" at info level; they are not part of the link report.
        lines = [line for line in err.splitlines() if not line.startswith("gpl_static ")]
        self.assertIn("error: linking failed", lines)
        after = lines[lines.index("error: linking failed") + 1:]
        # Every line of the linker's output is a continuation line, so the summary counts the failure once.
        self.assertTrue(after and all(line.startswith("  ") for line in after), err)
        self.assertEqual(out.splitlines()[-1], "Build failed: 1 error")
        self.assertEqual(list((project / "release").iterdir()), [])

    def test_build_replace_failure_that_is_not_a_lock(self):
        import stat
        exe = self.build_hello()
        before = exe.read_bytes()
        os.chmod(exe, stat.S_IREAD)
        self.addCleanup(os.chmod, exe, stat.S_IREAD | stat.S_IWRITE)
        # The system's reason is decoded strictly: it is UTF-8 whatever the system's language.
        code, out, err = self.build(str(exe.parent.parent))
        self.assertEqual(code, 2, err)
        prefix = 'error: cannot replace "%s": ' % exe
        lines = [line for line in err.splitlines() if line.startswith(prefix)]
        self.assertEqual(len(lines), 1, err)
        self.assertNotIn("in use", lines[0])
        self.assertEqual(exe.read_bytes(), before)
        self.assertEqual(sorted(p.name for p in exe.parent.iterdir()), ["hello.exe"])
        # A folder in the executable's place is not "in use" either.
        project = self.new_project(self.root / "folder")
        (project / "release" / "folder.exe").mkdir(parents=True)
        code, out, err = self.build(str(project))
        self.assertEqual(code, 2, err)
        prefix = 'error: cannot replace "%s": ' % (project / "release" / "folder.exe")
        lines = [line for line in err.splitlines() if line.startswith(prefix)]
        self.assertEqual(len(lines), 1, err)
        self.assertNotIn("in use", lines[0])

    def test_build_output_inside_project_is_not_an_asset(self):
        project = self.new_project(self.root / "game")
        # Packing off, so the assets are files the test can list; the packed case leaves the output folder out the same way.
        self.append_toml(project, '\n[build]\nassets = ["**/*"]\n[pack]\nenabled = false\n')
        write(project / "data" / "a.txt", "a")
        # The same folder spelled with a different case: Windows paths compare case-insensitively.
        output = Path(str(project).upper()) / "Out"
        for _ in range(2):
            self.assert_built(self.build(str(project), "--output", str(output)), output / "game.exe")
        files = sorted(p.relative_to(project / "out").as_posix() for p in (project / "out").rglob("*") if p.is_file())
        self.assertEqual(files, ["data/a.txt", "game.exe", "main.zs"])

    def test_hello_size_gate(self):
        # Debug builds stay unoptimized for debugging, so only a Release build says what a shipped hello weighs.
        if BUILD_CONFIG != "Release":
            self.skipTest("size gate measures release builds only")
        exe = self.build_hello()
        size = exe.stat().st_size
        self.assertLessEqual(size, SIZE_LIMIT, "core-only hello release is %d bytes" % size)

    def test_hello_imports_no_toolchain_runtime(self):
        exe = self.build_hello()
        imports = pe_imports(exe)
        self.assertTrue(imports, "no imports found")
        for name in ("libc++.dll", "libunwind.dll", "libwinpthread-1.dll"):
            self.assertNotIn(name, imports)

    def test_build_without_flag_and_no_desktop_prints_summary(self):
        project = self.new_project(self.root / "hello")
        exe = project / "release" / "hello.exe"
        self.assert_built(zodiac("build", str(project), "--module-dir", MODULE_DIR, env=no_desktop_env()), exe)
        broken = self.new_project(self.root / "broken", script="void main()\n{\nint(zz);\n}\n")
        code, out, err = zodiac("build", str(broken), "--module-dir", MODULE_DIR, env=no_desktop_env())
        self.assertEqual(code, 1)
        self.assertIn("main.zs:3:", err)
        self.assertRegex(out.splitlines()[-1], r"^Build failed: [1-9][0-9]* errors?$")

    def test_zodiacw_is_a_gui_twin_reporting_to_redirected_streams(self):
        self.assertTrue(GUI_EXE and Path(GUI_EXE).is_file(), "ZODIACW_EXE is not set or missing: %r" % GUI_EXE)
        self.assertEqual(Path(GUI_EXE).parent, Path(EXE).parent)
        self.assertEqual(pe_subsystem(GUI_EXE), 2)
        self.assertEqual(pe_subsystem(EXE), 3)
        project = self.new_project(self.root / "hello")
        exe = project / "release" / "hello.exe"
        self.assert_built(zodiac("build", str(project), "--module-dir", MODULE_DIR, exe=GUI_EXE, env=no_desktop_env()), exe)
        self.assertEqual(run_exe(exe), (0, "", ""))
        code, out, err = zodiac("build", str(project), "--no-window", "--module-dir", MODULE_DIR, "--output", str(self.root / "out"), exe=GUI_EXE)
        self.assertEqual((code, err), (0, ""), out)
        self.assertEqual(out.splitlines()[-1], "Build succeeded: %s" % (self.root / "out" / "hello.exe"))
        code, out, err = zodiac("build", "a", "b", exe=GUI_EXE)
        self.assertEqual((code, out), (2, ""))
        self.assertEqual(err.splitlines(), ["error: build takes at most one project directory or script; run zodiac help"])

    def test_build_usage_errors(self):
        code, out, err = zodiac("build", "a", "b")
        self.assertEqual((code, out), (2, ""))
        self.assertEqual(err.splitlines(), ["error: build takes at most one project directory or script; run zodiac help"])
        code, out, err = zodiac("build", "--output")
        self.assertEqual((code, err.splitlines()), (2, ["error: --output needs a value"]))
        code, out, err = zodiac("run", "--output", str(self.root / "out"))
        self.assertEqual((code, out, err.splitlines()), (2, "", ["error: --output applies to build only; run zodiac help"]))
        code, out, err = zodiac("modules", "--no-window")
        self.assertEqual((code, out, err.splitlines()), (2, "", ["error: --no-window applies to run and build only; run zodiac help"]))


if __name__ == "__main__":
    unittest.main()
