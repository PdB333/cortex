package cortex.fixture;

import java.util.ArrayList;
import java.util.List;
import java.util.function.IntUnaryOperator;

// Java fixture for the Vineflower provider: the same rules as the C# and
// native fixtures (armor absorbs a third of the damage).
public final class Actor {
    public static final int MAX_HEALTH = 100;
    public static int created;

    public static final class Snapshot {
        public int health;
        public int[] recent = new int[4];
    }

    private final String name;
    private int health = MAX_HEALTH;
    private int armor = 25;
    private final List<String> log = new ArrayList<>();

    public Actor(String name) {
        this.name = name;
        created++;
    }

    public String getName() { return name; }
    public int getHealth() { return health; }
    public int getArmor() { return armor; }

    public void applyDamage(int amount, DamageKind kind) {
        if (amount <= 0) throw new IllegalArgumentException("damage must be positive");
        int dealt;
        switch (kind) {
            case FIRE: dealt = amount * 2; break;
            case POISON: dealt = amount / 2; break;
            default: dealt = Math.max(1, amount - armor / 5); break;
        }
        int absorbed = armor > 0 ? Math.min(dealt / 3, armor) : 0;
        armor -= absorbed;
        health = Math.max(0, health - (dealt - absorbed));
        log.add(name + " took " + dealt);
    }

    public int heal(int amount) {
        try {
            health = Math.min(MAX_HEALTH, Math.addExact(health, amount));
        } catch (ArithmeticException overflow) {
            health = MAX_HEALTH;
        } finally {
            log.add("healed");
        }
        return health;
    }

    public static <T extends CharSequence> T pick(List<T> items, int index) {
        return index < 0 || index >= items.size() ? null : items.get(index);
    }

    public int scaled(int factor) {
        IntUnaryOperator scale = value -> value * factor;
        return scale.applyAsInt(health);
    }

    public Snapshot snapshot() {
        Snapshot s = new Snapshot();
        s.health = health;
        return s;
    }
}
