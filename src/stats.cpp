// Amostras de tempo nomeadas (ms); relatório a cada 5 s com p50, p99, máx e contagem > 1 ms.
#include "mod.h"
#include <stdlib.h>
#include <string.h>

namespace {
constexpr int k_series = 32, k_samples = 8192;
struct Series {
	const char* name;
	float v[k_samples];
	int n, dropped;
	float max;  // max e over contam também as amostras descartadas
	int over;
};
Series g_s[k_series];
int g_count;
CRITICAL_SECTION g_cs;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK init_cs(INIT_ONCE*, void*, void**) {
	InitializeCriticalSection(&g_cs);
	return TRUE;
}

int cmp_float(const void* a, const void* b) {
	float x = *(const float*)a, y = *(const float*)b;
	return (x > y) - (x < y);
}
}  // namespace

double qpc_ms(LONGLONG ticks) {
	static LARGE_INTEGER f;
	if (!f.QuadPart) QueryPerformanceFrequency(&f);
	return ticks * 1000.0 / f.QuadPart;
}

// name: literal estático (guardado por ponteiro, comparado por conteúdo).
void stats_add(const char* name, double ms) {
	InitOnceExecuteOnce(&g_once, init_cs, NULL, NULL);
	EnterCriticalSection(&g_cs);
	Series* s = NULL;
	for (int i = 0; i < g_count && !s; i++)
		if (!strcmp(g_s[i].name, name)) s = &g_s[i];
	if (!s && g_count < k_series) s = &g_s[g_count++], s->name = name;
	if (s) {
		if (ms > s->max) s->max = (float)ms;
		s->over += ms > 1.0;
		if (s->n < k_samples)
			s->v[s->n++] = (float)ms;
		else
			s->dropped++;
	}
	LeaveCriticalSection(&g_cs);
}

void stats_report() {
	InitOnceExecuteOnce(&g_once, init_cs, NULL, NULL);
	EnterCriticalSection(&g_cs);
	for (int i = 0; i < g_count; i++) {
		Series& s = g_s[i];
		if (!s.n) continue;
		qsort(s.v, s.n, sizeof(float), cmp_float);
		// p50/p99 só das primeiras k_samples; n inclui as descartadas
		logf("stats %-14s n=%5d p50=%7.3f p99=%7.3f max=%8.3f >1ms=%d%s", s.name, s.n + s.dropped, s.v[s.n / 2],
		     s.v[s.n * 99 / 100], s.max, s.over, s.dropped ? " (p50/p99 das 1as 8192)" : "");
		s.n = s.dropped = s.over = 0;
		s.max = 0;
	}
	LeaveCriticalSection(&g_cs);
}
