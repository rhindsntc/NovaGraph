"""One advisory machine lock shared by all native performance collection lanes."""
import contextlib
import fcntl
import os
from pathlib import Path
import stat

LOCK_PATH = Path('/tmp/nova-performance-measurement.lock')


@contextlib.contextmanager
def measurement_lock():
    """Fixed machine-global inode; never unlink it (which would split the lock)."""
    fd = os.open(LOCK_PATH, os.O_CREAT | os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0), 0o666)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError('measurement lock is not an ordinary single-link file')
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError('machine measurement lock is busy; no collection started') from error
        yield
    finally:
        os.close(fd)

