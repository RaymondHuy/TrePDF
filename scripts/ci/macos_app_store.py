"""Produce a signed universal Mac App Store installer for manual delivery."""
import argparse
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile

from app_version import app_version
from macos_compat import verify_tree
from macos_signing import import_p12, security, take_credentials, temporary_keychain
from package_uat import sign_macos_bundle

BUNDLE_ID = "com.trebase.pdf"
ENTITLEMENTS = Path(__file__).resolve().parents[2] / "platform/macos/AppStore.entitlements"
SECRETS = ("APPLE_DISTRIBUTION_BASE64", "APPLE_DISTRIBUTION_PASSWORD",
           "APPLE_INSTALLER_BASE64", "APPLE_INSTALLER_PASSWORD")


def build_number(value):
    if not re.fullmatch(r"[1-9][0-9]{0,3}(?:\.(?:0|[1-9][0-9]?)){0,2}", value):
        raise argparse.ArgumentTypeError("use an Apple build number such as 1, 42 or 42.1.3")
    return value


def identity(listing, prefixes):
    matches = []
    for digest, name in re.findall(r'^\s*\d+\) ([\da-fA-F]{40}) "([^"\n]+)"', listing, re.M):
        team = re.search(r" \(([A-Z0-9]{10})\)$", name)
        if name.startswith(prefixes) and team:
            matches.append((digest, name, team[1]))
    if len(matches) != 1:
        raise ValueError(f"Expected one valid {prefixes[0]} identity with its private key")
    return matches[0]


def command_output(*args):
    return subprocess.check_output(args, text=True).strip()


def prepare_bundle(app, version, build):
    info_path = app / "Contents/Info.plist"
    info = plistlib.loads(info_path.read_bytes())
    expected = {"CFBundleIdentifier": BUNDLE_ID, "CFBundleShortVersionString": version,
                "CFBundleExecutable": "Tre PDF", "LSApplicationCategoryType": "public.app-category.productivity"}
    for key, value in expected.items():
        if info.get(key) != value:
            raise ValueError(f"Expected {key}={value!r} in {info_path}")
    for arch in ("arm64", "x86_64"):
        verify_tree(app, arch, info["LSMinimumSystemVersion"])
    xcode, xcode_build = command_output("xcodebuild", "-version").splitlines()
    numbers = xcode.split()[1].split(".")
    major, minor, patch = (numbers + ["0", "0"])[:3]
    sdk = command_output("xcrun", "--sdk", "macosx", "--show-sdk-version")
    sdk_build = command_output("xcrun", "--sdk", "macosx", "--show-sdk-build-version")
    info.update(CFBundleVersion=build, DTPlatformName="macosx", DTPlatformVersion=sdk,
                DTSDKName="macosx" + sdk, DTSDKBuild=sdk_build, DTPlatformBuild=sdk_build,
                DTXcode=f"{int(major):02}{minor}{patch}", DTXcodeBuild=xcode_build.split()[-1],
                BuildMachineOSBuild=command_output("sw_vers", "-buildVersion"))
    info_path.write_bytes(plistlib.dumps(info, sort_keys=False))


def sign_installer(app, pkg, installer, chain):
    with tempfile.TemporaryDirectory(dir=pkg.parent) as work:
        unsigned = Path(work) / "unsigned.pkg"
        signed = Path(work) / "signed.pkg"
        print("Building installer payload with productbuild (up to 15 minutes)", flush=True)
        subprocess.run(["productbuild", "--component", str(app), "/Applications", unsigned],
                       check=True, timeout=900)
        print("Signing installer with productsign (up to 5 minutes)", flush=True)
        subprocess.run(["productsign", "--sign", installer, "--keychain", chain, unsigned, signed],
                       check=True, timeout=300)
        print("Verifying installer signature with pkgutil (up to 2 minutes)", flush=True)
        signature = subprocess.check_output(["pkgutil", "--check-signature", signed],
                                            text=True, timeout=120)
        if installer not in signature:
            raise RuntimeError("Unexpected installer signing certificate")
        print(signature, flush=True)
        signed.replace(pkg)


def package(app, output, version, build):
    values = take_credentials(SECRETS)
    prepare_bundle(app, version, build)
    output.mkdir(parents=True, exist_ok=True)
    pkg = output / f"TrePDF-{version}-{build}-macos-universal.pkg"
    with tempfile.TemporaryDirectory(prefix="trepdf-mas-") as temp:
        private = Path(temp)
        with temporary_keychain(private) as (chain, password):
            import_p12(chain, private, values["APPLE_DISTRIBUTION_BASE64"],
                       values["APPLE_DISTRIBUTION_PASSWORD"], "/usr/bin/codesign")
            import_p12(chain, private, values["APPLE_INSTALLER_BASE64"],
                       values["APPLE_INSTALLER_PASSWORD"], "/usr/bin/productsign")
            security("set-key-partition-list", "-S", "apple-tool:,apple:", "-s", "-k", password, chain)
            app_id, _, app_team = identity(security("find-identity", "-v", "-p", "codesigning", chain),
                                           ("Apple Distribution: ", "3rd Party Mac Developer Application: "))
            _, installer, pkg_team = identity(security("find-identity", "-v", "-p", "basic", chain),
                                              ("3rd Party Mac Developer Installer: ", "Mac Installer Distribution: "))
            if app_team != pkg_team:
                raise ValueError("Application and installer certificates must belong to the same Apple team")
            sign_macos_bundle(app, ["--sign", app_id, "--keychain", chain], entitlements=ENTITLEMENTS)
            print("Reading signed app entitlements with codesign (up to 1 minute)", flush=True)
            signed = subprocess.check_output(["codesign", "-d", "--entitlements", "-", "--xml", str(app)],
                                             timeout=60)
            if plistlib.loads(signed) != plistlib.loads(ENTITLEMENTS.read_bytes()):
                raise RuntimeError("Signed application entitlements differ from the Mac App Store entitlements")
            sign_installer(app, pkg, installer, chain)
    print(f"Ready for Transporter: {pkg}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check-version", "package"))
    parser.add_argument("--version", required=True, type=app_version)
    parser.add_argument("--build", required=True, type=build_number)
    parser.add_argument("--app", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.command == "package":
        if not args.app or not args.output:
            parser.error("package requires --app and --output")
        package(args.app.resolve(), args.output.resolve(), args.version, args.build)


if __name__ == "__main__":
    main()
