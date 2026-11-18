#include "ui_run_status_probe.hpp"
#include "../app/gui/interaction_probe.hpp"

namespace dfy::tests {
namespace {
// 为真实面板任务提供明确的单步计划及读、改、读候选。
// 步骤：返回计划 → 依次给出三个工具请求 → 核对读回并返回完整文本。
model::Response response(unsigned number, const model::Request& request) {
    using namespace model;
    if (number == 1) return {{Role::assistant,R"({"schema_version":1,"steps":[{"id":"mode","objective":"Set focus","completion_criterion":"Read mode","tools":["example.status/read","example.status/set_focus"]}]})"}};
    if (number >= 2 && number <= 4 && request.tools.size() == 2)
        return {{Role::assistant,"",{{std::to_string(number),request.tools[number == 3 ? 1 : 0].name,"{}"}}},FinishReason::tool_calls};
    if (number == 5 && request.messages.back().content.find("output:\nfocus") != std::string::npos)
        return {{Role::assistant,"Fixed status panel task completed"}};
    throw std::runtime_error("UI status test: unexpected model request");
}
}

// 在独占工作区安装并加载实际面板，所有模型请求都走固定替身。
// 步骤：构建离线应用 → 安装和加载工具包 → 交给业务线程。
std::unique_ptr<app::Application> UiRunStatusProbe::open(const std::filesystem::path& work) {
    const model::Config config{"offline-fixed",{},"status-probe",{},1000};
    auto application = std::make_unique<app::Application>(config,work/"plugins",work/"user",model::Backend{
        [this](const auto&,const auto& request) { return response(++requests_,request); }});
    application->plugins().install(package_);
    if (application->plugins().load("example.status") != 0) throw std::runtime_error("UI status test: cannot load panel");
    return application;
}

// 只由固定原生验收步骤提交或批准原待审动作。
// 步骤：收取本地步骤 → 提交原任务或复核查询无副作用 → 登记原候选批准。
void UiRunStatusProbe::drive(app::Application& application) {
    const auto action = action_.exchange(Action::none);
    if (action == Action::submit) {
        application.submit_verified("example.status/read","example.status/set_focus","focus","Set focus");
    } else if (action == Action::approve) {
        const auto pending = application.confirmations();
        if (requests_ != 3 || pending.size() != 1 || pending.front().state != agent::ConfirmationState::waiting)
            throw std::runtime_error("UI status test: reading altered pending work");
        if (application.decide(pending.front().id,pending.front().revision,true).state != agent::ConfirmationState::approved)
            throw std::runtime_error("UI status test: native approval failed");
    }
}
}

namespace dfy::ui {
// 通过真实主文档消息检查同一运行的待审和退出状态。
// 步骤：登记固定读探针 → 读取空闲及待审状态 → 验证拒绝和身份 → 原生批准后核对读回。
void InteractionProbe::start_run_status(std::function<void()> submit, std::function<void()> approve) {
    evaluate("install run query probe",R"JS((()=>{
        const host = window.chrome.webview;
        const replies = new Map();
        let sequence = 0;
        const request = { type:'state.read', version:'1', topic:'run' };
        host.addEventListener('message', ({data}) => {
            if (data?.event === 'state.snapshot') replies.set(data.request,data);
        });
        window.queryRun = async (fields={}) => {
            const id = 'probe-' + (++sequence);
            host.postMessage({...request,request:id,...fields});
            const end = performance.now()+3000;
            while (performance.now()<end) {
                if (replies.has(id)) {
                    const reply = replies.get(id); replies.delete(id); return reply;
                }
                await new Promise(resolve=>setTimeout(resolve,10));
            }
            throw new Error('missing status reply');
        };
        window.waitRun = async (predicate, fields={}) => {
            const end = performance.now()+4000;
            while (performance.now()<end) {
                const reply = await window.queryRun(fields);
                if (predicate(reply.run)) return reply.run;
                await new Promise(resolve=>setTimeout(resolve,20));
            }
            throw new Error('run did not reach expected state');
        };
        window.rejectedRequests = async (run) => {
            host.postMessage({...request,type:'confirmation.approve',request:'write'});
            host.postMessage({...request,version:'2',request:'wrong-version'});
            host.postMessage({...request,request:'missing-turn',session:run.session});
            const reply = await window.queryRun({session:run.session,turn:run.turn});
            return reply.run?.waiting_confirmations === 1 &&
                !replies.has('write') && !replies.has('wrong-version') && !replies.has('missing-turn');
        };
        return true;
    })())JS");
    expect("initial status rendered","document.querySelector('#run-status')?.textContent === '暂无运行'");
    wait_for("submit native status task",[submit = std::move(submit)] { submit(); return true; });
    evaluate("read waiting action",R"JS((async()=>{
        const run = await window.waitRun(run=>run?.waiting_confirmations === 1);
        window.statusTask = {session:run.session,turn:run.turn};
        return typeof run.session === 'string' && typeof run.turn === 'string' &&
            run.result === 'pending' && run.exit === 'pending' && run.verification === 'pending';
    })())JS");
    evaluate("refresh waiting display","document.querySelector('#refresh-run').click(); true");
    expect("waiting status rendered","document.querySelector('#run-status').textContent === '等待审批'");
    evaluate("reject different run identity",R"JS((async()=>{
        const original = window.statusTask;
        const reply = await window.queryRun({session:original.session,turn:String(BigInt(original.turn)+1n)});
        return reply.available && reply.run === null;
    })())JS");
    evaluate("reject writes and malformed requests","window.rejectedRequests(window.statusTask)");
    wait_for("approve through native fixture",[approve = std::move(approve)] { approve(); return true; });
    evaluate("read exited and verified task",R"JS((async()=>{
        const result = await window.waitRun(run=>run?.exit === 'succeeded',window.statusTask);
        return result.session === window.statusTask.session && result.turn === window.statusTask.turn &&
            result.result === 'succeeded' && result.waiting_confirmations === 0 && result.verification === 'satisfied';
    })())JS");
    evaluate("refresh completed display","document.querySelector('#refresh-run').click(); true");
    expect("verified status rendered","document.querySelector('#run-status').textContent === '已完成，读回已核验'");
    next();
}
}
