# PlatformIO extra script: adds a `pio run -t deploy` target that copies the
# freshly built firmware into releases/ under two names:
#   - nhl_scoreboard_latest.bin       (always the most recent build)
#   - nhl_scoreboard_<version>.bin    (permanent archive for this version,
#                                      e.g. nhl_scoreboard_v1.0.bin)
# The version is read from FIRMWARE_VERSION in src/config.h, so the archive
# name always matches the version shown on the device's boot splash.

import json
import os
import re
import shutil
import subprocess

Import("env")

# Where the repo's releases/ folder is served from. The firmware's OTA
# updater fetches manifest.json from here (see OTA_MANIFEST_URL in
# src/config.h) and downloads the binary named inside it.
RAW_BASE = ("https://raw.githubusercontent.com/ubiconet/NHLScoreboard/"
            "main/releases/")
LATEST_FILE = "nhl_scoreboard_latest.bin"


def _firmware_version():
    config_path = os.path.join(env["PROJECT_SRC_DIR"], "config.h")
    with open(config_path, "r", encoding="utf-8") as f:
        match = re.search(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', f.read())
    if not match:
        env.Exit("Could not find FIRMWARE_VERSION in src/config.h")
    return match.group(1)


def _published_version(releases_dir):
    """Version currently recorded in releases/manifest.json, or None."""
    try:
        with open(os.path.join(releases_dir, "manifest.json"), "r",
                  encoding="utf-8") as f:
            return json.load(f).get("version")
    except (OSError, ValueError):
        return None


def _git(args, project_dir):
    return subprocess.run(["git"] + args, cwd=project_dir,
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace")


def _publish_releases(project_dir, version):
    """Commits releases/ and pushes to GitHub so the device can see it."""
    add = _git(["add", "--", "releases"], project_dir)
    if add.returncode != 0:
        env.Exit("git add releases failed:\n" + add.stderr.strip())

    commit = _git(["commit", "-m", "Release nhl_scoreboard %s" % version],
                  project_dir)
    if commit.returncode != 0:
        if "nothing to commit" in (commit.stdout + commit.stderr):
            print("releases/ unchanged — nothing to publish")
            return
        env.Exit("git commit failed:\n" +
                 (commit.stdout + commit.stderr).strip())

    push = _git(["push"], project_dir)
    if push.returncode != 0:
        env.Exit(
            "git push FAILED — the release files are committed locally but\n"
            "NOT on GitHub, so devices cannot see them yet. Fix the error\n"
            "below and run `git push` manually:\n" +
            (push.stdout + push.stderr).strip())

    print("releases pushed to GitHub (manifest version %s)" % version)


def _deploy(source, target, env):
    version = _firmware_version()
    build_bin = os.path.join(env["PROJECT_BUILD_DIR"], env["PIOENV"],
                             "firmware.bin")
    project_dir = env["PROJECT_DIR"]
    releases_dir = os.path.join(project_dir, "releases")
    os.makedirs(releases_dir, exist_ok=True)

    # Guard: republishing the same version would be a no-op update that
    # devices correctly ignore (the updater only flashes strictly newer
    # versions). Bump FIRMWARE_VERSION in src/config.h first.
    published = _published_version(releases_dir)
    if published == version:
        env.Exit(
            "FIRMWARE_VERSION is still %s, which is already published in "
            "releases/manifest.json.\nBump FIRMWARE_VERSION in "
            "src/config.h before deploying, or devices will never pick "
            "this build up." % version)

    archived = os.path.join(releases_dir, "nhl_scoreboard_%s.bin" % version)
    latest = os.path.join(releases_dir, LATEST_FILE)

    shutil.copyfile(build_bin, archived)
    shutil.copyfile(build_bin, latest)

    # The manifest the firmware polls after boot. `version` lets a device
    # decide whether it is current; `file`/`url` say what to download.
    manifest = {
        "version": version,
        "file": LATEST_FILE,
        "url": RAW_BASE + LATEST_FILE,
    }
    manifest_path = os.path.join(releases_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    size_kb = os.path.getsize(archived) / 1024.0
    print("deployed %s (%.1f KB) ->\n  %s\n  %s\n  %s (version %s)"
          % (build_bin, size_kb,
             os.path.relpath(archived, project_dir),
             os.path.relpath(latest, project_dir),
             os.path.relpath(manifest_path, project_dir),
             version))

    # Optional reminder: uncommitted source changes are not part of any
    # commit yet, so this release's binaries can't be reproduced from the
    # repo history until they are committed.
    status = _git(["status", "--porcelain", "--untracked-files=no"], project_dir)
    dirty = [ln for ln in status.stdout.splitlines()
             if ln.strip() and not ln.strip().startswith("??")
             and " releases/" not in ln]
    if dirty:
        print("note: uncommitted source changes outside releases/ — commit "
              "them so this binary is reproducible from the repo")

    _publish_releases(project_dir, version)


# Depends on the firmware binary (concrete path — $-variable strings are not
# re-expanded in custom-target dependencies) so `pio run -t deploy` builds
# first. The guard keeps normal builds from paying any setup cost.
if "deploy" in COMMAND_LINE_TARGETS:
    firmware_bin = os.path.join(env["PROJECT_BUILD_DIR"], env["PIOENV"],
                                "firmware.bin")
    env.AddCustomTarget("deploy", firmware_bin, _deploy)
