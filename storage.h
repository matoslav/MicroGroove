// ============================================================
// CardputerGroovebox - storage.h
// Project save/load to SD: /groovebox/projects/P1.gbx .. P8.gbx
// A project = patterns + song + synth params + drum lane config
// + sample references (samples reloaded by filename on load).
// ============================================================
#pragma once
#include "config.h"

#define NUM_PROJECT_SLOTS 8

extern uint8_t g_curProject;    // 0..7

bool storageSaveProject(uint8_t slot);
bool storageLoadProject(uint8_t slot);
bool storageProjectExists(uint8_t slot);

// Slot saved most recently (marker on the SD), or -1 if none / unreadable.
int  storageLastSlot();
