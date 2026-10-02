"""Converts an original Xbox game (ISO or XBE) into a native program.

    python tools/convert.py <game.iso | default.xbe> [output dir] [--decompile] [--jobs N]

Steps: extract the disc -> identify the title -> find XDK library functions
(XbSymbolDatabase) -> optional Ghidra decompilation -> lift the x86 code to C
(tools/lift) -> compile the C with the runtime -> package. The result is
written next to the input as "<name>_win/" unless an output directory is
given:

    play.bat / KakutoChojin.exe / SDL3.dll   the game
    KakutoChojin.ini                         video settings
    game/                              files from the disc (ISO input)
    analysis/                          manifest, symbols, lifted C, decompiled C
    hdd/                               Xbox hard disk (saves), created on first start
    _build/                            compiler intermediates (can be deleted)

The tools come from deps/ (tools/setup.ps1). Everything derived from the
game stays in the output folder.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEPS = ROOT / "deps"
sys.path.insert(0, str(ROOT / "tools"))

CMAKE = DEPS / "cmake" / "bin" / "cmake.exe"
NINJA = DEPS / "ninja" / "ninja.exe"
REGIONS = {1: "North America", 2: "Japan", 4: "Rest of the world", 0x80000000: "Manufacturing"}


def step(n, text):
    print(f"[{n}/6] {text}", flush=True)


def fail(msg):
    print(f"\nERROR: {msg}", flush=True)
    sys.exit(1)


def run(cmd, log, cwd=ROOT, env=None):
    """Runs a tool with its output in `log`; stops the conversion on failure."""
    with open(log, "a", encoding="utf-8", errors="replace") as f:
        f.write("\n$ " + " ".join(str(c) for c in cmd) + "\n")
        f.flush()
        r = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        fail(f"{Path(str(cmd[0])).name} failed (exit {r.returncode}); see {log}")


def tool_env():
    env = dict(os.environ)
    paths = [DEPS / "llvm-mingw" / "bin", DEPS / "cmake" / "bin", DEPS / "ninja", DEPS / "python"]
    env["PATH"] = os.pathsep.join(str(p) for p in paths) + os.pathsep + env.get("PATH", "")
    return env


def title_info(xbe_path):
    d = Path(xbe_path).read_bytes()
    if d[:4] != b"XBEH":
        fail(f"{xbe_path} is not an XBE")
    base, = struct.unpack_from("<I", d, 0x104)
    timestamp, cert = struct.unpack_from("<II", d, 0x114)
    c = cert - base
    title_id, = struct.unpack_from("<I", d, c + 8)
    name = d[c + 12:c + 12 + 80].decode("utf-16le", "replace").split("\0")[0]
    region, = struct.unpack_from("<I", d, c + 0xA0)
    version, = struct.unpack_from("<I", d, c + 0xAC)
    return {"id": title_id, "name": name, "region": region, "version": version, "timestamp": timestamp}


def write_ini(path):
    if path.exists():
        return
    path.write_text(
        "[video]\n"
        "; Output height in pixels: 0 = original 480, or 720 / 1080 / 1440 / 2160\n"
        "resolution=1080\n"
        "; 1 = 16:9 - 3D scenes are widened, menus and HUD stay 4:3 in the middle\n"
        "widescreen=1\n"
        "; 1 = start in borderless fullscreen - Alt+Enter or F11 toggles\n"
        "fullscreen=0\n"
        "; Renderer: d3d11 or gl (OpenGL 4.5)\n"
        "gpu=d3d11\n"
        "; 1 = show the settings window at every start\n"
        "launcher=1\n",
        encoding="ascii", newline="\r\n")


def main():
    ap = argparse.ArgumentParser(description="Convert an original Xbox game into a native program")
    ap.add_argument("input", help="disc image (.iso, redump or xiso) or default.xbe")
    ap.add_argument("output", nargs="?", help="output directory (default: <name>_win next to the input)")
    ap.add_argument("--decompile", action="store_true",
                    help="also decompile to readable C with Ghidra (setup.ps1 -Ghidra); improves function discovery")
    ap.add_argument("--jobs", type=int, default=2, help="parallel compile jobs (default 2)")
    args = ap.parse_args()

    src = Path(args.input).resolve()
    if not src.exists():
        fail(f"{src} not found")
    out = Path(args.output).resolve() if args.output else src.parent / (src.stem + "_win")
    out.mkdir(parents=True, exist_ok=True)
    analysis = out / "analysis"
    analysis.mkdir(exist_ok=True)
    log = out / "convert.log"
    log.write_text("", encoding="utf-8")
    py = sys.executable
    env = tool_env()
    print(f"Input:  {src}\nOutput: {out}\n", flush=True)

    # 1. Game files
    if src.suffix.lower() == ".iso":
        step(1, "Extracting files from the disc image...")
        game = out / "game"
        if not (game / "default.xbe").exists():
            run([py, ROOT / "tools" / "xdvdfs.py", src, "extract", game], log)
        xbe = game / "default.xbe"
    else:
        step(1, "Using the XBE; game files are read from its folder")
        xbe, game = src, src.parent
    if not xbe.exists():
        fail("default.xbe not found on the disc")

    # 2. Which game and version
    info = title_info(xbe)
    key = f"{info['id']:08x}_{info['timestamp']:08x}"
    regions = ", ".join(v for k, v in REGIONS.items() if info["region"] & k) or f"0x{info['region']:x}"
    step(2, f"{info['name']}  (title {info['id']:08X}, version {info['version']}, {regions}, build {info['timestamp']:08x})")
    # hints/<title>_<build>.functions.txt: function entries (Ghidra's list for a
    # known version); .extra.txt: entries only reached through pointers.
    hint_functions = ROOT / "hints" / f"{key}.functions.txt"
    hint_extra = ROOT / "hints" / f"{key}.extra.txt"
    known = hint_functions.exists()
    if known:
        print(f"      known version: using hints/{key}.*", flush=True)
    else:
        print("      not tested with this tool yet. Without --decompile (Ghidra) the function search is less\n"
              "      complete, and the game may stop where the conversion missed code.", flush=True)

    # 3. Library functions and the per-title manifest
    step(3, "Identifying XDK library functions...")
    cli = DEPS / "XbSymbolDatabase" / "build" / "projects" / "cli" / "XbSymbolDatabaseCLI.exe"
    with open(analysis / "symbols.txt", "w") as f, open(analysis / "symbols_err.txt", "w") as e:
        if subprocess.run([str(cli), str(xbe), "-e"], stdout=f, stderr=e).returncode != 0:
            fail("XbSymbolDatabase failed; see analysis/symbols_err.txt")
    run([py, ROOT / "tools" / "gen_manifest.py", xbe, analysis / "symbols.txt", analysis / "manifest.txt"], log)
    tenv = dict(env, KT_XBE=str(xbe), KT_ANALYSIS=str(analysis))
    run([py, ROOT / "tools" / "analyze_rtti.py"], log, env=tenv)

    # 4. Ghidra (optional)
    functions = ["--functions", hint_functions] if known else []
    if args.decompile:
        step(4, "Decompiling with Ghidra (takes several minutes)...")
        if not (DEPS / "ghidra" / "support" / "analyzeHeadless.bat").exists():
            fail("Ghidra is not set up: run  powershell -ExecutionPolicy Bypass -File tools\\setup.ps1 -Ghidra")
        run(["cmd", "/c", ROOT / "tools" / "decompile.bat", DEPS / "ghidra", DEPS / "jdk"], log, env=tenv)
        functions = ["--functions", analysis / "decomp" / "index.tsv"]  # this run's list (same as the hint)
        print(f"      readable C: {analysis / 'decomp'}", flush=True)
    else:
        step(4, "Skipping decompilation to readable C (use --decompile)")

    # 5. x86 -> C
    step(5, "Translating the game code to C...")
    extra = ["--extra", hint_extra] if hint_extra.exists() else []
    run([py, ROOT / "tools" / "lift" / "lift.py", xbe, analysis / "manifest.txt", analysis / "lifted"] + functions + extra,
        log)

    # 6. Compile and package
    step(6, f"Compiling (Clang, {args.jobs} jobs; the first build takes a while)...")
    build = out / "_build"
    clang = DEPS / "llvm-mingw" / "bin"
    run([CMAKE, "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_MAKE_PROGRAM={NINJA}",
         f"-DCMAKE_C_COMPILER={clang / 'clang.exe'}", f"-DCMAKE_CXX_COMPILER={clang / 'clang++.exe'}", f"-DKT_LIFTED_DIR={analysis / 'lifted'}", f"-DKT_LIFTED_JOBS={args.jobs}"],
        log, env=env)
    run([CMAKE, "--build", build, "--target", "kt_run", "-j", str(args.jobs)], log, env=env)
    for old in ("kt_run.exe", "kt_host.log"):  # earlier versions of this tool
        (out / old).unlink(missing_ok=True)
    if (out / "kt_recomp.ini").exists() and not (out / "KakutoChojin.ini").exists():
        (out / "kt_recomp.ini").rename(out / "KakutoChojin.ini")
    for f in ("KakutoChojin.exe", "SDL3.dll"):
        shutil.copy2(build / "bin" / f, out / f)
    (out / "play.bat").write_text(
        "@echo off\r\n"
        "cd /d \"%~dp0\"\r\n"
        f"\"%~dp0KakutoChojin.exe\" \"{os.path.relpath(game, out) if game.is_relative_to(out) else game}\" "
        "analysis\\manifest.txt hdd\r\n"
        "if errorlevel 1 pause\r\n", encoding="mbcs", errors="replace")  # cmd reads the ANSI code page
    write_ini(out / "KakutoChojin.ini")

    print(f"\nDone. Start the game with \"{out / 'play.bat'}\"")
    print("Keys: arrows=D-pad  Z/X/C/V=A/B/X/Y  A/S=WHITE/BLACK  Q/W=LT/RT  Enter=START  Backspace=BACK")
    print("      Game controllers are supported too.")


if __name__ == "__main__":
    main()
