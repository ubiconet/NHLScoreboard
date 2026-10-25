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
# SITE_ROOT/firmware/<slug>/ of the sales site repo
# (github.com/ubiconet/scoreboard-site — static pages, served both from
# the Mac mini's local http.server tunnel and from Netlify). When
# SITE_ROOT doesn't exist it is cloned from SITE_REPO_URL automatically
# (git pushes use the machine's stored GitHub credentials). When the
# checkout is a git repository the mirror is committed and pushed — that
# push publishes the repo copy; the Mac mini side (git pull) and the
# Netlify CLI deploy are the site's own publish steps (see
# _publish_netlify below). The MINIBOARD_SITE_ROOT env var overrides the
# path without an edit. SITE_FIRMWARE_BASE, when set, rewrites the SITE
# copy of manifest.json to point its download URLs at the website.
SITE_REPO_URL = "https://github.com/ubiconet/scoreboard-site.git"
SITE_ROOT = os.environ.get("MINIBOARD_SITE_ROOT",
                           r"C:\projects\scoreboard-site")
SITE_SLUG = "nhl"          # /firmware/nhl/ on the site
# The SITE copy of manifest.json points its download URLs here (the
# device OTA checks the Netlify copy — see OTA_MANIFEST_URL in
# src/config.h; the GitHub releases/ manifest keeps the raw GitHub URLs).
SITE_FIRMWARE_BASE = "https://sports-miniboards.com/firmware/nhl"
# Netlify republish (best effort, after the site push): runs
# `netlify deploy --prod` when the CLI is on PATH. Needs a one-time
# `netlify login` (or NETLIFY_AUTH_TOKEN) and NETLIFY_SITE_ID when the
# checkout carries no .netlify/state.json link. Skipped with a note
# otherwise — e.g. on machines without Node.
NETLIFY_SITE_ID = os.environ.get("NETLIFY_SITE_ID", "")


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


def _natural_key(name):
    """Sort key so v3.10 sorts after v3.9 (plain string sort fails)."""
    return [int(p) if p.isdigit() else p for p in re.split(r"(\d+)", name)]


def _fmt_size(n):
    if n >= 1048576:
        return "%.1f MB" % (n / 1048576.0)
    if n >= 1024:
        return "%.1f KB" % (n / 1024.0)
    return "%d B" % n


_SITE_PAGE_STYLE = ("body{margin:0;background:#061b46;color:#fff;"
                    "font:16px system-ui,sans-serif}"
                    "main{max-width:720px;margin:5vh auto;padding:24px;"
                    "background:#0b2b62;border:2px solid #dfe9ff;"
                    "border-radius:8px}"
                    "h1{margin-top:0;font-size:24px}"
                    "h2{font-size:18px;margin:20px 0 6px;color:#f5c400}"
                    "table{width:100%;border-collapse:collapse}"
                    "td{padding:6px 8px;border-top:1px solid #1c4587}"
                    "td:last-child{text-align:right;color:#c5d3ee;"
                    "white-space:nowrap}"
                    "a{color:#f5c400;text-decoration:none}"
                    "a:hover{text-decoration:underline}"
                    ".hint{color:#c5d3ee;font-size:14px;line-height:1.5}")


def _write_site_indexes(site_root, slug):
    """Browsable index pages for the website's firmware folder.

    Netlify (like any static host) has no server-side directory listing
    to enable — a folder is browsable only when it carries its own
    index.html. Both pages regenerate on every deploy from the files
    actually on disk, so they can never drift from the real contents.
    """
    fw_root = os.path.join(site_root, "firmware")
    fw_dir = os.path.join(fw_root, slug)
    latest_names = (LATEST_FILE, LATEST_FACTORY_FILE, LATEST_BOOTLOADER_FILE,
                    LATEST_PARTITIONS_FILE, LATEST_BOOT_APP0_FILE)

    latest_rows, archive_rows = [], []
    for name in sorted(os.listdir(fw_dir), key=_natural_key, reverse=True):
        path = os.path.join(fw_dir, name)
        if name == "index.html" or not os.path.isfile(path):
            continue
        row = ('<tr><td><a href="%s">%s</a></td><td>%s</td></tr>'
               % (name, name, _fmt_size(os.path.getsize(path))))
        if name in latest_names or name == "manifest.json":
            latest_rows.append(row)
        else:
            archive_rows.append(row)

    page = (
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>miniboards firmware &mdash; %s</title>"
        "<style>%s</style></head><body><main>"
        "<h1>miniboards firmware &mdash; %s</h1>"
        "<h2>Latest release</h2><table>%s</table>"
        "<p class=\"hint\">Flash the <b>factory</b> image to a blank board "
        "(single file at 0x0 &mdash; bootloader + partitions included); the "
        "plain app image is for OTA / flashing at 0x10000. manifest.json "
        "carries the version metadata update tools read.</p>"
        "<h2>All versions</h2><table>%s</table>"
        "</main></body></html>"
        % (slug.upper(), _SITE_PAGE_STYLE, slug.upper(),
           "".join(latest_rows), "".join(archive_rows)))
    with open(os.path.join(fw_dir, "index.html"), "w",
              encoding="utf-8") as f:
        f.write(page)

    # Root /firmware page: one row per board folder that has published
    # binaries (other miniboard repos mirror into sibling folders).
    board_rows = []
    for board in sorted(d for d in os.listdir(fw_root)
                        if os.path.isdir(os.path.join(fw_root, d))):
        if not any(n.endswith(".bin") for n in
                   os.listdir(os.path.join(fw_root, board))):
            continue
        board_rows.append('<tr><td><a href="%s/">%s</a></td></tr>'
                          % (board, board.upper()))
    with open(os.path.join(fw_root, "index.html"), "w",
              encoding="utf-8") as f:
        f.write("<!doctype html><html><head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,"
                "initial-scale=1\"><title>miniboards firmware</title>"
                "<style>%s</style></head><body><main>"
                "<h1>miniboards firmware</h1>"
                "<p class=\"hint\">Release binaries for every miniboard "
                "&mdash; open a board for its downloads.</p>"
                "<table>%s</table></main></body></html>"
                % (_SITE_PAGE_STYLE, "".join(board_rows)))
    print("site firmware index pages updated")


def _netlify_repo_connected(exe):
    """True when the Netlify site deploys from its linked GitHub repo —
    then every push to scoreboard-site publishes Netlify automatically
    and no CLI upload is needed (the preferred flow)."""
    if not NETLIFY_SITE_ID:
        return False
    site = subprocess.run([exe, "api", "getSite", "--data",
                           '{"site_id":"%s"}' % NETLIFY_SITE_ID],
                          capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    try:
        return bool(json.loads(site.stdout or "{}").get("repo", {}).get("repo"))
    except ValueError:
        return False


def _publish_netlify(site_root):
    """Netlify republish after the firmware push, GitHub-first.

    Preferred flow: the site is repo-connected in Netlify, so the push to
    scoreboard-site already triggered the deploy — nothing to do here.
    Fallback (until that one-time link exists): `netlify deploy --prod`
    from a clean temp export of the site (everything except
    .git/.netlify — the repo history must never be uploaded).
    """
    # On Windows the npm dir holds an extensionless sh shim next to
    # netlify.cmd — shutil.which can return the shim, which subprocess
    # then fails to launch, so prefer the .cmd explicitly.
    exe = shutil.which("netlify.cmd") or shutil.which("netlify")
    if exe is None:
        print("netlify republish skipped — CLI not on PATH.\n"
              "Finish from the site machine: git pull && "
              "netlify deploy --prod")
        return
    if _netlify_repo_connected(exe):
        print("netlify publishes via the scoreboard-site GitHub push "
              "(site is repo-connected)")
        return
    export_dir = None
    try:
        import tempfile
        export_dir = tempfile.mkdtemp(prefix="netlify-export-")
        deploy_dir = os.path.join(export_dir, "site")
        shutil.copytree(site_root, deploy_dir,
                        ignore=shutil.ignore_patterns(".git", ".netlify"))
        cmd = [exe, "deploy", "--prod", "--dir", deploy_dir]
        if NETLIFY_SITE_ID:
            cmd += ["--site", NETLIFY_SITE_ID]
        dep = subprocess.run(cmd, capture_output=True, text=True,
                             encoding="utf-8", errors="replace")
        if dep.returncode == 0:
            tail = (dep.stdout or "").strip().splitlines()
            print("netlify republished: %s" %
                  (tail[-1] if tail else "deploy ok"))
        else:
            print("WARNING: netlify deploy failed:\n%s" %
                  ((dep.stdout or "") + (dep.stderr or "")).strip())
    finally:
        if export_dir:
            shutil.rmtree(export_dir, ignore_errors=True)


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
        if not SITE_REPO_URL:
            print("website mirror skipped — SITE_ROOT not found: %s" % SITE_ROOT)
            return
        print("website mirror: cloning %s" % SITE_REPO_URL)
        clone = subprocess.run(["git", "clone", SITE_REPO_URL, SITE_ROOT],
                               capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
        if clone.returncode != 0:
            print("website mirror skipped — clone failed:\n%s"
                  % clone.stderr.strip())
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
            # "...Url" -> "...File"; the bare lowercase "url" key maps to
            # "file" (and needs its own match — endswith("Url") misses it).
            file_key = "file" if key == "url" else key[:-3] + "File"
            if (key == "url" or key.endswith("Url")) and file_key in manifest:
                manifest[key] = base + manifest[file_key]
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        print("site manifest URLs rewritten to %s" % base)
    print("website mirror updated: %s" % fw_dir)
    _write_site_indexes(SITE_ROOT, SITE_SLUG)

    if not os.path.isdir(os.path.join(SITE_ROOT, ".git")):
        _publish_netlify(SITE_ROOT)  # plain folder: deploy it if we can
        return

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
        return
    print("website firmware pushed (firmware/%s %s)"
          % (SITE_SLUG, version))
    _publish_netlify(SITE_ROOT)


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
