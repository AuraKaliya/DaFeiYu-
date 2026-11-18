// 只从协议玩家消息取得绑定玩家的原话，显示名或系统广播不能成为授权来源。
// 步骤：按实际世界重置窗口 → 观察绑定身份 → 按游标返回同一世界的有界增量。
function ownerInbox(bot, world, owner, localFixture = false, capacity = 32, classify = () => null) {
  if (owner !== null && (typeof owner !== 'string' ||
    !/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/.test(owner))) throw new Error('无效玩家 UUID');
  if (!Number.isInteger(capacity) || capacity < 1 || capacity > 64) throw new Error('无效消息容量');
  let generation = null;
  let sequence = 0;
  let closed = false;
  const entries = [];

  // 以游戏读回维护会话边界，换世界后原游标和旧消息停止流转。
  function sync() {
    const next = closed ? null : world.read()?.generation ?? null;
    if (generation !== next) {
      generation = next;
      sequence = 0;
      entries.length = 0;
    }
    return generation;
  }
  // 原文和 UUID 均来自协议包，正式网络账号须通过消息签名校验。
  function receive(packet) {
    if (!owner || packet.sender !== owner || packet.sender === bot.player?.uuid ||
      (!localFixture && packet.verified !== true) || typeof packet.plainMessage !== 'string' ||
      !packet.plainMessage.trim() || Buffer.byteLength(packet.plainMessage) > 1024 || !sync()) return;
    if (sequence >= Number.MAX_SAFE_INTEGER) return;
    const entry = { sequence: ++sequence, text: packet.plainMessage };
    const offer = classify(entry.text);
    if (offer) entry.offer = offer;
    entries.push(entry);
    if (entries.length > capacity) entries.shift();
  }
  bot._client.on('playerChat', receive);
  return {
    // 按指定世界游标读取玩家消息，避免把其他绑定或缺失历史拼成完整记录。
    read(expected, after) {
      if (!owner || !sync() || expected !== generation) return null;
      if (!Number.isSafeInteger(after) || after < 0 || after > sequence) throw new Error('无效玩家消息游标');
      const oldest = entries[0]?.sequence ?? 1;
      const messages = [];
      let bytes = 0;
      for (const entry of entries) {
        if (entry.sequence <= after) continue;
        const size = Buffer.byteLength(JSON.stringify(entry));
        if (messages.length === 8 || bytes + size > 8192) break;
        messages.push(structuredClone(entry));
        bytes += size;
      }
      return { generation, owner, oldest, latest: sequence, gap: after < oldest - 1,
        more: (messages.at(-1)?.sequence ?? after) < sequence, messages };
    },
    // 仅在同一游戏来源中取得仍保留的原指令。
    get(expected, number) {
      if (!sync() || expected !== generation) return null;
      const entry = entries.find(item => item.sequence === number);
      return entry ? structuredClone(entry) : null;
    },
    // 撤销监听并归还原话窗口。
    close() { closed = true; bot._client.removeListener('playerChat', receive); sync(); },
  };
}
module.exports = { ownerInbox };
