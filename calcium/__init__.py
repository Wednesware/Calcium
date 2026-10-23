"""Calcium: an experimental Python-to-C compiler."""

__version__ = "0.1.0"

from .api import Calcium
from .models import Artifact, Diagnostic, Result

__all__ = ["Calcium", "Artifact", "Diagnostic", "Result", "__version__"]
