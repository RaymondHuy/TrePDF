"""Temporary iOS signing assets for the hosted UAT runner; never log secrets."""

from __future__ import annotations

import base64
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import plistlib
import re
import secrets
import shlex
import subprocess
import tempfile


def validate_profile(profile: dict, bundle_id: str, method: str) -> tuple[str, str]:
    expiry = profile.get("ExpirationDate")
    if not isinstance(expiry, datetime) or expiry.replace(tzinfo=timezone.utc) <= datetime.now(timezone.utc):
        raise ValueError("The iOS provisioning profile has expired")
    teams = profile.get("TeamIdentifier", [])
    if len(teams) != 1 or not re.fullmatch(r"[A-Z0-9]{10}", teams[0]):
        raise ValueError("The provisioning profile must identify one Apple development team")
    entitlements = profile.get("Entitlements", {})
    prefixes = profile.get("ApplicationIdentifierPrefix", [])
    if not any(entitlements.get("application-identifier") == f"{prefix}.{bundle_id}" for prefix in prefixes):
        raise ValueError("Use an explicit provisioning profile matching the requested bundle ID")
    development = entitlements.get("get-task-allow", False)
    devices = profile.get("ProvisionedDevices", [])
    if method == "development" and (not development or not devices):
        raise ValueError("Development export needs a development profile with registered devices")
    if method == "ad-hoc" and (development or not devices):
        raise ValueError("Ad hoc export needs a distribution profile with registered devices")
    if method == "app-store-connect" and (development or devices or profile.get("ProvisionsAllDevices")):
        raise ValueError("App Store export needs an App Store Connect distribution profile")
    uuid = profile.get("UUID", "")
    if not re.fullmatch(r"[A-Fa-f0-9-]{36}", uuid):
        raise ValueError("Invalid provisioning profile UUID")
    return teams[0], uuid


@contextmanager
def signing_assets(bundle_id: str, method: str):
    if method == "unsigned":
        yield None
        return
    required = ("IOS_CERTIFICATE_P12_BASE64", "IOS_CERTIFICATE_PASSWORD", "IOS_PROVISION_PROFILE_BASE64")
    if any(name not in os.environ or not os.environ[name] for name in required):
        raise ValueError("Signed iPad builds require all three IOS_* signing secrets; see docs/ipad-build.md")
    def run(args):
        try:
            return subprocess.run(args, check=True, capture_output=True)
        except subprocess.CalledProcessError:
            # Command arguments can contain P12/keychain passwords.
            raise RuntimeError(f"Signing setup failed during security {args[1]}") from None
    with tempfile.TemporaryDirectory(prefix="trepdf-ios-signing-") as directory:
        temporary = Path(directory)
        p12 = temporary / "certificate.p12"
        source_profile = temporary / "profile.mobileprovision"
        p12.write_bytes(base64.b64decode(os.environ[required[0]], validate=True))
        source_profile.write_bytes(base64.b64decode(os.environ[required[2]], validate=True))
        p12.chmod(0o600)
        source_profile.chmod(0o600)
        profile = plistlib.loads(run(["security", "cms", "-D", "-i", str(source_profile)]).stdout)
        team, uuid = validate_profile(profile, bundle_id, method)
        keychain = temporary / "build.keychain-db"
        password = secrets.token_urlsafe(32)
        old_search = shlex.split(run(["security", "list-keychains", "-d", "user"]).stdout.decode())
        installed_profile = Path.home() / "Library/MobileDevice/Provisioning Profiles" / f"{uuid}.mobileprovision"
        created_profile = False
        created_keychain = False
        try:
            run(["security", "create-keychain", "-p", password, str(keychain)])
            created_keychain = True
            run(["security", "set-keychain-settings", "-lut", "21600", str(keychain)])
            run(["security", "unlock-keychain", "-p", password, str(keychain)])
            run(["security", "import", str(p12), "-P", os.environ[required[1]], "-k", str(keychain),
                 "-T", "/usr/bin/codesign", "-T", "/usr/bin/security"])
            run(["security", "set-key-partition-list", "-S", "apple-tool:,apple:,codesign:",
                 "-s", "-k", password, str(keychain)])
            run(["security", "list-keychains", "-d", "user", "-s", str(keychain), *old_search])
            identities = run(["security", "find-identity", "-v", "-p", "codesigning", str(keychain)]).stdout.decode()
            identity = next((hashlib.sha1(cert).hexdigest().upper()
                             for cert in profile.get("DeveloperCertificates", [])
                             if hashlib.sha1(cert).hexdigest().upper() in identities), None)
            if not identity:
                raise ValueError("The certificate/private key does not match this provisioning profile")
            installed_profile.parent.mkdir(parents=True, exist_ok=True)
            if installed_profile.exists():
                if installed_profile.read_bytes() != source_profile.read_bytes():
                    raise ValueError("A different profile with the same UUID already exists locally")
            else:
                installed_profile.write_bytes(source_profile.read_bytes())
                installed_profile.chmod(0o600)
                created_profile = True
            yield {"team": team, "uuid": uuid, "identity": identity}
        finally:
            if created_profile:
                installed_profile.unlink(missing_ok=True)
            if created_keychain:
                subprocess.run(["security", "list-keychains", "-d", "user", "-s", *old_search],
                               capture_output=True, check=False)
                subprocess.run(["security", "delete-keychain", str(keychain)], capture_output=True, check=False)
