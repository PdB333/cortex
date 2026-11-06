// Live fixture for the JDWP provider (tests/dap_tests.cpp): the same rule as
// test_target's CortexDemoApplyDamage. Compiled with javac -g (local
// variable tables) and started with
//   java -agentlib:jdwp=transport=dt_socket,server=y,suspend=n,address=127.0.0.1:PORT -cp DIR LiveApp
public final class LiveApp {
    static final class Actor {
        String name = "demo";
        int health = 100;
        int armor = 25;
        int hits;
    }

    static int applyDamage(Actor target, int amount) {
        int absorbed = target.armor > 0 ? Math.min(amount / 3, target.armor) : 0;
        target.armor = target.armor - absorbed;
        target.health = target.health - (amount - absorbed); // CORTEX_HEALTH_WRITE
        target.hits = target.hits + 1;
        return target.health;
    }

    public static void main(String[] args) throws Exception {
        Actor actor = new Actor();
        String label = "tick";
        for (long ticks = 1; ; ++ticks) {
            int health = applyDamage(actor, 9);
            if (health <= 0) {
                actor.health = 100;
                actor.armor = 25;
            }
            System.out.println(label + " " + ticks + " health " + actor.health);
            Thread.sleep(20);
        }
    }
}
