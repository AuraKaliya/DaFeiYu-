interface Window {
  chrome: { webview: {
    // 向当前文档绑定的原生接收器发送明确的验证控制消息。
    postMessage(message: Readonly<Record<string, string | number>>): void;
    // 登记当前文档的宿主回执监听，不代表业务订阅已经成立。
    addEventListener(type: 'message', listener: (event: MessageEvent<unknown>) => void): void;
    // 页面卸载时归还本地监听，不取消已接纳业务。
    removeEventListener(type: 'message', listener: (event: MessageEvent<unknown>) => void): void;
  } };
}
