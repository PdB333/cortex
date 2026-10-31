export function format(text) {
  return `[cortex] ${text}`;
}
export class Formatter {
  constructor(prefix) { this.prefix = prefix; }
}
