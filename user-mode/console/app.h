#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stddef.h>
#include "control.h"
#include "monitor_protocol.h"
#include "processor_config.h"

extern const char *const g_TargetNames[HV_MONITOR_TARGETS];
extern volatile LONG g_Interrupted;
void AppError(const char *Operation, DWORD Error);
void AppClearScreen(void);
int AppChoice(const char *Prompt, int Minimum, int Maximum);
int AppVerify(void);
int AppShutdown(void);
int AppRemoveService(void);
void AppMonitorMenu(void);
void AppMonitorClose(void);

void AppHeading(const char *Text);
void AppPage(const char *Title);
void AppPause(void);
void AppFilesMenu(void);
void AppMonitorSummary(void);

int AppParseFieldValue(ULONG Target, ULONG Field, const char *Text, ULONG64 *Value);
void AppListFieldSymbols(ULONG Target, ULONG Field);
int AppCleanPath(WCHAR *Path);
int AppProcessorIo(HV_PROCESSOR_CONFIG *Config, int Write);
void AppCpuidMenu(void);
void AppTscMenu(void);
