/* ELF fixture for the Ghidra provider (tests/analysis_provider_tests.cpp):
 * the same armor/health rule as test_target's CortexDemoApplyDamage. Built
 * by make_elf_fixture.sh (gcc -O1, PIE, not stripped); committed so CI on
 * Windows needs no Linux compiler. */
#include <stdint.h>
#include <stdio.h>

struct CortexDemoActor {
    uint32_t magic;
    int32_t health;
    int32_t armor;
    uint32_t hits;
};

__attribute__((noinline)) void CortexDemoApplyDamage(struct CortexDemoActor* actor, int32_t amount) {
    volatile struct CortexDemoActor* target = actor;
    const int32_t absorbed = target->armor > 0 ? (amount / 3 < target->armor ? amount / 3 : target->armor) : 0;
    target->armor = target->armor - absorbed;
    target->health = target->health - (amount - absorbed);
    target->hits = target->hits + 1;
}

__attribute__((noinline)) int64_t CortexNativeAdd(int64_t a, int64_t b, int64_t c) { return a + b + c; }

int main(void) {
    struct CortexDemoActor actor = {0x52544341u, 100, 25, 0};
    for (int i = 0; i < 3; ++i) CortexDemoApplyDamage(&actor, 9);
    printf("%d %d %u %lld\n", actor.health, actor.armor, actor.hits, (long long)CortexNativeAdd(1, 2, 3));
    return 0;
}
