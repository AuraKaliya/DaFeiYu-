#include <dafeiyu/core.hpp>
#ifdef DFY_ABI_VERSION
#error Core public API must not expose the plugin ABI implementation.
#endif
#ifdef SQLITE_VERSION
#error Core public API must not expose storage headers.
#endif
#include <dafeiyu/contracts.hpp>
#include "run_view.hpp"
#include <iostream>

// 验证公开头文件契约和运行视图与内部存储字段的隔离。
// 步骤：检查内置方法模式 → 构造任务状态及公开视图 → 修改内部字段并比较视图 → 校验视图契约。
int main() {
    try {
        using dfy::Json;
        for (const auto& [id, definition] : dfy::sdk::contracts::builtins()) {
            for (const auto& [name, method] : definition.methods.items()) {
                (void)id;
                (void)name;
                dfy::sdk::validate_schema(method.at("input"));
                dfy::sdk::validate_schema(method.at("output"));
            }
        }

        Json state{{"id", "run"}, {"revision", 1}, {"intent_revision", 1}, {"execution", "running"},
            {"phase", "decision"}, {"goal", "uncertain"}, {"delivery", "none"},
            {"input", {{"text", "input"}, {"private_field", "internal"}}}, {"actor", "owner"},
            {"conversation", "conversation"}, {"candidate", nullptr}, {"results", Json::array()}, {"persona", nullptr},
            {"conversation_key", {{"instance", "instance"}, {"channel_instance", "channel"}, {"account", "account"},
                                     {"external_id", "room"}}},
            {"grants", {{"actions", Json::array({"action.add"})}}}, {"model_attempts", 0}, {"strategy_steps", 1},
            {"limits", {{"model_attempts", 8}, {"strategy_steps", 24}, {"nested_calls", 64}}}};
        state["observation"] = {{"kind", "run_ready"}};
        state["error"] = nullptr;
        state["wait_reason"] = nullptr;
        state["active_operation"] = nullptr;
        state["activity"] = "";
        state["goal_origin"] = "none";
        state["bindings"] = {{"bindings", {{"model", "model.scripted"}}}};

        const auto first = dfy::make_run_view(state).to_json();
        state["internal_scheduler"] = {{"unpublished_field", 123}};
        state["input"]["private_field"] = "changed internal input";

        if (first != dfy::make_run_view(state).to_json())
            throw std::runtime_error("storage change leaked to RunView");
        dfy::sdk::validate_value(dfy::sdk::contracts::run_view_schema(), first);

        std::cout << "Public headers, built-in schemas and storage-independent RunView passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
