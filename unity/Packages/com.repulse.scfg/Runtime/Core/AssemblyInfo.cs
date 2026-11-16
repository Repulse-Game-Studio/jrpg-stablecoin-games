// No Python twin: assembly attributes of Scfg.Core.
// The internal test hooks (Chains.Override, Chains.ResetRegistries) are visible to the NUnit tests and to the
// integration driver only, never to a game: with Unity's domain reload off a public hook would let one play session
// change the next one's registries.
#nullable enable
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Scfg.Core.Tests")]
[assembly: InternalsVisibleTo("Scfg.Core.Driver")]
