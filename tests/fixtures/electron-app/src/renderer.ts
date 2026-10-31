// Renderer code in TypeScript, bundled by esbuild into renderer/bundle.js.
import { greet, type Greeting } from './greet';
import { clamp } from './math';

interface Bridge {
  getSetting(key: string): Promise<unknown>;
  log(message: string): void;
}

declare global {
  interface Window { cortexDemo: Bridge }
}

export async function start(): Promise<Greeting> {
  const theme = await window.cortexDemo.getSetting('theme');
  const greeting = greet(String(theme), clamp(42, 0, 10));
  window.cortexDemo.log(greeting.text);
  return greeting;
}

start();
