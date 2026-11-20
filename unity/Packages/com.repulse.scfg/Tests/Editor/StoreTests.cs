// Twin of godot/tests/test_store.gd: ScfgStore's load and save on real files in a temporary directory.
#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Tests
{
    public sealed class StoreTests
    {
        private string _dir = "";

        [SetUp]
        public void Fresh()
        {
            _dir = Path.Combine(Path.GetTempPath(), "scfg-unity-store", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(_dir);
        }

        [TearDown]
        public void Clean()
        {
            Chmod(_dir, "755");
            try
            {
                Directory.Delete(_dir, true);
            }
            catch (IOException)
            {
            }
        }

        private string P(string name = "state.json") => Path.Combine(_dir, name);

        private static JsonObject Pending() => new JsonObject
        {
            {
                "s1", new JsonObject
                {
                    { "chain", "arbitrum" }, { "code", 1L }, { "created_at", 1700000000.5 }, { "cursor", 10L }, { "hint_at", null },
                    { "hint_tx", null }, { "id", "s1" }, { "item", "chapter2" }, { "kind", "unlock" }, { "match", null },
                    { "nonce", 7L }, { "price", "4.99" }, { "start_block", 9L }, { "state", "awaiting" }, { "title", "Chapter 2" },
                    { "token", "USDC" }, { "ui_status", "open" }, { "units", 4990007L },
                }
            },
        };

        private static JsonObject Receipts() => new JsonObject
        {
            {
                "arbitrum:0xab:0", new JsonObject
                {
                    { "amount", "4.99" }, { "block", 999L }, { "chain", "arbitrum" }, { "item", "chapter2" }, { "kind", "unlock" },
                    { "log_index", 0L }, { "payer", null }, { "recipient", "0x52908400098527886E0F7030069857D2E4169EE7" },
                    { "source", "payment" }, { "token", "USDC" }, { "tx_hash", "0xab" },
                }
            },
        };

        [Test]
        public void SaveThenLoadRoundTripsWithInts()
        {
            Assert.That(ScfgStore.Save(P(), Pending(), Receipts()), Is.Null);
            var got = ScfgStore.Load(P());
            Assert.That(got.Warning, Is.Null);
            Assert.That(got.Error, Is.Null);
            Assert.That(Json.Equal(got.Pending, Pending()), Is.True);
            Assert.That(Json.Equal(got.Receipts, Receipts()), Is.True);
            Assert.That(((JsonObject)got.Pending["s1"]!)["units"], Is.TypeOf<long>());
            Assert.That(((JsonObject)got.Pending["s1"]!)["created_at"], Is.EqualTo(1700000000.5));
            Assert.That(File.Exists(P() + ".tmp"), Is.False);
            Assert.That(File.ReadAllText(P()), Does.StartWith("{\"version\": 1, \"pending\": {"));
        }

        [Test]
        public void SaveCreatesTheDirectory()
        {
            var deep = Path.Combine(_dir, "a", "b", "state.json");
            Assert.That(ScfgStore.Save(deep, new JsonObject(), new JsonObject()), Is.Null);
            Assert.That(File.Exists(deep), Is.True);
        }

        [Test]
        public void MissingFileIsEmptyWithoutAWarning()
        {
            var got = ScfgStore.Load(P());
            Assert.That(got.Pending.Count + got.Receipts.Count, Is.EqualTo(0));
            Assert.That(got.Warning, Is.Null);
            Assert.That(got.Error, Is.Null);
        }

        [TestCase("not json at all", "is not valid JSON")]
        [TestCase("[1, 2]", "is not a JSON object")]
        [TestCase("{\"version\": 2, \"pending\": {}, \"receipts\": {}}", "has an unknown version 2")]
        [TestCase("{\"version\": true, \"pending\": {}, \"receipts\": {}}", "has an unknown version True")]
        [TestCase("{\"version\": 1, \"pending\": []}", "has no pending/receipts objects")]
        [TestCase("{\"version\": 1, \"pending\": {\"s\": {\"id\": 3}}, \"receipts\": {}}", "does not match the state schema")]
        public void CorruptFileIsEmptyWithAWarningAndKeptAside(string text, string why)
        {
            File.WriteAllText(P(), text);
            var got = ScfgStore.Load(P());
            Assert.That(got.Pending.Count + got.Receipts.Count, Is.EqualTo(0));
            Assert.That(got.Error, Is.Null);
            Assert.That(got.Warning, Does.Contain(why));
            Assert.That(got.Warning, Does.Contain(P() + ".corrupt"));
            Assert.That(File.Exists(P()), Is.False);
            Assert.That(File.ReadAllText(P() + ".corrupt"), Is.EqualTo(text));
        }

        [Test]
        public void InvalidUtf8IsACorruptFileNotPatchedText()
        {
            var bytes = Encoding.UTF8.GetBytes("{\"version\": 1, \"pending\": {}, \"receipts\": {\"k\": \"x\"}}");
            bytes[bytes.Length - 4] = 0xff;
            File.WriteAllBytes(P(), bytes);
            var got = ScfgStore.Load(P());
            Assert.That(got.Warning, Does.Contain("is not valid UTF-8"));
            Assert.That(File.ReadAllBytes(P() + ".corrupt"), Is.EqualTo(bytes));
        }

        [Test]
        public void ReceiptProblemsKeepTheFile()
        {
            var receipts = Receipts();
            ((JsonObject)receipts["arbitrum:0xab:0"]!).Remove("amount");
            Assert.That(ScfgStore.Save(P(), new JsonObject(), receipts), Is.Null);
            var got = ScfgStore.Load(P());
            Assert.That(got.Warning, Is.Null);
            Assert.That(got.Receipts.Count, Is.EqualTo(1));
            Assert.That(got.ReceiptProblems, Is.Not.Empty);
            Assert.That(File.Exists(P()), Is.True);
        }

        [Test]
        public void LaterCorruptionKeepsEarlierCopies()
        {
            File.WriteAllText(P(), "one");
            ScfgStore.Load(P());
            File.WriteAllText(P(), "two");
            var got = ScfgStore.Load(P());
            Assert.That(got.Warning, Does.Contain(P() + ".corrupt.2"));
            Assert.That(File.ReadAllText(P() + ".corrupt"), Is.EqualTo("one"));
            Assert.That(File.ReadAllText(P() + ".corrupt.2"), Is.EqualTo("two"));
        }

        [Test]
        public void LoadFallsBackToACompleteTmp()
        {
            Assert.That(ScfgStore.Save(P(), Pending(), Receipts()), Is.Null);
            File.Move(P(), P() + ".tmp"); // a save stopped between deleting the old file and moving the new one in
            var got = ScfgStore.Load(P());
            Assert.That(got.Warning, Is.Null);
            Assert.That(Json.Equal(got.Receipts, Receipts()), Is.True);
        }

        [Test]
        public void SaveReplacesAnExistingFileAtomically()
        {
            Assert.That(ScfgStore.Save(P(), new JsonObject(), new JsonObject()), Is.Null);
            Assert.That(ScfgStore.Save(P(), Pending(), Receipts()), Is.Null);
            Assert.That(Json.Equal(ScfgStore.Load(P()).Receipts, Receipts()), Is.True);
            Assert.That(File.Exists(P() + ".tmp"), Is.False);
        }

        private static bool OnWindows => Path.DirectorySeparatorChar == '\\';

        /// <summary>Opens <paramref name="path"/> as a scanner does: reading, without letting others delete or replace it.</summary>
        private static FileStream Hold(string path) => new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);

        [Test]
        public void AReplaceRefusedForAMomentIsTriedAgain()
        {
            // Windows refuses to replace a file another process has open without FILE_SHARE_DELETE (a virus scanner reading
            // the file just written, say): the replace waits for it, briefly. Elsewhere the open file is no obstacle.
            Assert.That(ScfgStore.Save(P(), new JsonObject(), new JsonObject()), Is.Null);
            var held = Hold(P());
            // a thread of its own, not a pool timer: the editor's thread pool can start a timer's callback late
            var release = new System.Threading.Thread(() =>
            {
                System.Threading.Thread.Sleep(10);
                held.Dispose();
            });
            release.Start();
            try
            {
                Assert.That(ScfgStore.Save(P(), Pending(), Receipts()), Is.Null);
            }
            finally
            {
                release.Join();
                held.Dispose();
            }
            Assert.That(Json.Equal(ScfgStore.Load(P()).Receipts, Receipts()), Is.True);
            Assert.That(File.Exists(P() + ".tmp"), Is.False);
        }

        [Test]
        public void AReplaceRefusedForLongIsAnErrorAndLeavesTheOldFile()
        {
            if (!OnWindows) Assert.Ignore("an open file blocks a replace on Windows only");
            Assert.That(ScfgStore.Save(P(), new JsonObject(), Receipts()), Is.Null);
            Exception? err;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            using (Hold(P()))
                err = ScfgStore.Save(P(), new JsonObject(), new JsonObject());
            watch.Stop();
            Assert.That(err, Is.Not.Null);
            Assert.That(err!.Message, Does.Contain("cannot replace"));
            Assert.That(ScfgStore.IsTransientReplaceError(err.InnerException!), Is.True, err.InnerException!.ToString());
            Assert.That(watch.ElapsedMilliseconds, Is.LessThan(2000), "the retries are short");
            Assert.That(Json.Equal(ScfgStore.Load(P()).Receipts, Receipts()), Is.True);
        }

        [Test]
        public void OnlySharingErrorsAreTriedAgain()
        {
            Assert.That(ScfgStore.IsTransientReplaceError(new IOException("x", unchecked((int)0x80070020))), Is.True, "sharing violation");
            Assert.That(ScfgStore.IsTransientReplaceError(new IOException("x", unchecked((int)0x80070497))), Is.True, "unable to remove replaced");
            Assert.That(ScfgStore.IsTransientReplaceError(new IOException("x", 1175)), Is.True, "a bare Win32 code");
            Assert.That(ScfgStore.IsTransientReplaceError(new IOException("x", unchecked((int)0x80070070))), Is.False, "disk full");
            Assert.That(ScfgStore.IsTransientReplaceError(new UnauthorizedAccessException("x")), Is.False);
            Assert.That(ScfgStore.IsTransientReplaceError(new FileNotFoundException("x")), Is.False);
        }

        [Test]
        public void AFailedWriteLeavesTheOldFile()
        {
            Assert.That(ScfgStore.Save(P(), new JsonObject(), Receipts()), Is.Null);
            Directory.CreateDirectory(P() + ".tmp"); // the .tmp cannot be written
            var err = ScfgStore.Save(P(), new JsonObject(), new JsonObject());
            Assert.That(err, Is.Not.Null);
            Assert.That(err!.Message, Does.Contain("cannot write"));
            Directory.Delete(P() + ".tmp");
            Assert.That(Json.Equal(ScfgStore.Load(P()).Receipts, Receipts()), Is.True);
        }

        [Test]
        public void UnreadableFileIsLeftUntouched()
        {
            if (!Chmod(_dir, "755")) Assert.Ignore("needs chmod (Linux / macOS)");
            File.WriteAllText(P(), "{\"version\": 1, \"pending\": {}, \"receipts\": {}}");
            Chmod(P(), "000");
            if (CanRead(P())) Assert.Ignore("running as root: permissions are not enforced");
            var got = ScfgStore.Load(P());
            Assert.That(got.Error, Is.Not.Null);
            Assert.That(got.Error!.Message, Does.Contain("cannot read"));
            Assert.That(got.Warning, Is.Null);
            Chmod(P(), "644");
            Assert.That(File.Exists(P() + ".corrupt"), Is.False);
            Assert.That(File.ReadAllText(P()), Does.Contain("\"version\": 1"));
        }

        [Test]
        public void ACorruptFileThatCannotBeMovedAsideIsAnError()
        {
            if (!Chmod(_dir, "755")) Assert.Ignore("needs chmod (Linux / macOS)");
            File.WriteAllText(P(), "garbage");
            Chmod(_dir, "555"); // no rename inside the directory
            if (CanCreate(_dir)) Assert.Ignore("running as root: permissions are not enforced");
            var got = ScfgStore.Load(P());
            Chmod(_dir, "755");
            Assert.That(got.Error, Is.Not.Null);
            Assert.That(got.Error!.Message, Does.Contain("could not be moved aside"));
            Assert.That(File.ReadAllText(P()), Is.EqualTo("garbage"));
        }

        private static bool CanRead(string path)
        {
            try
            {
                File.ReadAllBytes(path);
                return true;
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
            catch (IOException)
            {
                return false;
            }
        }

        private static bool CanCreate(string dir)
        {
            var probe = Path.Combine(dir, "probe");
            try
            {
                File.WriteAllText(probe, "");
                File.Delete(probe);
                return true;
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
            catch (IOException)
            {
                return false;
            }
        }

        /// <summary>chmod through /bin/chmod (System.IO has no portable API on .NET Standard 2.1).</summary>
        internal static bool Chmod(string path, string mode)
        {
            if (!File.Exists("/bin/chmod") || !(File.Exists(path) || Directory.Exists(path))) return false;
            try
            {
                using var p = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("/bin/chmod", mode + " \"" + path + "\"")
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                });
                p!.WaitForExit();
                return p.ExitCode == 0;
            }
            catch (Exception)
            {
                return false;
            }
        }
    }
}
