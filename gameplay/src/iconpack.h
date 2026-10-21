// The gameplay mod's own HUD icons (ABS and TC): scripts\nfsmw_hud_icons.bin, a two-texture pack
// made from the game's own FRONTEND\FrontB.lzc. Not shipped (it is the game's art); built on the
// first start when it isn't there.
#pragma once
#include <string>

// Writes <gameDir>scripts\nfsmw_hud_icons.bin (gameDir ends with a backslash). False with the
// reason in *error if the game's file isn't as expected or the pack can't be written.
bool IconPack_Build(const std::string& gameDir, std::string* error);
