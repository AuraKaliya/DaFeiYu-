#include "../src/interaction_probe.hpp"
#include <fstream>

namespace dfy::ui {
// 窗口独立选择实际包，由夹具控制端在两窗共同采样后重载原插件。
// 步骤：读取原目录并选包 → 保存本窗读数 → 核对同一后台的新修订及新代次。
void InteractionProbe::start_plugin_windows(std::wstring_view mode, const std::filesystem::path& data) {
    const bool manager = mode==L"plugins-manage";
    expect("original resident handshake","document.querySelector('#dialogue-state')?.dataset.ready==='true'");
    evaluate("open the native plugin directory",R"JS((()=>{
        window.pane=()=>document.querySelector('.plugin-settings');
        window.chosen=()=>document.querySelector('[data-selected-package]');
        window.chrome.webview.addEventListener('message',({data})=>{
            if(data.event==='plugins.directory' && data.state==='ready') window.observedPackages=data;
        });
        document.querySelector('#board-settings').click(); return true;
    })())JS");
    expect("both packages come from the real resident","pane()?.dataset.directoryStatus==='ready' && "
        "document.querySelector('[data-package=\"test.mail\"]')?.textContent.includes('已加载') && "
        "document.querySelector('[data-package=\"test.second\"]')?.textContent.includes('已安装') && !chosen()");
    const auto target = manager ? "test.mail" : "test.second";
    evaluate("explicit selection in this window","window.packageChoice="+Json(target).dump()+
        "; document.querySelector('[data-package=\"'+packageChoice+'\"]').click(); true");
    expect("selection comes from this window","chosen()?.dataset.selectedPackage===packageChoice");
    evaluate("remember original resident, revision and active generation",R"JS((()=>{
        const sample=window.observedPackages;
        window.originalDirectory=[pane().dataset.directoryInstance,pane().dataset.directoryRevision,sample.packages.find(p=>p.id==='test.mail').generation];
        return originalDirectory[0]===sample.instance && originalDirectory[1]===sample.revision && originalDirectory[2]!=='0';
    })())JS");
    call("write each original sample",L"Runtime.evaluate",{{"expression","originalDirectory"},{"returnByValue",true}},
        [file=data/(manager ? "plugins-manage.json" : "plugins-watch.json")](const Json& reply) {
            std::ofstream(file) << reply.at("result").at("value").dump();
        });
    wait_for("control process reloaded the original generation",[file=data/"plugin-reloaded"] { return std::filesystem::is_regular_file(file); });
    evaluate("request a fresh original directory","document.querySelector('#refresh-plugins').click(); true");
    expect("both windows read the new generation on their original resident","pane()?.dataset.directoryStatus==='ready' && "
        "pane().dataset.directoryInstance===originalDirectory[0] && pane().dataset.directoryRevision!==originalDirectory[1] && "
        "observedPackages.packages.some(p=>p.id==='test.mail' && p.loaded && p.generation!=='0' && p.generation!==originalDirectory[2]) && "
        "chosen()?.dataset.selectedPackage===packageChoice && chosen().dataset.selectedGeneration===observedPackages.packages.find(p=>p.id===packageChoice).generation");
    next();
}
}
