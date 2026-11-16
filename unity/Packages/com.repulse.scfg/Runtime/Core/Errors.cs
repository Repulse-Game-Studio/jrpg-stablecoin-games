// Twin of scfg_core/errors.py (ConfigError) and of RestoreError in scfg_core/restore.py, plus the
// Python built-in exceptions the core raises where Python would (ValueError, TypeError, KeyError,
// IndexError, ArithmeticError, OverflowError). The classes follow Python's tree: ConfigError is a
// ValueError, OverflowError is an ArithmeticError and not a ValueError. RpcError and TransportError
// are added with rpc.py's and coio.py's twins.
using System;

namespace Scfg.Core
{
    /// <summary>Python <c>ValueError</c>.</summary>
    public class ValueError : Exception
    {
        public ValueError() { }
        public ValueError(string message) : base(message) { }
        public ValueError(string message, Exception inner) : base(message, inner) { }
    }

    /// <summary>Python <c>TypeError</c>.</summary>
    public class TypeError : Exception
    {
        public TypeError() { }
        public TypeError(string message) : base(message) { }
    }

    /// <summary>Python <c>KeyError</c>: <c>d[key]</c> on a missing key. The message is <c>repr(key)</c>, as Python's str(e).</summary>
    public class KeyError : Exception
    {
        public string Key { get; }

        public KeyError(string key) : base(Py.Repr(key))
        {
            Key = key;
        }
    }

    /// <summary>Python <c>IndexError</c>: <c>seq[i]</c> out of range.</summary>
    public class IndexError : Exception
    {
        public IndexError() : base("list index out of range") { }
        public IndexError(string message) : base(message) { }
    }

    /// <summary>Python <c>ArithmeticError</c>: the base of OverflowError (and of decimal's signals); not a ValueError.</summary>
    public class ArithmeticError : Exception
    {
        public ArithmeticError() { }
        public ArithmeticError(string message) : base(message) { }
    }

    /// <summary>Python <c>OverflowError</c>, as <c>int(float('inf'))</c> raises it.</summary>
    public class OverflowError : ArithmeticError
    {
        public OverflowError(string message) : base(message) { }
    }

    /// <summary>
    /// Invalid configuration or programmer error. Raised immediately so it shows up in testing.
    /// <see cref="Code"/> is a stable machine-readable identifier of the reason, <c>subject.problem</c>
    /// (for example <c>chains.empty</c>). Conformance vectors compare it instead of the message.
    /// Python's <c>ConfigError(ScfgError, ValueError)</c>: here a ValueError.
    /// </summary>
    public class ConfigError : ValueError
    {
        public string Code { get; }

        public ConfigError(string message, string code) : base(message)
        {
            Code = code;
        }
    }

    /// <summary>
    /// restore.py's RestoreError. <see cref="Code"/> (also the message): invalid_hash, tx_not_found,
    /// tx_reverted, tx_not_a_payment or network_unavailable.
    /// </summary>
    public class RestoreError : Exception
    {
        public string Code { get; }

        public RestoreError(string code) : base(code)
        {
            Code = code;
        }
    }
}
