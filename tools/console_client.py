#!/usr/bin/env python3
"""
console_client.py — Interactive serial terminal for the RNS Transport Node.

Features:
  - Auto-detects WisBlock serial port
  - Color-coded output (green for prompts, yellow for warnings)
  - Command history (up/down arrows)
  - Log-to-file option

Usage:
    python console_client.py
    python console_client.py --port /dev/ttyACM0
    python console_client.py --port COM3 --log session.txt
"""
import argparse
import serial
import sys
import os
import glob
import time
import threading

try:
    import readline  # enables arrow-key history on Unix
except ImportError:
    pass

# ANSI colors
C_RESET  = '\033[0m'
C_GREEN  = '\033[32m'
C_YELLOW = '\033[33m'
C_CYAN   = '\033[36m'
C_BOLD   = '\033[1m'
C_DIM    = '\033[2m'


def find_port():
    patterns = ['/dev/ttyACM*', '/dev/tty.usbmodem*', 'COM*']
    for pat in patterns:
        ports = sorted(glob.glob(pat))
        if ports:
            return ports[0]
    return None


def reader_thread(ser, logfile=None):
    """Background thread: read and display serial output."""
    while True:
        try:
            if ser.in_waiting:
                data = ser.read(ser.in_waiting)
                text = data.decode('utf-8', errors='replace')
                # Colorize the prompt
                text = text.replace('rns> ', f'{C_GREEN}rns>{C_RESET} ')
                sys.stdout.write(text)
                sys.stdout.flush()
                if logfile:
                    logfile.write(data.decode('utf-8', errors='replace'))
                    logfile.flush()
            else:
                time.sleep(0.02)
        except (serial.SerialException, OSError):
            print(f"\n{C_YELLOW}[Disconnected]{C_RESET}")
            break
        except Exception:
            break


def main():
    parser = argparse.ArgumentParser(description='RNS Transport Node Console')
    parser.add_argument('--port', '-p', help='Serial port')
    parser.add_argument('--baud', '-b', type=int, default=115200)
    parser.add_argument('--log', '-l', help='Log file path')
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("No serial port found. Connect the device or use --port.")
        sys.exit(1)

    logfile = None
    if args.log:
        logfile = open(args.log, 'a')

    print(f"{C_BOLD}╔═══════════════════════════════════════════╗{C_RESET}")
    print(f"{C_BOLD}║  RNS Transport Node — Serial Console      ║{C_RESET}")
    print(f"{C_BOLD}╚═══════════════════════════════════════════╝{C_RESET}")
    print(f"  Port: {C_CYAN}{port}{C_RESET} @ {args.baud} baud")
    print(f"  {C_DIM}Press Ctrl+C to exit{C_RESET}\n")

    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"Error opening {port}: {e}")
        sys.exit(1)

    # Start background reader
    t = threading.Thread(target=reader_thread, args=(ser, logfile), daemon=True)
    t.start()

    # Send initial newline to get prompt
    time.sleep(0.5)
    ser.write(b'\r\n')

    # Interactive input loop
    try:
        while True:
            try:
                line = input()
                ser.write((line + '\r\n').encode())
                if logfile:
                    logfile.write(f"> {line}\n")
                    logfile.flush()
            except EOFError:
                break
    except KeyboardInterrupt:
        print(f"\n{C_DIM}[Exit]{C_RESET}")
    finally:
        ser.close()
        if logfile:
            logfile.close()


if __name__ == '__main__':
    main()
