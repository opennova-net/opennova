"""Run opennova-serve for a parity cell and stop it as its console's Ctrl+Break does.

    python -I serve_host.py --exe <opennova-serve.exe> --cwd <dir> --log <file>
        --pid-file <file> --stop-file <file> -- <opennova-serve arguments>

Starts opennova-serve in `--cwd` (its game.cfg, admin.cfg and saves are read there) in a
process group of its own sharing this process's console, its output to `--log`, and writes
its pid to `--pid-file`. It then waits until `--stop-file` appears or the server exits; at
the stop file it sends the group CTRL_BREAK_EVENT, which opennova-serve's console handler
takes as a stop (the session's round reset and STOP goodbye, the socket closed, `stopped`
logged), and waits 20 s for it. The exit code is opennova-serve's own, or 124 when it had to
be terminated. run_parity_topology.ps1 (-OpenNovaHostKind serve) starts it hidden, so the
console the break travels on is this process's own. Windows only.
"""
import argparse
import os
import signal
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--cwd", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--pid-file", required=True)
    ap.add_argument("--stop-file", required=True)
    ap.add_argument("serve_args", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    serve_args = args.serve_args[1:] if args.serve_args[:1] == ["--"] else args.serve_args
    with open(args.log, "wb") as log:
        server = subprocess.Popen([args.exe, *serve_args], cwd=args.cwd, stdout=log,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        with open(args.pid_file, "w", encoding="ascii") as f:
            f.write(str(server.pid))
        while server.poll() is None and not os.path.exists(args.stop_file):
            time.sleep(0.2)
        if server.poll() is None:
            os.kill(server.pid, signal.CTRL_BREAK_EVENT)
            try:
                server.wait(20)
            except subprocess.TimeoutExpired:
                server.terminate()
                server.wait(10)
                return 124
        return server.returncode


if __name__ == "__main__":
    sys.exit(main())
