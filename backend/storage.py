"""Local asset storage, independent from task execution and database."""
from pathlib import Path

class FileStorage:
    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
    def write(self, task_id, name, content):
        folder = self.root / task_id
        folder.mkdir(exist_ok=True)
        path = folder / name
        path.write_bytes(content)
        return f'/assets/{task_id}/{name}'
