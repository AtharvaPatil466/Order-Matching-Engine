"""Put the compiled bcs_engine module and the Phase 1 packages on sys.path."""
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)
