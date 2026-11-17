#include <dafeiyu/digest.hpp>
#include <dafeiyu/tool_catalog.hpp>
#include <dafeiyu/run_options.hpp>
#include <iostream>

namespace {
// 报告公开版本摘要与原字段不一致。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 从正式目录取得经过真实校验的规范 schema 摘要，工厂不执行。
std::string fingerprint(std::string schema) {
    dfy::agent::ToolCatalog catalog;
    catalog.add({"fixture/read",1,1,{"read","fixture",std::move(schema)},false,true},
        [](const auto&) { return dfy::agent::completed_tool({true,dfy::agent::ToolEffect::not_applied,"",dfy::agent::ToolError::none}); });
    const std::vector<std::string> ids{"fixture/read"}; return *catalog.snapshot(ids).descriptions()[0].schema_digest;
}
}
// 核对系统摘要向量及 schema 规范化，不读取模型配置或凭据。
int main() {
    try {
        check(dfy::platform::sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA-256 mismatch");
        check(dfy::platform::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 vector mismatch");
        const auto first = fingerprint(R"({"type":"object","properties":{},"additionalProperties":false})");
        check(first == fingerprint(R"({ "additionalProperties":false,"properties":{},"type":"object" })"),"schema formatting changed fingerprint");
        check(first != fingerprint(R"({"type":"object","properties":{"x":{"type":"integer"}},"additionalProperties":false})"),"schema change hidden");
        dfy::model::Config config{"qwen","PRIVATE_ENDPOINT","fixture","PRIVATE_ENV",1000}; dfy::agent::RunOptions options;
        const auto versions = dfy::agent::public_run_versions(config,options,std::chrono::seconds(1));
        config.base_url = "DIFFERENT_PRIVATE_ENDPOINT"; config.api_key_env = "DIFFERENT_PRIVATE_ENV";
        check(dfy::agent::public_run_versions(config,options,std::chrono::seconds(1)).public_model_config == versions.public_model_config,
            "public digest consumed endpoint or credential reference");
        config.timeout_ms = 2000;
        check(dfy::agent::public_run_versions(config,options,std::chrono::seconds(1)).public_model_config != versions.public_model_config,"public model setting change hidden");
        options.policy.model_limit = 2;
        check(dfy::agent::public_run_versions(config,options,std::chrono::seconds(1)).execution_policy != versions.execution_policy &&
            versions.control_extension == "none" && !versions.application_commit,"policy change hidden or code version fabricated");
        config.timeout_ms = 1000; const auto changed = dfy::agent::public_run_versions(config,options,std::chrono::seconds(1));
        const auto diff = dfy::agent::compare_public_versions(versions,changed);
        check(diff.changed == std::vector<std::string>{"execution_policy"} && !diff.unknown.empty(),"version comparison invented equality or missed public change");
        auto before = versions; before.tools.push_back({"fixture/read",1,10,"fixture-v1",first}); auto after = before; after.tools[0].instance = 20;
        check(dfy::agent::compare_public_versions(before,after).changed.empty(),"runtime generation became software version difference");
        after.tools[0].schema = "different-schema";
        check(dfy::agent::compare_public_versions(before,after).changed == std::vector<std::string>{"tool/fixture/read/schema"},"tool contract difference lost");
        std::cout << "Version fingerprint checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
