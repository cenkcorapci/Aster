"""Aster Python client.

Facade layer over the Thrift-generated protocol (//aster/rpc:aster.thrift).
Transport and generated code land in milestone M5; this module fixes the
public API surface.
"""

from aster.client import Client, Collection, Hit

__all__ = ["Client", "Collection", "Hit"]
__version__ = "0.1.0"
