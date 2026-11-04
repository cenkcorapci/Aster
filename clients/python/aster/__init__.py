"""Aster Python client (HTTP JSON transport against ``aster serve``)."""

from aster.client import (
    AsterConnectionError,
    AsterError,
    Client,
    Collection,
    Document,
    Hit,
    NotFoundError,
)

__all__ = [
    "AsterConnectionError",
    "AsterError",
    "Client",
    "Collection",
    "Document",
    "Hit",
    "NotFoundError",
]
__version__ = "0.2.0"
