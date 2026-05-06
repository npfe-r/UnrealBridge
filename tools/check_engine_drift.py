#!/usr/bin/env python3
"""Detect drift between a UE project's EngineAssociation and the bridge editor.

`.uproject` files pin an engine version via the `EngineAssociation` field —
either a literal version string ("5.7") or a GUID pointing at a registered
custom build. When the connected bridge editor is a different major.minor,
loaded assets / DLLs may be silently incompatible (the editor refuses to load
the project but bridge already attached, etc).

This tool reports the drift so you can switch editors before debugging
mysterious mismatches.

Usage:
    python tools/check_engine_drift.py                       # auto-discover .uproject
    python tools/check_engine_drift.py --uproject PATH       # explicit
    python tools/check_engine_drift.py --project NAME        # multi-editor disambiguator
    python tools/check_engine_drift.py --json                # machine-readable
    python tools/check_engine_drift.py --strict              # exit 1 on patch-level diff too

Exit codes: 0 = match, 1 = drift, 2 = setup failure (no .uproject, bridge
unreachable, parse error, etc.).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
BRIDGE_PY = REPO_ROOT / ".claude" / "skills" / "unreal-bridge" / "scripts" / "bridge.py"

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def find_uproject(start: Path) -> Optional[Path]:
    """Walk up from `start` looking for any `.uproject`. Stops at filesystem root."""
    for parent in [start, *start.parents]:
        for f in parent.glob("*.uproject"):
            return f
    return None


def parse_engine_association(uproject_path: Path) -> str:
    """Read EngineAssociation. May be a version string ("5.7") or GUID."""
    # .uproject files are sometimes UTF-8 BOM, sometimes not.
    text = uproject_path.read_text(encoding="utf-8-sig")
    data = json.loads(text)
    return str(data.get("EngineAssociation", "")).strip()


def get_bridge_engine_version(project_filter: Optional[str], timeout: int) -> Optional[str]:
    """Ask the connected bridge editor for its FEngineVersion::Current()."""
    if not BRIDGE_PY.exists():
        return None
    cmd = ["python", str(BRIDGE_PY)]
    if project_filter:
        cmd.append(f"--project={project_filter}")
    cmd += ["exec",
            "import unreal; print(unreal.UnrealBridgeEditorLibrary.get_engine_version())"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              encoding="utf-8", timeout=timeout)
    except (subprocess.TimeoutExpired, OSError):
        return None
    if proc.returncode != 0:
        return None
    # bridge.py prints the script's stdout; last non-empty line is the version.
    for line in reversed(proc.stdout.strip().splitlines()):
        line = line.strip()
        if line and not line.startswith("["):  # filter bridge.py's own log lines
            return line
    return None


def normalize_version(v: str) -> Optional[tuple]:
    """Extract (major, minor[, patch]) tuple from an engine version string.

    Handles:
        "5.7"                                → (5, 7)
        "5.7.1"                              → (5, 7, 1)
        "5.7.1-48512491+++UE5+Release-5.7"   → (5, 7, 1)
    """
    m = re.match(r"^(\d+)\.(\d+)(?:\.(\d+))?", v)
    if not m:
        return None
    parts = [int(m.group(1)), int(m.group(2))]
    if m.group(3):
        parts.append(int(m.group(3)))
    return tuple(parts)


def is_guid(s: str) -> bool:
    return bool(re.match(r"^\{[0-9A-Fa-f-]{36}\}$", s))


def main() -> int:
    ap = argparse.ArgumentParser(description="Check .uproject engine vs connected bridge editor.")
    ap.add_argument("--uproject", help="Path to .uproject (auto-discover if omitted)")
    ap.add_argument("--project", help="Project name filter forwarded to bridge.py "
                                      "(disambiguates multi-editor setups)")
    ap.add_argument("--timeout", type=int, default=10, help="Bridge timeout seconds")
    ap.add_argument("--json", action="store_true", help="Emit JSON instead of text")
    ap.add_argument("--strict", action="store_true",
                    help="Treat patch-level differences as drift too")
    args = ap.parse_args()

    # 1) Resolve .uproject
    if args.uproject:
        uproject = Path(args.uproject)
    else:
        uproject = find_uproject(Path.cwd())

    if not uproject or not uproject.exists():
        msg = "no .uproject found (use --uproject PATH)"
        print(json.dumps({"error": msg}) if args.json else f"error: {msg}", file=sys.stderr)
        return 2

    try:
        assoc = parse_engine_association(uproject)
    except (json.JSONDecodeError, OSError) as e:
        msg = f"failed to parse {uproject}: {e}"
        print(json.dumps({"error": msg}) if args.json else f"error: {msg}", file=sys.stderr)
        return 2

    # 2) Query bridge
    bridge_ver = get_bridge_engine_version(args.project, args.timeout)
    if bridge_ver is None:
        msg = "bridge unreachable (start the editor with the UnrealBridge plugin)"
        print(json.dumps({"error": msg}) if args.json else f"error: {msg}", file=sys.stderr)
        return 2

    # 3) Compare
    bridge_tuple = normalize_version(bridge_ver)
    project_tuple: Optional[tuple] = None
    project_repr: str = assoc
    project_kind: str

    if not assoc:
        project_kind = "unset"
        project_repr = "(empty EngineAssociation)"
    elif is_guid(assoc):
        project_kind = "guid"
        # Custom / source builds — can't normalize without a registry lookup.
        # Report as "unresolved" rather than failing.
    else:
        project_kind = "version"
        project_tuple = normalize_version(assoc)

    drift = False
    note = ""

    if project_kind == "version" and project_tuple and bridge_tuple:
        # Major.minor must match. Patch differences only matter under --strict.
        if project_tuple[:2] != bridge_tuple[:2]:
            drift = True
            note = "major.minor mismatch — definitely incompatible"
        elif args.strict and len(project_tuple) >= 3 and len(bridge_tuple) >= 3 \
                and project_tuple[2] != bridge_tuple[2]:
            drift = True
            note = "patch-level mismatch (strict mode)"
        else:
            note = "match"
    elif project_kind == "guid":
        note = ("EngineAssociation is a GUID (custom / source build); "
                "can't auto-resolve without registry lookup")
    elif project_kind == "unset":
        drift = True
        note = "EngineAssociation is empty — UE will prompt to associate"

    out = {
        "uproject": str(uproject),
        "uproject_engine_association": assoc,
        "uproject_engine_kind": project_kind,
        "bridge_engine_version": bridge_ver,
        "bridge_engine_tuple": list(bridge_tuple) if bridge_tuple else None,
        "drift": drift,
        "note": note,
    }

    if args.json:
        print(json.dumps(out, indent=2, ensure_ascii=False))
    else:
        print(f"Project   : {uproject}")
        print(f"  EngineAssociation: {project_repr}  [{project_kind}]")
        print(f"Bridge editor:")
        print(f"  Engine version   : {bridge_ver}")
        print()
        marker = "✗" if drift else "✓"
        print(f"  [{marker}] {note}")

    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main())
