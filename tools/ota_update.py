#!/usr/bin/env python3
"""
ota_update.py — Push firmware to WisBlock 1W via Adafruit nRF52 bootloader.

This script automates the standard Adafruit nRF52 DFU process:
  1. Connects to the device over USB serial
  2. Sends the DFU trigger command (reboots into bootloader)
  3. Waits for the bootloader serial port to appear
  4. Uses adafruit-nrfutil to perform the actual DFU

The bootloader is NEVER modified. An interrupted transfer simply leaves
the old firmware in place — the device cannot be bricked.

Requirements:
    pip install adafruit-nrfutil pyserial

Usage:
    python ota_update.py --port /dev/ttyACM0 --firmware firmware.zip
    python ota_update.py --port /dev/ttyACM0 --hex firmware.hex
"""
import argparse
import serial
import time
import subprocess
import sys
import os
import glob


def find_serial_port():
    """Auto-detect WisBlock serial port."""
    patterns = ['/dev/ttyACM*', '/dev/tty.usbmodem*', 'COM*']
    for pattern in patterns:
        ports = glob.glob(pattern)
        if ports:
            return ports[0]
    return None


def trigger_dfu(port, baud=115200):
    """Send DFU command via the RNS console to reboot into bootloader."""
    print(f"Connecting to {port} at {baud} baud...")
    try:
        ser = serial.Serial(port, baud, timeout=2)
        time.sleep(0.5)

        # Send the 'dfu' command
        ser.write(b'\r\n')
        time.sleep(0.2)
        ser.write(b'dfu\r\n')
        time.sleep(0.5)

        # Read any response
        response = ser.read(ser.in_waiting or 1)
        print(f"Device response: {response.decode('utf-8', errors='replace').strip()}")

        ser.close()
        print("DFU command sent. Waiting for bootloader...")
        time.sleep(3)
        return True
    except serial.SerialException as e:
        print(f"Serial error: {e}")
        return False


def wait_for_bootloader(timeout=15):
    """Wait for the bootloader serial port to appear."""
    print(f"Waiting up to {timeout}s for bootloader port...")
    start = time.time()
    while time.time() - start < timeout:
        port = find_serial_port()
        if port:
            print(f"Bootloader detected on {port}")
            return port
        time.sleep(0.5)
    return None


def create_dfu_package(hex_file, output_zip):
    """Create a DFU package from a .hex file using adafruit-nrfutil."""
    cmd = [
        'adafruit-nrfutil', 'dfu', 'genpkg',
        '--dev-type', '0x0052',
        '--application', hex_file,
        output_zip
    ]
    print(f"Creating DFU package: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Error creating DFU package: {result.stderr}")
        return False
    return True


def perform_dfu(port, zip_file):
    """Perform the actual DFU transfer."""
    cmd = [
        'adafruit-nrfutil', 'dfu', 'serial',
        '--package', zip_file,
        '--port', port,
        '-b', '115200',
        '--singlebank'
    ]
    print(f"Flashing: {' '.join(cmd)}")
    result = subprocess.run(cmd)
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(
        description='OTA firmware update for RNS Transport Node (WisBlock 1W)')
    parser.add_argument('--port', '-p', help='Serial port (auto-detected if omitted)')
    parser.add_argument('--firmware', '-f', help='DFU package (.zip)')
    parser.add_argument('--hex', help='Firmware .hex file (will be packaged)')
    parser.add_argument('--no-trigger', action='store_true',
                        help='Skip DFU trigger (device already in bootloader)')
    parser.add_argument('--baud', type=int, default=115200)

    args = parser.parse_args()

    # Determine firmware file
    if args.firmware and args.hex:
        print("Error: specify --firmware OR --hex, not both")
        sys.exit(1)

    zip_file = args.firmware
    if args.hex:
        if not os.path.exists(args.hex):
            print(f"Error: {args.hex} not found")
            sys.exit(1)
        zip_file = args.hex.replace('.hex', '_dfu.zip')
        if not create_dfu_package(args.hex, zip_file):
            sys.exit(1)

    if not zip_file:
        # Look for default build output
        default_paths = [
            '.pio/build/wisblock_1w_transport/firmware.zip',
            'firmware.zip',
        ]
        for p in default_paths:
            if os.path.exists(p):
                zip_file = p
                break
        if not zip_file:
            print("Error: no firmware file specified. Use --firmware or --hex")
            sys.exit(1)

    if not os.path.exists(zip_file):
        print(f"Error: {zip_file} not found")
        sys.exit(1)

    # Find port
    port = args.port or find_serial_port()
    if not port:
        print("Error: no serial port found. Connect the device or specify --port")
        sys.exit(1)

    print(f"╔═══════════════════════════════════════════╗")
    print(f"║  RNS Transport Node — OTA Firmware Update ║")
    print(f"╚═══════════════════════════════════════════╝")
    print(f"  Port:     {port}")
    print(f"  Firmware: {zip_file}")
    print()

    # Trigger DFU mode
    if not args.no_trigger:
        if not trigger_dfu(port, args.baud):
            print("Failed to trigger DFU. Try double-tapping the reset button.")
            sys.exit(1)

        # Wait for bootloader to enumerate
        bl_port = wait_for_bootloader()
        if not bl_port:
            print("Bootloader not detected. Try double-tapping reset manually.")
            sys.exit(1)
        port = bl_port

    # Perform DFU
    print("\nStarting firmware transfer...")
    if perform_dfu(port, zip_file):
        print("\n✓ Firmware update complete! Device will restart automatically.")
    else:
        print("\n✗ Firmware update failed.")
        print("  The previous firmware is still intact.")
        print("  Try: double-tap reset → drag .uf2 to USB drive")
        sys.exit(1)


if __name__ == '__main__':
    main()
