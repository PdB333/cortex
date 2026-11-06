// ES module loaded with a dynamic import from main.js.
import { format } from './format.mjs';
export { format };
export * from './format.mjs';

export default function describe(name) {
  return format(`${name} is running`);
}
export const describeVersion = 2;
