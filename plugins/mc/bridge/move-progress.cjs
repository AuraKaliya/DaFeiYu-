// 在原移动工作被读取时采样脚位、到目标格中心的距离和本次寻路状态。
// 步骤：绑定目标及寻路监听 → 按读回时刻核对位移 → 在原工作退出时保存末次读数并释放监听。
function monitorMove(bot, target, goal, result, now = () => performance.now()) {
  let path = 'unknown', previous, lastMovedAtMs, startDistance;
  const onPath = update => {
    if (bot.pathfinder.goal === goal) path = ['success', 'partial', 'noPath', 'timeout'].includes(update?.status) ? update.status : 'unknown';
  };
  // 只用实际位置确认位移，求解超时和缺少位置均保留各自的状态。
  // 步骤：读取角色脚位 → 比较本次与前次的位移 → 结合路径及抵达证据标注阶段。
  function sample(arrived = false) {
    const sampledAtMs = Math.floor(now());
    const position = bot.entity?.position;
    const targetCell = { x: target.x, y: target.y, z: target.z };
    const state = result.progress = { phase: 'unknown', path, targetCell, sampledAtMs };
    if (!position || ![position.x, position.y, position.z].every(Number.isFinite)) {
      previous = lastMovedAtMs = undefined;
      return;
    }
    state.position = { x: position.x, y: position.y, z: position.z };
    state.distance = Math.hypot(position.x - target.x - 0.5, position.y - target.y, position.z - target.z - 0.5);
    state.vertical = target.y - position.y;
    if (startDistance !== undefined) state.startDistance = startDistance;
    if (!previous) previous = state.position;
    else if (Math.hypot(position.x - previous.x, position.y - previous.y, position.z - previous.z) >= 0.05) {
      previous = state.position;
      lastMovedAtMs = sampledAtMs;
    }
    if (lastMovedAtMs !== undefined) state.lastMovedAtMs = lastMovedAtMs;
    state.phase = arrived ? 'arrived' : path === 'noPath' ? 'no_path' :
      lastMovedAtMs !== undefined && sampledAtMs - lastMovedAtMs <= 500 ? 'moving' : 'waiting';
  }
  bot.on('path_update', onPath);
  sample();
  // 起点只记这一帧，随后找回实体时仍沿用原工作起点。
  startDistance = result.progress.distance;
  if (startDistance !== undefined) result.progress.startDistance = startDistance;
  return { sample,
    // 解除本次寻路监听并保存工作退出时的最后一帧。
    stop(arrived) { bot.removeListener('path_update', onPath); sample(arrived); },
  };
}

module.exports = { monitorMove };
