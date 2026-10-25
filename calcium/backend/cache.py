"""Content-addressed object cache; link every build against the current libraries."""

import json
import os
import shutil
import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path


def cached_object(command, source, output, *, directory, cache, identity, on_compile=None):
    """Hash preprocessed C, including actual SDK/system header contents.

    Failed preprocessing/compilation never publishes an entry. Corrupt or absent
    entries are misses; an unavailable cache must not prevent a normal build.
    Debug builds bypass this helper to retain their actual source locations.
    """
    preprocessed = subprocess.run([*command, "-E", str(source)], capture_output=True, timeout=120)
    if preprocessed.returncode:
        return None
    normalized = preprocessed.stdout.replace(os.fsencode(directory), b"/calcium/generated")
    flags = [part.replace(str(directory), "/calcium/generated") for part in command]
    digest = sha256(json.dumps([identity, flags], sort_keys=True).encode() + normalized).hexdigest()
    entry = Path(cache) / digest[:2] / digest
    try:
        metadata = json.loads((entry / "metadata.json").read_text())
        data = (entry / "object.o").read_bytes()
        if isinstance(metadata["warnings"], str) and metadata["sha256"] == sha256(data).hexdigest():
            output.write_bytes(data)
            return {"hit": True, "key": digest, "warnings": metadata["warnings"]}
    except (OSError, ValueError, KeyError, TypeError):
        pass
    if on_compile:
        on_compile()
    run = subprocess.run(
        [*command, "-c", str(source), "-o", str(output)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    if run.returncode:
        return {"hit": False, "key": digest, "run": run}
    try:
        entry.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".staging-", dir=entry.parent) as staging:
            staged = Path(staging) / "entry"
            staged.mkdir()
            shutil.copyfile(output, staged / "object.o")
            (staged / "metadata.json").write_text(
                json.dumps(
                    {
                        "sha256": sha256(output.read_bytes()).hexdigest(),
                        "warnings": run.stderr,
                    }
                )
            )
            try:
                os.rename(staged, entry)
            except OSError:
                # Repair a corrupt existing entry. Readers validate the content
                # hash, so a concurrent read between replacements is just a miss.
                os.replace(staged / "object.o", entry / "object.o")
                os.replace(staged / "metadata.json", entry / "metadata.json")
    except OSError:
        pass
    return {"hit": False, "key": digest, "warnings": run.stderr}
