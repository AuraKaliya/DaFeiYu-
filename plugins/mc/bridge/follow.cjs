const { setTimeout: delay } = require('node:timers/promises');
const { dryStand, SurfaceFollowGoal, monitorSurface } = require('./navigation.cjs');

// 在同一跟随工作内等待玩家重现或可行路线，保留同一动态目标。
// 步骤：采样当前位置 → 跟随或按新证据探路 → 回传进展 → 按原工作退出。
async function followPlayer(bot, owner, range, check, result, probe, probeFirst) {
  if (!Number.isInteger(range) || range < 2 || range > 5) throw new Error('跟随距离须为 2 至 5 格');
  const surface = monitorSurface(bot, result);
  let goal, playerPosition, lastPosition, progressSince, lastProbe, nextProbe = 0;
  let retried = false, blocked = probeFirst, terrainRevision = 0, path = 'unknown';
  const stop = () => {
    if (!goal) return;
    goal = undefined;
    bot.pathfinder.setGoal(null);
    bot.clearControlStates();
  };
  const recordPath = update => {
    if (goal) path = pathStatus(update.status);
  };
  // 只使角色或玩家附近的世界变化成为新探路的依据。
  const changed = (before, after) => {
    const point = after?.position ?? before?.position;
    if (point && (point.distanceTo(bot.entity.position) <= 16 || (playerPosition && point.distanceTo(playerPosition) <= 16)))
      ++terrainRevision;
  };
  bot.on('path_update', recordPath);
  bot.on('blockUpdate', changed);
  try {
    for (;;) {
      check();
      const now = performance.now();
      const entity = Object.values(bot.players).find(player => player.uuid === owner)?.entity;
      const position = bot.entity.position;
      const terrain = surface.sample();
      playerPosition = entity?.position;
      if (!playerPosition) {
        stop();
        blocked = true;
        lastProbe = undefined;
        result.progress = { phase: 'waiting_player', terrain, sampledAtMs: Math.floor(now) };
      } else {
        const distance = position.distanceTo(playerPosition);
        const nearby = distance <= range + 0.5 && (!dryStand(bot, playerPosition) ||
          (!bot.entity.isInWater && dryStand(bot, position)));
        const evidence = `${position.floored()}:${playerPosition.floored()}:${terrainRevision}`;
        if (blocked && (nearby || (evidence !== lastProbe && now >= nextProbe))) {
          lastProbe = evidence;
          nextProbe = now + 500;
          path = nearby ? 'reachable' : pathStatus(probe(position, new SurfaceFollowGoal(bot, entity, range)));
          blocked = path !== 'reachable';
        }
        if (!blocked) {
          const movedGoal = goal && distance > range + 1 && goal.isEnd(position.floored()) &&
            (goal.x !== Math.floor(playerPosition.x) || goal.y !== Math.floor(playerPosition.y) || goal.z !== Math.floor(playerPosition.z));
          if (goal?.entity !== entity || movedGoal) {
            goal = new SurfaceFollowGoal(bot, entity, range);
            bot.pathfinder.setGoal(goal, true);
            lastPosition = position.clone();
            progressSince = now;
            retried = false;
          }
          if (nearby || position.distanceTo(lastPosition) >= 0.55) {
            lastPosition = position.clone();
            progressSince = now;
            retried = false;
          }
          if (!nearby && now - progressSince >= 3500) {
            if (retried) {
              blocked = true;
              lastProbe = evidence;
              stop();
            } else {
              goal = new SurfaceFollowGoal(bot, entity, range);
              bot.pathfinder.setGoal(goal, true);
              progressSince = now;
              retried = true;
            }
          }
        }
        result.progress = { phase: blocked ? 'blocked' : nearby ? 'in_range' : retried ? 'replanning' : 'following',
          terrain, distance: Math.round(distance * 10) / 10, vertical: Math.round((playerPosition.y - position.y) * 10) / 10,
          path: nearby ? 'reachable' : path, sampledAtMs: Math.floor(now) };
      }
      await delay(125);
    }
  } finally {
    bot.removeListener('path_update', recordPath);
    bot.removeListener('blockUpdate', changed);
    surface.stop();
    stop();
    result.position = bot.entity?.position?.clone() ?? null;
  }
}

// 保留寻路器已验证的成功、明确无路和未得出结论三种结果。
function pathStatus(status) {
  return status === 'success' ? 'reachable' : status === 'noPath' ? 'unreachable' : 'unknown';
}

module.exports = { followPlayer };
