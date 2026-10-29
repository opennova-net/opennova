#pragma once

namespace opennova::gameprofile {

/* The game ids the in-match rules still branch on (world::SessionRules::title and its kin). A
   title's id is title_game_id's (base/title/title.h); the titles themselves are base/title's
   registry. Versions program lane C deletes the enum when the rules read TitleRules. */
typedef enum GameId {
    GAME_JO = 0,    /* Joint Operations: Typhoon Rising            */
    GAME_JO_DEMO,   /* Joint Operations demo                        */
    GAME_DFX,       /* Delta Force: Xtreme                          */
    GAME_DFX2,      /* Delta Force: Xtreme 2 (Combined Arms era)    */
    GAME_BHD,       /* Delta Force: Black Hawk Down                 */
    GAME_COUNT
} GameId;

} // namespace opennova::gameprofile
