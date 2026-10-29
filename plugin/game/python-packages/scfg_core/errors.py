"""Exception types raised to the developer."""


class ScfgError(Exception):
    """Base class for all Stablecoin for Games errors."""


class ConfigError(ScfgError, ValueError):
    """Invalid configuration or programmer error. Raised immediately so it shows up in testing."""
