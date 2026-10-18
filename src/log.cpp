#include "mod.h"
#include <stdio.h>
#include <stdarg.h>

static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static LARGE_INTEGER g_t0, g_freq;

void log_open(const char* path) {
	InitializeCriticalSection(&g_cs);
	QueryPerformanceFrequency(&g_freq);
	QueryPerformanceCounter(&g_t0);
	g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
}

// Linha: [segundos desde a carga, thread, contador de Present do jogo] mensagem
void logf(const char* fmt, ...) {
	if (g_file == INVALID_HANDLE_VALUE) return;
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	unsigned frame = g_version_ok ? *(volatile unsigned*)game::present_counter : 0;
	char buf[2048];
	int n = snprintf(buf, sizeof buf, "[%9.3f t%05lu f%u] ", double(t.QuadPart - g_t0.QuadPart) / g_freq.QuadPart,
	                 GetCurrentThreadId(), frame);
	va_list a;
	va_start(a, fmt);
	int m = vsnprintf(buf + n, sizeof buf - n - 1, fmt, a);
	va_end(a);
	n += m < 0 ? 0 : (m < int(sizeof buf) - n - 1 ? m : int(sizeof buf) - n - 2);
	buf[n++] = '\n';
	DWORD w;
	EnterCriticalSection(&g_cs);
	WriteFile(g_file, buf, n, &w, NULL);
	LeaveCriticalSection(&g_cs);
}
