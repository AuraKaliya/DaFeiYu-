#include "frame_scope.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::ui;
// 在来源、代次或撤销语义违约时使定向验收失败。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证不同来源和相似字符串不能借用固定离线资源入口。
void origins() {
    check(local_resource(center_url) && local_resource(probe_url(1)),"local assets rejected");
    check(center_document(std::wstring(center_url)+L"#/status") && !center_document(std::wstring(center_url)+L"?other=1"),"main document boundary changed");
    for (const auto uri : {L"https://center.dafeiyu.invalid.evil/index.html",L"http://center.dafeiyu.invalid/index.html",
        L"https://center.dafeiyu.invalid:444/index.html",L"file:///C:/secret",L"https://example.com/",L"data:text/html,hello"})
        check(!local_resource(uri),"unregistered resource origin allowed");
}
// 验证激活、方法授权、导航撤销和销毁后重放，不把前端声明当成宿主身份。
// 步骤：核对未激活 → 激活正确文档 → 拒绝越权和旧代次 → 撤销后拒绝重新导航。
void authorization() {
    FrameScope scope(1);
    check(scope.authorize(scope.url(),"1","probe.echo") == FrameDecision::inactive,"unloaded frame authorized");
    check(scope.navigate(scope.url()),"initial authorized navigation rejected");
    check(scope.authorize(scope.url(),"1","probe.echo") == FrameDecision::allowed,"authorized probe rejected");
    check(scope.authorize(scope.url()+L"#/settings","1","probe.echo") == FrameDecision::allowed,"same-document fragment rejected");
    check(scope.authorize(center_url,"1","probe.echo") == FrameDecision::wrong_source,"parent origin impersonated child");
    check(scope.authorize(scope.url(),"2","probe.echo") == FrameDecision::stale_view,"forged view accepted");
    check(scope.authorize(scope.url(),"1","core.chat.submit") == FrameDecision::forbidden_method,"business method exposed to probe");
    check(!scope.navigate(scope.url()) && scope.authorize(scope.url(),"1","probe.echo") == FrameDecision::inactive,"reload retained old grant");

    FrameScope replacement(2); check(replacement.navigate(replacement.url()),"replacement did not activate");
    check(replacement.authorize(scope.url(),"1","probe.echo") == FrameDecision::wrong_source,"old document entered new instance");
    replacement.revoke(); replacement.revoke();
    check(!replacement.navigate(replacement.url()),"revoked frame reactivated");
    FrameScope unexpected(3); check(!unexpected.navigate(center_url) && !unexpected.navigate(unexpected.url()),"wrong first navigation recovered authority");
}
}
// 验收纯授权规则，不启动浏览器；真实 Frame 接线将在下一单元验证。
int main() {
    try { origins(); authorization(); std::cout << "UI Frame scope checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
