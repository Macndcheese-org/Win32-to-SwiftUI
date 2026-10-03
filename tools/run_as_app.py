#!/usr/bin/env python3
"""Start a Windows program as a Mac app: its own Dock name and a native app icon.

    run_as_app.py --prefix ~/mnc-11.18/w2s-prefix 'C:\\Program Files\\Notepad++\\notepad++.exe' [args...]

Builds (or refreshes) `<Name>.app` for the program with app_stub.py, and starts the
program from it, so macOS names it, and draws its icon in the user's icon style
(Liquid Glass, clear, tinted), like any Mac app's. A prototype of what the launcher
would do for every program it starts; see app_stub.py.
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app_stub  # noqa: E402

DEFAULT_ENGINE = "/Applications/MacNdCheese Launcher.app/Contents/Resources/wine-unified"
DEFAULT_APPS = os.path.expanduser("~/Library/Application Support/MacNCheese/Apps")


def unix_path(prefix, windows_path):
    """a Windows path of the prefix's C: or of Z: as a Unix path"""
    drive, _, rest = windows_path.partition(":")
    rest = rest.replace("\\", "/")
    if drive.upper() == "C":
        return os.path.join(prefix, "drive_c", rest.lstrip("/"))
    if drive.upper() == "Z":
        return rest
    raise SystemExit("only C: and Z: paths: %s" % windows_path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--engine", default=DEFAULT_ENGINE)
    ap.add_argument("--apps", default=DEFAULT_APPS, help="where the stubs are kept")
    ap.add_argument("--name", help="the app's name (default: the exe's file name)")
    ap.add_argument("exe", help="the program, as a Windows path")
    ap.add_argument("args", nargs="*")
    opts = ap.parse_args()

    prefix = os.path.expanduser(opts.prefix)
    os.makedirs(opts.apps, exist_ok=True)
    stub = app_stub.build(opts.engine, unix_path(prefix, opts.exe), opts.apps, opts.name)

    env = dict(os.environ, WINEPREFIX=prefix, WINELOADER=stub, WINEDEBUG=os.environ.get("WINEDEBUG", "-all"),
               DYLD_FALLBACK_LIBRARY_PATH=os.environ.get("DYLD_FALLBACK_LIBRARY_PATH", "/usr/local/lib"),
               WINEDLLOVERRIDES=os.environ.get("WINEDLLOVERRIDES", "mscoree,mshtml="))
    proc = subprocess.Popen([stub, opts.exe] + opts.args, env=env, start_new_session=True,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("started %s (pid %d) from %s" % (opts.exe, proc.pid, os.path.dirname(os.path.dirname(os.path.dirname(stub)))))


if __name__ == "__main__":
    main()
