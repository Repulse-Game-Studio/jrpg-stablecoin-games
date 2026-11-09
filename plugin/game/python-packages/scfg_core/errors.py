"""Exception types raised to the developer."""


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
