const fs = require('node:fs/promises');
const path = require('node:path');
const { isDeepStrictEqual } = require('node:util');
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/;

// 检查显式配置的本地只读观察端，按真实世界标识固定来源。
function stoneWitness(config) {
  if (!config) return null;
  if (!path.isAbsolute(config.directory ?? '') || !uuid.test(config.world) || typeof config.dimension !== 'string')
    throw new Error('无效的只读观察端配置');
  const { directory, world, dimension } = config;
  // 只读取观察端登记的本次流名，不能让世界或消息指定路径。
  async function streamName() {
    const name = (await fs.readFile(path.join(directory, 'active-stream'), 'utf8')).trim();
    if (!uuid.test(name)) throw new Error('无效的观察流名');
    return name;
  }
  return {
    // 在实际挖掘前记录最后一个完整服务端事件的偏移与序号。
    async begin(currentDimension) {
      if (currentDimension !== dimension) throw new Error('观察端维度不匹配');
      const stream = await streamName();
      const file = await fs.open(path.join(directory, stream + '.jsonl'), 'r');
      try {
        const { size } = await file.stat();
        if (!size) return { stream, offset: 0, seq: 0 };
        const buf = Buffer.alloc(Math.min(size, 65536));
        const start = size - buf.length;
        const { bytesRead } = await file.read(buf, 0, buf.length, start);
        const end = buf.subarray(0, bytesRead).lastIndexOf(10);
        const previous = end > 0 ? buf.subarray(0, end).lastIndexOf(10) : -1;
        if (end < 0 || (previous < 0 && start > 0)) throw new Error('无法确定起始观察水位');
        const last = JSON.parse(buf.subarray(previous + 1, end).toString());
        if (last.stream !== stream || !Number.isSafeInteger(last.seq) || last.seq < 1) throw new Error('无效水位');
        return { stream, offset: start + end + 1, seq: last.seq };
      } finally { await file.close(); }
    },
    // 从固定边界读取完整行；换流、截断、缺序或过量一律留下未知。
    async read(mark) {
      if (await streamName() !== mark.stream) throw new Error('观察端已换流');
      const file = await fs.open(path.join(directory, mark.stream + '.jsonl'), 'r');
      try {
        const { size } = await file.stat();
        const length = size - mark.offset;
        if (length < 0 || length > 256 * 1024) throw new Error('观察窗口无效');
        const buf = Buffer.alloc(length);
        const { bytesRead } = await file.read(buf, 0, length, mark.offset);
        const raw = buf.subarray(0, bytesRead);
        const end = raw.lastIndexOf(10);
        if (end < 0) return [];
        const rows = raw.subarray(0, end).toString().split(/\r?\n/).map(line => JSON.parse(line));
        if (rows.some((row, i) => row.stream !== mark.stream || row.seq !== mark.seq + i + 1)) throw new Error('观察序号断档');
        return rows.filter(row => row.world === world);
      } finally { await file.close(); }
    },
  };
}

// 从一个原生采掘目标关联真实掉落、合堆和拾取，无法解释的转移保守留空。
// 步骤：对齐双端出生实体 → 跟踪仍含自采单块的地面堆 → 用拾取后库存与余量收紧范围。
function pickupEvidence(rows, actor, target, clientDrops) {
  const bySequence = new Map();
  for (const row of rows) {
    if (bySequence.has(row.seq) && !isDeepStrictEqual(bySequence.get(row.seq), row)) return null;
    bySequence.set(row.seq, row);
  }
  rows = [...bySequence.values()].sort((a, b) => a.seq - b.seq);
  const here = row => row.actor === actor && row.x === target.x && row.y === target.y && row.z === target.z;
  const origins = rows.filter(row => row.kind === 'blockDrop' && here(row));
  if (origins.length !== 1 || origins[0].amount !== 1 ||
    !rows.some(row => row.kind === 'blockBroken' && here(row) && row.candidateAmount === 1)) return null;
  const source = origins[0];
  if (!clientDrops.has(source.networkId) || !rows.some(row => row.kind === 'added' && row.item === source.item &&
    row.networkId === source.networkId && row.amount === 1)) return null;
  let item = source.item, ground = 1, byFish = false, byOthers = false, last = source.seq;
  for (const row of rows) {
    if (!ground || row.seq <= source.seq) continue;
    if (row.kind === 'merge' && (row.item === item || row.target === item)) {
      const after = rows.find(v => v.kind === 'mergeReadback' && v.cause === row.seq);
      if (!after || after.sourceRemaining !== 0 || after.targetRemaining !== row.amount + row.targetAmount ||
        (row.item === item ? row.amount : row.targetAmount) !== ground) return null;
      item = row.target;
      ground = after.targetRemaining;
      last = after.seq;
    }
    if (row.kind === 'pickup' && row.item === item) {
      const after = rows.find(v => v.kind === 'pickupReadback' && v.cause === row.seq && v.actor === row.actor);
      if (!after || after.ground !== row.remaining || after.ground < 0 || after.ground >= ground ||
        after.afterStock - row.beforeStock !== ground - after.ground) return null;
      if (row.actor === actor) byFish = true; else byOthers = true;
      ground = after.ground;
      last = after.seq;
    }
  }
  return { stream: source.stream, first: source.seq, last, lower: byFish && !ground && !byOthers ? 1 : 0,
    upper: byFish ? 1 : 0, onGround: ground > 0 };
}
module.exports = { stoneWitness, pickupEvidence };
