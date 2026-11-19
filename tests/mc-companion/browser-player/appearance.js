// 在 WebMC 的本地渲染器中为本局原角色应用已准备的材质，换实体或重连时重新绑定。
// 步骤：等待角色进入玩家视野 → 核对名字与本机身份 → 将纹理交给上游玩家模型。
let applying = false;
setInterval(async () => {
  const player = window.bot?.players?.DfyCompanion;
  const renderer = window.appViewer?.backend?.backendMethods;
  if (!player?.entity || applying || player.uuid !== '34fbcf7f-a9c8-3945-98b4-00e02e34ab18' ||
      typeof renderer?.updatePlayerSkin !== 'function') return;
  // 游戏实体会先于异步模型就绪；模型就绪后上游按实体和材质 URL 复用已加载纹理。
  applying = true;
  try { await renderer.updatePlayerSkin(player.entity.id, player.username, player.uuid, '/companion.png'); }
  catch (error) { console.warn('大肥鱼材质暂未加载', error.message); }
  finally { applying = false; }
}, 1500);

// 在桌面中文聊天框确认发送，候选词输入期间把回车留给输入法。
window.addEventListener('keydown', event => {
  const input = event.target;
  if (event.key !== 'Enter' || event.isComposing || event.keyCode === 229 || event.repeat ||
      !(input instanceof HTMLInputElement) || input.id !== 'chatinput' || !input.form || !input.value.trim()) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  input.form.requestSubmit();
}, true);
