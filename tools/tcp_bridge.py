"""
WebSocket-to-TCP bridge for Ratspeak Hub connectivity.

Bridges a WebSocket connection from the flasher web UI to a raw TCP
server (e.g. rns.ratspeak.org:4242). The browser cannot make raw TCP
connections, so this proxy relays binary frames between the two.

Usage:
    python tools/tcp_bridge.py [--ws-port 8765] [--tcp-host rns.ratspeak.org] [--tcp-port 4242]

The flasher web UI connects via: ws://127.0.0.1:8765
"""
from __future__ import annotations

import argparse
import asyncio
import signal
import sys

try:
    import websockets
    from websockets.asyncio.server import serve as ws_serve
except ImportError:
    print("ERROR: 'websockets' package is required.")
    print("Install with:  pip install websockets")
    sys.exit(1)


async def bridge_handler(
    websocket,
    tcp_host: str,
    tcp_port: int,
) -> None:
    """Handle one WebSocket client by bridging to the TCP server."""
    client_addr = websocket.remote_address
    print(f"[bridge] WebSocket client connected: {client_addr}")

    try:
        reader, writer = await asyncio.open_connection(tcp_host, tcp_port)
        print(f"[bridge] TCP connected to {tcp_host}:{tcp_port}")
    except Exception as exc:
        print(f"[bridge] TCP connection failed: {exc}")
        await websocket.close(1011, f"TCP connect failed: {exc}")
        return

    async def ws_to_tcp() -> None:
        try:
            async for message in websocket:
                if isinstance(message, bytes):
                    writer.write(message)
                    await writer.drain()
                elif isinstance(message, str):
                    writer.write(message.encode("utf-8"))
                    await writer.drain()
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            writer.close()

    async def tcp_to_ws() -> None:
        try:
            while True:
                data = await reader.read(4096)
                if not data:
                    break
                await websocket.send(data)
        except Exception:
            pass
        finally:
            await websocket.close()

    await asyncio.gather(ws_to_tcp(), tcp_to_ws())
    print(f"[bridge] Session ended for {client_addr}")


async def main_async(ws_host: str, ws_port: int, tcp_host: str, tcp_port: int) -> None:
    handler = lambda ws: bridge_handler(ws, tcp_host, tcp_port)

    async with ws_serve(handler, ws_host, ws_port) as server:
        print(f"\n  Ratspeak Hub TCP Bridge")
        print(f"  WebSocket:  ws://{ws_host}:{ws_port}")
        print(f"  TCP target: {tcp_host}:{tcp_port}")
        print(f"  Press Ctrl+C to stop.\n")
        await asyncio.get_event_loop().create_future()  # run forever


def main() -> None:
    parser = argparse.ArgumentParser(description="WebSocket-to-TCP bridge for Ratspeak Hub")
    parser.add_argument("--ws-host", default="127.0.0.1", help="WebSocket bind host")
    parser.add_argument("--ws-port", type=int, default=8765, help="WebSocket bind port")
    parser.add_argument("--tcp-host", default="rns.ratspeak.org", help="TCP server host")
    parser.add_argument("--tcp-port", type=int, default=4242, help="TCP server port")
    args = parser.parse_args()

    try:
        asyncio.run(main_async(args.ws_host, args.ws_port, args.tcp_host, args.tcp_port))
    except KeyboardInterrupt:
        print("\n[bridge] Stopped.")


if __name__ == "__main__":
    main()
