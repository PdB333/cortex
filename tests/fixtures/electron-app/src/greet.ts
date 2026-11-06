export interface Greeting {
  text: string;
  level: number;
}

export function greet(theme: string, level: number): Greeting {
  const text = `Hello from the ${theme} theme`;
  return { text, level };
}
