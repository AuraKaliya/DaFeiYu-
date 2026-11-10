#pragma once
#include "store.hpp"

namespace dfy {
class CommandLedger {
    StateStore& store_;
    StateWriter writer_;

public:
    // 连接状态库并领取宿主命令回执的写权限。
    explicit CommandLedger(StateStore& store) : store_(store), writer_(store.claim("commands", {"command"})) {}

    // 读取指定命令标识对应的持久化回执。
    Json receipt(const std::string& id) { return store_.get("command", id); }

    // 执行宿主变更命令，并复用同一命令标识对应的持久化结果。
    // 步骤：提取命令标识 → 核对并复用已有回执 → 在事务内执行命令并保存结果。
    Json execute(const std::string& name, Json args, const std::function<Json(const Json&)>& invoke) {
        const auto key = args.value("_command_id", identifier());
        args.erase("_command_id");

        const auto previous = receipt(key);
        if (!previous.is_null()) {
            require(previous.at("name") == name && previous.at("arguments") == args, "COMMAND_ID_CONFLICT");
            return previous.at("result");
        }

        Json result;
        store_.atomic([&] {
            result = invoke(args);
            writer_.put("command", key, {{"id", key}, {"name", name}, {"arguments", args}, {"result", result}});
        });
        return result;
    }
};
}
