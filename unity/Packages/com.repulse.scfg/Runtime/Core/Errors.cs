// Twin of scfg_core/errors.py (ConfigError) and of RestoreError in scfg_core/restore.py, plus the
// Python built-in exceptions the core raises where Python would (ValueError, TypeError, KeyError,
// IndexError, ArithmeticError, OverflowError) and the decimal module's signals the core can raise
// (DecimalException, InvalidOperation, Overflow). The classes follow Python's tree: ConfigError is a
// ValueError, OverflowError and the decimal signals are ArithmeticErrors and not ValueErrors.
// Also errors.py's print_error. RpcError, RpcUnavailable and TransportError live in Rpc.cs and Coio.cs. RuntimeError
// and concurrent.futures' CancelledError and InvalidStateError are here for Worker.cs's Job (Python's Future).
//
// Naming: the core keeps Python's names on purpose, so each file can be read side by side with its twin. Exceptions
// carry Python's class names without an Exception suffix (Scfg.Core.ArithmeticError shares its simple name with
// System.ArithmeticError: qualify it in a file that opens both namespaces), Task-returning methods have no Async
// suffix (Engine.Tick, RpcClient.Call, ITransport.PostJson), and a Job's Done(), Cancelled() and Running() are
// methods, as on Python's Future.
#nullable enable
using System;

namespace Scfg.Core
{
    public static class Errors
    {
        /// <summary>
        /// Python <c>print_error</c>, the default <c>on_error</c>: a crash the core contained (one session's step, one
        /// stored receipt, one restore candidate) is written to standard error and the work goes on.
        /// </summary>
        public static void PrintError(Exception e) => Console.Error.WriteLine(e.ToString()); // culture-ok: an exception's text
    }

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

        private KeyError(string message, bool verbatim) : base(message)
        {
            Key = message;
        }

        /// <summary>
        /// The KeyError for a key that is not a str (None, a number): <paramref name="repr"/> is the key's Python
        /// repr, used as it is for the message and <see cref="Key"/> (str(KeyError(5)) is 5, not '5').
        /// </summary>
        internal static KeyError OfRepr(string repr) => new KeyError(repr, verbatim: true);
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

    /// <summary>Python <c>decimal.DecimalException</c>, the base of the decimal module's signals.</summary>
    public class DecimalException : ArithmeticError
    {
        public DecimalException() { }
        public DecimalException(string message) : base(message) { }
    }

    /// <summary>
    /// Python <c>decimal.InvalidOperation</c>, as a quantize whose result needs more than the context's 28
    /// digits raises it. A DecimalException, so not a ValueError, as in Python.
    /// </summary>
    public class InvalidOperation : DecimalException
    {
        public InvalidOperation(string message) : base(message) { }
    }

    /// <summary>
    /// Python <c>decimal.Overflow</c>: a context operation whose result is above Emax, which the default
    /// context traps. (Python's Overflow also subclasses Inexact and Rounded, which the port does not model.)
    /// Not the built-in <see cref="OverflowError"/>.
    /// </summary>
    public class Overflow : DecimalException
    {
        public Overflow(string message) : base(message) { }
    }

    /// <summary>Python <c>RuntimeError</c>: <see cref="Job.Result"/> of a job that is not done.</summary>
    public class RuntimeError : Exception
    {
        public RuntimeError() { }
        public RuntimeError(string message) : base(message) { }
    }

    /// <summary>
    /// Python <c>concurrent.futures.CancelledError</c>: the outcome of a <see cref="Job"/> cancelled before it ran
    /// (<see cref="Job.Result"/> throws it, <see cref="Job.Exception"/> returns it).
    /// </summary>
    public sealed class CancelledError : Exception
    {
        public CancelledError() : base("job cancelled") { }
    }

    /// <summary>Python <c>concurrent.futures.InvalidStateError</c>: completing a <see cref="Job"/> that is already done.</summary>
    public sealed class InvalidStateError : Exception
    {
        public InvalidStateError(string message) : base(message) { }
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
