"""Tier 2 registry: imports every scenario module so its @scenario functions register.

Tasks adding scenarios create conformance/python/scfg_conformance/scenarios/<topic>.py and
list it here; nothing else needs to change.
"""

from __future__ import annotations

import importlib

from .scenario import SOURCES

SCENARIO_MODULES = [
    "scfg_conformance.scenarios.smoke",
    "scfg_conformance.scenarios.config",
    "scfg_conformance.scenarios.custom_chains",
    "scfg_conformance.scenarios.evm",
    "scfg_conformance.scenarios.evm_receipts",
    "scfg_conformance.scenarios.arc",
    "scfg_conformance.scenarios.solana",
    "scfg_conformance.scenarios.solana_rpc",
    "scfg_conformance.scenarios.restore",
    "scfg_conformance.scenarios.rpc",
]

for _name in SCENARIO_MODULES:
    importlib.import_module(_name)

__all__ = ["SCENARIO_MODULES", "SOURCES"]
