"""SQLite repository; replace this module when moving to a managed database."""
import json
import sqlite3
from pathlib import Path

class Repository:
    def __init__(self, path):
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        with self.connect() as c:
            c.execute('PRAGMA journal_mode=WAL')
            c.execute('CREATE TABLE IF NOT EXISTS tasks (id TEXT PRIMARY KEY, data TEXT NOT NULL)')
            c.execute('CREATE TABLE IF NOT EXISTS weapons (id TEXT PRIMARY KEY, data TEXT NOT NULL)')
    def connect(self):
        return sqlite3.connect(self.path, timeout=30)
    def save(self, table, data):
        assert table in ('tasks', 'weapons')
        with self.connect() as c:
            c.execute(f'INSERT OR REPLACE INTO {table} VALUES (?, ?)', (data['id'], json.dumps(data, ensure_ascii=False)))
    def get(self, table, key):
        assert table in ('tasks', 'weapons')
        with self.connect() as c:
            row = c.execute(f'SELECT data FROM {table} WHERE id=?', (key,)).fetchone()
        return json.loads(row[0]) if row else None
    def list(self, table):
        assert table in ('tasks', 'weapons')
        with self.connect() as c:
            return [json.loads(r[0]) for r in c.execute(f'SELECT data FROM {table} ORDER BY rowid DESC')]
