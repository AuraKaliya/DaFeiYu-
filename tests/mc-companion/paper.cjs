const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const { createInterface } = require('node:readline');
const { until } = require('./players.cjs');

// 只选用显式准备的回环测试服，沿用其世界，不创建或接受许可。
// 步骤：定位指定测试目录 → 核对监听及身份配置 → 返回运行位置。
function fixturePaths() {
  const root = process.env.DFY_MC_TEST_ROOT;
  if (!root || !path.isAbsolute(root)) throw new Error('请指定独立的 DFY_MC_TEST_ROOT');
  const server = path.join(root, 'servers', 'stone-1.21.4');
  const properties = fs.readFileSync(path.join(server, 'server.properties'), 'utf8');
  const settings = new Map();
  for (const line of properties.split(/\r?\n/)) {
    if (!line || line.startsWith('#')) continue;
    const match = /^([a-z0-9.-]+)=(.*)$/.exec(line);
    if (!match || line.endsWith('\\') || settings.has(match[1])) throw new Error('测试配置不是唯一的单行键值');
    settings.set(match[1], match[2]);
  }
  for (const [key, value] of Object.entries({ 'server-ip': '127.0.0.1', 'server-port': '25579',
    'online-mode': 'false', 'enable-rcon': 'false', 'enable-query': 'false', 'level-name': 'fixture-world' })) {
    if (settings.get(key) !== value) throw new Error('隔离测试服配置不符：' + key);
  }
  return { root, server };
}

// 启动并独占本次服务器进程，向夹具提供本地控制台与独立证据目录。
// 步骤：打开日志并等待就绪 → 执行协议探针 → 请求正常停服并核对实际退出。
async function withPaper(probe) {
  const { root, server } = fixturePaths();
  const java = process.env.DFY_MC_JAVA;
  const jar = process.env.DFY_MC_PAPER_JAR;
  if (!java || !jar || !path.isAbsolute(java) || !path.isAbsolute(jar))
    throw new Error('请指定 DFY_MC_JAVA 和 DFY_MC_PAPER_JAR 的绝对路径');
  const evidence = fs.mkdtempSync(path.join(root, 'evidence', 'run-'));
  const log = fs.openSync(path.join(evidence, 'console.log'), 'wx');
  const child = spawn(java, ['-Xms256m', '-Xmx1G', '-jar', jar, '--nogui'],
    { cwd: server, windowsHide: true, stdio: 'pipe' });
  const lines = createInterface({ input: child.stdout });
  let ready = false;
  let closed = false;
  let startupError;
  // 仅识别本进程的启动记录，输出仍完整保留供复核。
  lines.on('line', line => {
    fs.writeSync(log, line + '\n');
    if (/\]: Done \([\d.]+s\)!/.test(line)) ready = true;
  });
  child.stderr.on('data', chunk => fs.writeSync(log, chunk));
  child.stdin.on('error', error => { startupError = error; });
  child.once('error', error => { startupError = error; });
  child.once('close', () => { closed = true; });

  try {
    await until(() => {
      if (startupError) throw startupError;
      if (closed) throw new Error('服务未就绪，详见 ' + evidence);
      return ready;
    }, '本地 Paper 就绪', 90000);
    const send = line => child.stdin.write(line + '\n');
    send('gamerule spawnRadius 0');
    send('setworldspawn 0 -60 0');
    return await probe({ send, port: 25579, evidence, server });
  } finally {
    try {
      if (!closed) {
        if (child.stdin.writable) child.stdin.write('stop\n');
        try {
          await until(() => closed, 'Paper 正常退出', 45000);
        } catch (error) {
          child.kill();
          await until(() => closed, '本次夹具进程退出');
          throw error;
        }
      }
      if (ready && child.exitCode !== 0) throw new Error('Paper 收尾失败：' + evidence);
    } finally {
      lines.close();
      fs.closeSync(log);
      console.log('证据目录：' + evidence);
    }
  }
}

module.exports = { withPaper };
