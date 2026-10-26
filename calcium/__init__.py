"""Calcium: Python-to-C compiler."""

__version__ = "26.10"

from .api import Calcium
from .models import Artifact, Diagnostic, Result

__all__ = ["Calcium", "Artifact", "Diagnostic", "Result", "__version__"]
