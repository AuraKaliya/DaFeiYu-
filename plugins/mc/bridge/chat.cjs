// 复制收到聊天时客户端已知的玩家位置，保留未跟踪实体的情况。
function sceneAtMessage(bot, owner, dimension) {
  const entity = Object.values(bot.players).find(player => player.uuid === owner)?.entity;
  const position = entity?.position;
  const known = position && [position.x, position.y, position.z].every(Number.isFinite);
  const yaw = Number.isFinite(entity?.yaw) ? entity.yaw : entity?.headYaw;
  const facing = known && [yaw, entity.pitch].every(Number.isFinite);
  const height = entity?.eyeHeight;
  return { dimension, sampledAtMs: Math.floor(performance.now()),
    position: known ? { x: position.x, y: position.y, z: position.z } : null,
    ...(facing && { facing: { yaw, pitch: entity.pitch } }),
    ...(facing && height > 0 && height <= 2 && { eye: { x: position.x, y: position.y + height, z: position.z } }) };
}

// 只从协议玩家消息取得绑定玩家的原话，显示名或系统广播不能成为授权来源。
// 步骤：按实际世界重置窗口 → 观察绑定身份 → 按游标返回同一世界的有界增量。
function ownerInbox(bot, world, owner, localFixture = false, capacity = 32, classify = () => null, withPresence = false) {
  if (owner !== null && (typeof owner !== 'string' ||
    !/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/.test(owner))) throw new Error('无效玩家 UUID');
  if (!Number.isInteger(capacity) || capacity < 1 || capacity > 64) throw new Error('无效消息容量');
  let generation = null;
  let sequence = 0;
  let closed = false;
  const entries = [];
  let presence = null, trackingPresence = false, lastDeparture = 0;

  // 同一游戏代次内所有事实共享一个有界游标。
  function append(fact) {
    if (sequence >= Number.MAX_SAFE_INTEGER) return null;
    const entry = { sequence: ++sequence, ...fact };
    entries.push(entry);
    if (entries.length > capacity) entries.shift();
    return entry;
  }
  // 记录绑定玩家的实际在场变化，重复名单通知复用原事实。
  function recordPresence(type) {
    if (presence?.type !== type) presence = append({ type }) ?? presence;
    if (presence?.type === 'player.left') lastDeparture = presence.sequence;
  }

  // 以游戏读回维护会话边界，换世界后原游标和旧消息停止流转。
  function sync(snapshot = closed ? null : world.read()) {
    const next = snapshot?.generation ?? null;
    if (generation !== next) {
      generation = next;
      sequence = 0;
      entries.length = 0;
      presence = null;
      trackingPresence = false;
      lastDeparture = 0;
    }
    return generation;
  }
  // 原文和 UUID 均来自协议包，正式网络账号须通过消息签名校验。
  function receive(packet) {
    if (!owner || packet.sender !== owner || packet.sender === bot.player?.uuid ||
      (!localFixture && packet.verified !== true) || typeof packet.plainMessage !== 'string' ||
      !packet.plainMessage.trim() || Buffer.byteLength(packet.plainMessage) > 1024) return;
    const snapshot = closed ? null : world.read();
    if (!sync(snapshot) || (trackingPresence && presence?.type !== 'player.entered')) return;
    if (sequence >= Number.MAX_SAFE_INTEGER) return;
    const offer = classify(packet.plainMessage, generation, sequence + 1);
    append({ text: packet.plainMessage, ...(withPresence && {
      type: 'player.message', sceneAtMessage: sceneAtMessage(bot, owner, snapshot.dimension),
    }), ...(offer && { offer }) });
  }
  bot._client.on('playerChat', receive);
  const joined = player => { if (player.uuid === owner && sync() && trackingPresence) recordPresence('player.entered'); };
  const left = player => { if (player.uuid === owner && sync() && trackingPresence) recordPresence('player.left'); };
  if (withPresence) { bot.on('playerJoined', joined); bot.on('playerLeft', left); }
  return {
    // 按指定世界游标读取玩家消息，避免把其他绑定或缺失历史拼成完整记录。
    read(expected, after) {
      if (!owner || !sync() || expected !== generation) return null;
      if (!Number.isSafeInteger(after) || after < 0 || after > sequence) throw new Error('无效玩家消息游标');
      if (withPresence && !trackingPresence) {
        trackingPresence = true;
        if (Object.values(bot.players).some(p => p.uuid === owner)) recordPresence('player.entered');
      }
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
        more: (messages.at(-1)?.sequence ?? after) < sequence, messages,
        ...(withPresence && { lastDeparture }), ...(presence && { presence: { ...presence } }) };
    },
    // 工作只能在原玩家在场周期内继续执行。
    inPresence(expected, order) {
      return sync() === expected && (!withPresence || (presence?.type === 'player.entered' && order >= presence.sequence));
    },
    // 仅在同一游戏来源中取得仍保留的原指令。
    get(expected, number) {
      if (!sync() || expected !== generation) return null;
      const entry = entries.find(item => item.sequence === number);
      return entry ? structuredClone(entry) : null;
    },
    // 撤销监听并归还原话窗口。
    close() {
      closed = true;
      bot._client.removeListener('playerChat', receive);
      bot.removeListener('playerJoined', joined);
      bot.removeListener('playerLeft', left);
      sync();
    },
  };
}
// 只向仍在原游戏中的绑定玩家私聊，聊天失败不改变已经发生的游戏事实。
function tellOwner(bot, owner, text) {
  const player = Object.values(bot.players).find(entry => entry.uuid === owner);
  if (!player || !/^[A-Za-z0-9_]{1,16}$/.test(player.username)) return false;
  try { bot.whisper(player.username, text); return true; }
  catch { return false; }
}
module.exports = { ownerInbox, tellOwner };
