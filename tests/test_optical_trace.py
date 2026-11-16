"""Wire-level fixtures only: no physical heater or production service access."""
import os
import pty
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('VCONTROLD_TEST_BINARY', ROOT / 'build-trace/vcontrold'))


class Heater:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.extra = False
        self.ready = threading.Event()
        self.stop = threading.Event()
        self.requests = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            self.connection, _ = self.listener.accept()
        except OSError:
            return
        self.connection.settimeout(.1)
        self.ready.set()
        pending = b''
        while not self.stop.is_set():
            try:
                data = self.connection.recv(256)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            pending += data
            while pending:
                if pending[0] == 4:
                    pending = pending[1:]
                    # Extra bytes must be captured before the next TX flush.
                    self.connection.sendall(b'\x05\xed\xfe' if self.extra else b'\x05')
                elif len(pending) < 5:
                    break
                else:
                    command, pending = pending[:5], pending[5:]
                    self.requests.append(command)
                    if command != b'\x01\xf7\x08\x00\x02':
                        return
                    self.connection.sendall(b'\x12\x01')  # 27.4 C, little endian

    def send(self, data):
        assert self.ready.wait(3)
        self.connection.sendall(data)

    def close(self):
        self.stop.set()
        if self.ready.is_set():
            self.connection.close()
        self.listener.close()
        self.thread.join(2)


class OpticalTraceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.heater = Heater()
        cfg = (ROOT / 'xml/kw/vcontrold.xml').read_text().replace('ID="2053"', 'ID="2098"')
        cfg = cfg.replace('href="vito.xml"', 'href="' + str(ROOT / 'xml/kw/vito.xml') + '"')
        self.config = self.directory / 'vcontrold.xml'
        self.config.write_text(cfg)
        self.trace = self.directory / 'wire.log'
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0)); self.port = s.getsockname()[1]
        self.log = (self.directory / 'stderr.log').open('w+')
        self.process = None

    def launch(self, trace=True, foreground=True):
        self.heater.extra = trace
        args = [str(BINARY), '-x', str(self.config), '-d', '127.0.0.1:' + str(self.heater.port),
                '-p', str(self.port), '-L', '127.0.0.1', '-l', str(self.directory / 'daemon.log'),
                '-P', str(self.directory / 'daemon.pid')]
        if foreground: args += ['-n']
        if trace: args += ['-t', str(self.trace)]
        self.process = subprocess.Popen(args, stdout=self.log, stderr=self.log)
        if trace and foreground:
            self.assertTrue(self.heater.ready.wait(4), self.failure())

    def failure(self):
        self.log.flush(); return (self.directory / 'stderr.log').read_text()

    def connect(self):
        deadline = time.monotonic() + 4
        while True:
            try:
                connection = socket.create_connection(('127.0.0.1', self.port), timeout=1)
                connection.settimeout(3)
                self.assertIn(b'vctrld>', self.frame(connection))
                return connection
            except ConnectionRefusedError:
                if time.monotonic() > deadline: self.fail(self.failure())
                time.sleep(.02)

    def frame(self, connection):
        result = b''
        while b'vctrld>' not in result:
            data = connection.recv(4096)
            if not data: self.fail('TCP disconnected: ' + self.failure())
            result += data
        return result

    def wait_trace(self, fragment):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.trace.exists() and fragment in self.trace.read_text(): return
            time.sleep(.01)
        self.fail('Missing trace ' + fragment + '\n' + self.failure())

    def tearDown(self):
        if self.process is not None:
            self.process.terminate()
            try: self.process.wait(timeout=4)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.heater.close()
        self.log.close()
        self.temp.cleanup()

    def test_idle_and_reply_capture_survives_partial_commands_and_reconnect(self):
        self.launch()
        self.heater.send(b'\xaa\x55')
        self.wait_trace('AA 55')
        with self.connect() as client:
            client.sendall(b'getTem')
            self.heater.send(b'\xde\xad')
            self.wait_trace('DE AD')
            client.sendall(b'pA\n')
            self.assertIn(b'27.4', self.frame(client))
            client.sendall(b'close\n')
            self.frame(client)
            self.heater.send(b'\xbe\xef')
            self.wait_trace('BE EF')
        self.heater.send(b'\xca\xfe')
        self.wait_trace('CA FE')
        with self.connect() as client:
            client.sendall(b'getTempA\n')
            self.assertIn(b'27.4', self.frame(client))
        text = self.trace.read_text()
        self.assertIn('RX-PRE-TX-DISCARD len=2 ED FE', text)
        self.assertIn('TX len=1 04', text)
        self.assertIn('RX-REPLY len=2 12 01', text)
        self.assertEqual(len(self.heater.requests), 2)
        for line in text.splitlines():
            self.assertRegex(line, r'^\d{4}-.*Z mono=\d+\.\d{9} [A-Z-]+ len=\d+')
        self.assertEqual(self.trace.stat().st_mode & 0o777, 0o600)

    def test_real_tty_preflush_records_bytes_and_keeps_capture_after_session_close(self):
        fixture = self.directory / 'tty-fixture'
        subprocess.run(['cc', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE', '-D_XOPEN_SOURCE=700',
                        '-I', str(ROOT / 'src'), str(ROOT / 'tests/trace_tty_fixture.c'),
                        str(ROOT / 'src/trace.c'), '-o', str(fixture)], check=True)
        master, slave = pty.openpty()
        process = subprocess.Popen([str(fixture), os.ttyname(slave), str(self.trace)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=False)
        try:
            def stage(expected):
                self.assertTrue(select.select([process.stdout], [], [], 3)[0])
                self.assertEqual(process.stdout.readline().strip(), expected)
            stage(b'READY')
            os.write(master, b'\xed\xfe')
            process.stdin.write(b'x'); process.stdin.flush()
            stage(b'FLUSHED')
            os.write(master, b'\x12\x01')
            stage(b'IDLE')
            os.write(master, b'\xab\xcd')
            self.wait_trace('AB CD')
            process.stdin.write(b'x'); process.stdin.flush()
            self.assertEqual(process.wait(timeout=3), 0)
            text = self.trace.read_text()
            self.assertIn('RX-PRE-TX-DISCARD len=2 ED FE', text)
            self.assertIn('RX-REPLY len=2 12 01', text)
            self.assertIn('RX-IDLE len=2 AB CD', text)
        finally:
            if process.poll() is None: process.kill(); process.wait()
            process.stdin.close(); process.stdout.close()
            os.close(master); os.close(slave)

    def test_normal_mode_still_reads_without_creating_trace(self):
        self.launch(trace=False)
        with self.connect() as client:
            client.sendall(b'getTempA\n')
            self.assertIn(b'27.4', self.frame(client))
        self.assertFalse(self.trace.exists())

    def test_trace_requires_foreground_and_does_not_open_optical_port_on_error(self):
        self.launch(foreground=False)
        self.assertEqual(self.process.wait(timeout=3), 2, self.failure())
        self.assertIn('-t requires -n', self.failure())
        self.assertFalse(self.heater.ready.is_set())

    def test_unwritable_trace_fails_before_optical_access(self):
        self.trace = self.directory / 'missing' / 'wire.log'
        # Don't use launch's ready wait for this deliberately failing startup.
        args = [str(BINARY), '-n', '-x', str(self.config), '-d', '127.0.0.1:' + str(self.heater.port),
                '-p', str(self.port), '-L', '127.0.0.1', '-l', str(self.directory / 'daemon.log'),
                '-t', str(self.trace), '-P', str(self.directory / 'daemon.pid')]
        self.process = subprocess.Popen(args, stdout=self.log, stderr=self.log)
        self.assertEqual(self.process.wait(timeout=3), 2, self.failure())
        self.assertFalse(self.heater.ready.is_set())


if __name__ == '__main__': unittest.main()
