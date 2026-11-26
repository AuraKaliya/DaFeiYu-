// 管理协议的复合值须是具名字段对象。
export function managementRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

// 保留后台签发的完整无符号 64 位十进制身份，不经过浮点转换。
export function managementNumber(value: unknown): value is string {
  return typeof value === 'string' && /^(0|[1-9]\d{0,19})$/.test(value) &&
    (value.length < 20 || value <= '18446744073709551615');
}
