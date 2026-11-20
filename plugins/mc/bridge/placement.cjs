const { goals } = require('mineflayer-pathfinder');
const { setTimeout: delay } = require('node:timers/promises');
const vacant = new Set(['air', 'cave_air', 'void_air']);
const offsets = [[0,-1,0], [0,0,-1], [0,0,1], [-1,0,0], [1,0,0], [0,1,0]];

// 检查所有占用格是否已加载且为空，同时保护目标内的玩家和生物。
function checkCells(bot, cells) {
  for (const point of cells) {
    if (!vacant.has(bot.blockAt(point)?.name)) throw new Error('目标已占用或尚未加载');
    if (Object.values(bot.entities).some(entity => {
      if (entity !== bot.entity && entity.type !== 'player' && entity.type !== 'mob') return false;
      const p = entity.position, half = (entity.width ?? 0.6) / 2;
      return p.x + half > point.x && p.x - half < point.x + 1 && p.z + half > point.z && p.z - half < point.z + 1 &&
        p.y + (entity.height ?? 1.8) > point.y && p.y < point.y + 1;
    })) throw new Error('目标内有角色或生物');
  }
}

// 只考虑能承担完整方块的相邻面，门必须以目标正下方作为支撑。
function supportGoal(bot, point, door) {
  const faces = (door ? offsets.slice(0, 1) : offsets).filter(([x, y, z]) =>
    bot.blockAt(point.offset(x, y, z))?.shapes?.some(s => s.join(',') === '0,0,0,1,1,1'))
    .map(([x, y, z]) => point.clone().set(x, y, z));
  if (!faces.length) throw new Error('目标旁没有可用的完整支撑面');
  return new goals.GoalPlaceBlock(point, bot.world, { range: 4.3, faces });
}

// 从真实背包读取同名材料总量，跨栈数量也参与核对。
function stock(bot, name) {
  return bot.inventory.items().filter(item => item.name === name).reduce((sum, item) => sum + item.count, 0);
}

// 放置一个指定方块或一扇双格门，并用服务端读回确认效果。
// 步骤：核对目标和材料 → 移到可访问的支撑面 → 重新核对后放置 → 读取方块和库存。
async function placeBlock(bot, point, type, check, walk, result) {
  const door = type.name.endsWith('_door');
  if ((!door && type.boundingBox !== 'block') || !bot.registry.itemsByName[type.name])
    throw new Error('需要背包中可放置的实心方块或门');
  const cells = door ? [point, point.offset(0, 1, 0)] : [point];
  checkCells(bot, cells);
  if (!stock(bot, type.name)) throw new Error('背包没有指定建材');

  const eye = () => bot.entity.position.offset(0, bot.entity.eyeHeight, 0);
  const goal = supportGoal(bot, point, door);
  if (!goal.getFaceAndRef(eye())) await walk(bot, goal);
  check();
  const item = bot.inventory.items().find(stack => stack.name === type.name);
  if (!item) throw new Error('建材已不在背包');
  if (bot.heldItem?.type !== item.type) await bot.equip(item, 'hand');
  check();
  checkCells(bot, cells);
  const support = supportGoal(bot, point, door).getFaceAndRef(eye());
  if (!support || bot.heldItem?.name !== type.name) throw new Error('支撑、视线或手持材料已经变化');
  const reference = bot.blockAt(support.ref);
  const sneaking = bot.getControlState('sneak');
  result.materialBefore = stock(bot, type.name);
  result.placementAttempted = true;
  let acknowledged = false;
  try {
    bot.setControlState('sneak', true);
    await bot.placeBlock(reference, support.face.scaled(-1));
    acknowledged = true;
  } finally {
    bot.setControlState('sneak', sneaking);
    for (let retry = 0; retry < 4; ++retry) {
      result.blocks = cells.map(({ x, y, z }, index) => {
        const block = bot.blockAt(cells[index]);
        const properties = block?.getProperties() ?? {};
        return { x, y, z, block: block?.name ?? null, ...(door && { half: properties.half, facing: properties.facing }) };
      });
      result.materialAfter = stock(bot, type.name);
      const observed = result.blocks.every((block, index) => block.block === type.name &&
        (!door || block.half === (index ? 'upper' : 'lower')));
      result.placed = acknowledged && observed &&
        (bot.game.gameMode === 'creative' || result.materialAfter < result.materialBefore);
      if (!acknowledged || result.placed) break;
      await delay(150);
    }
  }
  if (!result.placed) throw new Error('方块或消耗的建材尚未核实');
}

module.exports = { placeBlock };
