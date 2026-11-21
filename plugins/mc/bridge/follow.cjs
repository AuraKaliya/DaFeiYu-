const { setTimeout: delay } = require('node:timers/promises');
const { dryStand, SurfaceFollowGoal, monitorSurface } = require('./navigation.cjs');

// 追踪绑定玩家的实体，只有失去位置或持续没有进展才结束这段跟随。
// 步骤：跟踪实体或等待重现 → 更新动态目标并检查位移 → 有限重算或归还角色控制。
async function followPlayer(bot, owner, range, check, result) {
  if (!Number.isInteger(range) || range < 2 || range > 5) throw new Error('跟随距离须为 2 至 5 格');
  const surface = monitorSurface(bot, result);
  let goal, lastPosition, missingSince = null, progressSince = performance.now();
  let retried = false;
  try {
    for (;;) {
      check();
      const now = performance.now();
      const entity = Object.values(bot.players).find(player => player.uuid === owner)?.entity;
      const position = bot.entity.position;
      const terrain = surface.sample();
      if (!entity?.position) {
        if (goal) {
          bot.pathfinder.setGoal(null);
          bot.clearControlStates();
          goal = undefined;
        }
        missingSince ??= now;
        result.progress = { phase: 'waiting_player', terrain };
        if (now - missingSince >= 5000) throw new Error('一段时间内未重新看到绑定玩家');
      } else {
        if (goal?.entity !== entity) {
          goal = new SurfaceFollowGoal(bot, entity, range);
          bot.pathfinder.setGoal(goal, true);
          lastPosition = position.clone();
          progressSince = now;
          retried = false;
        }
        missingSince = null;
        const distance = position.distanceTo(entity.position);
        const nearby = distance <= range + 0.5 && (!dryStand(bot, entity.position) ||
          (!bot.entity.isInWater && dryStand(bot, position)));
        // 路径仍认为自己在旧目标内时，按实际距离及时追上已经挪动的玩家。
        if (distance > range + 1 && goal.isEnd(position.floored()) &&
          (goal.x !== Math.floor(entity.position.x) || goal.y !== Math.floor(entity.position.y) ||
            goal.z !== Math.floor(entity.position.z))) {
          goal = new SurfaceFollowGoal(bot, entity, range);
          bot.pathfinder.setGoal(goal, true);
        }
        if (nearby || position.distanceTo(lastPosition) >= 0.55) {
          lastPosition = position.clone();
          progressSince = now;
          retried = false;
        }
        if (!nearby && now - progressSince >= 3500) {
          if (retried) throw new Error('重算跟随路线后仍无法靠近玩家');
          goal = new SurfaceFollowGoal(bot, entity, range);
          bot.pathfinder.setGoal(goal, true);
          progressSince = now;
          retried = true;
        }
        result.progress = { phase: nearby ? 'in_range' : retried ? 'replanning' : 'following', terrain,
          distance: Math.round(distance * 10) / 10 };
      }
      await delay(125);
    }
  } finally {
    surface.stop();
    if (goal) {
      bot.pathfinder.setGoal(null);
      bot.clearControlStates();
    }
    result.position = bot.entity?.position?.clone() ?? null;
  }
}

module.exports = { followPlayer };
