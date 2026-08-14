#include "action/action.h"

#include <iostream>
#include <string>

namespace {
int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestAutomaticRollback() {
    action::Clear();
    int value = 0;
    {
        action::Transaction tx;
        value = 1;
        action::Record("set one", [&value] { value = 0; return true; });
    }
    Check(value == 0, "uncommitted transaction rolls back on scope exit");
    Check(action::List().empty(), "successful automatic rollback clears journal entries");
}

void TestCommit() {
    action::Clear();
    int value = 0;
    {
        action::Transaction tx;
        value = 1;
        action::Record("set one", [&value] { value = 0; return true; });
        tx.Commit();
    }
    Check(value == 1, "committed transaction keeps state");
    Check(action::List().size() == 1, "committed transaction keeps undo journal entry");
    action::RollbackAll();
    Check(value == 0, "committed action remains manually reversible");
}

void TestNestedCommitInsideRollback() {
    action::Clear();
    int value = 0;
    {
        action::Transaction outer;
        value = 1;
        action::Record("outer", [&value] { value = 0; return true; });
        {
            action::Transaction inner;
            value = 2;
            action::Record("inner", [&value] { value = 1; return true; });
            inner.Commit();
        }
        Check(value == 2, "inner commit survives until outer transaction completes");
    }
    Check(value == 0, "outer rollback includes committed inner actions");
    Check(action::List().empty(), "nested rollback leaves a clean journal");
}

void TestExplicitRollback() {
    action::Clear();
    int value = 10;
    action::Transaction tx;
    value = 20;
    action::Record("change", [&value] { value = 10; return true; });
    const auto result = tx.Rollback();
    Check(result.size() == 1 && result.front().ok, "explicit transaction rollback reports success");
    Check(value == 10, "explicit rollback restores state");
    Check(!tx.active(), "rolled back transaction becomes inactive");
}
}

int main() {
    TestAutomaticRollback();
    TestCommit();
    TestNestedCommitInsideRollback();
    TestExplicitRollback();
    if (failures) {
        std::cerr << failures << " transaction test(s) failed\n";
        return 1;
    }
    std::cout << "PASS: action transactions commit, rollback, and nesting\n";
    return 0;
}
