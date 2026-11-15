"""Minimal client for the QEMU HMP monitor socket that emu/run.sh can expose."""
import socket
import time


class Monitor:
    def __init__(self, path, timeout=20):
        self.path = path
        self._connect(timeout)

    def _connect(self, timeout):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(self.path)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.1)
        self._read_until_prompt()

    def _read_until_prompt(self):
        buf = b''
        while not buf.endswith(b'(qemu) '):
            chunk = self.s.recv(65536)
            if not chunk:
                break
            buf += chunk
        return buf

    def cmd(self, line):
        """Reconnects once if QEMU restarted (emu/run.sh after a software reset)."""
        try:
            self.s.sendall(line.encode() + b'\n')
            out = self._read_until_prompt()
            if out.endswith(b'(qemu) '):
                return out
        except OSError:
            pass
        self.s.close()
        self._connect(60)
        self.s.sendall(line.encode() + b'\n')
        return self._read_until_prompt()
