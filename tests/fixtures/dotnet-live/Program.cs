// Same rule as test_target's CortexDemoApplyDamage. Built in Debug (portable
// PDB) so netcoredbg can bind line breakpoints.
using System;
using System.Threading;

public sealed class Actor
{
    public string Name = "demo";
    public int Health = 100;
    public int Armor = 25;
    public int Hits;
}

public static class Program
{
    public static int ApplyDamage(Actor target, int amount)
    {
        int absorbed = target.Armor > 0 ? Math.Min(amount / 3, target.Armor) : 0;
        target.Armor = target.Armor - absorbed;
        target.Health = target.Health - (amount - absorbed); // CORTEX_HEALTH_WRITE
        target.Hits = target.Hits + 1;
        return target.Health;
    }

    public static void Main(string[] args)
    {
        // The test attaches netcoredbg by process id: write it where asked.
        if (args.Length > 0) System.IO.File.WriteAllText(args[0], Environment.ProcessId.ToString());
        var actor = new Actor();
        for (long ticks = 1; ; ++ticks)
        {
            int health = ApplyDamage(actor, 9);
            if (health <= 0) { actor.Health = 100; actor.Armor = 25; }
            Console.WriteLine($"tick {ticks} health {actor.Health} armor {actor.Armor}");
            Thread.Sleep(20);
        }
    }
}
