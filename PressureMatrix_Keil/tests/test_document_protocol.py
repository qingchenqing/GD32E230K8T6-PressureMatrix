"""Verify the D-drive Keil ARM binary against the supplied protocol.

Requires unicorn, pyelftools and pyserial. Build the Keil project first.
Run: py -3 -m unittest discover -s tests -v
"""
from pathlib import Path
import importlib.util
import io
import struct
import unittest

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('monitor', ROOT / 'tools/monitor.py')
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)
ROWS = [7, 6, 5, 4, 3, 2, 1, 0, 8, 9, 10, 11, 12, 13, 14, 15]


def expected_packet(values, sequence=0):
    body = bytes.fromhex('5A A5 01 10 03 02') + struct.pack('>HH', sequence, 155)
    body += bytes(values) + bytes(16)
    return body + bytes([monitor.crc8(body)])


def emulator():
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    cpu.mem_map(0x08000000, 0x10000)
    cpu.mem_map(0x20000000, 0x2000)
    cpu.mem_map(0x40013000, 0x1000)
    cpu.mem_write(0x4001381C, struct.pack('<I', 0x80))  # USART TBE ready
    with (ROOT / 'Keil/output/PressureMatrix.axf').open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s['st_value'] for s in
                   elf.get_section_by_name('.symtab').iter_symbols()}
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD':
                cpu.mem_write(segment['p_vaddr'], segment.data())
    cpu.reg_write(UC_ARM_REG_SP, 0x20002000)
    cpu.reg_write(UC_ARM_REG_LR, 0x0800F001)
    return cpu, symbols


def send_frame(values, sequence):
    cpu, symbols = emulator()
    raw = struct.pack('<128H', *values)
    cpu.mem_write(0x20001000, raw)
    cpu.reg_write(UC_ARM_REG_R0, 0x20001000)
    cpu.reg_write(UC_ARM_REG_R1, sequence)
    packet = bytearray()

    def capture(uc, access, address, size, value, user_data):
        if address == 0x40013828:  # Actual USART0 TDATA register write
            packet.append(value & 255)

    cpu.hook_add(UC_HOOK_MEM_WRITE, capture)
    cpu.emu_start(symbols['serial_send_frame'], 0x0800F000, count=200000)
    if cpu.reg_read(UC_ARM_REG_PC) != 0x0800F000:
        raise AssertionError('Sender did not return')
    if cpu.mem_read(0x20001000, 256) != raw:
        raise AssertionError('Sender modified input samples')
    if bytes(cpu.mem_read(symbols['g_serial_tx_packet'], 155)) != bytes(packet):
        raise AssertionError('UART bytes differ from the packet buffer')
    return bytes(packet)


class DocumentProtocolTests(unittest.TestCase):
    def test_document_zero_example(self):
        self.assertEqual(send_frame([0] * 128, 0),
                         bytes.fromhex('5A A5 01 10 03 02 00 00 00 9B') +
                         bytes(144) + bytes([0x17]))

    def test_all_points_sequence_and_crc(self):
        values = list(range(128))
        wire = [values[row * 8 + col] for row in ROWS for col in range(8)]
        wire = [v for start in range(0, 128, 8)
                for v in wire[start:start + 8][::-1]]
        for sequence in (0, 1, 0x1234, 0xFFFF):
            packet = send_frame(values, sequence)
            self.assertEqual(len(packet), 155)
            self.assertEqual(packet, expected_packet(wire, sequence))
            self.assertEqual(monitor.read_packed_frame(io.BytesIO(packet)),
                             (sequence, [v for start in range(0, 128, 8)
                                         for v in values[start:start + 8][::-1]]))

    def test_timeouts_keep_reserved_bytes_zero(self):
        for values in ([65535] * 128, [0, 255, 65535, 128] * 32):
            wire = [0 if values[row * 8 + col] == 65535 else values[row * 8 + col]
                    for row in ROWS for col in range(8)]
            wire = [v for start in range(0, 128, 8)
                    for v in wire[start:start + 8][::-1]]
            self.assertEqual(send_frame(values, 0), expected_packet(wire))

    def test_user_captured_packet(self):
        original = bytes.fromhex(
            "5A A5 01 10 03 02 00 3C 00 9B "
            "14 14 14 14 14 16 14 15 14 15 14 15 15 15 14 15 "
            "14 14 14 14 15 14 14 15 15 14 14 1A 3D 70 1D 17 "
            "14 15 14 15 2C FF FF 27 15 15 15 15 15 18 17 15 "
            "1A 14 14 15 14 15 13 15 15 15 14 14 14 15 14 15 "
            "17 15 14 14 15 15 14 15 15 15 14 14 15 15 14 15 "
            "14 14 15 14 14 14 13 15 15 15 15 14 15 14 14 15 "
            "15 14 14 14 14 14 13 15 14 15 14 14 14 13 14 15 "
            "14 14 14 14 14 16 14 15 15 15 14 14 14 15 13 15 "
            "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 10")
        self.assertEqual(len(original), 155)
        self.assertEqual(monitor.crc8(original[:154]), original[154])
        raw = [0] * 128
        for packet_row, source_row in enumerate(ROWS):
            raw[source_row * 8:source_row * 8 + 8] = original[
                10 + packet_row * 8:18 + packet_row * 8]
        actual = send_frame(raw, 0x003C)
        self.assertEqual(len(actual), 155)
        self.assertEqual(actual[:10], original[:10])
        for row in range(16):
            start = 10 + row * 8
            self.assertEqual(actual[start:start + 8], original[start:start + 8][::-1])
        self.assertEqual(actual[138:154], original[138:154])
        self.assertEqual(actual[154], monitor.crc8(actual[:154]))

    def test_crc_known_vector(self):
        self.assertEqual(monitor.crc8(b'123456789'), 0xF4)

    def test_parser_rejects_corruption_and_resynchronizes(self):
        good = expected_packet(list(range(128)), 0x1234)
        expected = monitor.read_packed_frame(io.BytesIO(good))
        for index in (2, 3, 4, 5, 8, 9, 10, 154):
            bad = bytearray(good)
            bad[index] ^= 1
            self.assertEqual(monitor.read_packed_frame(io.BytesIO(
                b'noise\x5a' + bad + good)), expected)
        bad = bytearray(good)
        bad[138] = 1
        bad[154] = monitor.crc8(bad[:154])
        self.assertEqual(monitor.read_packed_frame(io.BytesIO(bad + good)), expected)

    def test_main_sequences_count_packets_instead_of_scans(self):
        cpu, symbols = emulator()
        skip = {symbols[name] & ~1 for name in
                ('power_led_on', 'systick_config', 'matrix_init', 'serial_init',
                 'matrix_scan_frame', 'matrix_adc_error_count')}
        packet = bytearray()
        clock_calls = 0

        def capture(uc, address, size, user_data):
            nonlocal clock_calls
            if address in skip:
                uc.reg_write(UC_ARM_REG_R0, 0)
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
            elif address == (symbols['systick_millis'] & ~1):
                uc.reg_write(UC_ARM_REG_R0, clock_calls * 50)
                clock_calls += 1
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
            elif address == (symbols['send_byte'] & ~1):
                packet.append(uc.reg_read(UC_ARM_REG_R0) & 255)
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
                if len(packet) == 465:
                    uc.emu_stop()

        cpu.hook_add(UC_HOOK_CODE, capture)
        cpu.emu_start(symbols['main'], 0x0800F000, count=1000000)
        self.assertEqual(bytes(packet), b''.join(
            expected_packet([0] * 128, seq) for seq in range(3)))
        scans = struct.unpack('<I', cpu.mem_read(symbols['g_matrix_frame_number'], 4))[0]
        self.assertGreater(scans, 3)


if __name__ == '__main__':
    unittest.main()
