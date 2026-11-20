// No Python twin: destroying a Unity object the package made, in play mode and in edit mode (the edit-mode tests).
#nullable enable
using UnityEngine;

namespace Scfg
{
    internal static class ScfgObjects
    {
        /// <summary>
        /// Object.Destroy in play mode, Object.DestroyImmediate in edit mode (where Destroy logs an error and does
        /// nothing). A null (or already destroyed) object is left alone.
        /// </summary>
        public static void Destroy(Object? o)
        {
            if (o == null) return;
            if (Application.isPlaying) Object.Destroy(o);
            else Object.DestroyImmediate(o);
        }
    }
}
