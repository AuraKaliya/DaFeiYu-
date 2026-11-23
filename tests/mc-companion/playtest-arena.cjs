const wall = {
  repair: [3, -59, -3], doorLower: [7, -60, -3], doorUpper: [7, -59, -3],
  repairStand: [3.5, -60, -0.5], doorStand: [7.5, -60, -1.25],
  existingDoor: [[4, -60, 2], [4, -59, 2]],
};
const course = [
  [-10.5, -60, -5.5], [-10.5, -60, -11.5], [-3.5, -60, -11.5], [-3.5, -60, -5.5],
];
const waterCourse = { west: [-37.5, -60, -24.5], east: [-23.5, -60, -24.5] };
const fourCells = [[3, -60, -3], [4, -60, -3], [3, -59, -3], [4, -59, -3]];
const layeredStands = {
  pads: [[32, -60, -13], [33, -59, -13]],
  sources: [[30, -60, -16], [31, -60, -16]],
  foothold: [34, -58, -13],
  upper: [34.5, -56, -13.5],
};

// 在独立体验世界布置补墙缺格、预留门洞和方向可辨的左右地标。
function prepareWall(send, withDirt, withExistingDoor, moveDoor) {
  if (moveDoor) {
    send('kill @e[type=minecraft:item,x=-1,y=-62,z=-4,dx=12,dy=5,dz=8]');
    send('fill -1 -61 -4 10 -61 3 minecraft:grass_block');
    send('fill -1 -60 -4 10 -58 3 minecraft:air');
  }
  for (const command of ['gamerule randomTickSpeed 0',
    'fill 2 -60 -3 8 -59 -3 minecraft:dirt', 'setblock 3 -59 -3 minecraft:air',
    'fill 7 -60 -3 7 -59 -3 minecraft:air', 'fill -3 -60 1 -3 -59 1 minecraft:oak_planks',
    'fill 1 -60 -3 1 -59 -3 minecraft:oak_planks', 'fill 9 -60 -3 9 -59 -3 minecraft:oak_log',
    `tp DfyGuestCheck ${wall.repairStand.join(' ')} facing 3.5 -58.5 -3`]) send(command);
  if (moveDoor) send(`tp DfyGuestCheck ${wall.doorStand.join(' ')} 180 4`);
  else send('give DfyCompanion minecraft:oak_door 1');
  if (withDirt) send('give DfyCompanion minecraft:dirt 8');
  if (withExistingDoor) {
    send('setblock 4 -61 2 minecraft:stone');
    send(`setblock ${wall.existingDoor[0].join(' ')} minecraft:oak_door[half=lower,facing=south]`);
    send(`setblock ${wall.existingDoor[1].join(' ')} minecraft:oak_door[half=upper,facing=south]`);
  }
}

// 在专用地块准备可连续步行并留有转弯、停顿和重新起步空间的路线。
function prepareCourse(send) {
  send('fill -13 -61 -14 -2 -61 -4 minecraft:stone');
  send('fill -13 -60 -14 -2 -58 -4 minecraft:air');
}

// 固定四格开口、独立门洞及可用于跟随和取材的地面。
function prepareFourCells(send, combined) {
  for (const command of ['kill @e[type=minecraft:item,x=-9,y=-64,z=-4,dx=18,dy=6,dz=8]',
    'fill -9 -61 -4 9 -61 3 minecraft:grass_block', 'fill -9 -60 -2 9 -58 3 minecraft:air',
    'setblock 2 -62 0 minecraft:stone', 'fill 3 -60 -3 4 -59 -3 minecraft:air',
    ...(combined ? ['tp DfyCompanion -7.5 -60 -0.5', 'tp DfyGuestCheck -3.5 -60 -0.5'] :
      ['tp DfyGuestCheck 6.5 -60 -1.5', 'give DfyCompanion minecraft:dirt 3'])]) send(command);
}

// 布置两岸同高及低一格的水道，边界墙约束近处路线从水中经过。
function prepareWater(send) {
  for (const command of ['fill -40 -62 -28 -20 -62 -21 minecraft:stone',
    'fill -40 -61 -28 -20 -61 -21 minecraft:stone', 'fill -40 -60 -28 -20 -57 -21 minecraft:air',
    'fill -40 -61 -28 -20 -57 -28 minecraft:stone', 'fill -40 -61 -21 -20 -57 -21 minecraft:stone',
    'fill -33 -61 -27 -29 -61 -22 minecraft:water',
    'fill -32 -61 -17 -28 -57 -13 minecraft:stone',
    `tp DfyCompanion ${waterCourse.west.join(' ')}`, `tp DfyGuestCheck ${waterCourse.west.join(' ')}`]) send(command);
}

// 布置高台和两处垫脚位，并按场景提供取材地块和最后一个落脚点。
// 步骤：清理场地并定位两名玩家 → 按场景放置取材地块或发放材料。
function prepareLayered(send, resupply) {
  for (const command of ['fill 26 -61 -19 39 -61 -9 minecraft:stone',
    'fill 26 -60 -19 39 -53 -9 minecraft:air', 'setblock 34 -57 -14 minecraft:stone',
    'setblock 33 -59 -14 minecraft:stone', 'tp DfyCompanion 30.5 -60 -13.5',
    'tp DfyGuestCheck 32.5 -60 -13.5']) send(command);
  if (resupply) {
    for (const point of layeredStands.sources) send(`setblock ${point.join(' ')} minecraft:dirt`);
    send(`setblock ${layeredStands.foothold.join(' ')} minecraft:stone`);
  } else {
    send('give DfyCompanion minecraft:dirt 2');
  }
}

// 让独立协议玩家读回所选场地的原状和检验点，未加载或不符时等待下一次读取。
// 步骤：从客户端读取墙格或路面与净空 → 核对夹具预期 → 返回可保存的读回证据。
function readArena(player, wallProbe, withExistingDoor) {
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
    if (withExistingDoor) {
      observed.existingDoor = wall.existingDoor.map(pos => block(...pos));
      if (observed.existingDoor.some(name => name !== 'oak_door')) return null;
    }
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

module.exports = { wall, course, waterCourse, fourCells, layeredStands,
  prepareWall, prepareCourse, prepareFourCells, prepareWater, prepareLayered, readArena };
