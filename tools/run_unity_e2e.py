"""Run the Unity desktop end-to-end test: a Linux player (Mono or IL2CPP) of the dev project
(unity/Project) with the package's real payment sheet, Worker and localhost checkout page, paying on
a local chain.

The player runs the e2e driver (unity/Project/Assets/E2E/E2EDriver.cs), which speaks the Godot
e2e's protocol (JSON lines on stdin, "SCFG-REPLY" lines back), so this runner reuses the Godot
runner's chain families and scenario (tools/run_godot_e2e.py). The replies come on a named pipe
(SCFG_E2E_OUT): a Unity player redirects its own stdout into its log file. Per run:

  0. Broken e2e configs (bad JSON, a config Configure refuses): the player quits with code 1 by
     itself.
  1. Unlock Chapter 2 by manual transfer: copy the amount and address from the sheet, pay them
     from the chain side, wait for "Payment confirmed", check SCFG.Owns("chapter2") and the
     receipt in persistentDataPath/scfg/state.json. The demo pauses the game (timeScale 0) while
     the sheet is open.
  2. Tip with the browser wallet: open the localhost page the game opens in Playwright Chromium
     with a mock wallet (EIP-6963 for evm/arc, Wallet Standard for solana) and pay there.
  3. Restart the player: Chapter 2 is still owned.

--backend mono (default) or il2cpp (Managed Stripping Level High) builds the player first with
the Unity editor (--unity), unless --player gives an already built one. --family evm (default)
pays USDC on Anvil, arc native USDC on Anvil dressed as Arc, solana USDC on
solana-test-validator; they need `anvil` (Foundry) or `solana-test-validator` (Agave) on PATH,
Playwright's Chromium and `solders` (a dev dependency, so run it with `uv run`).

The player runs with -batchmode -nographics (UI Toolkit still lays out, focuses and dispatches
events; nothing is drawn). With SCFG_E2E_WINDOWED=1 it opens a 1280x720 window instead, under
`xvfb-run` when that is installed (CI), else on $DISPLAY. Its user data (HOME, XDG_*) lives in a
temporary directory, never the developer's. Every error, exception and warning the player logs
fails the run.

    uv run python tools/run_unity_e2e.py --unity ~/Unity/Hub/Editor/<version>/Editor/Unity \\
        [--backend mono|il2cpp] [--family evm|arc|solana] [--player DIR]
"""

from __future__ import annotations

import argparse
import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import run_godot_e2e as scenario  # noqa: E402  (families, scenario steps, helpers)
from run_godot_e2e import E2EError  # noqa: E402

ROOT = TOOLS.parent
PROJECT = ROOT / "unity" / "Project"
EXECUTABLE = "scfg-e2e.x86_64"
REPLY_PREFIX = b"SCFG-REPLY "
QUITTING = b"SCFG-QUITTING\n"
PROBLEM_PREFIX = "SCFG-PROBLEM "
REPLY_TIMEOUT = 30
BUILD_TIMEOUT = 3600


def build_player(unity: str, backend: str, out: Path, log: Path) -> Path:
    """Build the e2e player with the editor in batch mode; returns the executable."""
    cmd = [
        unity, "-batchmode", "-nographics", "-quit", "-projectPath", str(PROJECT),
        "-executeMethod", "Scfg.E2E.ScfgE2EBuild.BuildLinux",
        "-scfgBackend", backend, "-scfgOut", str(out), "-logFile", str(log),
    ]
    started = time.monotonic()
    done = subprocess.run(cmd, timeout=BUILD_TIMEOUT)
    exe = out / EXECUTABLE
    if done.returncode != 0 or not exe.exists():
        tail = log.read_text(errors="replace")[-6000:] if log.exists() else ""
        raise E2EError(f"the {backend} player did not build (exit {done.returncode}):\n{tail}")
    print(f"built the {backend} player in {time.monotonic() - started:.0f} s: {exe}")
    return exe


def problems(lines: list[str]) -> list[str]:
    """What the player reported as an error, exception, assert or warning."""
    return [line.rstrip("\n")[len(PROBLEM_PREFIX):] for line in lines
            if line.startswith(PROBLEM_PREFIX)]


class Game:
    """The player running the e2e driver; the same interface as run_godot_e2e.Game."""

    def __init__(self, exe: Path, env: dict, log: Path):
        self.log_path = log
        self._err = log.with_suffix(".stderr").open("wb")
        self._out = b""
        self._noise: list[str] = []
        self._fifo = log.with_suffix(".out")
        os.mkfifo(self._fifo)
        # opened for reading before the player starts, so its open for writing never blocks
        self._fd = os.open(self._fifo, os.O_RDONLY | os.O_NONBLOCK)
        env = {**env, "SCFG_E2E_OUT": str(self._fifo)}
        args = [str(exe), "-logFile", str(log)]
        if env.get("SCFG_E2E_WINDOWED") == "1":
            args += ["-screen-fullscreen", "0", "-screen-width", "1280", "-screen-height", "720"]
            if shutil.which("xvfb-run"):
                args = ["xvfb-run", "-a", "-s", "-screen 0 1280x720x24", *args]
        else:
            args += ["-batchmode", "-nographics"]
        self.proc = subprocess.Popen(
            args, cwd=exe.parent, env=env, stdin=subprocess.PIPE, stdout=self._err,
            stderr=subprocess.STDOUT,
        )

    def call(self, op: str, **args):
        line = json.dumps({"op": op, **args}).encode()
        try:
            self.proc.stdin.write(line + b"\n")
            self.proc.stdin.flush()
        except BrokenPipeError:
            raise E2EError(f"the game exited before {op}:\n{self.output()}") from None
        deadline = time.monotonic() + REPLY_TIMEOUT
        while True:
            out = self._read_line(deadline)
            if out is None:
                raise E2EError(f"the driver gave no reply to {op}:\n{self.output()}")
            if out.startswith(REPLY_PREFIX):
                break
            self._noise.append(out.decode(errors="replace"))
            if out == QUITTING:
                self._close_stdin()
        reply = json.loads(out[len(REPLY_PREFIX):])
        if "error" in reply:
            raise E2EError(f"{op} {args}: {reply['error']}")
        return reply["ok"]

    def _close_stdin(self) -> None:
        if self.proc.stdin and not self.proc.stdin.closed:
            try:
                self.proc.stdin.close()
            except BrokenPipeError:
                pass

    def _read_line(self, deadline):
        """The next line from the player, or None at the deadline or once it exited and the pipe is
        drained. Before the player opened the pipe a read is empty, not the end."""
        while b"\n" not in self._out:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            ready, _, _ = select.select([self._fd], [], [], min(left, 0.2))
            chunk = os.read(self._fd, 1 << 16) if ready else b""
            if chunk:
                self._out += chunk
                continue
            if self.proc.poll() is not None:
                return None  # exited and drained
            if ready:
                time.sleep(0.05)  # no writer yet (or between writers): select keeps saying ready
        line, _, self._out = self._out.partition(b"\n")
        return line + b"\n"

    def wait_exit(self, timeout: float) -> int:
        deadline = time.monotonic() + timeout
        while (out := self._read_line(deadline)) is not None:
            self._noise.append(out.decode(errors="replace"))
            if out == QUITTING:
                self._close_stdin()
        try:
            return self.proc.wait(max(deadline - time.monotonic(), 0.1))
        except subprocess.TimeoutExpired:
            raise E2EError(
                f"the game did not quit within {timeout:g} s:\n{self.output()}"
            ) from None

    def snapshot(self) -> dict:
        return self.call("snapshot")

    def click(self, name: str, where: str = "screen"):
        return self.call("click", name=name, **{"in": where})

    def quit(self) -> int:
        self.call("quit")
        self._close_stdin()
        try:
            code = self.proc.wait(60)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise E2EError(f"the game did not quit:\n{self.output()}") from None
        self._drain()
        return code

    def _drain(self) -> None:
        while (out := self._read_line(time.monotonic() + 2)) is not None:
            self._noise.append(out.decode(errors="replace"))
        self._noise.extend(x.decode(errors="replace") + "\n" for x in self._out.splitlines())
        self._out = b""

    def lines(self) -> list[str]:
        return list(self._noise)

    def output(self) -> str:
        log = self.log_path.read_text(errors="replace") if self.log_path.exists() else ""
        return "".join(self._noise) + log[-8000:]

    def close(self) -> None:
        if self.proc.poll() is None:
            self._close_stdin()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        os.close(self._fd)
        self._fifo.unlink(missing_ok=True)
        self._err.close()


BAD_HOOKS = (
    ("[1, 2]", "not a JSON object"),
    (json.dumps({"configure": {"wallet": "0x123"}, "items": {}}), "scfg: "),
)


def bad_hooks_quit_at_once(exe: Path, env: dict, work: Path) -> None:
    for i, (text, reason) in enumerate(BAD_HOOKS):
        config = work / f"bad_hook_{i}.json"
        config.write_text(text)
        game = Game(exe, {**env, "SCFG_E2E_CONFIG": str(config)}, work / f"bad_hook_{i}.log")
        try:
            code = game.wait_exit(60)
            lines = game.lines()
        finally:
            game.close()
        bad = problems(lines)
        if code != 1 or not bad or not all(reason in line for line in bad):
            raise E2EError(
                f"with the e2e config {text!r} the player should report why ({reason!r}) and quit "
                f"with code 1 (it exited with {code}):\n{''.join(lines)}"
            )
    print(f"0. bad e2e configs: the player quits with code 1 ({len(BAD_HOOKS)} cases)")


def quit_cleanly(game: Game) -> list[str]:
    code = game.quit()
    if code != 0:
        raise E2EError(f"the game exited with code {code}:\n{game.output()}")
    return game.lines()


def run(unity: str | None, backend: str, family: str, player: str | None) -> int:
    fam_cls = scenario.FAMILIES[family]
    exe_tool = shutil.which(fam_cls.tool)
    if not exe_tool:
        print(fam_cls.missing)
        return 2
    if not player and not unity:
        print("give --unity (the editor, to build the player) or --player (a built one)")
        return 2
    from playwright.sync_api import sync_playwright

    work = Path(tempfile.mkdtemp(prefix=f"scfg-unity-e2e-{backend}-"))
    home = work / "home"
    env = scenario.isolated_env(home)
    fam = None
    game = None
    started = time.monotonic()
    try:
        if player:
            exe = Path(player) / EXECUTABLE if Path(player).is_dir() else Path(player)
        else:
            exe = build_player(unity, backend, work / "player", work / "build.log")
        fam = fam_cls(exe_tool, work)
        config = work / "e2e_config.json"
        config.write_text(json.dumps(fam.config()))
        env.update({"SCFG_E2E_CONFIG": str(config), "SCFG_E2E_CHAIN": json.dumps(fam.chain())})
        print(f"Unity e2e: {family} on the {backend} player {exe}")
        bad_hooks_quit_at_once(exe, env, work)
        outputs: list[str] = []
        with sync_playwright() as p:
            browser = scenario.launch_chromium(p)
            game = Game(exe, env, work / "game1.log")
            state_file = Path(game.call("user_dir")) / "scfg" / "state.json"
            if not state_file.is_relative_to(home):
                raise E2EError(f"user data is not isolated: {state_file}")
            print("1. unlock by manual transfer")
            scenario.unlock_by_manual_transfer(game, fam, state_file)
            print("2. tip with the browser wallet")
            scenario.tip_by_browser_wallet(game, fam, browser, state_file)
            outputs += quit_cleanly(game)
            game.close()
            print("3. restart")
            game = Game(exe, env, work / "game2.log")
            scenario.still_owned_after_restart(game)
            outputs += quit_cleanly(game)
            browser.close()
        bad = problems(outputs)
        if bad:
            raise E2EError("the player logged errors or warnings:\n" + "\n".join(bad))
    except Exception as e:
        print(e if isinstance(e, E2EError) else traceback.format_exc())
        print(f"Unity e2e ({backend}, {family}) FAILED after {time.monotonic() - started:.1f} s "
              f"(logs in {work})")
        return 1
    finally:
        if game is not None:
            game.close()
        if fam is not None:
            fam.cleanup()
    shutil.rmtree(work, ignore_errors=True)
    print(f"Unity e2e ({backend}, {family}) PASSED in {time.monotonic() - started:.1f} s")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--unity", help="the Unity editor binary (builds the player)")
    ap.add_argument("--player", help="an already built e2e player (its folder or executable)")
    ap.add_argument("--backend", choices=["mono", "il2cpp"], default="mono",
                    help="scripting backend of the player to build (default: mono)")
    ap.add_argument("--family", choices=sorted(scenario.FAMILIES), default="evm",
                    help="chain family (default: evm)")
    args = ap.parse_args()
    return run(args.unity, args.backend, args.family, args.player)


if __name__ == "__main__":
    raise SystemExit(main())
