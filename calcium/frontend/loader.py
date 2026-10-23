import io
import tokenize
from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path


@dataclass(frozen=True)
class Source:
    path: Path
    text: str
    digest: str


def load(path: str | Path) -> Source:
    path = Path(path).resolve()
    # Reading/decoding never imports or executes the input or its dependencies.
    raw = path.read_bytes()
    encoding, _ = tokenize.detect_encoding(io.BytesIO(raw).readline)
    text = raw.decode(encoding)
    return Source(path, text, sha256(raw).hexdigest())
