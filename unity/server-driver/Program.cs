// No Python twin: the JSON-lines driver for the shared server security suite (tests/server_security/). It runs one
// Scfg.Desktop.CheckoutServer (Runtime/Desktop/LocalServer.cs) at a time, with fake callbacks, and pumps it about
// every millisecond as the game does every frame, like godot/tests/integration/server_driver.gd does for the GDScript
// server and rpgmaker/test/integration/server_driver.ts for the Node one.
//
//   dotnet build unity/server-driver -c Release
//   dotnet unity/server-driver/bin/Release/net8.0/Scfg.Server.Driver.dll
//
// Protocol (tests/server_security/server_handles.py): one JSON command per stdin line, one reply line per command, in
// order, `SCFG-REPLY {"ok": <result>}` or `SCFG-REPLY {"error": "<why>"}`:
//   start  {config}  stop the previous server, start a new one (listening once Start() returns);
//          ok = {url, port, secret}
//          config = {assets: {name: base64}, session_info, status,
//                    logo: null | "broken" | {b64, ctype},
//                    build: null | {mode: "ok", value, delay_s?} | {mode: "fail"} | {mode: "raise"}
//                           | {mode: "never"},
//                    build_timeout_s?}
//   events        ok = {calls: [[name, args]...], jobs: [{done, cancelled}...], running}: every callback the server
//                 made (on_tx, build_tx) and the state of each build job
//   stop          stop the server; ok = {running}
// End of stdin quits (the server is stopped first). Nothing but replies goes to stdout; stderr carries only real
// faults (an exception escaping Pump()), and the suite counts every stderr line as an error.
//
// The main loop must keep turning between commands (the server is pumped, and delayed builds complete, by this loop),
// so stdin is read on a thread, which only queues the lines; the server and its jobs live on the main thread alone.
// The server keeps its real clock (a Stopwatch): the suite's deadlines and build timeouts are wall-clock seconds.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using Scfg.Core;
using Scfg.Desktop;

namespace Scfg.Server.Driver
{
    public sealed class DriverError : Exception
    {
        public DriverError(string message) : base(message) { }
    }

    public static class Program
    {
        private const string ReplyPrefix = "SCFG-REPLY ";

        private static readonly object InboxLock = new object();
        private static readonly Queue<string?> Inbox = new Queue<string?>(); // command lines; null marks the end of stdin
        private static readonly Stopwatch Clock = Stopwatch.StartNew();

        private static CheckoutServer? _server;
        private static List<object?> _calls = new List<object?>();
        private static List<Job> _jobs = new List<Job>();
        private static List<(Job Job, object? Value, double Due)> _delayed = new List<(Job, object?, double)>();
        private static JsonObject _config = new JsonObject();

        private static double Now() => Clock.Elapsed.TotalSeconds;

        /// <summary>A missing key reads as null (JavaScript's <c>config.key</c>), not as a KeyError.</summary>
        private static object? Opt(JsonObject d, string key) => d.TryGetValue(key, out var v) ? v : null;

        private static double Number(object? v) => Convert.ToDouble(v ?? 0L, CultureInfo.InvariantCulture);

        private static string Clip(string line) => line.Length > 200 ? line.Substring(0, 200) : line;

        private static void StopServer()
        {
            if (_server != null) _server.Stop();
        }

        private static Job BuildTx(string account)
        {
            _calls.Add(new List<object?> { "build_tx", new List<object?> { account } });
            var build = (JsonObject)Opt(_config, "build")!;
            var mode = Opt(build, "mode") as string;
            if (mode == "raise") throw new RuntimeError("submit failed");
            var job = new Job();
            _jobs.Add(job);
            if (mode == "fail")
            {
                job.SetException(new RuntimeError("timed out"));
            }
            else if (mode == "ok")
            {
                double delay = Number(Opt(build, "delay_s"));
                if (delay > 0) _delayed.Add((job, Opt(build, "value"), Now() + delay));
                else job.SetResult(Opt(build, "value"));
            }
            return job;
        }

        private static object? Start(object? cfg)
        {
            var config = cfg as JsonObject ?? throw new DriverError("config must be an object");
            StopServer();
            _config = config;
            _calls = new List<object?>();
            _jobs = new List<Job>();
            _delayed = new List<(Job, object?, double)>();
            var assets = Opt(config, "assets") as JsonObject ?? new JsonObject();
            var options = new CheckoutServerOptions
            {
                LoadAsset = name =>
                {
                    if (!(Opt(assets, name) is string b64)) throw new DriverError("no asset " + name);
                    return Convert.FromBase64String(b64);
                },
                SessionInfo = Opt(config, "session_info") as JsonObject ?? new JsonObject(),
                OnTx = (tx, sender) => _calls.Add(new List<object?> { "on_tx", new List<object?> { tx, sender } }),
                GetStatus = () => Opt(config, "status") as JsonObject ?? new JsonObject(),
            };
            var logo = Opt(config, "logo");
            if (logo is string s && s == "broken")
            {
                options.LoadLogo = () => throw new DriverError("gone");
            }
            else if (logo is JsonObject l)
            {
                var body = Convert.FromBase64String((string)Opt(l, "b64")!);
                var ctype = (string)Opt(l, "ctype")!;
                options.LoadLogo = () => (body, ctype);
            }
            if (Opt(config, "build") is JsonObject) options.BuildTx = BuildTx;
            var server = new CheckoutServer(options);
            if (config.TryGetValue("build_timeout_s", out var timeout)) server.BuildTimeoutS = Number(timeout);
            _server = server;
            server.Start();
            return new JsonObject { { "url", server.Url }, { "port", (long)server.Port }, { "secret", server.Secret } };
        }

        private static object? Run(JsonObject cmd)
        {
            switch (Opt(cmd, "op"))
            {
                case "start":
                    return Start(Opt(cmd, "config") ?? new JsonObject());
                case "events":
                    return new JsonObject
                    {
                        { "calls", _calls },
                        { "jobs", _jobs.Select(j => (object?)new JsonObject { { "done", j.Done() }, { "cancelled", j.Cancelled() } }).ToList() },
                        { "running", _server != null && _server.Running },
                    };
                case "stop":
                    StopServer();
                    return new JsonObject { { "running", _server != null && _server.Running } };
            }
            throw new DriverError("unknown op " + Json.Dumps(Opt(cmd, "op")));
        }

        private static string ReplyFor(string line)
        {
            object? cmd;
            try
            {
                cmd = Json.Loads(line);
            }
            catch (Exception)
            {
                return Json.Dumps(new JsonObject { { "error", "not JSON: " + Clip(line) } });
            }
            if (!(cmd is JsonObject obj)) return Json.Dumps(new JsonObject { { "error", "not a JSON object: " + Clip(line) } });
            try
            {
                return Json.Dumps(new JsonObject { { "ok", Run(obj) } });
            }
            catch (Exception e)
            {
                return Json.Dumps(new JsonObject { { "error", e.Message } });
            }
        }

        private static void ReadStdin()
        {
            var stdin = new StreamReader(Console.OpenStandardInput(), new UTF8Encoding(false));
            string? line;
            do
            {
                line = stdin.ReadLine();
                lock (InboxLock) Inbox.Enqueue(line);
            } while (line != null);
        }

        public static void Main()
        {
            var stdout = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
            var reader = new Thread(ReadStdin) { IsBackground = true, Name = "stdin" };
            reader.Start();
            while (true)
            {
                string?[] lines;
                lock (InboxLock)
                {
                    lines = Inbox.ToArray();
                    Inbox.Clear();
                }
                foreach (var line in lines)
                {
                    if (line == null)
                    {
                        StopServer();
                        return;
                    }
                    stdout.WriteLine(ReplyPrefix + ReplyFor(line));
                }
                double now = Now();
                foreach (var d in _delayed.ToArray())
                {
                    if (now < d.Due) continue;
                    _delayed.Remove(d);
                    if (!d.Job.Done()) d.Job.SetResult(d.Value);
                }
                if (_server != null)
                {
                    try
                    {
                        _server.Pump();
                    }
                    catch (Exception e) // the server promises this never happens: a real fault, for the suite to see
                    {
                        Console.Error.WriteLine("Pump() threw: " + e);
                    }
                }
                Thread.Sleep(1);
            }
        }
    }
}
