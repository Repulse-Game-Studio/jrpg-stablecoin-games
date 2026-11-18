// The glue's internals (test hooks: an injected transport, clock, scheduler, platform and store path) are visible to
// the package's own Unity Test Framework and Editor assemblies only, never to a game's.
#nullable enable
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Scfg.Tests.Editor")]
[assembly: InternalsVisibleTo("Scfg.Tests.Runtime")]
[assembly: InternalsVisibleTo("Scfg.Editor")]
