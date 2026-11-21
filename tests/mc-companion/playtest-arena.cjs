const wall = {
  repair: [3, -59, -3], doorLower: [7, -60, -3], doorUpper: [7, -59, -3],
  repairStand: [3.5, -60, -0.5], doorStand: [7.5, -60, -1.25],
};
const course = [
  [-10.5, -60, -5.5], [-10.5, -60, -11.5], [-3.5, -60, -11.5], [-3.5, -60, -5.5],
];

// 在独立体验世界布置补墙缺格、预留门洞和方向可辨的左右地标。
function prepareWall(send) {
  for (const command of ['gamerule randomTickSpeed 0',
    'fill 2 -60 -3 8 -59 -3 minecraft:dirt', 'setblock 3 -59 -3 minecraft:air',
    'fill 7 -60 -3 7 -59 -3 minecraft:air', 'fill -3 -60 1 -3 -59 1 minecraft:oak_planks',
    'fill 1 -60 -3 1 -59 -3 minecraft:oak_planks', 'fill 9 -60 -3 9 -59 -3 minecraft:oak_log',
    `tp DfyGuestCheck ${wall.repairStand.join(' ')} facing 3.5 -58.5 -3`,
    'give DfyCompanion minecraft:dirt 8', 'give DfyCompanion minecraft:oak_door 1']) send(command);
}

// 在专用地块准备可连续步行并留有转弯、停顿和重新起步空间的路线。
function prepareCourse(send) {
  send('fill -13 -61 -14 -2 -61 -4 minecraft:stone');
  send('fill -13 -60 -14 -2 -58 -4 minecraft:air');
}

// 让独立协议玩家读回所选场地的原状和检验点，未加载或不符时等待下一次读取。
// 步骤：从客户端读取墙格或路面与净空 → 核对夹具预期 → 返回可保存的读回证据。
function readArena(player, wallProbe) {
  const block = (x, y, z) => player.blockAt(player.entity.position.clone().set(x, y, z))?.name;
  if (wallProbe) {
    const observed = {
      repair: block(...wall.repair), repairSupport: block(3, -60, -3),
      door: [block(...wall.doorLower), block(...wall.doorUpper)], doorSupport: block(7, -61, -3),
      landmarks: [block(1, -59, -3), block(9, -59, -3)],
    };
    if (observed.repair !== 'air' || observed.repairSupport !== 'dirt' ||
        observed.door.some(name => name !== 'air') || !observed.doorSupport || observed.doorSupport === 'air' ||
        observed.landmarks[0] !== 'oak_planks' || observed.landmarks[1] !== 'oak_log') return null;
    return { wall, observed };
  }

  const observed = [];
  for (let z = -12; z <= -6; ++z) for (let x = -11; x <= -4; ++x) {
    const floor = block(x, -61, z), feet = block(x, -60, z), head = block(x, -59, z);
    if (floor !== 'stone' || feet !== 'air' || head !== 'air') return null;
    observed.push({ x, z, floor, feet, head });
  }
  return { course, observed };
}

module.exports = { wall, course, prepareWall, prepareCourse, readArena };
