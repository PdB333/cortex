using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Cortex.Fixture
{
    public enum DamageKind : byte { Physical = 1, Fire = 2, Poison = 4 }

    public interface IDamageable
    {
        int Health { get; }
        void ApplyDamage(int amount, DamageKind kind);
    }

    [Serializable]
    public sealed class Actor : IDamageable
    {
        public const int MaxHealth = 100;
        public static int Created;
        private int health = MaxHealth;
        private readonly List<string> log = new List<string>();

        public Actor(string name)
        {
            Name = name;
            Created++;
        }

        public string Name { get; }
        public int Health => health;
        public int Armor { get; set; } = 25;

        public void ApplyDamage(int amount, DamageKind kind)
        {
            if (amount <= 0)
                throw new ArgumentOutOfRangeException(nameof(amount), "damage must be positive");
            int taken = kind switch
            {
                DamageKind.Fire => amount * 2,
                DamageKind.Poison => amount / 2,
                _ => Math.Max(1, amount - Armor / 5),
            };
            health = Math.Max(0, health - taken);
            log.Add($"{Name} took {taken}");
        }

        public int Heal(int amount)
        {
            try
            {
                checked { health = Math.Min(MaxHealth, health + amount); }
            }
            catch (OverflowException)
            {
                health = MaxHealth;
            }
            finally
            {
                log.Add("healed");
            }
            return health;
        }

        public static T Pick<T>(IList<T> items, int index) where T : class
        {
            return index >= 0 && index < items.Count ? items[index] : null;
        }

        public long Classify(int code)
        {
            switch (code)
            {
                case 0: return 10L;
                case 1: return 20L;
                case 2: return 30L;
                case 3: return 40L;
                default: return -1L;
            }
        }

        public double Scale(float factor) => health * (double)factor + 0.5;

        public sealed class Snapshot
        {
            public int Health;
            public int[] Recent = new int[4];
        }
    }

    public static class Native
    {
        [DllImport("kernel32.dll", EntryPoint = "GetTickCount64")]
        public static extern ulong Ticks();

        public static string Describe(Actor actor) => "Actor " + actor.Name + " at " + actor.Health;
    }
}
