#include "resident.hpp"
#include "local_pipe.hpp"
#include "protocol.hpp"
#include <dafeiyu/digest.hpp>
#include <nlohmann/json.hpp>
#include <random>

namespace dfy::management {
namespace {
using Json = nlohmann::json;

// 在相同的用户管道内区分管理命令与业务消息，失效的客户端须重新建立观察身份。
// 步骤：核对版本与操作 → 处理连接、查询或退出 → 将有效客户端的业务请求交给统一协议。
Json handle(Host& host, const std::string& identity, const std::string& instance, bool& stopping, const Json& request) {
    if (!request.is_object() || request.at("v")!=1) throw std::invalid_argument("invalid protocol");
    const auto& op = request.at("op").get_ref<const std::string&>();
    if (op=="status" && request.size()==2)
        return {{"state",stopping ? "stopping" : host.dialogue().display ? "running" : "starting"},{"instance",instance},{"definition",identity}};
    if (op=="stop" && request.size()==3 && request.at("instance")==instance) {
        stopping = true;
        host.close();
        return {{"state","stopping"},{"instance",instance}};
    }
    if (stopping) throw std::runtime_error("host is stopping");
    if (op=="connect" && request.size()==3) {
        if (request.at("definition")!=identity) throw std::invalid_argument("host configuration differs");
        return {{"client",host.open_client()},{"instance",instance}};
    }

    const auto& client = request.at("client").get_ref<const std::string&>();
    if (!host.visit_client(client)) throw std::invalid_argument("client expired");
    if (op=="disconnect" && request.size()==3) {
        host.close_client(client);
        return {{"state","disconnected"}};
    }
    if (op=="request" && request.size()==4) return dispatch(&host,request.at("body"),client);
    throw std::invalid_argument("unknown operation");
}
}

// 规范化启动路径并计算标识，配置内容和凭据不进入握手。
std::string startup_identity(const std::filesystem::path& model, const std::filesystem::path& packages,
    const std::filesystem::path& trace) {
    Json paths = Json::array();
    for (const auto& path : {model,packages,trace}) {
        const auto value = path.empty() ? std::u8string{} : std::filesystem::weakly_canonical(std::filesystem::absolute(path)).generic_u8string();
        paths.push_back(std::string(value.begin(),value.end()));
    }
    return platform::sha256(paths.dump());
}

// 先取得端点再创建应用，退出期间继续回答状态查询并等待工作实际结束。
// 步骤：取得用户目录所有权 → 建立宿主与本次实例号 → 接纳有界交换 → 返回实际收尾结果。
int serve_resident(const std::filesystem::path& profile, const std::string& identity, Host::Open open) {
    LocalPipe pipe(profile);
    std::random_device random;
    const auto instance = std::to_string(random())+"-"+std::to_string(random())+"-"+std::to_string(random());
    Host host(std::move(open));
    bool stopping = false;
    while (!host.exited()) {
        pipe.serve_one(std::chrono::milliseconds(100),[&](const std::string& input) {
            try { return Json{{"v",1},{"result",handle(host,identity,instance,stopping,Json::parse(input))}}.dump(); }
            catch (const Json::exception&) { return Json{{"v",1},{"error","invalid management message"}}.dump(); }
            catch (const std::invalid_argument& error) { return Json{{"v",1},{"error",error.what()}}.dump(); }
            catch (const std::runtime_error&) { return Json{{"v",1},{"error","management operation unavailable"}}.dump(); }
        });
    }
    return host.failed() ? 1 : 0;
}

// 在独立连接内交换一条请求，显式校验回包版本和业务拒绝。
nlohmann::json resident_request(const std::filesystem::path& profile, Json request) {
    request["v"] = 1;
    auto response = Json::parse(local_request(profile,request.dump()));
    if (response.at("v")!=1) throw std::runtime_error("management: incompatible reply");
    if (response.contains("error")) throw std::runtime_error("management: "+response.at("error").get<std::string>());
    return std::move(response.at("result"));
}
}
