"""Display 16x8 ADC frames from the board's USART0 TX pin."""

import argparse
import struct
import time

try:
    import serial
except ImportError as exc:
    raise SystemExit("Install pyserial first: python -m pip install pyserial") from exc


PAYLOAD_BYTES = 4 + 16 * 8 * 2 + 2  # Legacy 12-bit packet after A5 5A.
PACKET_DATA_BYTES = 16 * 8 + 16


def source_row_for_display(display_row):
    """Reverse rows inside each 8-row bank for the physical display order."""
    return 7 - display_row if display_row < 8 else 23 - display_row


def read_exact(port, count):
    data = bytearray()
    while len(data) < count:
        chunk = port.read(count - len(data))
        if chunk:
            data.extend(chunk)
    return bytes(data)


def crc16_ccitt(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xFFFF
    return crc


def crc8(data):
    """MSB-first CRC8, polynomial 0x07, initial value 0x00."""
    crc = 0
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc << 1) ^ (0x07 if crc & 0x80 else 0)) & 0xFF
    return crc


def read_frame(port):
    previous = 0
    while True:
        current = port.read(1)
        if not current:
            continue
        value = current[0]
        if previous == 0xA5 and value == 0x5A:
            payload = port.read(PAYLOAD_BYTES)
            if len(payload) != PAYLOAD_BYTES:
                previous = 0
                continue
            expected = struct.unpack_from("<H", payload, PAYLOAD_BYTES - 2)[0]
            if (sum(payload[:-2]) & 0xFFFF) != expected:
                previous = 0
                continue
            sequence = struct.unpack_from("<I", payload)[0]
            values = struct.unpack_from("<128H", payload, 4)
            return sequence, values
        previous = value


def read_packed_frame(port):
    """Read the current 5A A5 / 159-byte / CRC8 matrix protocol.

    The firmware sends rows Y7..Y0 followed by Y8..Y15; this function
    restores physical row-major order for the GUI.
    """
    previous = 0
    while True:
        current = port.read(1)
        if not current:
            continue
        value = current[0]
        if previous == 0x5A and value == 0xA5:
            header = read_exact(port, 8)
            if len(header) != 8 or header[0] != 0x01 or header[1] != 0x10:
                previous = header[-1] if header else 0
                continue
            command = struct.unpack_from(">H", header, 2)[0]
            frame_length = struct.unpack_from(">H", header, 6)[0]
            if command != 0x0302 or frame_length != 11 + PACKET_DATA_BYTES:
                previous = header[-1]
                continue
            data_and_crc = read_exact(port, frame_length - 10)
            packet = bytes((0x5A, 0xA5)) + header + data_and_crc
            if crc8(packet[:-1]) != packet[-1]:
                previous = packet[-1]
                continue
            data = packet[10:-1]
            sequence = struct.unpack_from(">H", header, 4)[0]
            packet_values = data[:128]
            packet_invalid = data[128:144]
            values = [0] * 128
            for packet_index, value8 in enumerate(packet_values):
                packet_row, col = divmod(packet_index, 8)
                source_row = 7 - packet_row if packet_row < 8 else packet_row
                values[source_row * 8 + col] = value8
                if packet_invalid[packet_index // 8] & (1 << (packet_index % 8)):
                    values[source_row * 8 + col] = 65535
            return sequence, values
        previous = value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="serial port, for example COM5")
    parser.add_argument("--every", type=int, default=10,
                        help="display every Nth received frame (default: 10; ignored with --interval)")
    parser.add_argument("--interval", type=float,
                        help="display at most one frame per N seconds, while still receiving every frame")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--text", action="store_true",
                      help="read ASCII output (firmware SERIAL_OUTPUT_MODE=0)")
    mode.add_argument("--binary", action="store_true",
                      help="read legacy raw 12-bit packets (firmware mode 1)")
    mode.add_argument("--packed8", action="store_true",
                      help="read 0..255 packets (default; firmware mode 2)")
    args = parser.parse_args()
    if args.every < 1:
        parser.error("--every must be at least 1")
    if args.interval is not None and args.interval <= 0:
        parser.error("--interval must be greater than zero")

    next_display_at = 0.0

    def should_display(received):
        nonlocal next_display_at
        if args.interval is None:
            return received % args.every == 0
        now = time.monotonic()
        if now < next_display_at:
            return False
        next_display_at = now + args.interval
        return True

    with serial.Serial(args.port, 115200, timeout=1) as port:
        received = 0
        if args.binary or not args.text:
            packed = not args.binary
            while True:
                sequence, values = (read_packed_frame(port) if packed else read_frame(port))
                received += 1
                if not should_display(received):
                    continue
                scale = "0..255" if packed else "raw ADC, 0..4095"
                print(f"\nframe {sequence}  ({scale}; 65535=timeout)")
                print("        COL1 COL2 COL3 COL4 COL5 COL6 COL7 COL8")
                for row in range(16):
                    source_row = source_row_for_display(row)
                    cells = values[source_row * 8:(source_row + 1) * 8]
                    print(f"ROW{row + 1:02d} " + " ".join(f"{v:4d}" for v in cells))
        else:
            show = False
            while True:
                line = port.readline().decode("ascii", errors="replace").rstrip("\r\n")
                if line.startswith("FRAME,"):
                    received += 1
                    show = should_display(received)
                    if show:
                        print()
                if show and line:
                    print(line)


if __name__ == "__main__":
    main()
