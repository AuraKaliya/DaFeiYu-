const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { randomUUID } = require('node:crypto');
const { once } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const { openBridge } = require('../../plugins/mc/bridge/pipe.cjs');

// 用独立目录的插件配置把真实 DLL 接到受控世界来源。
// 步骤：签发管道短名并保存用户设置 → 运行原生安装与执行探针 → 核对调用数并关闭。
async function main() {
  const [exe, packagePath, temp] = process.argv.slice(2);
  fs.mkdirSync(temp, { recursive: true });
  const root = fs.mkdtempSync(path.join(temp, 'world-'));
  const config = path.join(root, 'user', 'plugins', 'mc.companion');
  fs.mkdirSync(config, { recursive: true });
  const channel = randomUUID();
  fs.writeFileSync(path.join(config, 'settings.json'), JSON.stringify({ channel }));
  let reads = 0;
  const state = { generation: randomUUID(), actor: randomUUID(), dimension: 'overworld',
    position: { x: -1.5, y: -60, z: 0.5 }, cobblestone: 5, signalAgeMs: 20 };
  const bridge = await openBridge(channel, {
    // 第二次查询模拟原世界已撤销。
    read() { return ++reads === 1 ? state : null; },
    // 替身没有待释放的游戏资源。
    close() {},
  });
  try {
    const child = spawn(exe, [packagePath, root], { stdio: 'inherit', windowsHide: true });
    const [code] = await once(child, 'exit');
    assert.equal(code, 0);
    assert.equal(reads, 2);
  } finally {
    await bridge.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
