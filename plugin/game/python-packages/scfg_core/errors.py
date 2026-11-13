"""Exception types raised to the developer, and the default report of a contained crash."""

import sys
import traceback


class ScfgError(Exception):
    """Base class for all Stablecoin for Games errors."""


class ConfigError(ScfgError, ValueError):
    """Invalid configuration or programmer error. Raised immediately so it shows up in testing.

    `code` is a stable machine-readable identifier of the reason, `<subject>.<problem>`
    (for example `chains.empty`). Conformance vectors compare it instead of the message.
    """

    def __init__(self, message: str, *, code: str):
        super().__init__(message)
        self.code = code


def print_error(exc: BaseException) -> None:
    """The default `on_error`: a crash the core contained (one session's step, one stored
    receipt, one restore candidate) is printed to stderr and the work goes on."""
    traceback.print_exception(type(exc), exc, exc.__traceback__, file=sys.stderr)
