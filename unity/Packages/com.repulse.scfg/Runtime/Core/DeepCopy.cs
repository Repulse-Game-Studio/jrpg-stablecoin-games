// No Python twin: copy.deepcopy for the Engine's JSON-like state (pending sessions, receipts), mirroring
// rpgmaker/src/core/copy.ts. Python uses the standard library.
using System.Collections.Generic;
using System.Numerics;

namespace Scfg.Core
{
    public static class DeepCopy
    {
        /// <summary>
        /// A deep copy of a JSON value: lists and <see cref="JsonObject"/>s are copied (keys keep their order),
        /// immutable values (null, bool, int, long, BigInteger, double, string) are returned as they are.
        /// Anything else is a TypeError: Engine state is plain data.
        /// </summary>
        public static object? Of(object? value) => Walk(value, 0);

        private static object? Walk(object? v, int depth)
        {
            switch (v)
            {
                case null:
                case bool _:
                case int _:
                case long _:
                case BigInteger _:
                case double _:
                case string _:
                    return v;
                case List<object?> list:
                {
                    Check(depth);
                    var o = new List<object?>(list.Count);
                    foreach (var x in list) o.Add(Walk(x, depth + 1));
                    return o;
                }
                case JsonObject obj:
                {
                    Check(depth);
                    var o = new JsonObject();
                    foreach (var kv in obj) o[kv.Key] = Walk(kv.Value, depth + 1);
                    return o;
                }
                default:
                    throw new TypeError("cannot copy a " + Py.TypeName(v));
            }
        }

        private static void Check(int depth)
        {
            if (depth >= Json.MaxDepth) throw new ValueError("value nested deeper than Json.MaxDepth (or a circular reference)");
        }
    }
}
