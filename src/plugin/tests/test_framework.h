// Minimal self-contained test framework (no third-party code).
#pragma once

#include <cstdio>
#include <vector>

namespace e2t::test {

struct TestCase {
    const char* name;
    void (*function)();
};

std::vector<TestCase>& registry();
void reportCheck(bool ok, const char* expression, const char* file, int line);
int failedChecksInCurrentTest();

struct Registrar {
    Registrar(const char* name, void (*function)()) { registry().push_back({name, function}); }
};

}  // namespace e2t::test

#define E2T_TEST(name)                                                        \
    static void name();                                                       \
    static const ::e2t::test::Registrar name##_registrar(#name, &name);       \
    static void name()

#define CHECK(expression) \
    ::e2t::test::reportCheck(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
