// Hooks into speed.exe v1.3: gear selection (InputPlayer) and clutch (EngineRacer).
#pragma once

// Verifies the code at every hook site and installs the hooks. Returns false (and patches
// nothing) if the executable is not the expected version.
bool Game_Install();
