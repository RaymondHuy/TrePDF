import argparse
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import macos_app_store as store


class MacAppStoreTests(unittest.TestCase):
    def test_build_numbers_and_distribution_identities(self):
        for value in ("0", "0001", "1.100", "1.2.3.4", "1-beta", "10000"):
            with self.assertRaises(argparse.ArgumentTypeError):
                store.build_number(value)
        self.assertEqual(store.build_number("1234.0.12"), "1234.0.12")
        digest = "A" * 40
        name = "Apple Distribution: Example (ABCD123456)"
        listing = f'  1) {digest} "{name}"'
        self.assertEqual(store.identity(listing, ("Apple Distribution: ",)),
                         (digest, name, "ABCD123456"))
        for invalid in (listing.replace("Apple Distribution", "Developer ID Application"),
                        listing + "\n" + listing):
            with self.assertRaises(ValueError):
                store.identity(invalid, ("Apple Distribution: ",))

    def test_publish_only_the_verified_installer(self):
        installer = "Mac Installer Distribution: Example (ABCD123456)"
        chain = "test.keychain-db"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pkg = root / "Tre PDF.pkg"
            app = root / "Tre PDF.app"

            def run(command, *, check, timeout):
                self.assertTrue(check)
                self.assertGreater(timeout, 0)
                self.assertEqual(command[:-1], ["productbuild", "--component", str(app), "/Applications"])
                Path(command[-1]).write_bytes(b"payload")

            def sign(command):
                self.assertEqual(command[:-2], ["productsign", "--sign", installer, "--keychain", chain])
                self.assertEqual(Path(command[-2]).read_bytes(), b"payload")
                Path(command[-1]).write_bytes(b"signed")

            for valid in (False, True):
                with self.subTest(valid=valid):
                    pkg.write_bytes(b"previous")

                    def verify(command, *, text, timeout):
                        self.assertTrue(text)
                        self.assertGreater(timeout, 0)
                        self.assertEqual(command[:-1], ["pkgutil", "--check-signature"])
                        self.assertEqual(Path(command[-1]).read_bytes(), b"signed")
                        self.assertEqual(pkg.read_bytes(), b"previous")
                        return installer if valid else "Different certificate"

                    with patch.object(store.subprocess, "run", side_effect=run), \
                         patch.object(store, "run_productsign", side_effect=sign), \
                         patch.object(store.subprocess, "check_output", side_effect=verify) as verification:
                        if valid:
                            store.sign_installer(app, pkg, installer, chain)
                        else:
                            with self.assertRaisesRegex(RuntimeError, "certificate"):
                                store.sign_installer(app, pkg, installer, chain)
                        verification.assert_called_once()
                    self.assertEqual(pkg.read_bytes(), b"signed" if valid else b"previous")
                    self.assertEqual(list(root.iterdir()), [pkg])

    def test_timed_out_signer_logs_only_functions_and_terminates(self):
        trace = "Path: /private/example\nCall graph:\n  +   42 SecKeyCreateSignature  (in Security) + 1\n"
        with patch.object(store.subprocess, "Popen") as popen, \
             patch.object(store.subprocess, "check_output", return_value=trace) as sample, \
             patch("builtins.print") as logged:
            signer = popen.return_value.__enter__.return_value
            signer.pid = 123
            signer.returncode = None
            signer.wait.side_effect = subprocess.TimeoutExpired("productsign", 300)
            with self.assertRaisesRegex(RuntimeError, "5-minute"):
                store.run_productsign(["productsign"])
            popen.assert_called_once_with(["productsign"], stdin=subprocess.DEVNULL)
            self.assertEqual(sample.call_args.args[0][:2], ["/usr/bin/sample", "123"])
            logged.assert_called_once_with("productsign stack: SecKeyCreateSignature", flush=True)
            signer.kill.assert_called_once()

    @unittest.skipUnless(sys.platform == "darwin", "uses macOS packaging tools")
    def test_universal_bundle_entitlements_and_product_payload(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            app = root / "Tre PDF.app"
            contents = app / "Contents"
            (contents / "MacOS").mkdir(parents=True)
            source = root / "main.c"
            source.write_text("int main(void) { return 0; }\n")
            subprocess.run(["clang", "-arch", "arm64", "-arch", "x86_64",
                            "-mmacosx-version-min=13.0", source, "-o", contents / "MacOS/Tre PDF"],
                           check=True)
            info = {"CFBundleIdentifier": store.BUNDLE_ID, "CFBundleShortVersionString": "1.2.3",
                    "CFBundlePackageType": "APPL", "CFBundleExecutable": "Tre PDF",
                    "LSMinimumSystemVersion": "13.0", "LSApplicationCategoryType": "public.app-category.productivity"}
            plist = contents / "Info.plist"
            plist.write_bytes(plistlib.dumps(info))
            store.prepare_bundle(app, "1.2.3", "15")
            self.assertEqual(plistlib.loads(plist.read_bytes())["CFBundleVersion"], "15")
            store.sign_macos_bundle(app, ["--sign", "-", "--timestamp=none"], entitlements=store.ENTITLEMENTS)
            signed = subprocess.check_output(["codesign", "-d", "--entitlements", "-", "--xml", app])
            self.assertEqual(plistlib.loads(signed), plistlib.loads(store.ENTITLEMENTS.read_bytes()))
            package = root / "test.pkg"
            subprocess.run(["productbuild", "--component", app, "/Applications", package], check=True)
            subprocess.run(["pkgutil", "--expand", package, root / "expanded"], check=True)
            self.assertIn(store.BUNDLE_ID, (root / "expanded/Distribution").read_text())

    def test_app_and_installer_keys_are_imported_in_separate_keychains(self):
        actions = []
        active = set()

        def security(operation, *args):
            actions.append((operation, args))
            if operation == "create-keychain":
                self.assertFalse(active)
                active.add(args[-1])
            elif operation == "delete-keychain":
                active.remove(args[-1])
            elif operation == "find-identity":
                self.assertIn(args[-1], active)
                kind = "Apple Distribution" if "codesigning" in args else "3rd Party Mac Developer Installer"
                return f'  1) {"A" * 40} "{kind}: Example (ABCD123456)"'
            return ""

        with tempfile.TemporaryDirectory() as output, \
             patch.object(store, "take_credentials", return_value=dict.fromkeys(store.SECRETS, "dGVzdA==")), \
             patch.object(store, "prepare_bundle"), patch.object(store, "sign_macos_bundle"), \
             patch.object(store.subprocess, "check_output", return_value=store.ENTITLEMENTS.read_bytes()), \
             patch("macos_signing.security", side_effect=security), \
             patch.object(store, "security", side_effect=security), patch.object(store, "sign_installer") as sign:
            store.package(Path("Tre PDF.app"), Path(output), "1.0.0", "1")
        imports = [args for op, args in actions if op == "import"]
        self.assertEqual(len(imports), 2)
        app_chain, installer_chain = [args[args.index("-k") + 1] for args in imports]
        self.assertNotEqual(app_chain, installer_chain)
        self.assertIn("/usr/bin/codesign", imports[0])
        self.assertIn("/usr/bin/productsign", imports[1])
        partitions = [args for op, args in actions if op == "set-key-partition-list"]
        self.assertEqual([args[-1] for args in partitions], [app_chain, installer_chain])
        for args in partitions:
            self.assertNotIn("-s", args)
        self.assertEqual(sign.call_args.args[-1], installer_chain)
        self.assertFalse(active)
