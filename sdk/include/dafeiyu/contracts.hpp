#pragma once
#include <dafeiyu/run_view.hpp>
#include <dafeiyu/schema.hpp>
#include <map>

namespace dfy::sdk::contracts {
inline Json object(Json properties = Json::object(), Json required = Json::array(), bool additional = false) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", additional}};
}
inline Json array(Json items = Json::object()) { return {{"type", "array"}, {"items", std::move(items)}}; }
inline Json method(Json input, Json output, std::string effects = "read", std::string resource = "instance") {
    return {{"input", std::move(input)}, {"output", std::move(output)}, {"effects", std::move(effects)}, {"resource", std::move(resource)}};
}
inline Json run_view_schema() {
    const Json text{{"type", "string"}}, integer{{"type", "integer"}, {"minimum", 0}};
    const auto input = object({{"text", text}, {"actor", text}, {"conversation", text}}, Json::array({"text", "actor", "conversation"}));
    const auto counter = object({{"used", integer}, {"limit", integer}}, Json::array({"used", "limit"}));
    const auto conversation = object({{"instance", text}, {"channel_instance", text}, {"account", text}, {"external_id", text}},
        Json::array({"instance", "channel_instance", "account", "external_id"}));
    return object({{"schema_version", {{"type", "integer"}, {"enum", Json::array({RunView::schema_version})}}},
        {"id", text}, {"revision", integer}, {"intent_revision", integer}, {"execution", text}, {"phase", text}, {"goal", text}, {"delivery", text},
        {"input", input}, {"conversation_key", conversation}, {"candidate", Json::object()}, {"results", array()}, {"persona", Json::object()}, {"admitted_actions", array(text)},
        {"observation", object(Json::object(), Json::array(), true)}, {"model_bindings", array(text)}, {"error", Json::object()}, {"wait_reason", Json::object()}, {"active_operation", Json::object()}, {"activity_id", text}, {"goal_origin", text},
        {"budget", object({{"model_attempts", counter}, {"strategy_steps", counter}, {"nested_calls", counter}}, Json::array({"model_attempts", "strategy_steps", "nested_calls"}))}},
        Json::array({"schema_version", "id", "revision", "intent_revision", "execution", "phase", "goal", "delivery", "input", "conversation_key", "candidate", "results", "persona", "admitted_actions", "budget", "observation", "model_bindings", "error", "wait_reason", "active_operation", "activity_id", "goal_origin"}));
}
struct Definition { int version; Json methods; };
inline const std::map<std::string, Definition>& builtins() {
    static const auto definitions = [] {
        const Json text{{"type", "string"}}, integer{{"type", "integer"}, {"minimum", 1}}, boolean{{"type", "boolean"}};
        const auto record = object(Json::object(), Json::array(), true);
        const auto items = object({{"items", array()}}, Json::array({"items"}));
        const auto persona = object({{"instance_id", text}, {"revision", integer}, {"persona", text}, {"sources", array()}},
            Json::array({"instance_id", "revision", "persona", "sources"}));
        const auto strategy_request = object({{"decision_id", text}, {"trigger", text}, {"run", run_view_schema()}, {"state", record}},
            Json::array({"decision_id", "trigger", "run", "state"}));
        auto review_request = strategy_request;
        review_request["properties"]["candidate"] = record;
        review_request["required"].push_back("candidate");
        const auto strategy_result = object({{"proposal", record}, {"accept", boolean}, {"state", record}}, Json::array({"state"}));
        const auto receipt = object({{"id", text}, {"status", {{"type", "string"}, {"enum", Json::array({"sent", "not-sent", "unknown"})}}},
            {"receipt_id", text}}, Json::array({"id", "status"}));
        const auto delivery_input = object({{"id", text}, {"target", text}, {"text", text}, {"intent_revision", integer}, {"binding", record}},
            Json::array({"id", "target", "text", "intent_revision", "binding"}));
        auto deliver = method(delivery_input, receipt, "external-write", "channel"); deliver["query"] = "query_delivery";
        return std::map<std::string, Definition>{
            {"dafeiyu.model", {2, {{"generate", method(record, object({{"events", array()}}, Json::array({"events"})))}}}},
            {"dafeiyu.strategy", {4, {{"propose", method(strategy_request, strategy_result)}, {"review_finish", method(review_request, strategy_result)}}}},
            {"dafeiyu.channel", {3, {{"deliver", deliver}, {"query_delivery", method(record, receipt)}}}},
            {"dafeiyu.memory", {2, {{"snapshot", method(record, persona)}, {"validate", method(persona, object({{"valid", boolean}}, Json::array({"valid"})))},
                {"retrieve", method(record, items)}}}},
            {"dafeiyu.context", {2, {{"collect", method(record, items)}}}},
            {"dafeiyu.activity", {3, {{"evaluate_trigger", method(record, record)}, {"on_settlement", method(object({{"activity_id", text}, {"run", run_view_schema()}}, Json::array({"activity_id", "run"})), record)}}}},
            {"dafeiyu.action-executor", {1, {{"execute", method(object({{"action_id", text}, {"definition_revision", integer}, {"arguments", record}, {"operation_id", text}},
                Json::array({"action_id", "definition_revision", "arguments", "operation_id"})), record)}}}}
        };
    }();
    return definitions;
}
// Built-in contracts have one SDK-owned definition; domain contracts remain open.
inline Json resolve_service(Json service) {
    const auto found = builtins().find(service.at("contract").get<std::string>());
    if (found == builtins().end()) return service;
    if (service.at("version") != found->second.version) throw std::runtime_error("BUILTIN_CONTRACT_VERSION");
    auto methods = found->second.methods;
    if (service.at("contract") == "dafeiyu.action-executor") {
        const auto record = object(Json::object(), Json::array(), true);
        for (const auto& capability : service.value("capabilities", Json::array())) {
            if (capability == "readiness") methods["readiness"] = method(methods.at("execute").at("input"), object({{"available", {{"type", "boolean"}}}}, Json::array({"available"})));
            else if (capability == "query") methods["query_operation"] = method(methods.at("execute").at("input"), record);
            else if (capability == "jobs") for (const auto* name : {"query_job", "reattach_job", "cancel_job"}) methods[name] = method(record, record);
            else throw std::runtime_error("ACTION_CAPABILITY_UNSUPPORTED");
        }
    }
    if (service.contains("methods") && service.at("methods") != methods) throw std::runtime_error("BUILTIN_CONTRACT_METHODS");
    service["methods"] = methods;
    return service;
}
inline void validate_role(std::string_view role, const Json& service) {
    std::string contract;
    if (role == "model" || role == "strategy" || role == "channel" || role == "memory") contract = "dafeiyu." + std::string(role);
    else if (role.starts_with("model.")) contract = "dafeiyu.model";
    else if (role.starts_with("context.")) contract = "dafeiyu.context";
    else if (role == "activity") contract = "dafeiyu.activity";
    else return;
    const auto& expected = builtins().at(contract);
    if (service.at("contract") != contract || service.at("version") != expected.version || service.at("methods") != expected.methods)
        throw std::runtime_error("BINDING_CONTRACT_MISMATCH: " + std::string(role));
}
}
