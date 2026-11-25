const targetMs = 5000;

// 按该固定场景预先确定的有效答复，判定玩家端是否在五秒内收到反馈。
// 步骤：核对原话时点 → 找到同一采样窗口内的首条有效回复 → 按已覆盖的时段判定结果。
function firstUsefulFeedback(sample, usefulText) {
  if (!Number.isFinite(sample.sentAtMs) || !Array.isArray(sample.replies)) return { status: 'unconfirmed' };
  const first = sample.replies.find(reply => reply.text === usefulText &&
    Number.isFinite(reply.atMs) && reply.atMs >= sample.sentAtMs);
  if (first) {
    const elapsedMs = first.atMs - sample.sentAtMs;
    return { status: elapsedMs <= targetMs ? 'on_time' : 'late', elapsedMs };
  }
  if (Number.isFinite(sample.observedThroughMs) && sample.observedThroughMs - sample.sentAtMs > targetMs)
    return { status: 'late' };
  return { status: 'unconfirmed' };
}

// 由同一协议玩家记录一次发话及其后的伙伴私聊，在断线或等待退出时结束采样。
// 步骤：订阅实际收件和断线 → 发出原话并等待场景回执 → 核对约定文本或现场读数对应的答复 → 保存窗口和判定。
async function measurePlayerFeedback(player, message, usefulText, wait, save, now = () => performance.now()) {
  const sample = { message, clock: 'protocol_player_monotonic_ms', replies: [] };
  const heard = (sender, text) => {
    if (sender === 'DfyCompanion' && sample.observedThroughMs === undefined)
      sample.replies.push({ text, atMs: now() });
  };
  const ended = () => { sample.observedThroughMs ??= now(); };
  player.on('whisper', heard);
  player.once('end', ended);
  try {
    const sentAtMs = now();
    player.chat(message);
    sample.sentAtMs = sentAtMs;
    await wait();
  } finally {
    ended();
    player.removeListener('whisper', heard);
    player.removeListener('end', ended);
    sample.usefulText = typeof usefulText === 'function' ? usefulText() : usefulText;
    save({ ...sample, ...firstUsefulFeedback(sample, sample.usefulText) });
  }
}

module.exports = { firstUsefulFeedback, measurePlayerFeedback };
