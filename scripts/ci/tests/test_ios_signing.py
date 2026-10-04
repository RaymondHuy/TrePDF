"""Check that CI refuses expired or mismatched signing profiles before importing keys."""

from copy import deepcopy
from datetime import datetime
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ios_signing import validate_profile


class SigningProfileTests(unittest.TestCase):
    def setUp(self):
        self.profile = {
            "ExpirationDate": datetime(2100, 1, 1),
            "TeamIdentifier": ["ABCDE12345"],
            # Legacy app ID prefixes need not match the development team ID.
            "ApplicationIdentifierPrefix": ["OLDID12345"],
            "UUID": "12345678-1234-1234-1234-123456789ABC",
            "ProvisionedDevices": ["test-ipad-udid"],
            "Entitlements": {
                "application-identifier": "OLDID12345.com.example.trepdf",
                "get-task-allow": True,
            },
        }

    def test_accepts_matching_development_profile_with_legacy_prefix(self):
        self.assertEqual(validate_profile(self.profile, "com.example.trepdf", "development"),
                         ("ABCDE12345", self.profile["UUID"]))

    def test_rejects_expired_profile(self):
        self.profile["ExpirationDate"] = datetime(2000, 1, 1)
        with self.assertRaisesRegex(ValueError, "expired"):
            validate_profile(self.profile, "com.example.trepdf", "development")

    def test_rejects_wrong_bundle_and_wildcard(self):
        for identifier in ("OLDID12345.com.other.app", "OLDID12345.*"):
            with self.subTest(identifier=identifier):
                self.profile["Entitlements"]["application-identifier"] = identifier
                with self.assertRaisesRegex(ValueError, "explicit"):
                    validate_profile(self.profile, "com.example.trepdf", "development")

    def test_development_certificate_cannot_use_distribution_methods(self):
        for method in ("ad-hoc", "app-store-connect"):
            with self.subTest(method=method), self.assertRaises(ValueError):
                validate_profile(self.profile, "com.example.trepdf", method)

    def test_ad_hoc_requires_registered_devices(self):
        self.profile["Entitlements"]["get-task-allow"] = False
        validate_profile(self.profile, "com.example.trepdf", "ad-hoc")
        self.profile.pop("ProvisionedDevices")
        with self.assertRaises(ValueError):
            validate_profile(self.profile, "com.example.trepdf", "ad-hoc")

    def test_app_store_rejects_device_and_enterprise_profiles(self):
        self.profile["Entitlements"]["get-task-allow"] = False
        with self.assertRaises(ValueError):
            validate_profile(self.profile, "com.example.trepdf", "app-store-connect")
        self.profile.pop("ProvisionedDevices")
        validate_profile(self.profile, "com.example.trepdf", "app-store-connect")
        self.profile["ProvisionsAllDevices"] = True
        with self.assertRaises(ValueError):
            validate_profile(self.profile, "com.example.trepdf", "app-store-connect")

    def test_rejects_missing_identity_metadata(self):
        for key in ("TeamIdentifier", "UUID", "ExpirationDate", "ApplicationIdentifierPrefix"):
            profile = deepcopy(self.profile)
            del profile[key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_profile(profile, "com.example.trepdf", "development")


if __name__ == "__main__":
    unittest.main()
