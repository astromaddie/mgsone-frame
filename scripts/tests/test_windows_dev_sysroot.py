import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "windows_dev" / "normalize_frame_sysroot.py"
SPEC = importlib.util.spec_from_file_location("normalize_frame_sysroot", MODULE_PATH)
normalize_frame_sysroot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(normalize_frame_sysroot)


class NormalizeFrameSysrootTests(unittest.TestCase):
    def make_sysroot(self, base: Path) -> Path:
        root = base / "sysroot"
        (root / "lib/aarch64-linux-gnu").mkdir(parents=True)
        (root / "usr/lib/aarch64-linux-gnu").mkdir(parents=True)
        return root

    def test_absolute_frame_link_becomes_relative_and_stays_in_sysroot(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sysroot(Path(temporary))
            library = root / "lib/aarch64-linux-gnu/libc-2.43.so"
            library.write_bytes(b"frame libc")
            link = root / "usr/lib/aarch64-linux-gnu/libc.so.6"
            link.symlink_to("/lib/aarch64-linux-gnu/libc-2.43.so")

            changed = normalize_frame_sysroot.normalize_sysroot(root)

            self.assertEqual(changed, 1)
            self.assertFalse(link.readlink().is_absolute())
            self.assertEqual(link.resolve(), library.resolve())

    def test_absolute_link_chain_resolves_inside_sysroot(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sysroot(Path(temporary))
            library = root / "lib/aarch64-linux-gnu/libc-2.43.so"
            library.write_bytes(b"frame libc")
            first = root / "usr/lib/aarch64-linux-gnu/libc.so.6"
            first.symlink_to("/lib/aarch64-linux-gnu/libc-2.43.so")
            second = root / "usr/lib/aarch64-linux-gnu/libc.so"
            second.symlink_to("/usr/lib/aarch64-linux-gnu/libc.so.6")

            changed = normalize_frame_sysroot.normalize_sysroot(root)

            self.assertEqual(changed, 2)
            self.assertFalse(first.readlink().is_absolute())
            self.assertFalse(second.readlink().is_absolute())
            self.assertEqual(first.resolve(), library.resolve())
            self.assertEqual(second.resolve(), library.resolve())

    def test_escape_is_rejected_without_rewriting_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sysroot(Path(temporary))
            link = root / "usr/lib/aarch64-linux-gnu/libc.so.6"
            link.symlink_to("/etc/passwd")

            with self.assertRaises(normalize_frame_sysroot.SysrootError):
                normalize_frame_sysroot.normalize_sysroot(root)

            self.assertEqual(link.readlink().as_posix(), "/etc/passwd")

    def test_relative_dangling_link_is_rejected_without_rewriting_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sysroot(Path(temporary))
            absolute = root / "usr/lib/aarch64-linux-gnu/libc.so.6"
            absolute.symlink_to("/lib/aarch64-linux-gnu/libc-2.43.so")
            (root / "lib/aarch64-linux-gnu/libc-2.43.so").write_bytes(b"libc")
            dangling = root / "usr/lib/aarch64-linux-gnu/libstdc++.so.6"
            dangling.symlink_to("libstdc++.so.6.0.32")

            with self.assertRaises(normalize_frame_sysroot.SysrootError):
                normalize_frame_sysroot.normalize_sysroot(root)

            self.assertEqual(absolute.readlink().as_posix(), "/lib/aarch64-linux-gnu/libc-2.43.so")
            self.assertEqual(dangling.readlink().as_posix(), "libstdc++.so.6.0.32")

    def test_relative_escape_is_rejected_without_rewriting_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sysroot(Path(temporary))
            absolute = root / "usr/lib/aarch64-linux-gnu/libc.so.6"
            absolute.symlink_to("/lib/aarch64-linux-gnu/libc-2.43.so")
            (root / "lib/aarch64-linux-gnu/libc-2.43.so").write_bytes(b"libc")
            escape = root / "usr/lib/aarch64-linux-gnu/libstdc++.so.6"
            escape.symlink_to("../../../../etc/passwd")

            with self.assertRaises(normalize_frame_sysroot.SysrootError):
                normalize_frame_sysroot.normalize_sysroot(root)

            self.assertEqual(absolute.readlink().as_posix(), "/lib/aarch64-linux-gnu/libc-2.43.so")
            self.assertEqual(escape.readlink().as_posix(), "../../../../etc/passwd")

    def test_parent_components_after_symlink_cannot_escape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sysroot(Path(temporary))
            link_dir = root / "usr/lib/aarch64-linux-gnu"
            alias = link_dir / "alias"
            alias.symlink_to("/")
            target = link_dir / "escape"
            target.symlink_to("alias/../etc/passwd")
            (root / "usr/lib/etc").mkdir(parents=True)
            (root / "usr/lib/etc/passwd").write_bytes(b"misleading lexical target")

            with self.assertRaises(normalize_frame_sysroot.SysrootError):
                normalize_frame_sysroot.normalize_sysroot(root)

            self.assertEqual(alias.readlink().as_posix(), "/")
            self.assertEqual(target.readlink().as_posix(), "alias/../etc/passwd")


if __name__ == "__main__":
    unittest.main()
