// The few package internals the e2e driver needs, reached by reflection: the package grants its internals to its own
// test assemblies only (InternalsVisibleTo), never to an assembly a game could name. The IL2CPP players are stripped
// at Managed Stripping Level High, so link.xml next to this file keeps exactly the members named here. A member that
// is missing (renamed in the package) fails the driver's setup with its name, before any command runs.
//
//   - the facade's state (SCFG.Current, internal): its Platform (the recording platform goes in), Waits (the open
//     screens, each a ScreenWait's Screen) and OwnTransport (the facade's own UnityWebRequestTransport, for "post");
//   - a screen's registry (ScfgScreenView.Keys, Control, Press and Focused, internal): what a snapshot lists and what
//     a click presses;
//   - Scfg.Core.Chains.Override (internal), which points a built-in chain at the local node.
#nullable enable
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.ExceptionServices;
using Scfg.Core;
using UnityEngine.UIElements;

namespace Scfg.E2E
{
    internal static class E2EInternals
    {
        private const BindingFlags Static = BindingFlags.NonPublic | BindingFlags.Static;
        private const BindingFlags Instance = BindingFlags.NonPublic | BindingFlags.Instance;
        private const BindingFlags PublicField = BindingFlags.Public | BindingFlags.Instance;

        private static readonly PropertyInfo? CurrentProperty = typeof(SCFG).GetProperty("Current", Static);
        private static readonly Type? StateType = typeof(SCFG).GetNestedType("State", BindingFlags.NonPublic);
        private static readonly Type? WaitType = typeof(SCFG).GetNestedType("ScreenWait", BindingFlags.NonPublic);
        private static readonly FieldInfo? PlatformField = StateType?.GetField("Platform", PublicField);
        private static readonly FieldInfo? WaitsField = StateType?.GetField("Waits", PublicField);
        private static readonly FieldInfo? OwnTransportField = StateType?.GetField("OwnTransport", PublicField);
        private static readonly FieldInfo? ScreenField = WaitType?.GetField("Screen", PublicField);
        private static readonly PropertyInfo? KeysProperty = typeof(ScfgScreenView).GetProperty("Keys", Instance);
        private static readonly PropertyInfo? FocusedProperty = typeof(ScfgScreenView).GetProperty("Focused", Instance);
        private static readonly MethodInfo? ControlMethod =
            typeof(ScfgScreenView).GetMethod("Control", Instance, null, new[] { typeof(string) }, null);
        private static readonly MethodInfo? PressMethod =
            typeof(ScfgScreenView).GetMethod("Press", Instance, null, new[] { typeof(string) }, null);
        private static readonly MethodInfo? OverrideMethod =
            typeof(Chains).GetMethod("Override", Static, null, new[] { typeof(bool), typeof(Chain) }, null);

        /// <summary>The members this file needs that were not found (empty when the driver can run).</summary>
        public static List<string> Missing()
        {
            var missing = new List<string>();
            void Need(object? member, string name)
            {
                if (member == null) missing.Add(name);
            }
            Need(CurrentProperty, "SCFG.Current");
            Need(PlatformField, "SCFG.State.Platform");
            Need(WaitsField, "SCFG.State.Waits");
            Need(OwnTransportField, "SCFG.State.OwnTransport");
            Need(ScreenField, "SCFG.ScreenWait.Screen");
            Need(KeysProperty, "ScfgScreenView.Keys");
            Need(FocusedProperty, "ScfgScreenView.Focused");
            Need(ControlMethod, "ScfgScreenView.Control(string)");
            Need(PressMethod, "ScfgScreenView.Press(string)");
            Need(OverrideMethod, "Scfg.Core.Chains.Override(bool, Chain)");
            return missing;
        }

        private static object State() => CurrentProperty!.GetValue(null)!;

        /// <summary>The facade's platform (the clipboard, links and the platform checks).</summary>
        public static void SetPlatform(IScfgPlatform platform) => PlatformField!.SetValue(State(), platform);

        /// <summary>The screens the open Pay and Restore flows wait on, oldest first.</summary>
        public static List<IScfgScreen> Screens()
        {
            var screens = new List<IScfgScreen>();
            if (WaitsField!.GetValue(State()) is IEnumerable waits)
                foreach (var wait in waits)
                    if (ScreenField!.GetValue(wait) is IScfgScreen screen) screens.Add(screen);
            return screens;
        }

        /// <summary>The facade's own transport (made once it first needs one), or null.</summary>
        public static ITransport? OwnTransport() => OwnTransportField!.GetValue(State()) as ITransport;

        /// <summary>The keys of the screen's registered controls in its current build.</summary>
        public static IEnumerable<string> Keys(ScfgScreenView view) => (IEnumerable<string>)KeysProperty!.GetValue(view)!;

        /// <summary>The control registered under <paramref name="key"/>, or null.</summary>
        public static VisualElement? Control(ScfgScreenView view, string key) =>
            ControlMethod!.Invoke(view, new object[] { key }) as VisualElement;

        /// <summary>Presses the enabled control <paramref name="key"/> (its own action); false if there is none.</summary>
        public static bool Press(ScfgScreenView view, string key) => (bool)PressMethod!.Invoke(view, new object[] { key })!;

        /// <summary>The focused control's key ("" when the focus is elsewhere).</summary>
        public static string Focused(ScfgScreenView view) => (string)FocusedProperty!.GetValue(view)!;

        /// <summary>Replaces the built-in chain <c>chain.Key</c> of one network.</summary>
        public static void OverrideChain(bool testnet, Chain chain)
        {
            try
            {
                OverrideMethod!.Invoke(null, new object[] { testnet, chain });
            }
            catch (TargetInvocationException e) when (e.InnerException != null)
            {
                // the core's own KeyError or ValueError, as a direct call would throw it
                ExceptionDispatchInfo.Capture(e.InnerException).Throw();
            }
        }
    }
}
