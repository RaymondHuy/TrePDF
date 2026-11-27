"""Developer ID signing credentials and Apple notarization for macOS CI."""

import base64
from contextlib import contextmanager, ExitStack
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import subprocess
import tempfile


SECRET_NAMES = ("APPLE_CERTIFICATE_BASE64", "APPLE_CERTIFICATE_PASSWORD",
                "APPLE_API_KEY_BASE64", "APPLE_API_KEY_ID", "APPLE_API_ISSUER_ID")


def take_credentials(names=SECRET_NAMES):
    # Do not pass the workflow secrets through to child processes (including TrePDF).
    values = {name: os.environ.pop(name, "") for name in names}
    missing = [name for name, value in values.items() if not value]
    if missing:
        raise ValueError("Missing macOS signing secrets: " + ", ".join(missing))
    return values


def security(operation, *arguments):
    # Some arguments contain passwords. Never include the command line in an exception.
    result = subprocess.run(["/usr/bin/security", operation, *arguments], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"macOS signing failed during security {operation} (exit {result.returncode})")
    return result.stdout


@contextmanager
def temporary_keychain(private):
    with ExitStack() as cleanup:
        chain = str(private / "signing.keychain-db")
        password = secrets.token_urlsafe(32)
        previous = shlex.split(security("list-keychains", "-d", "user"))
        security("create-keychain", "-p", password, chain)
        cleanup.callback(security, "delete-keychain", chain)
        cleanup.callback(security, "list-keychains", "-d", "user", "-s", *previous)
        security("list-keychains", "-d", "user", "-s", chain, *previous)
        security("set-keychain-settings", "-lut", "21600", chain)
        security("unlock-keychain", "-p", password, chain)
        yield chain, password


def import_p12(chain, private, encoded, password, tool, *additional_tools):
    cert = private / "certificate.p12"
    cert.write_bytes(base64.b64decode("".join(encoded.split()), validate=True))
    cert.chmod(0o600)
    access = ["-T", tool]
    for additional_tool in additional_tools:
        access.extend(("-T", additional_tool))
    security("import", str(cert), "-k", chain, "-f", "pkcs12", "-P", password, *access)
    cert.unlink()


@contextmanager
def signing_assets(values):
    with tempfile.TemporaryDirectory(prefix="trepdf-developer-id-") as directory, ExitStack() as stack:
        private = Path(directory)
        key = private / "AuthKey.p8"
        key.write_bytes(base64.b64decode("".join(values["APPLE_API_KEY_BASE64"].split()), validate=True))
        key.chmod(0o600)
        if not key.read_bytes().startswith(b"-----BEGIN PRIVATE KEY-----"):
            raise ValueError("APPLE_API_KEY_BASE64 must encode an App Store Connect .p8 key")
        chain, password = stack.enter_context(temporary_keychain(private))
        import_p12(chain, private, values["APPLE_CERTIFICATE_BASE64"],
                   values["APPLE_CERTIFICATE_PASSWORD"], "/usr/bin/codesign")
        security("set-key-partition-list", "-S", "apple-tool:,apple:", "-s", "-k", password, chain)
        identities = security("find-identity", "-v", "-p", "codesigning", chain)
        matches = re.findall(r'^\s*\d+\) ([0-9A-Fa-f]{40}) "Developer ID Application: ', identities, re.M)
        if len(matches) != 1:
            raise ValueError("Expected one valid Developer ID Application certificate with its private key in the .p12")
        yield (["--sign", matches[0], "--keychain", chain, "--timestamp"],
               ["--key", str(key), "--key-id", values["APPLE_API_KEY_ID"], "--issuer", values["APPLE_API_ISSUER_ID"]])


def notarize(submission, staple_to, auth):
    print(f"Submitting {submission.name} to Apple for notarization", flush=True)
    result = subprocess.run(["xcrun", "notarytool", "submit", str(submission), *auth,
                             "--wait", "--timeout", "60m", "--output-format", "json"],
                            stdout=subprocess.PIPE, text=True)
    try:
        record = json.loads(result.stdout)
    except ValueError:
        raise RuntimeError(f"notarytool submit returned no JSON result (exit {result.returncode})") from None
    status, identifier = record.get("status"), record.get("id")
    print(f"Apple submission {identifier}: {status}", flush=True)
    if result.returncode or status != "Accepted":
        if identifier and status == "Invalid":
            subprocess.run(["xcrun", "notarytool", "log", identifier, *auth], check=False)
        raise RuntimeError(f"Apple did not accept {submission.name} (submission {identifier}, status {status})")
    subprocess.run(["xcrun", "stapler", "staple", str(staple_to)], check=True)
    subprocess.run(["xcrun", "stapler", "validate", str(staple_to)], check=True)
