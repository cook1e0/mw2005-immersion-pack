// Traffic density multiplier, and no SUV cops.
#pragma once

// Patches AITrafficManager's spawn rate and traffic cap, and
// redirects cop spawns that ask for an SUV. Called from Game_Install.
void Traffic_Install();
