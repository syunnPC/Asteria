# SPDX-License-Identifier: Apache-2.0
"""QMP client for one QEMU process.

QEMU is started with its QMP monitor on one end of a socket pair
(`-chardev socket,id=qmp,fd=N -mon chardev=qmp,mode=control`), so there is no
socket file to wait for and nothing QEMU prints before the client attaches
is lost. A reader thread parses the newline-delimited JSON stream; command
responses are matched by `id`, and asynchronous events are kept in order so
callers can wait for them.
"""
from __future__ import annotations

import itertools
import json
import pathlib
import socket
import tempfile
import threading
import time


class QmpError(Exception):
    """Raised for an error response, a closed connection or a timeout."""


class Qmp:
    def __init__(self, connection: socket.socket, *, scratch: pathlib.Path | None = None):
        self._socket = connection
        self._stream = connection.makefile('rwb')
        self._lock = threading.Condition()
        self._responses: dict[str, dict] = {}
        self._events: list[dict] = []
        self._closed = False
        self._ids = itertools.count(1)
        self._scratch = scratch
        self.greeting: dict | None = None
        self._reader = threading.Thread(target=self._read, name='qmp-reader', daemon=True)
        self._reader.start()

    # Connection -------------------------------------------------------------

    def _read(self) -> None:
        try:
            for raw in self._stream:
                message = json.loads(raw)
                with self._lock:
                    if 'QMP' in message:
                        self.greeting = message
                    elif 'event' in message:
                        self._events.append(message)
                    elif 'id' in message:
                        self._responses[message['id']] = message
                    self._lock.notify_all()
        except (OSError, ValueError):
            pass
        finally:
            with self._lock:
                self._closed = True
                self._lock.notify_all()

    def handshake(self, timeout: float = 10.0) -> None:
        with self._lock:
            if not self._lock.wait_for(lambda: self.greeting is not None or self._closed, timeout):
                raise QmpError('no QMP greeting within %.0f s' % timeout)
            if self.greeting is None:
                raise QmpError('QMP connection closed before the greeting')
        self.execute('qmp_capabilities')

    def close(self) -> None:
        try:
            self._socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._socket.close()

    @property
    def closed(self) -> bool:
        with self._lock:
            return self._closed

    # Commands ---------------------------------------------------------------

    def execute(self, command: str, arguments: dict | None = None, *, timeout: float = 30.0):
        identifier = 'asteria-%d' % next(self._ids)
        request = {'execute': command, 'id': identifier}
        if arguments:
            request['arguments'] = arguments
        with self._lock:
            if self._closed:
                raise QmpError('QMP connection is closed (%s)' % command)
        try:
            self._stream.write((json.dumps(request) + '\n').encode())
            self._stream.flush()
        except OSError as error:
            raise QmpError('cannot send %s: %s' % (command, error)) from error
        with self._lock:
            if not self._lock.wait_for(lambda: identifier in self._responses or self._closed, timeout):
                raise QmpError('%s did not answer within %.0f s' % (command, timeout))
            response = self._responses.pop(identifier, None)
        if response is None:
            raise QmpError('QMP connection closed during %s' % command)
        if 'error' in response:
            raise QmpError('%s failed: %s' % (command, response['error'].get('desc', response['error'])))
        return response.get('return')

    def hmp(self, command_line: str, *, cpu: int | None = None, timeout: float = 30.0) -> str:
        """Runs a human-monitor command through QMP (on CPU `cpu` if given)
        and returns its text."""
        arguments: dict = {'command-line': command_line}
        if cpu is not None:
            arguments['cpu-index'] = cpu
        return self.execute('human-monitor-command', arguments, timeout=timeout)

    # Events -----------------------------------------------------------------

    def events(self) -> list[dict]:
        with self._lock:
            return list(self._events)

    def wait_event(self, name: str, timeout: float, *, since: int = 0) -> dict:
        """Waits for an event named `name` at or after position `since`."""
        def find():
            for event in self._events[since:]:
                if event['event'] == name:
                    return event
            return None
        with self._lock:
            if not self._lock.wait_for(lambda: find() is not None or self._closed, timeout):
                raise QmpError('no %s event within %.0f s' % (name, timeout))
            found = find()
        if found is None:
            raise QmpError('QMP connection closed while waiting for %s' % name)
        return found

    def event_count(self) -> int:
        with self._lock:
            return len(self._events)

    # Typed wrappers -----------------------------------------------------------

    def status(self) -> dict:
        return self.execute('query-status')

    def cont(self) -> None:
        self.execute('cont')

    def stop(self, timeout: float = 10.0) -> None:
        """Stops the guest and waits until QEMU reports it stopped."""
        if not self.status().get('running', False):
            return
        mark = self.event_count()
        self.execute('stop')
        self.wait_event('STOP', timeout, since=mark)

    def quit(self) -> None:
        try:
            self.execute('quit', timeout=10.0)
        except QmpError:
            pass

    def cpus(self) -> list[dict]:
        return self.execute('query-cpus-fast')

    def _dump(self, command: str, arguments: dict, size: int) -> bytes:
        directory = self._scratch or pathlib.Path(tempfile.gettempdir())
        with tempfile.NamedTemporaryFile(dir=directory, prefix='qmp-', suffix='.bin', delete=False) as handle:
            path = pathlib.Path(handle.name)
        try:
            self.execute(command, {**arguments, 'filename': str(path)})
            data = path.read_bytes()
        finally:
            path.unlink(missing_ok=True)
        if len(data) != size:
            raise QmpError('%s returned %d bytes, expected %d' % (command, len(data), size))
        return data

    def memsave(self, address: int, size: int, *, cpu: int = 0) -> bytes:
        """Reads guest-virtual memory through CPU `cpu`'s translation."""
        return self._dump('memsave', {'val': address, 'size': size, 'cpu-index': cpu}, size)

    def pmemsave(self, address: int, size: int) -> bytes:
        """Reads guest-physical memory."""
        return self._dump('pmemsave', {'val': address, 'size': size}, size)

    def screendump(self, path: pathlib.Path, format: str = 'png') -> None:
        """Saves the display to `path` ('png' or 'ppm')."""
        self.execute('screendump', {'filename': str(path), 'format': format})
        if format == 'ppm':
            from AsteriaVm import Display
            data = path.read_bytes()
            try:
                normalized = Display.normalize_qmp_ppm(data)
            except Display.DisplayError as error:
                raise QmpError(str(error)) from error
            if normalized != data:
                path.write_bytes(normalized)


def wait_until(predicate, timeout: float, interval: float = 0.05) -> bool:
    """Polls `predicate` until it is true or `timeout` elapses (for guest
    state that QEMU exposes only by query, such as a CPU reaching an idle
    loop)."""
    deadline = time.monotonic() + timeout
    while True:
        if predicate():
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(interval)
