#pragma once
#include "store.hpp"
namespace dfy {
class CommandLedger {
    StateStore& store_;
    StateWriter writer_;
public:
    explicit CommandLedger(StateStore& store) : store_(store), writer_(store.claim("commands", {"command"})) {}
    Json receipt(const std::string& id) { return store_.get("command", id); }
    Json execute(const std::string& name, Json args, const std::function<Json(const Json&)>& invoke) {
        const auto key = args.value("_command_id", identifier()); args.erase("_command_id");
        const auto previous = receipt(key);
        if (!previous.is_null()) {
            require(previous.at("name") == name && previous.at("arguments") == args, "COMMAND_ID_CONFLICT");
            return previous.at("result");
        }
        Json result;
        store_.atomic([&] { result = invoke(args); writer_.put("command", key, {{"id", key}, {"name", name}, {"arguments", args}, {"result", result}}); });
        return result;
    }
};
}
