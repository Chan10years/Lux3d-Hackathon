#pragma once

// Bakes the top-down layers of Lvl_ZZZ that the city map texture (T_CityAtlas) is built from.
// Trigger: create Saved/ZZZ/atlas_bake.flag while the level is open in the editor (no PIE). Output: Saved/ZZZ/atlas.
namespace TrioAtlasBake
{
    void Startup();
    void Shutdown();
}
