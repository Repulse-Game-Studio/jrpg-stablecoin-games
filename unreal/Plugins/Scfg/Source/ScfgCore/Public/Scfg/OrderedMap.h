// No Python twin: a dict[str, V] in insertion order, as Python's dict keeps it (the chain registries, a chain's
// tokens, a Config's custom chains, items, rpc_urls and confirmations).
//
// Assigning an existing key keeps its position; a key that was erased and is assigned again goes last. Lookup is a
// linear scan: these dicts hold a handful of entries (the largest is a registry of six chains), so a hash index
// would cost more than it saves. Values are values (copying the map copies them), unlike Json's shared dicts.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scfg {

template <class V>
class OrderedMap {
public:
    using Entry = std::pair<std::string, V>;
    using const_iterator = typename std::vector<Entry>::const_iterator;

    OrderedMap() = default;
    // dict(items): a repeated key keeps its first position and its last value.
    OrderedMap(std::initializer_list<Entry> items) {
        for (const Entry& e : items) set(e.first, e.second);
    }

    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    // key in d.
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    // &d[key], or nullptr when the key is absent. Valid until the next set() of a new key or erase().
    const V* find(std::string_view key) const {
        for (const Entry& e : entries_) {
            if (e.first == key) return &e.second;
        }
        return nullptr;
    }
    V* find(std::string_view key) {
        for (Entry& e : entries_) {
            if (e.first == key) return &e.second;
        }
        return nullptr;
    }
    // d[key] = value.
    void set(std::string key, V value) {
        if (V* at = find(key)) {
            *at = std::move(value);
            return;
        }
        entries_.emplace_back(std::move(key), std::move(value));
    }
    // del d[key] without the KeyError: true when the key was there.
    bool erase(std::string_view key) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].first == key) {
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }
    // list(d), in order.
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        out.reserve(entries_.size());
        for (const Entry& e : entries_) out.push_back(e.first);
        return out;
    }

    // (key, value) pairs in order.
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }

private:
    std::vector<Entry> entries_;
};

}  // namespace scfg
