# PlatformIO extra script: adds a `pio run -t deploy` target that copies the
# freshly built firmware into releases/ under two names:
#   - nhl_scoreboard_latest.bin       (always the most recent build)
#   - nhl_scoreboard_<version>.bin    (permanent archive for this version,
#                                      e.g. nhl_scoreboard_v1.0.bin)
# It also publishes the bootloader, OTA partition table, and boot_app0
# components needed for a USB flasher to perform the exact PlatformIO upload.
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
LATEST_FACTORY_FILE = "nhl_scoreboard_latest_factory.bin"
LATEST_BOOTLOADER_FILE = "nhl_scoreboard_latest_bootloader.bin"
LATEST_PARTITIONS_FILE = "nhl_scoreboard_latest_partitions.bin"
LATEST_BOOT_APP0_FILE = "nhl_scoreboard_latest_boot_app0.bin"

# Optional website mirror: every release is ALSO copied into
# SITE_ROOT/firmware/<slug>/ for the miniboards website (the site itself
# is published separately, e.g. by an AI agent). Point SITE_ROOT at the
# site's local checkout — when that folder is a git repository the mirror
# is committed and pushed (that push is the site's publish); a plain
# folder just receives the files for whatever publishes the site. When
# the folder doesn't exist the mirror is skipped with a note, so the
# GitHub-only flow keeps working on machines without the site checkout.
# The MINIBOARD_SITE_ROOT env var overrides the path without an edit.
# SITE_FIRMWARE_BASE, when set, rewrites the SITE copy of manifest.json to
# point its download URLs at the website instead of GitHub raw.
SITE_ROOT = os.environ.get("MINIBOARD_SITE_ROOT",
                           r"C:\projects\miniboards-site")
SITE_SLUG = "nhl"          # /firmware/nhl/ on the site
SITE_FIRMWARE_BASE = ""    # e.g. "https://miniboards.example.com/firmware/nhl"


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

    commit = _git(["commit", "-m", "Release nhl_scoreboard %s" % version,
                   "-m", "Co-authored-by: Copilot "
                        "<223556219+Copilot@users.noreply.github.com>"],
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


def _publish_website(version, releases_dir):
    """Mirrors the release into the miniboards website's firmware folder.

    Copies the versioned and latest binaries (app, factory image, and the
    bootloader/partitions/boot_app0 components) plus manifest.json into
    SITE_ROOT/firmware/<slug>/. A git checkout gets a commit + push (the
    site's own pipeline then deploys it); a plain folder just receives
    the files. Skipped with a note when SITE_ROOT doesn't exist, so the
    GitHub-only flow is unaffected on machines without the site.
    """
    if not os.path.isdir(SITE_ROOT):
        print("website mirror skipped — SITE_ROOT not found: %s\n"
              "(set MINIBOARD_SITE_ROOT or SITE_ROOT in this script to "
              "enable)" % SITE_ROOT)
        return

    fw_dir = os.path.join(SITE_ROOT, "firmware", SITE_SLUG)
    os.makedirs(fw_dir, exist_ok=True)
    for name in ("nhl_scoreboard_%s.bin" % version,
                 "nhl_scoreboard_%s_factory.bin" % version,
                 LATEST_FILE, LATEST_FACTORY_FILE, LATEST_BOOTLOADER_FILE,
                 LATEST_PARTITIONS_FILE, LATEST_BOOT_APP0_FILE,
                 "manifest.json"):
        src = os.path.join(releases_dir, name)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(fw_dir, name))

    # The site's manifest copy can point its URLs at the website itself;
    # without SITE_FIRMWARE_BASE it stays byte-identical to the GitHub one.
    if SITE_FIRMWARE_BASE:
        manifest_path = os.path.join(fw_dir, "manifest.json")
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        base = SITE_FIRMWARE_BASE.rstrip("/") + "/"
        for key in list(manifest):
            file_key = key[:-3] + "File"  # "...Url" -> "...File"
            if key.endswith("Url") and file_key in manifest:
                manifest[key] = base + manifest[file_key]
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        print("site manifest URLs rewritten to %s" % base)
    print("website mirror updated: %s" % fw_dir)

    if not os.path.isdir(os.path.join(SITE_ROOT, ".git")):
        return  # plain folder: staged for whatever publishes the site

    add = _git(["add", "--", "firmware"], SITE_ROOT)
    if add.returncode != 0:
        print("WARNING: site git add failed:\n" + add.stderr.strip())
        return
    commit = _git(["commit", "-m", "firmware(%s): %s" % (SITE_SLUG, version)],
                  SITE_ROOT)
    if commit.returncode != 0:
        if "nothing to commit" in (commit.stdout + commit.stderr):
            print("site firmware unchanged — nothing to publish")
        else:
            print("WARNING: site git commit failed:\n" +
                  (commit.stdout + commit.stderr).strip())
        return
    push = _git(["push"], SITE_ROOT)
    if push.returncode != 0:
        print("WARNING: site git push FAILED — committed locally only:\n" +
              (push.stdout + push.stderr).strip())
    else:
        print("website firmware pushed (firmware/%s %s)"
              % (SITE_SLUG, version))


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
    factory = os.path.join(
        releases_dir, "nhl_scoreboard_%s_factory.bin" % version)
    latest_factory = os.path.join(releases_dir, LATEST_FACTORY_FILE)
    build_dir = os.path.dirname(build_bin)
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    boot_app0 = os.path.join(
        env["PROJECT_PACKAGES_DIR"], "framework-arduinoespressif32",
        "tools", "partitions", "boot_app0.bin")

    shutil.copyfile(build_bin, archived)
    shutil.copyfile(build_bin, latest)
    shutil.copyfile(bootloader, os.path.join(releases_dir,
                                             LATEST_BOOTLOADER_FILE))
    shutil.copyfile(partitions, os.path.join(releases_dir,
                                             LATEST_PARTITIONS_FILE))
    shutil.copyfile(boot_app0, os.path.join(releases_dir,
                                            LATEST_BOOT_APP0_FILE))
    esptool = os.path.join(
        env["PROJECT_PACKAGES_DIR"], "tool-esptoolpy", "esptool.py")
    merge = [
        env.subst("$PYTHONEXE"), esptool, "--chip", "esp32s3", "merge_bin",
        "--flash_mode", "qio", "--flash_freq", "80m", "--flash_size", "8MB",
        "-o", factory,
        "0x0", bootloader,
        "0x8000", partitions,
        "0xe000", boot_app0,
        "0x10000", build_bin,
    ]
    merged = subprocess.run(merge, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    if merged.returncode != 0:
        env.Exit("factory image creation failed:\n" + merged.stderr.strip())
    shutil.copyfile(factory, latest_factory)

    # The manifest the firmware polls after boot. `version` lets a device
    # decide whether it is current; `file`/`url` say what to download.
    manifest = {
        "version": version,
        "file": LATEST_FILE,
        "url": RAW_BASE + LATEST_FILE,
        "factoryFile": LATEST_FACTORY_FILE,
        "factoryUrl": RAW_BASE + LATEST_FACTORY_FILE,
        "bootloaderFile": LATEST_BOOTLOADER_FILE,
        "bootloaderUrl": RAW_BASE + LATEST_BOOTLOADER_FILE,
        "partitionsFile": LATEST_PARTITIONS_FILE,
        "partitionsUrl": RAW_BASE + LATEST_PARTITIONS_FILE,
        "bootApp0File": LATEST_BOOT_APP0_FILE,
        "bootApp0Url": RAW_BASE + LATEST_BOOT_APP0_FILE,
    }
    manifest_path = os.path.join(releases_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    size_kb = os.path.getsize(archived) / 1024.0
    print("deployed %s (%.1f KB) ->\n  %s\n  %s\n  %s\n  %s\n  %s (version %s)"
          % (build_bin, size_kb,
             os.path.relpath(archived, project_dir),
             os.path.relpath(latest, project_dir),
             os.path.relpath(factory, project_dir),
             os.path.relpath(latest_factory, project_dir),
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
    _publish_website(version, releases_dir)


# Depends on the firmware binary (concrete path — $-variable strings are not
# re-expanded in custom-target dependencies) so `pio run -t deploy` builds
# first. The guard keeps normal builds from paying any setup cost.
if "deploy" in COMMAND_LINE_TARGETS:
    firmware_bin = os.path.join(env["PROJECT_BUILD_DIR"], env["PIOENV"],
                                "firmware.bin")
    env.AddCustomTarget("deploy", firmware_bin, _deploy)
