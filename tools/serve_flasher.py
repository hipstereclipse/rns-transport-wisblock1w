from __future__ import annotations

import argparse
import http.server
import mimetypes
import pathlib
import subprocess
import socketserver
import sys
import threading
import time
import webbrowser


# Ensure .uf2 firmware files are served with a binary MIME type
mimetypes.add_type("application/octet-stream", ".uf2")


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler with no-cache headers for development."""

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def log_message(self, fmt: str, *args: object) -> None:  # type: ignore[override]
        # Colour 200/304 green, 4xx/5xx red in terminals that support ANSI
        code = args[1] if len(args) > 1 else ""
        colour = ""
        reset = "\033[0m"
        if str(code).startswith(("4", "5")):
            colour = "\033[91m"
        elif str(code).startswith(("2", "3")):
            colour = "\033[92m"
        sys.stderr.write(f"{colour}{self.address_string()} - {fmt % args}{reset}\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Serve the flasher UI over HTTP for Chrome/Edge Web Serial support.",
    )
    parser.add_argument("--host", default="127.0.0.1", help="Host interface to bind to")
    parser.add_argument("--port", type=int, default=8000, help="Port to bind to")
    parser.add_argument("--no-open", action="store_true", help="Do not open a browser automatically")
    parser.add_argument(
        "--hub-bridge",
        action="store_true",
        help="Also start the local WebSocket-to-TCP bridge for Ratspeak Hub access",
    )
    parser.add_argument("--hub-ws-host", default="127.0.0.1", help="WebSocket bind host for the local hub bridge")
    parser.add_argument("--hub-ws-port", type=int, default=8765, help="WebSocket bind port for the local hub bridge")
    parser.add_argument("--hub-tcp-host", default="rns.ratspeak.org", help="Upstream Ratspeak Hub TCP host")
    parser.add_argument("--hub-tcp-port", type=int, default=4242, help="Upstream Ratspeak Hub TCP port")
    args = parser.parse_args()

    workspace_root = pathlib.Path(__file__).resolve().parent.parent
    hub_proc: subprocess.Popen[str] | None = None

    class ReusableTCPServer(socketserver.TCPServer):
        allow_reuse_address = True

    handler = lambda *a, **kw: NoCacheHandler(*a, directory=str(workspace_root), **kw)

    try:
        hub_url = f"ws://{args.hub_ws_host}:{args.hub_ws_port}"
        if args.hub_bridge:
            bridge_script = workspace_root / "tools" / "tcp_bridge.py"
            hub_cmd = [
                sys.executable,
                str(bridge_script),
                "--ws-host",
                args.hub_ws_host,
                "--ws-port",
                str(args.hub_ws_port),
                "--tcp-host",
                args.hub_tcp_host,
                "--tcp-port",
                str(args.hub_tcp_port),
            ]
            print(f"\n  Starting Ratspeak Hub bridge: {hub_url} -> {args.hub_tcp_host}:{args.hub_tcp_port}")
            hub_proc = subprocess.Popen(hub_cmd, cwd=str(workspace_root))

        with ReusableTCPServer((args.host, args.port), handler) as httpd:
            url = f"http://{args.host}:{args.port}/flasher/"
            if args.hub_bridge:
                url += f"?hubWs={hub_url}"
            print(f"\n  Serving  {workspace_root}")
            print(f"  Flasher  {url}")
            print("  Network  The RAK3401/RAK13302 firmware is LoRa-only; hub access uses this host's network connection.")
            print(f"  Press Ctrl+C to stop.\n")

            if not args.no_open:
                threading.Thread(target=lambda: (time.sleep(0.5), webbrowser.open(url)), daemon=True).start()

            httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server...")
    except OSError as exc:
        if "address already in use" in str(exc).lower() or getattr(exc, "winerror", None) == 10048:
            print(f"\n  ERROR: Port {args.port} is already in use.")
            print(f"  Try:  python {pathlib.Path(__file__).name} --port {args.port + 1}\n")
            sys.exit(1)
        raise
    finally:
        if hub_proc and hub_proc.poll() is None:
            hub_proc.terminate()
            try:
                hub_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                hub_proc.kill()


if __name__ == "__main__":
    main()
