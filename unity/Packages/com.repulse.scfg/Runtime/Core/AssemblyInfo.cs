// No Python twin: assembly attributes of Scfg.Core.
// The internal test hooks (Chains.Override, Chains.ResetRegistries) are visible to the NUnit tests, the integration
// driver and the Unity dev project's e2e driver (unity/Project/Assets/E2E, assembly Scfg.E2E) only, never to a game:
// with Unity's domain reload off a public hook would let one play session change the next one's registries.
#nullable enable
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Scfg.Core.Tests")]
[assembly: InternalsVisibleTo("Scfg.Core.Driver")]
[assembly: InternalsVisibleTo("Scfg.E2E")]
