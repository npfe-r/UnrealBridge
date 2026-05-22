#!/usr/bin/env python3
"""Full C++ rebuild + editor relaunch for the UnrealBridge plugin.

Use this whenever a hot-reload via Live Coding won't work:
    - You added / removed / renamed a UFUNCTION / UCLASS / UPROPERTY
    - You changed a struct layout or reflection metadata
    - Live Coding reports Failure on `hot_reload.py`
    - The editor is already down

Flow:
  1. (if editor is up) ask it to quit_editor via the bridge
  2. wait for UnrealEditor.exe to exit (kill as last resort)
  3. run sync_plugin.bat (unless --no-sync)
  4. run the target project's Build.bat (compiles the editor module)
  5. launch UnrealEditor.exe <uproject> detached
  6. poll `bridge.py ping` until ready (or --no-wait)

Paths are read from sync_plugin.bat's DST line by default; override with
--project-dir / --uproject / --editor-exe. The target project is the one
sync_plugin.bat pushes to — usually NOT this repo.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time

# Windows consoles in non-en locales (e.g. cp936) can't encode every Unicode
# code point that ends up in MSVC localized error text or in our utf-8-replaced
# subprocess stdout. Reconfigure so writes never crash mid-build.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(errors="replace")

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
BRIDGE_PY  = SCRIPT_DIR / "bridge.py"
REPO_ROOT  = SCRIPT_DIR.parents[3]
SYNC_BAT   = REPO_ROOT / "sync_plugin.bat"

# Resolve UnrealEditor.exe in this order:
#   1. --editor-exe CLI arg
#   2. UNREAL_EDITOR_EXE env var (full path to UnrealEditor.exe)
#   3. UE_ROOT env var (engine root; we append Binaries/Win64/UnrealEditor.exe)
# No hardcoded default — paths are user-specific and should not land in
# the repo.
def resolve_default_editor_exe() -> str:
    explicit = os.environ.get("UNREAL_EDITOR_EXE")
    if explicit:
        return explicit
    ue_root = os.environ.get("UE_ROOT")
    if ue_root:
        return str(pathlib.Path(ue_root) / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe")
    return ""


def parse_project_dir_from_sync_bat() -> pathlib.Path | None:
    """Pull the target project's root directory from sync_plugin.bat's DST line.

    The bat writes `set "DST=...\\Plugins\\UnrealBridge"`, so the project
    root is two parents up.
    """
    if not SYNC_BAT.exists():
        return None
    try:
        content = SYNC_BAT.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None
    m = re.search(r'set\s+"DST=([^"]+)"', content, flags=re.IGNORECASE)
    if not m:
        return None
    dst = pathlib.Path(m.group(1))
    # DST ends with \Plugins\<PluginName>; project root is two levels up.
    if dst.parent.name.lower() == "plugins":
        return dst.parent.parent
    return None


def find_uproject(project_dir: pathlib.Path) -> pathlib.Path | None:
    for p in project_dir.glob("*.uproject"):
        return p
    return None


def is_editor_running() -> bool:
    """True if ANY UnrealEditor.exe is running on the machine.

    Used as a coarse signal — for project-aware shutdown the caller should
    resolve the target editor's PID via `get_editor_pid_for_uproject` and
    use `is_pid_running(pid)` instead, so we never wipe a sibling project's
    editor.
    """
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq UnrealEditor.exe", "/NH"],
            text=True, stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False
    return "UnrealEditor.exe" in out


def is_pid_running(pid: int) -> bool:
    """True iff a process with the given PID is currently running."""
    if pid <= 0:
        return False
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"PID eq {pid}", "/NH"],
            text=True, stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False
    # `tasklist` prints "INFO: No tasks are running which match the specified
    # criteria." on miss (returncode still 0). Match the literal PID instead.
    return str(pid) in out and "INFO:" not in out


def get_editor_info_for_uproject(uproject: pathlib.Path,
                                 discovery_timeout_ms: int = 1500
                                 ) -> "tuple[int | None, bool]":
    """Query bridge discovery; return (matching_pid, discovery_succeeded).

    `matching_pid` is the PID of the editor whose project_path matches the
    given .uproject, or None if no match. `discovery_succeeded` is True iff
    the discovery probe completed and returned parseable JSON — we got
    authoritative info about all editors on this machine. When False, the
    network probe timed out / errored / returned garbage and we cannot tell
    whether a running UnrealEditor.exe is ours or a sibling's.

    The boolean is what lets us safely proceed in multi-editor setups: when
    discovery succeeded with no match for our uproject, any running editor
    is provably a sibling we should leave alone.
    """
    target = uproject.resolve()
    try:
        proc = subprocess.run(
            [sys.executable, str(BRIDGE_PY), "--json", "list-editors",
             "--discovery-timeout", str(discovery_timeout_ms)],
            capture_output=True, text=True, timeout=10,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return (None, False)
    if proc.returncode != 0:
        return (None, False)
    try:
        eps = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return (None, False)
    if not isinstance(eps, list):
        return (None, False)
    for ep in eps:
        ep_project_raw = ep.get("project_path") or ""
        if not ep_project_raw:
            continue
        try:
            if pathlib.Path(ep_project_raw).resolve() == target:
                pid = int(ep.get("pid") or 0)
                return (pid if pid > 0 else None, True)
        except (OSError, ValueError):
            continue
    return (None, True)


def quit_editor_gracefully(timeout_s: float,
                           target_pid: "int | None" = None,
                           project_name: "str | None" = None) -> bool:
    """Try bridge.exec('quit_editor()'); wait for the target editor to exit.

    `project_name` (the .uproject stem) is forwarded as bridge.py's `--project`
    discovery filter, so the quit command goes to our editor even when sibling
    editors are running on the same machine. `target_pid`, when given, scopes
    the wait loop to only that PID.
    """
    script = (
        "import unreal\n"
        "try:\n"
        "    unreal.SystemLibrary.quit_editor()\n"
        "except Exception as e:\n"
        "    print('quit_editor failed:', e)\n"
    )
    cmd = [sys.executable, str(BRIDGE_PY)]
    if project_name:
        cmd += ["--project", project_name]
    cmd += ["--timeout", "10", "exec", script]
    subprocess.run(cmd, capture_output=True, text=True)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if target_pid is not None:
            if not is_pid_running(target_pid):
                return True
        else:
            if not is_editor_running():
                return True
        time.sleep(0.5)
    return False


def force_kill_pid(pid: int) -> None:
    """taskkill /F /PID <pid> + brief wait for exit."""
    if pid <= 0:
        return
    subprocess.run(
        ["taskkill", "/F", "/PID", str(pid)],
        capture_output=True, text=True,
    )
    for _ in range(20):
        if not is_pid_running(pid):
            return
        time.sleep(0.25)


def force_kill_editor() -> None:
    """Global taskkill of every UnrealEditor.exe — opt-in via --allow-global-kill
    only. Kills sibling editors running other projects; do not use casually."""
    subprocess.run(
        ["taskkill", "/F", "/IM", "UnrealEditor.exe"],
        capture_output=True, text=True,
    )
    for _ in range(20):
        if not is_editor_running():
            return
        time.sleep(0.25)


def run_sync(verbose: bool) -> int:
    if not SYNC_BAT.exists():
        sys.stderr.write(f"sync_plugin.bat not found at {SYNC_BAT}\n")
        return 3
    print(f"[rebuild] syncing plugin via {SYNC_BAT.name} ...")
    p = subprocess.run(
        ["cmd.exe", "/c", str(SYNC_BAT)],
        capture_output=not verbose, text=True,
    )
    if p.returncode != 0:
        if not verbose and p.stdout:
            sys.stderr.write(p.stdout)
        if not verbose and p.stderr:
            sys.stderr.write(p.stderr)
        sys.stderr.write(f"sync_plugin.bat failed (rc={p.returncode})\n")
        return 3
    return 0


def run_build(project_dir: pathlib.Path, verbose: bool) -> int:
    build_bat = project_dir / "Build.bat"
    if not build_bat.exists():
        sys.stderr.write(f"Build.bat not found at {build_bat}\n")
        return 4
    print(f"[rebuild] compiling editor target: {build_bat}")
    # Stream stdout so long compiles don't look hung.
    proc = subprocess.Popen(
        ["cmd.exe", "/c", str(build_bat)],
        cwd=str(project_dir),
        stdout=None if verbose else subprocess.PIPE,
        stderr=None if verbose else subprocess.STDOUT,
        text=True,
        # Build.bat output mixes ASCII (UBT) with whatever the active code
        # page is (zh-CN systems → GBK). Force utf-8 + errors='replace' so
        # smart quotes / Chinese path fragments don't crash the streamer.
        encoding="utf-8",
        errors="replace",
    )
    if not verbose and proc.stdout:
        for line in proc.stdout:
            # Only surface lines that look informative; drop heavy UBT chatter.
            lower = line.lower()
            if any(tag in lower for tag in ("error", "warning", "total time", "compiling", "link:")):
                sys.stdout.write(line)
    rc = proc.wait()
    if rc != 0:
        sys.stderr.write(f"Build.bat failed (rc={rc})\n")
        return 4
    return 0


def launch_editor(editor_exe: pathlib.Path, uproject: pathlib.Path) -> None:
    print(f"[rebuild] launching: {editor_exe} {uproject.name}")
    # Detach so this script returns as soon as the editor starts.
    # Windows: CREATE_NEW_PROCESS_GROUP + CREATE_NO_WINDOW.
    #
    # Do NOT use DETACHED_PROCESS here. DETACHED_PROCESS leaves the new
    # process with no console at all and INVALID_HANDLE_VALUE for stdio.
    # When UE later spawns ShaderCompileWorker children with handle
    # inheritance, SCW's stdio init either fails or deadlocks — the editor
    # then hangs on its global-shader-compile barrier with zero SCW
    # subprocesses ever appearing in tasklist (visible symptom: editor at
    # ~2 GB RAM, idle CPU, log silent for minutes after the last
    # `LogShaderCompilers: Num Already Dispatched: …` line).
    # CREATE_NO_WINDOW gives the editor its own (hidden) console with
    # valid stdio handles, which is what double-clicking the .uproject
    # in Explorer effectively does.
    flags = 0
    if os.name == "nt":
        flags = 0x00000200 | 0x08000000  # CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW
    subprocess.Popen(
        [str(editor_exe), str(uproject)],
        creationflags=flags,
        close_fds=True,
    )


def wait_for_bridge(timeout_s: float, project_name: "str | None" = None) -> bool:
    deadline = time.time() + timeout_s
    last_reported = 0.0
    cmd = [sys.executable, str(BRIDGE_PY), "--json"]
    if project_name:
        cmd += ["--project", project_name]
    cmd += ["--timeout", "3", "ping"]
    while time.time() < deadline:
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode == 0:
            try:
                data = json.loads(p.stdout)
                if data.get("success") and data.get("ready") is True:
                    return True
            except json.JSONDecodeError:
                pass
        now = time.time()
        if now - last_reported > 15:
            remaining = int(deadline - now)
            print(f"[rebuild] waiting for bridge ... ({remaining}s remaining)")
            last_reported = now
        time.sleep(2.0)
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--project-dir",
                    help="UE project root (contains Build.bat + .uproject). "
                         "Default: parsed from sync_plugin.bat DST line.")
    ap.add_argument("--uproject",
                    help="Explicit path to .uproject (default: first *.uproject "
                         "under --project-dir).")
    ap.add_argument("--editor-exe", default="",
                    help="UnrealEditor.exe path. If omitted, falls back to "
                         "UNREAL_EDITOR_EXE env var, then UE_ROOT env var.")
    ap.add_argument("--no-sync", action="store_true")
    ap.add_argument("--no-build", action="store_true",
                    help="Skip Build.bat. Useful if you've already compiled.")
    ap.add_argument("--no-launch", action="store_true",
                    help="Don't relaunch the editor after build.")
    ap.add_argument("--no-wait", action="store_true",
                    help="Don't poll the bridge after launching.")
    ap.add_argument("--wait-timeout", type=float, default=300,
                    help="Seconds to wait for bridge to come up (default: 300).")
    ap.add_argument("--quit-timeout", type=float, default=30,
                    help="Seconds to wait for graceful editor quit (default: 30).")
    ap.add_argument("--verbose", action="store_true",
                    help="Stream sync/build output to stdout.")
    ap.add_argument("--allow-global-kill", action="store_true",
                    help="If graceful quit fails and we couldn't match this "
                         "editor's PID via discovery, fall back to taskkill "
                         "/F /IM UnrealEditor.exe (kills ALL editor instances). "
                         "Off by default — multi-editor setups would lose work.")
    args = ap.parse_args()

    # Resolve project dir
    if args.project_dir:
        project_dir = pathlib.Path(args.project_dir)
    else:
        project_dir = parse_project_dir_from_sync_bat()
        if project_dir is None:
            sys.stderr.write(
                "Could not parse project dir from sync_plugin.bat. "
                "Pass --project-dir.\n"
            )
            return 5
    if not project_dir.is_dir():
        sys.stderr.write(f"project-dir does not exist: {project_dir}\n")
        return 5

    # Resolve uproject
    if args.uproject:
        uproject = pathlib.Path(args.uproject)
    else:
        uproject = find_uproject(project_dir)
        if uproject is None:
            sys.stderr.write(f"No .uproject found under {project_dir}\n")
            return 5
    if not uproject.is_file():
        sys.stderr.write(f"uproject does not exist: {uproject}\n")
        return 5

    editor_exe_raw = args.editor_exe or resolve_default_editor_exe()
    if not editor_exe_raw and not args.no_launch:
        sys.stderr.write(
            "UnrealEditor.exe path not provided.\n"
            "Pass --editor-exe, or set UNREAL_EDITOR_EXE or UE_ROOT.\n"
        )
        return 5
    editor_exe = pathlib.Path(editor_exe_raw) if editor_exe_raw else pathlib.Path()
    if not args.no_launch and not editor_exe.is_file():
        sys.stderr.write(f"UnrealEditor.exe not found at {editor_exe}\n")
        return 5

    # 1+2: shut the editor down — project-aware so we never wipe sibling
    # editors running other projects.
    target_pid, discovery_ok = get_editor_info_for_uproject(uproject)

    if target_pid is not None:
        print(f"[rebuild] this project's editor PID = {target_pid}")
        if is_pid_running(target_pid):
            print(f"[rebuild] editor is running — asking it to quit ...")
            if not quit_editor_gracefully(args.quit_timeout, target_pid,
                                          project_name=uproject.stem):
                print(f"[rebuild] graceful quit timed out — taskkill /F /PID {target_pid}")
                force_kill_pid(target_pid)
            if is_pid_running(target_pid):
                sys.stderr.write(
                    f"Editor (PID {target_pid}) still running after taskkill — aborting.\n"
                )
                return 6
    elif discovery_ok:
        # Discovery worked authoritatively — any running UnrealEditor.exe
        # is a sibling we should leave alone. Fall through to sync/build/launch.
        if is_editor_running():
            print("[rebuild] sibling editor(s) detected but none match this "
                  "uproject — leaving them alone.")
        else:
            print("[rebuild] no editor running for this project — proceeding.")
    elif is_editor_running():
        # Discovery failed AND an editor is running — we can't tell whether
        # it's ours or a sibling's. Refuse to force-kill blindly.
        if args.allow_global_kill:
            print("[rebuild] discovery failed; --allow-global-kill set — "
                  "falling back to global quit.")
            if not quit_editor_gracefully(args.quit_timeout, None):
                print("[rebuild] graceful quit timed out — global force kill.")
                force_kill_editor()
            if is_editor_running():
                sys.stderr.write("Editor still running after taskkill — aborting.\n")
                return 6
        else:
            sys.stderr.write(
                "[rebuild] discovery probe failed and an UnrealEditor.exe is "
                "running — cannot tell whether it's this project's editor or "
                "a sibling's.\n"
                f"  Target uproject: {uproject}\n"
                "Refusing to force-kill UnrealEditor.exe blindly.\n"
                "Either: (a) check that UnrealBridge plugin is enabled in the "
                "running editor (so discovery responds); (b) wait for it to "
                "finish loading past the splash if it's still starting; "
                "(c) pass --allow-global-kill if you're sure no sibling "
                "editors are running.\n"
            )
            return 6
    # else: discovery failed, no editor running — nothing to do, fall through.

    # 3: sync
    if not args.no_sync:
        rc = run_sync(args.verbose)
        if rc != 0:
            return rc

    # 4: build
    if not args.no_build:
        rc = run_build(project_dir, args.verbose)
        if rc != 0:
            return rc

    # 5: launch
    if not args.no_launch:
        launch_editor(editor_exe, uproject)

    # 6: wait for bridge — filter by project so we don't lock onto a
    # sibling editor's bridge while ours is still loading.
    if not args.no_launch and not args.no_wait:
        if wait_for_bridge(args.wait_timeout, project_name=uproject.stem):
            print("[rebuild] bridge is ready.")
            return 0
        sys.stderr.write(
            f"Bridge did not come up within {args.wait_timeout:.0f}s.\n"
        )
        return 7

    return 0


if __name__ == "__main__":
    sys.exit(main())
