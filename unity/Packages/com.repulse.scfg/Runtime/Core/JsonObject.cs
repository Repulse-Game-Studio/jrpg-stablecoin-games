// No Python twin: Python's dict as the core uses it (str keys, insertion order). The universal JSON object.
using System;
using System.Collections;
using System.Collections.Generic;

namespace Scfg.Core
{
    /// <summary>
    /// A dict with str keys that keeps insertion order the Python way: assigning an existing key keeps its
    /// position, and a removed key that is assigned again goes to the end. <c>d[key]</c> on a missing key is a
    /// KeyError. Adding or removing a key while iterating makes the iteration throw
    /// InvalidOperationException (Python's "dictionary changed size during iteration").
    /// </summary>
    public sealed class JsonObject : IEnumerable<KeyValuePair<string, object?>>
    {
        private readonly Dictionary<string, int> _index = new Dictionary<string, int>(StringComparer.Ordinal);
        // Slots in insertion order; a removed key leaves a null slot until the next compaction.
        private readonly List<string?> _keys = new List<string?>();
        private readonly List<object?> _values = new List<object?>();
        private int _version;

        public int Count => _index.Count;

        /// <summary>Python <c>d[key]</c>; a missing key is a KeyError. Setting keeps an existing key's position.</summary>
        public object? this[string key]
        {
            get
            {
                if (_index.TryGetValue(key, out int slot)) return _values[slot];
                throw new KeyError(key);
            }
            set
            {
                if (_index.TryGetValue(key, out int slot))
                {
                    _values[slot] = value;
                    return;
                }
                _index[key] = _keys.Count;
                _keys.Add(key);
                _values.Add(value);
                _version++;
            }
        }

        /// <summary>Python <c>d[key] = value</c>, for collection initializers.</summary>
        public void Add(string key, object? value) => this[key] = value;

        /// <summary>Python <c>key in d</c>.</summary>
        public bool ContainsKey(string key) => _index.ContainsKey(key);

        public bool TryGetValue(string key, out object? value)
        {
            if (_index.TryGetValue(key, out int slot))
            {
                value = _values[slot];
                return true;
            }
            value = null;
            return false;
        }

        /// <summary>Python <c>d.get(key, fallback)</c>.</summary>
        public object? Get(string key, object? fallback = null) => TryGetValue(key, out var v) ? v : fallback;

        /// <summary>Python <c>del d[key]</c> without the KeyError: true when the key was there.</summary>
        public bool Remove(string key)
        {
            if (!_index.TryGetValue(key, out int slot)) return false;
            _index.Remove(key);
            _keys[slot] = null;
            _values[slot] = null;
            _version++;
            int dead = _keys.Count - _index.Count;
            if (dead > 16 && dead > _index.Count) Compact();
            return true;
        }

        private void Compact()
        {
            int w = 0;
            for (int r = 0; r < _keys.Count; r++)
            {
                var k = _keys[r];
                if (k == null) continue;
                _keys[w] = k;
                _values[w] = _values[r];
                _index[k] = w;
                w++;
            }
            _keys.RemoveRange(w, _keys.Count - w);
            _values.RemoveRange(w, _values.Count - w);
        }

        /// <summary>The keys in order (a snapshot).</summary>
        public IReadOnlyList<string> Keys
        {
            get
            {
                var o = new List<string>(_index.Count);
                foreach (var k in _keys)
                    if (k != null) o.Add(k);
                return o;
            }
        }

        /// <summary>The values in key order (a snapshot).</summary>
        public IReadOnlyList<object?> Values
        {
            get
            {
                var o = new List<object?>(_index.Count);
                for (int i = 0; i < _keys.Count; i++)
                    if (_keys[i] != null) o.Add(_values[i]);
                return o;
            }
        }

        public IEnumerator<KeyValuePair<string, object?>> GetEnumerator()
        {
            int version = _version;
            for (int i = 0; i < _keys.Count; i++)
            {
                if (version != _version) throw Changed();
                var k = _keys[i];
                if (k != null) yield return new KeyValuePair<string, object?>(k, _values[i]);
            }
            if (version != _version) throw Changed();
        }

        private static InvalidOperationException Changed() =>
            new InvalidOperationException("dictionary changed size during iteration");

        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    }
}
