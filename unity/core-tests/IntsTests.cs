// No Python twin: tests of Ints (twin of scfg_core/ints.py). Expected values are ints.py's own outputs.
// ToLong: a Python int the port holds as a long is an OverflowError beyond it (Python's class for an int a C long
// cannot hold), at every site that narrows one, so no System.OverflowException escapes the core.
using System.Collections.Generic;
using System.Numerics;
using System.Threading.Tasks;
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class IntsTests
    {
        [Test]
        public void Limits()
        {
            Assert.That(Ints.IntMaxStrDigits, Is.EqualTo(4300));
            Assert.That(Ints.DigitRunLimit, Is.EqualTo(18));
        }

        [TestCase("111111111111111111", 111111111111111111L)] // 18 significant digits
        [TestCase("999999999999999999", 999999999999999999L)]
        [TestCase("1111111111111111111", null)] // 19
        [TestCase("0100000000000000000", 100000000000000000L)] // 19 characters, 18 significant
        [TestCase("000000000000000000", 0L)]
        [TestCase("", 0L)]
        [TestCase("7", 7L)]
        public void DigitRunValue(string run, long? want)
        {
            Assert.That(Ints.DigitRunValue(run), Is.EqualTo(want));
        }

        [Test]
        public void DigitRunLeadingZerosAreFree()
        {
            Assert.That(Ints.DigitRunValue(new string('0', 100) + new string('1', 18)), Is.EqualTo(111111111111111111L));
            Assert.That(Ints.DigitRunValue(new string('0', 100) + new string('1', 19)), Is.Null);
            Assert.That(Ints.DigitRunValue(new string('0', 100)), Is.EqualTo(0L));
            // zeros of any script: Arabic-Indic, and a mix of scripts
            Assert.That(Ints.DigitRunValue(new string('\u0660', 30) + "\u0665"), Is.EqualTo(5L));
            var mixed = new System.Text.StringBuilder();
            for (int i = 0; i < 20; i++) mixed.Append("0\u0660");
            Assert.That(Ints.DigitRunValue(mixed + "7"), Is.EqualTo(7L));
            Assert.That(Ints.DigitRunValue(string.Concat(System.Linq.Enumerable.Repeat("\U0001D7CE", 30)) + "\U0001D7D7"), Is.EqualTo(9L));
            Assert.That(Ints.DigitRunValue(new string('9', 1_000_000)), Is.Null);
        }

        [Test]
        public void IntFromRefusesOverlongDigitStrings()
        {
            Assert.That(Ints.IntFrom(new string(' ', 5000) + "7"), Is.EqualTo(new BigInteger(7)));
            Assert.That(Ints.IntFrom("-" + new string('9', 4300)), Is.EqualTo(1 - BigInteger.Pow(10, 4300)));
            var e = Assert.Throws<ValueError>(() => Ints.IntFrom(new string('9', 4301)))!;
            Assert.That(e.Message, Is.EqualTo("Exceeds the limit (4300 digits) for integer string conversion: value has 4301 digits; " +
                                              "use sys.set_int_max_str_digits() to increase the limit"));
            Assert.Throws<ValueError>(() => Ints.IntFrom("-" + new string('9', 4301)));
            Assert.That(Assert.Throws<ValueError>(() => Ints.IntFrom(new string('x', 5000)))!.Message,
                Does.StartWith("invalid literal for int() with base 10: 'xxx"));
            Assert.That(Ints.IntFrom(12L), Is.EqualTo(new BigInteger(12)));
        }

        [Test]
        public void JsonLoadsReadsIntegersWithIntFrom()
        {
            Assert.That(Ints.JsonLoads("1" + new string('0', 4299)), Is.TypeOf<BigInteger>());
            Assert.That(Ints.JsonLoads("[1.5e3]"), Is.EqualTo(new System.Collections.Generic.List<object?> { 1500.0 }));
            Assert.That(Assert.Throws<ValueError>(() => Ints.JsonLoads("[" + new string('1', 4301) + "]"))!.Message,
                Does.StartWith("Exceeds the limit (4300 digits) for integer string conversion: value has 4301 digits"));
        }

        // ---- ToLong ------------------------------------------------------------------------------------------------

        [Test]
        public void ToLongIsAnOverflowErrorBeyondLong()
        {
            Assert.That(Ints.ToLong(long.MaxValue, "x"), Is.EqualTo(long.MaxValue));
            Assert.That(Ints.ToLong(long.MinValue, "x"), Is.EqualTo(long.MinValue));
            Assert.That(Ints.ToLong(BigInteger.Zero, "x"), Is.EqualTo(0L));
            var e = Assert.Throws<OverflowError>(() => Ints.ToLong((BigInteger)long.MaxValue + 1, "slot"))!;
            Assert.That(e.Message, Is.EqualTo("slot out of range: 9223372036854775808"));
            Assert.That(e, Is.InstanceOf<ArithmeticError>().And.Not.InstanceOf<ValueError>());
            Assert.That(Assert.Throws<OverflowError>(() => Ints.ToLong((BigInteger)long.MinValue - 1, "block"))!.Message,
                        Is.EqualTo("block out of range: -9223372036854775809"));
        }

        [Test]
        public void AStoredIntBeyondLongIsAnOverflowError()
        {
            // Watcher.Int reads a stored session's cursor, start_block, block and log_index
            Assert.That(Watcher.Int(5L, "cursor"), Is.EqualTo(5L));
            Assert.That(Watcher.Int(true, "cursor"), Is.EqualTo(1L));
            Assert.That(Assert.Throws<OverflowError>(() => Watcher.Int(BigInteger.Pow(2, 64), "cursor"))!.Message,
                        Is.EqualTo("cursor out of range: 18446744073709551616"));
            Assert.Throws<TypeError>(() => Watcher.Int(1.0, "cursor"));
        }

        private const string Wallet = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";

        private static JsonObject TransferLog(string logIndex, string blockNumber) => new JsonObject
        {
            { "address", "0x036CbD53842c5426634e7929541eC2318f3dCF7e" },
            { "topics", new List<object?> { Evm.TransferTopic, Evm.AddressTopic(Wallet), Evm.AddressTopic(Wallet) } },
            { "data", "0x" + new string('0', 63) + "1" }, { "transactionHash", "0x" + new string('a', 64) },
            { "logIndex", logIndex }, { "blockNumber", blockNumber }, { "blockHash", "0x" + new string('b', 64) },
        };

        [Test]
        public void ALogIndexOrBlockNumberBeyondLongIsAnOverflowError()
        {
            Assert.That(Evm.ParseTransferLog(TransferLog("0x7fffffffffffffff", "0x1"))!.LogIndex, Is.EqualTo(long.MaxValue));
            Assert.Throws<OverflowError>(() => Evm.ParseTransferLog(TransferLog("0x8000000000000000", "0x1")));
            Assert.Throws<OverflowError>(() => Evm.ParseTransferLog(TransferLog("0x1", "0x10000000000000000")));
        }

        [Test]
        public void AReceiptLogIndexBeyondLongIsAnOverflowError()
        {
            var receipt = new JsonObject { { "chain", "base" }, { "tx_hash", "0x" + new string('a', 64) }, { "log_index", BigInteger.Pow(2, 64) } };
            Assert.Throws<OverflowError>(() => Receipts.AddReceipt(new JsonObject(), receipt));
            receipt["log_index"] = "18446744073709551616"; // int() of a str
            Assert.Throws<OverflowError>(() => Receipts.AddReceipt(new JsonObject(), receipt));
        }

        private static RpcClient Node(Handler handler) =>
            new RpcClient(new[] { "https://a.rpc" }, new FakeTransport(new Dictionary<string, Handler> { { "https://a.rpc", handler } }));

        [Test]
        public void AnEvmReceiptBlockBelowLongIsAnOverflowErrorNotAnOverflowException()
        {
            // block <= head, so only a block below long.MinValue reaches the narrowing (Python sends hex(block) as is).
            var chain = Chains.Testnet["base"];
            var receipt = new JsonObject
            {
                { "chain", "base" }, { "tx_hash", "0x" + new string('a', 64) }, { "log_index", 0L },
                { "block", -BigInteger.Pow(2, 70) }, { "amount", "1.00" }, { "token", "USDC" },
            };
            var rpc = Node((m, p) => Fakes.Ok("0x10"));
            Assert.ThrowsAsync<OverflowError>(() => Reverify.CheckEvmReceipt(receipt, rpc, chain, new[] { Wallet }));
        }

        [Test]
        public void ASolanaReceiptSlotBelowLongIsAnOverflowErrorNotAnOverflowException()
        {
            var chain = Chains.Testnet["solana"];
            const string owner = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM";
            var receipt = new JsonObject
            {
                { "chain", "solana" }, { "tx_hash", "sig" }, { "log_index", 0L }, { "block", -BigInteger.Pow(2, 70) },
                { "amount", "1.00" }, { "token", "USDC" }, { "recipient", owner },
            };
            var rpc = new Scfg.Core.Solana.SolanaRpc(Node((m, p) => Fakes.Ok(100L)));
            Assert.ThrowsAsync<OverflowError>(() => new SolanaBackend().Reverify(receipt, rpc, chain, new[] { owner }));
        }
    }
}
