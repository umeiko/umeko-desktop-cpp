"""Process-scoped exclusive lease using only the bundled standard library."""
import sqlite3


class SchedulerLock:
    def __init__(self, path):
        self.path, self.connection = path, None

    def acquire(self):
        connection = sqlite3.connect(self.path, timeout=0, isolation_level=None)
        try:
            connection.execute('BEGIN EXCLUSIVE')
        except sqlite3.OperationalError:
            connection.close()
            return False
        self.connection = connection
        return True

    def release(self):
        if self.connection is not None:
            self.connection.rollback()
            self.connection.close()
            self.connection = None
