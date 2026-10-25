#pragma once
#include "processor_config.h"
struct _PER_CPU_DATA;

VOID ProcessorInitialize(struct _PER_CPU_DATA *Cpu);
NTSTATUS ProcessorGet(HV_PROCESSOR_CONFIG *Config);
NTSTATUS ProcessorSet(const HV_PROCESSOR_CONFIG *Config);
NTSTATUS ProcessorCommitCurrent(struct _PER_CPU_DATA *Cpu);
