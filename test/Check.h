#pragma once
#include <cstdio>
#include <cstdlib>
static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)
#define DONE() do { std::printf(g_failures ? "%d FAILED\n" : "ok\n", g_failures); return g_failures ? 1 : 0; } while (0)
