// The glue's internals (test hooks: an injected transport, clock, scheduler and store path) are visible to the
// package's own Unity Test Framework assemblies only.
#nullable enable
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Scfg.Tests.Editor")]
[assembly: InternalsVisibleTo("Scfg.Tests.Runtime")]
[assembly: InternalsVisibleTo("Scfg.Editor")]
