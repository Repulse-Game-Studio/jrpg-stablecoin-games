// Twin of scfg_core/evm.py: EIP-55 addresses, event topics and parse_transfer_log.
#include "Scfg/Evm.h"

#include "Scfg/Bytes.h"
#include "Scfg/Err.h"
#include "Scfg/Keccak.h"
#include "Scfg/Py.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace scfg::evm::detail {

// re.fullmatch(r"0x[0-9a-fA-F]{digits}", s): _HEX40 and _WORD (ASCII digits only; no trailing newline).
bool is_hex_word(std::string_view s, size_t digits) {
    if (s.size() != digits + 2 || s[0] != '0' || s[1] != 'x') return false;
    for (size_t i = 2; i < s.size(); ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

std::string type_name(const Json& v) { return std::string(v.type_name()); }

// v.lower() of a log field: an AttributeError for a non-str, as Python's.
Result<std::string> lower_attr(const Json& v) {
    if (!v.is_str()) return Err::make("AttributeError", "'" + type_name(v) + "' object has no attribute 'lower'");
    return py::lower_for_ascii(v.as_str());
}

// len(v), or Python's TypeError.
Result<size_t> py_len(const Json& v) {
    if (v.is_list()) return v.as_list().size();
    if (v.is_dict()) return v.as_dict().size();
    if (v.is_str()) return py::len(v.as_str());
    return Err::type_error("object of type '" + type_name(v) + "' has no len()");
}

// _topic_to_address: to_checksum_address("0x" + topic[-40:]), with what CPython 3.9 raises for a topic that is not a
// str (3.12 makes a dict's topic[-40:] a KeyError, since slices became hashable).
Result<std::string> topic_to_address(const Json& topic) {
    if (topic.is_list()) return Err::type_error("can only concatenate str (not \"list\") to str");
    if (topic.is_dict()) return Err::type_error("unhashable type: 'slice'");
    if (!topic.is_str()) return Err::type_error("'" + type_name(topic) + "' object is not subscriptable");
    std::string_view s = topic.as_str();
    size_t n = py::len(s);
    size_t i = 0;
    for (size_t skip = n > 40 ? n - 40 : 0; skip > 0; --skip) py::utf8_decode(s, i);  // the last 40 code points
    return to_checksum_address(Json("0x" + std::string(s.substr(i))));
}

}  // namespace scfg::evm::detail

namespace scfg::evm {

Result<std::string> to_checksum_address(const Json& address) {
    if (!address.is_str() || !detail::is_hex_word(address.as_str(), 40)) {
        SCFG_TRY(std::string shown, py::repr(address));
        return Err::value_error("not a 20-byte hex address: " + shown);
    }
    std::string lower = py::lower_ascii(std::string_view(address.as_str()).substr(2));  // hex digits: ASCII
    std::string digest = bytes::to_hex(keccak::keccak256(lower));                     // lowercase hex
    std::string out = "0x";
    out.reserve(42);
    for (size_t i = 0; i < lower.size(); ++i) {
        char c = lower[i];
        bool up = digest[i] >= '8' && c >= 'a';  // int(digest[i], 16) >= 8 is '8'..'9' or 'a'..'f'; a digit has no case
        out += up ? static_cast<char>(c - 'a' + 'A') : c;
    }
    return out;
}

bool is_checksum_address(const Json& address) {
    Result<std::string> checksummed = to_checksum_address(address);  // fails only with a ValueError
    return checksummed.ok() && checksummed.value() == address.as_str();
}

Result<std::string> normalize_address(const Json& address) {
    SCFG_TRY(std::string checksummed, to_checksum_address(address));
    std::string_view body = std::string_view(address.as_str()).substr(2);  // a str of 40 hex digits by now
    if (body == py::lower_ascii(body) || body == detail::upper_ascii(body)) return checksummed;
    if (checksummed != address.as_str()) return Err::value_error("bad EIP-55 checksum: " + address.as_str());
    return checksummed;
}

Result<std::string> address_topic(const Json& address) {
    SCFG_TRY(std::string checksummed, to_checksum_address(address));
    return "0x" + std::string(24, '0') + py::lower_ascii(std::string_view(checksummed).substr(2));
}

transfers::Transfer TransferLog::to_transfer() const {
    return transfers::Transfer{token, Json(sender), recipient, value, Json(tx_hash), Json(log_index), block_number};
}

Result<std::optional<TransferLog>> parse_transfer_log(const Json& log) {
    if (!log.is_dict()) {
        return Err::make("AttributeError", "'" + detail::type_name(log) + "' object has no attribute 'get'");
    }
    const Json::Dict& d = log.as_dict();
    Json topics = d.get("topics");
    if (!py::truthy(topics)) topics = Json::list();  // log.get("topics") or []
    if (py::truthy(d.get("removed"))) return std::nullopt;
    SCFG_TRY(size_t count, detail::py_len(topics));
    if (count != 3) return std::nullopt;
    // topics[0].lower() != TRANSFER_TOPIC. A str's topics[0] is one character, never the topic; a dict's is a KeyError
    // (its keys are str). So past this point topics is a list.
    if (topics.is_str()) return std::nullopt;
    if (topics.is_dict()) return Err::key_error("0");
    const Json::List& list = topics.as_list();
    SCFG_TRY(std::string topic0, detail::lower_attr(list[0]));
    if (topic0 != TRANSFER_TOPIC) return std::nullopt;  // lower_for_ascii is exact against ASCII text
    Json data = d.get("data");
    if (!data.is_str() || !detail::is_hex_word(data.as_str(), 64)) return std::nullopt;

    // The fields in Python's order, so the first bad one raises what Python raises.
    TransferLog t;
    SCFG_TRY(Json address, py::get(log, "address"));
    SCFG_TRY(t.token, to_checksum_address(address));
    SCFG_TRY(t.sender, detail::topic_to_address(list[1]));
    SCFG_TRY(t.recipient, detail::topic_to_address(list[2]));
    SCFG_TRY(t.value, BigInt::from_hex(std::string_view(data.as_str()).substr(2)));  // int(data, 16) of 64 hex digits
    SCFG_TRY(Json tx_hash, py::get(log, "transactionHash"));
    SCFG_TRY(t.tx_hash, detail::lower_attr(tx_hash));
    SCFG_TRY(Json log_index_text, py::get(log, "logIndex"));
    SCFG_TRY(t.log_index, py::py_int_hex(log_index_text));
    SCFG_TRY(Json block_number_text, py::get(log, "blockNumber"));
    SCFG_TRY(t.block_number, py::py_int_hex(block_number_text));
    SCFG_TRY(Json block_hash, py::get(log, "blockHash"));
    SCFG_TRY(t.block_hash, detail::lower_attr(block_hash));
    return std::optional<TransferLog>(std::move(t));
}

}  // namespace scfg::evm
