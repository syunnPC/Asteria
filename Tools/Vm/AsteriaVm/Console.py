# SPDX-License-Identifier: Apache-2.0
"""The guest debug channel as a stream of lines.

The kernel and (through the kernel) the guest test program write to one
debug channel: the ACPI debug port on IA-64, the ISA debug console at port
0x402 on AMD64. QEMU connects it to one end of a socket pair; a reader
thread splits the byte stream into lines, tees it to `Debug.log`, and wakes
waiters. Lines are classified by prefix: `ASTERIA:` lines come from the
kernel, `USER:` lines from the guest program, and anything else (firmware
output on the same channel) is kept for the log but never interpreted.
"""
from __future__ import annotations

import pathlib
import re
import socket
import threading
from typing import Callable

KERNEL_PREFIX = 'ASTERIA:'
USER_PREFIX = 'USER:'
# Kernel diagnostics that end a case immediately: a field that is exactly
# FAIL, ERROR or FAULT, or a family name ending in -ERROR or -FAULT, for
# example `ASTERIA:PAGE-ALLOCATOR:FAIL`,
# `ASTERIA:INITIAL-IVT-INSTALL-ERROR:TABLE-REJECTED` or `ASTERIA:AMD64-FAULT:RIP=...`.
FAILURE = re.compile(r'^ASTERIA:(?:[A-Z0-9=_.-]+:)*?(?:[A-Z0-9_-]*-(?:ERROR|FAULT)|FAIL|ERROR|FAULT)(?::|$)')


class ConsoleTimeout(Exception):
    """Raised when an awaited line does not arrive in time."""


class Console:
    def __init__(self, connection: socket.socket, log: pathlib.Path):
        self._socket = connection
        self._log = log.open('wb')
        self._lock = threading.Condition()
        self._lines: list[str] = []
        self._partial = b''
        self._closed = False
        self.corrupt: list[str] = []
        self._reader = threading.Thread(target=self._read, name='console-reader', daemon=True)
        self._reader.start()

    def _read(self) -> None:
        try:
            while True:
                data = self._socket.recv(65536)
                if not data:
                    break
                self._log.write(data)
                self._log.flush()
                self._accept(data)
        except OSError:
            pass
        finally:
            with self._lock:
                if self._partial:
                    self._append(self._partial)
                    self._partial = b''
                self._closed = True
                self._lock.notify_all()
            self._log.close()

    def _accept(self, data: bytes) -> None:
        with self._lock:
            buffer = self._partial + data
            *complete, self._partial = buffer.split(b'\n')
            for raw in complete:
                self._append(raw)
            self._lock.notify_all()

    def _append(self, raw: bytes) -> None:
        # A firmware console that serves as the channel (hp-i2000's COM1)
        # ends its lines with CR CR LF.
        raw = raw.rstrip(b'\r')
        text = raw.decode('ascii', errors='replace')
        if b'\0' in raw or '�' in text:
            self.corrupt.append(text)
        self._lines.append(text)

    def close(self) -> None:
        try:
            self._socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._socket.close()
        self._reader.join(timeout=5)

    @property
    def lines(self) -> list[str]:
        with self._lock:
            return list(self._lines)

    def kernel_lines(self) -> list[str]:
        return [line for line in self.lines if line.startswith(KERNEL_PREFIX)]

    def user_lines(self) -> list[str]:
        return [line[len(USER_PREFIX):] for line in self.lines if line.startswith(USER_PREFIX)]

    def failures(self) -> list[str]:
        return [line for line in self.lines if FAILURE.match(line)]

    def wait_for(self, predicate: Callable[[str], bool] | str, timeout: float, *, since: int = 0,
                 abort: Callable[[], str | None] | None = None, allow_stop: bool = False) -> tuple[int, str]:
        """Waits for the first line at index >= `since` that matches.

        `predicate` is a callable or a literal substring. `abort`, polled while
        waiting, returns a reason to stop early (for example the QEMU process
        exiting) or None. Kernel failures and unexpected STOP lines also end the
        wait; allow_stop is reserved for a case awaiting stop completion.
        """
        matches = predicate if callable(predicate) else (lambda line, text=predicate: text in line)
        deadline_reason: list[str] = []

        def ready() -> bool:
            for index in range(since, len(self._lines)):
                if matches(self._lines[index]):
                    return True
            if any(FAILURE.match(line) or (not allow_stop and line.startswith("ASTERIA:STOP:"))
                   for line in self._lines[since:]):
                return True
            if abort is not None:
                reason = abort()
                if reason:
                    deadline_reason.append(reason)
                    return True
            return self._closed

        with self._lock:
            finished = self._lock.wait_for(ready, timeout) if abort is None else self._poll(ready, timeout)
            for index in range(since, len(self._lines)):
                if matches(self._lines[index]):
                    return index, self._lines[index]
            failures = [line for line in self._lines[since:] if FAILURE.match(line)]
        stops = [line for line in self.lines[since:] if line.startswith("ASTERIA:STOP:")]
        if stops and not allow_stop:
            raise ConsoleTimeout("kernel stop: %s" % stops[0])
        if failures:
            raise ConsoleTimeout('kernel failure: %s' % failures[0])
        if deadline_reason:
            raise ConsoleTimeout(deadline_reason[0])
        if not finished:
            raise ConsoleTimeout('no matching line within %.0f s' % timeout)
        raise ConsoleTimeout('debug channel closed before a matching line')

    def _poll(self, ready: Callable[[], bool], timeout: float) -> bool:
        """wait_for with a periodic re-check (the abort callback needs polling)."""
        import time
        deadline = time.monotonic() + timeout
        while not ready():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            self._lock.wait(min(remaining, 0.25))
        return True
