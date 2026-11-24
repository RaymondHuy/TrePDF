import base64
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import macos_signing as signing


class DeveloperIDTests(unittest.TestCase):
    def test_missing_secrets_fail_without_fallback(self):
        with patch.dict(os.environ, {"APPLE_CERTIFICATE_PASSWORD": "test-value"}, clear=True):
            with self.assertRaisesRegex(ValueError, "APPLE_CERTIFICATE_BASE64") as error:
                signing.take_credentials()
            self.assertNotIn("test-value", str(error.exception))
            self.assertNotIn("APPLE_CERTIFICATE_PASSWORD", os.environ)

    def test_wrong_certificate_restores_keychain_and_removes_private_files(self):
        inputs = dict.fromkeys(signing.SECRET_NAMES, "fixture")
        inputs.update(APPLE_CERTIFICATE_BASE64=base64.b64encode(b"certificate").decode(),
                      APPLE_API_KEY_BASE64=base64.b64encode(b"-----BEGIN PRIVATE KEY-----\nfixture").decode())
        operations = []
        def security(operation, *arguments):
            operations.append((operation, *arguments))
            if operation == "find-identity":
                return '  1) ' + 'A' * 40 + ' "Apple Distribution: Wrong certificate"'
            return '"/original.keychain-db"'
        with patch.object(signing, "security", side_effect=security):
            with self.assertRaisesRegex(ValueError, "Developer ID Application"):
                with signing.signing_assets(inputs):
                    self.fail("Expected signing setup to fail")
        restore, delete = operations[-2:]
        self.assertEqual(restore, ("list-keychains", "-d", "user", "-s", "/original.keychain-db"))
        self.assertEqual(delete[0], "delete-keychain")
        self.assertFalse(Path(delete[1]).parent.exists())

    def test_rejected_submission_stops_before_stapling(self):
        result = subprocess.CompletedProcess([], 0, json.dumps({"status": "Invalid", "id": "submission"}))
        with patch.object(subprocess, "run", return_value=result) as run, redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "did not accept"):
                signing.notarize(Path("TrePDF.dmg"), Path("TrePDF.dmg"), [])
        self.assertEqual([call.args[0][1:3] for call in run.call_args_list],
                         [["notarytool", "submit"], ["notarytool", "log"]])
