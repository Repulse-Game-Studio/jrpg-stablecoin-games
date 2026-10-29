"""Exception types raised to the developer."""


class CryptoPayError(Exception):
    """Base class for all cryptopay errors."""


class ConfigError(CryptoPayError, ValueError):
    """Invalid configuration or programmer error. Raised immediately so it shows up in testing."""
