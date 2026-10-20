"""Regression checks for selecting a consistent Visual Studio toolchain."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("setup", Path(__file__).resolve().parents[1] / "scripts/setup.py")
SETUP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SETUP)


class VisualStudioSelectionTests(unittest.TestCase):
    def test_clang_selection_replaces_mismatched_cache_and_preserves_outputs(self):
        instance = "C:/Program Files/VS/2022/BuildTools"
        generator = "Visual Studio 17 2022"
        for old_generator, old_instance in (
            ("Visual Studio 18 2026", "C:/Program Files/VS/18/BuildTools"),
            (generator, "C:/Program Files/VS/2022/Other"),
            (generator, instance),
        ):
            with self.subTest(old_generator=old_generator, old_instance=old_instance), tempfile.TemporaryDirectory() as temp:
                build = Path(temp)
                cache = build / "CMakeCache.txt"
                cache.write_text(f"CMAKE_GENERATOR:INTERNAL={old_generator}\n"
                                 f"CMAKE_GENERATOR_INSTANCE:INTERNAL={old_instance}\n"
                                 "CMAKE_GENERATOR_TOOLSET:INTERNAL=ClangCL\n")
                (build / "CMakeFiles").mkdir()
                (build / "gen").mkdir()
                generated = build / "gen/game.cpp"
                generated.write_text("// preserved game code\n")
                with patch.object(SETUP, "WINDOWS", True), \
                     patch.object(SETUP.shutil, "which", return_value=None), \
                     patch.dict(os.environ, {"M2_COMPILER": "clang"}), \
                     patch.object(SETUP, "vs_with_clang", return_value={
                         "installationVersion": "17.14.1", "installationPath": instance.replace("/", "\\")}), \
                     patch.object(SETUP.subprocess, "run") as query, \
                     patch.object(SETUP, "run") as run:
                    query.return_value.stdout = json.dumps({"generators": [
                        {"name": "Visual Studio 18 2026"}, {"name": generator}]})
                    SETUP.configure_and_build(str(build))
                configure = run.call_args_list[0].args[0]
                if (old_generator, old_instance) == (generator, instance):
                    self.assertTrue(cache.exists())
                    self.assertNotIn("-G", configure)
                else:
                    self.assertFalse(cache.exists())
                    self.assertFalse((build / "CMakeFiles").exists())
                    self.assertEqual(configure[configure.index("-G") + 1], generator)
                    self.assertIn("-DCMAKE_GENERATOR_INSTANCE=" + instance, configure)
                    self.assertEqual(configure[configure.index("-T") + 1], "ClangCL")
                self.assertEqual(generated.read_text(), "// preserved game code\n")

    def test_discovery_requires_vs2022_compiler_and_msbuild_integration(self):
        with patch.object(SETUP.os.path, "exists", return_value=True), \
             patch.object(SETUP.subprocess, "run") as query:
            query.return_value.stdout = "[]"
            self.assertIsNone(SETUP.vs_with_clang())
        command = query.call_args.args[0]
        self.assertEqual(command[command.index("-version") + 1], "[17,18)")
        self.assertIn("Microsoft.VisualStudio.Component.VC.Llvm.Clang", command)
        self.assertIn("Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset", command)

    def test_forced_clang_stops_when_vs2022_clang_is_missing(self):
        with patch.object(SETUP, "WINDOWS", True), \
             patch.object(SETUP.shutil, "which", return_value=None), \
             patch.dict(os.environ, {"M2_COMPILER": "clang"}), \
             patch.object(SETUP, "vs_with_clang", return_value=None), \
             patch.object(SETUP, "run") as run:
            with self.assertRaisesRegex(SystemExit, "Visual Studio 2022 has no Clang tools"):
                SETUP.configure_and_build("unused-build-directory")
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
