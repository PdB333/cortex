// Live fixture for the CDP provider (tests/cdp_tests.cpp, tests/live_e2e.py).
// Same rule as test_target's CortexDemoApplyDamage: armor absorbs a third of
// the damage until it is gone. Started with node --inspect=127.0.0.1:<port>.
'use strict';

const actor = { name: 'demo', health: 100, armor: 25, hits: 0 };
let ticks = 0;

function applyDamage(target, amount) {
  const absorbed = target.armor > 0 ? Math.min(Math.floor(amount / 3), target.armor) : 0;
  target.armor = target.armor - absorbed;
  target.health = target.health - (amount - absorbed); // CORTEX_HEALTH_WRITE
  target.hits = target.hits + 1;
  return target.health;
}

setInterval(() => {
  ticks += 1;
  applyDamage(actor, 9);
  if (actor.health <= 0) {
    actor.health = 100;
    actor.armor = 25;
  }
  // A pause Cortex did not ask for: it must be recorded and resumed.
  if (ticks % 50 === 0) debugger;
  console.log(`tick ${ticks} health ${actor.health} armor ${actor.armor}`);
}, 20);
