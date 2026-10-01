// check.h — the tests' whole harness. TEST(name) registers a function, CHECK(cond) prints the file and line of a
// failure and goes on, and main() runs every test and returns how many checks failed. No framework: a failure is a file
// and a line, and this file is all there is to read.
#pragma once

#include <cstdio>

namespace crsrt::test {

using Fn = void (*)();

// Registers a test at static initialisation, in the order the files are linked and the tests written.
struct Register {
    Register(const char *name, Fn fn);
};

// Counts a failed check. Called by CHECK.
void fail(const char *file, int line, const char *text);

}  // namespace crsrt::test

#define TEST(name)                                                     \
    static void name();                                                \
    static const crsrt::test::Register name##_registered(#name, name); \
    static void name()

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) crsrt::test::fail(__FILE__, __LINE__, #cond); \
    } while (0)
