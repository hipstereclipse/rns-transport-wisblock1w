from __future__ import annotations

import argparse
import functools
import http.server
import pathlib
import socketserver
import threading
import time
import webbrowser

                                                                                                                                                                                                                                                                                                                                                                                
def main() -> None:
    parser = argparse.ArgumentParser(description="Serve the flasher over HTTP for Chrome/Edge Web Serial and File System Access support.")
    parser.add_argument("--host", default="127.0.0.1", help="Host interface to bind to")
    parser.add_argument("--port", type=int, default=8000, help="Port to bind to")
    parser.add_argument("--no-open", action="store_true", help="Do not open a browser automatically")
    args = parser.parse_args()

    workspace_root = pathlib.Path(__file__).resolve().parent.parent
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(workspace_root))

    class ReusableTCPServer(socketserver.TCPServer):
        allow_reuse_address = True

    with ReusableTCPServer((args.host, args.port), handler) as httpd:
        url = f"http://{args.host}:{args.port}/flasher/"
        print(f"Serving {workspace_root} at {url}")
        print("Open the flasher in Chrome or Edge, not an embedded preview pane.")

        if not args.no_open:
            threading.Thread(target=lambda: (time.sleep(0.5), webbrowser.open(url)), daemon=True).start()

        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nStopping server...")


if __name__ == "__main__":
    main()
