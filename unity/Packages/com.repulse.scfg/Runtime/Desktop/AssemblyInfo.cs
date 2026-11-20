// No Python twin: assembly attributes of Scfg.Desktop.
// The CheckoutServer's internal test hooks (ConnectionCount, AcceptedCount, ConnectionStates, NonBlocking in
// LocalServer.cs) are visible to the NUnit tests only, never to a game.
#nullable enable
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Scfg.Core.Tests")]
