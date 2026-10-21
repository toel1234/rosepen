#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Static validation for Rosepen packaging files.

Checks the Flatpak manifest and the GitHub Actions workflow without building
anything. With --check-noble, also verifies that every package listed in
fork/scripts/install-build-deps-ubuntu.sh exists in Ubuntu noble.

Run as: python3 -I fork/packaging/lint-packaging.py [--check-noble]
"""

import argparse
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

try:
    import yaml
except ImportError:
    print("python3-yaml missing", file=sys.stderr)
    sys.exit(2)

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "fork/packaging/flatpak/io.github.toel1234.Rosepen.yml"
WORKFLOW = ROOT / ".github/workflows/build.yml"
DEPS_SCRIPT = ROOT / "fork/scripts/install-build-deps-ubuntu.sh"
CACHE = ROOT / "build/CMakeCache.txt"

HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX40 = re.compile(r"^[0-9a-f]{40}$")

errors = []


def fail(message):
    errors.append(message)


def load_yaml(path, label):
    try:
        with path.open("r", encoding="utf-8") as handle:
            return yaml.safe_load(handle)
    except FileNotFoundError:
        fail(f"{label} not found: {path.relative_to(ROOT)}")
    except yaml.YAMLError as exc:
        fail(f"{label} is not valid YAML: {exc}")
    return None


def cache_value(key):
    try:
        text = CACHE.read_text(encoding="utf-8")
    except FileNotFoundError:
        fail(f"CMake cache not found: {CACHE.relative_to(ROOT)}")
        return None
    match = re.search(rf"^{re.escape(key)}:STRING=(.*)$", text, re.MULTILINE)
    if not match:
        fail(f"{key} missing from {CACHE.relative_to(ROOT)}")
        return None
    return match.group(1).strip()


def walk_sources(modules, path="modules"):
    for index, module in enumerate(modules or []):
        if not isinstance(module, dict):
            continue
        name = module.get("name", f"#{index}")
        where = f"{path}[{index}] ({name})"
        for source in module.get("sources", []) or []:
            if not isinstance(source, dict):
                continue
            kind = source.get("type")
            if kind in ("archive", "file"):
                digest = source.get("sha256", "")
                if not HEX64.match(str(digest)):
                    fail(f"{where}: {kind} source has no 64-hex sha256")
            elif kind == "git":
                if not HEX40.match(str(source.get("commit", ""))):
                    fail(f"{where}: git source has no 40-hex commit")
                if not source.get("tag"):
                    fail(f"{where}: git source has no tag")
        walk_sources(module.get("modules"), f"{where}.modules")


def check_manifest():
    data = load_yaml(MANIFEST, "Flatpak manifest")
    if data is None:
        return
    app_id = cache_value("FORK_APP_ID")
    if app_id is not None and data.get("id") != app_id:
        fail(f"manifest id {data.get('id')!r} != cached FORK_APP_ID {app_id!r}")
    if data.get("command") != "rosepen":
        fail(f"manifest command must be 'rosepen', got {data.get('command')!r}")
    if data.get("runtime") != "org.gnome.Platform":
        fail(f"manifest runtime must be 'org.gnome.Platform', got {data.get('runtime')!r}")
    if data.get("sdk") != "org.gnome.Sdk":
        fail(f"manifest sdk must be 'org.gnome.Sdk', got {data.get('sdk')!r}")
    if not str(data.get("runtime-version", "")).strip():
        fail("manifest runtime-version is missing")
    walk_sources(data.get("modules"))
    print(f"manifest: {MANIFEST.relative_to(ROOT)}")


def check_workflow():
    data = load_yaml(WORKFLOW, "workflow")
    if data is None:
        return

    # PyYAML resolves the bare key `on` as boolean True.
    triggers = data.get("on", data.get(True))
    if not isinstance(triggers, dict):
        fail("workflow: no trigger mapping")
        triggers = {}
    push = triggers.get("push") or {}
    branches = push.get("branches") or []
    if "main" not in branches:
        fail("workflow: push trigger does not include branch main")
    if "workflow_dispatch" not in triggers:
        fail("workflow: workflow_dispatch trigger missing")

    permissions = data.get("permissions") or {}
    if permissions.get("contents") != "read":
        fail("workflow: permissions.contents must be read")

    concurrency = data.get("concurrency") or {}
    if not concurrency.get("group") or concurrency.get("cancel-in-progress") is not True:
        fail("workflow: concurrency group/cancel-in-progress missing")

    jobs = data.get("jobs") or {}
    job = jobs.get("linux")
    if not isinstance(job, dict):
        fail("workflow: job 'linux' missing")
        return
    if job.get("runs-on") != "ubuntu-24.04":
        fail(f"workflow: runs-on must be ubuntu-24.04, got {job.get('runs-on')!r}")
    timeout = job.get("timeout-minutes")
    if not isinstance(timeout, int) or timeout <= 0:
        fail("workflow: timeout-minutes missing or not a positive integer")

    env = job.get("env") or {}
    for key in ("CCACHE_DIR", "CCACHE_MAXSIZE", "BUILD_TYPE", "SKIP_APT_FILE"):
        if key not in env:
            fail(f"workflow: env {key} missing")

    steps = job.get("steps") or []
    all_runs = []
    checkout_ok = False
    upload_ok = False
    for step in steps:
        if not isinstance(step, dict):
            continue
        uses = str(step.get("uses", ""))
        if re.match(r"^actions/checkout@", uses):
            checkout = step.get("with") or {}
            if str(checkout.get("submodules", "")).lower() == "recursive":
                checkout_ok = True
            else:
                fail("workflow: checkout does not set submodules: recursive")
            if str(checkout.get("fetch-depth")) != "1":
                fail("workflow: checkout does not set fetch-depth: 1")
        if re.match(r"^actions/upload-artifact@", uses):
            upload = step.get("with") or {}
            if "build/appimage/Rosepen-x86_64.AppImage" not in str(upload.get("path", "")):
                fail("workflow: upload-artifact path is not the AppImage")
            if upload.get("if-no-files-found") != "error":
                fail("workflow: upload-artifact if-no-files-found must be error")
            if "retention-days" not in upload:
                fail("workflow: upload-artifact retention-days missing")
            if not str(upload.get("name", "")).startswith("Rosepen-x86_64-"):
                fail("workflow: upload-artifact name must start with Rosepen-x86_64-")
            upload_ok = True
        if "run" in step:
            all_runs.append(str(step["run"]))

    if not checkout_ok:
        fail("workflow: actions/checkout step missing")
    if not upload_ok:
        fail("workflow: actions/upload-artifact step missing")

    runs = "\n".join(all_runs)
    for script in (
        "fork/scripts/install-build-deps-ubuntu.sh",
        "fork/scripts/build-local.sh",
        "fork/scripts/verify-branding.sh",
        "fork/packaging/build-appimage.sh",
    ):
        if script not in runs:
            fail(f"workflow: does not invoke {script}")
    print(f"workflow: {WORKFLOW.relative_to(ROOT)}")


def dependency_packages():
    text = DEPS_SCRIPT.read_text(encoding="utf-8")
    match = re.search(r"^packages=\((.*?)^\)", text, re.MULTILINE | re.DOTALL)
    if not match:
        fail("install-build-deps-ubuntu.sh: packages=( ... ) array not found")
        return []
    packages = []
    for line in match.group(1).splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            packages.extend(line.split())
    return packages


def host_is_noble():
    """True only when /etc/os-release says the host runs Ubuntu noble (24.04)."""
    try:
        for line in open("/etc/os-release", encoding="utf-8"):
            if line.strip() == "VERSION_CODENAME=noble":
                return True
    except OSError:
        pass
    return False


def apt_cache_has(package):
    """Return True/False when the local apt cache can decide, None otherwise.

    The local cache describes the HOST release, so it only counts as a noble
    verdict when the host actually runs noble (24.04); otherwise None (SKIP).
    """
    if not host_is_noble():
        return None
    try:
        result = subprocess.run(
            ["apt-cache", "show", package],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.returncode == 0


def check_noble():
    packages = dependency_packages()
    if not packages:
        return
    missing = []
    skipped = []
    via_apt = 0
    network_down = False
    for package in packages:
        status = None
        if not network_down:
            url = "https://packages.ubuntu.com/noble/" + urllib.parse.quote(package)
            try:
                with urllib.request.urlopen(url, timeout=10) as response:
                    body = response.read().decode("utf-8", "replace")
                status = "No such package" not in body
            except urllib.error.HTTPError as exc:
                if exc.code == 404:
                    status = False
                else:
                    network_down = True
            except (urllib.error.URLError, TimeoutError, OSError):
                network_down = True
        if status is None:
            status = apt_cache_has(package)
            if status is not None:
                via_apt += 1
        if status is False:
            missing.append(package)
        elif status is None:
            skipped.append(package)

    if network_down:
        print("noble: packages.ubuntu.com unreachable; used local apt metadata "
              f"for {via_apt} package(s)")
    for package in missing:
        fail(f"package not found in Ubuntu noble: {package}")
    for package in skipped:
        print(f"noble check SKIP: {package} (network unavailable, no local apt metadata)")
    print(f"noble: checked {len(packages)} packages, {len(missing)} missing, "
          f"{len(skipped)} skipped")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-noble", action="store_true",
                        help="verify dependency packages against Ubuntu noble")
    args = parser.parse_args()

    check_manifest()
    check_workflow()
    if args.check_noble:
        check_noble()

    if errors:
        for message in errors:
            print(f"FAIL: {message}", file=sys.stderr)
        return 1
    print("PACKAGING_LINT_OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
