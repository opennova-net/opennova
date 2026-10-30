#pragma once

// THE FRONTEND: the game's mode table above the in-match session. Retail's whole program is one
// loop over four modes, each a descriptor {name, init, update, render, shutdown}: "MainMenu" (the
// menus), "PreMenu" (a multiplayer mission's pre-game: the join state machine, pre.mnu's
// PRE_GAME_MENU), "Game Loop" (the mission) and "Post Menu" (the mission exit's router). A push
// records the next mode and resets the key queue; the loop then runs the current mode's
// shutdown, switches, and runs the next mode's init, after which it resets its bank and re-reads
// its clock. No next mode ends the loop, whose return saves game.cfg and shuts the subsystems
// down. The start ahead of it forces Num Lock on and reads game.cfg, whose mpreset ends the
// process there.
// [orig: Game_Run @0x4A7FB0 - Game_LoadConfig @0x4A7FBB, Game_MainLoop(&g_GameModeMainMenu)
//  @0x4A7FF7, Game_SaveConfig @0x4A7FFF, Game_ShutdownSubsystems @0x4A8004; Game_MainLoop
//  @0x52B630 - the update's nonzero return ends the frame loop @0x52BAEA, the shutdown @0x52BAFF,
//  the switch @0x52BB08, the next init @0x52B70D, the mode-init reset @0x52B70F..0x52B757, the
//  clock's re-read @0x52B766; UI_NavHistoryPush @0x52BB30 - Input_ResetKeyQueue @0x52BB30,
//  g_PendingGameMode @0x52BB3B; the descriptors g_GameModeMainMenu @0x83B400 (Menu_InitShellResources
//  @0x552500, Menu_UpdateFrame @0x5528A0, Menu_TeardownShellAndCloseBinkVideos @0x54E430),
//  g_GameModePreMenu @0x83C9D0 (PreMenu_Init @0x5693E0, MultiPlayer_JoinSessionStateMachine
//  @0x56A320, PreMenu_Shutdown @0x568890), g_GameModeGameLoop @0x82F340 (GameLoop_Init @0x526370,
//  Game_ProcessMainFrame @0x5263F0, Game_TeardownMission @0x522350), g_GameModePostMenu @0x83C980
//  (PostMenu_Init @0x568360, PostMenu_RouteMissionExit @0x568460)]
//
// The Frontend owns the mode, the in-mission screen up (menu::ScreenStack), the save points and
// the routing; an embedder (the Godot shell today, apps/game next) supplies legs alone: the
// stores' files, the menu's and the mission load's device work, the process's exits. A leg left
// empty is work the embedder has none of. Retail's PreMenu is the multiplayer start's: a host's
// passes through it at once, a join waits in it for the game start; our session's load runs
// under the join's wait (the join screen follows it), so a PreMenu with a load up is the
// mission's load for every gate below.

#include <runtime/inmatch/main_loop_clock.h>
#include <runtime/menu/screen_stack.h>
#include <runtime/world/tick_accumulator.h>

#include <cstdint>
#include <functional>
#include <string>

namespace opennova::inmatch {
class GameConfigBlock;
}
namespace opennova::profile {
class PlayerProfiles;
}

namespace opennova::frontend {

enum class Mode : uint8_t {
	None,     // before the loop, or past its end
	MainMenu, // "MainMenu"
	PreMenu,  // "PreMenu"
	GameLoop, // "Game Loop"
	PostMenu, // "Post Menu"
};

// The descriptor's name ("MainMenu", "PreMenu", "Game Loop", "Post Menu"); "" for None.
const char *mode_name(Mode mode);

// The process's start ahead of the loop: the command line's init forces Num Lock on (saving its
// state for the exit), then the start reads game.cfg, the block's first use. False when that read
// set mpreset: the process ends there with exit code 0, nothing saved and no shutdown run, so Num
// Lock stays as the start forced it.
// [orig: WinMain @0x763A80 - Game_ParseCommandLineAndInit @0x763B5B (Input_SetNumLockState(1)
//  @0x4A7386), Game_Run @0x763BA9 (Game_LoadConfig @0x4A7FBB); Game_LoadConfig @0x551480 - the
//  mpreset crt_exit(0) @0x5514A1..0x5514AC]
struct BootLegs {
	std::function<void()> force_num_lock;
	// The start's read of game.cfg; true when its file set mpreset.
	std::function<bool()> read_game_config;
};
bool boot(const BootLegs &legs);

// What Esc does over the frontend's state (the shell's escape route). Over live play the Escape
// key belongs to the input frame's keyboard pass (its escape row's case runs in the update's
// drain), so this route is the tools window's interact mode's and the in-mission screens'.
enum class EscapeRoute : uint8_t {
	None,            // nothing: the key is the keyboard pass's, or the round is over, or the menu's
	CancelJoinScreen, // the join screen's Cancel answers it
	CancelLoad,      // a load's Esc: the joiner wait's (the game-start wait aborts the join, the
	                 // final wait leaves for the game loop)
	EscapeAction,    // live play: the HUD's escape chain closes a window, else the in-game menu opens
	Resume,          // INGAME or WEAPON: the screen closes and play resumes
	CloseScreen,     // CMAP or VEHICLE: the screen's own close
};

// Where the use-item key's arms and the weapon-screen toggle land.
enum class UseItemRoute : uint8_t {
	None,
	CloseScreen, // an open screen closes instead (the drain runs under DEATH and STAT alone)
	OpenArmory,  // weapon.mnu WEAPON over live play
	OpenVehicle, // vehicle.mnu VEHICLE over live play
};

// The mission's exit as the main frame and the Post Menu route it (inmatch/mission_exit.h).
enum class ExitRoute : uint8_t {
	None,         // already back in the menu with nothing loading: the teardown ran
	RestartRound, // Game_RestartRoundSP: the same mission again, no menu, no loading screen
	MissionBox,   // a Mission box's Game Loop: the mission the box names starts
	LeaveToMenu,  // the mission's own end out of a session (or a rotation's end): no abort
	JoinerReload, // a joiner's round over: the Game Loop reloads the host's next mission
	MapChange,    // an in-session host's round end: the Post Menu loads the next rotation entry
	Abort,        // the abort-to-menu leg
};

struct MissionExit {
	bool has_session = false;  // a mission's runtime stands
	int32_t stored_reason = 0; // the reason the session stored (0: none)
	bool mp_session = false;
	bool joiner = false;
	bool mission_box_named = false; // the Mission box names a mission
};

struct ExitDecision {
	ExitRoute route = ExitRoute::None;
	int32_t reason = 0; // the exit reason the route's teardown reads
};

// What a finished load's mission start does next.
enum class MissionStarted : uint8_t {
	AwaitAdmission, // a joiner's world stands, its admission still to come
	Reveal,         // a joiner past its game-start wait: its input reset ran at that wait's end
	ResetAndReveal, // every other start: the whole input reset, then the reveal
};

// Where a failed load goes.
enum class LoadFailure : uint8_t {
	JoinScreen, // the join screen stays up with the connection's reason and Cancel alone
	Abort,      // the abort-to-menu leg
};

// A quit request's step.
enum class QuitRequest : uint8_t {
	Begin,       // the first: its saves ran, the shutdown follows
	FinishDrain, // a repeat while the music drain waits: end the drain, quit now
	Ignored,     // a repeat with nothing to hurry
};

// A screen's close edge.
enum class ScreenClose : uint8_t {
	None,            // the screen was not the one up
	Closed,          // closed: the screen history's return and the whole input reset are owed
	ClosedPassThrough, // closed under its pass-through word: the close's input frame also takes
	                   // the menu cursor's offset (world::pass_through_close_axis)
};

struct FrontendLegs {
	// --- the stores ---
	// The process's profile and cfg block, resolved at each use (null: none).
	std::function<profile::PlayerProfiles *()> profiles;
	std::function<inmatch::GameConfigBlock *()> game_config;
	// The save points' writes [orig: PlayerProfile_SaveToFiles @0x54BE00; Game_SaveConfig
	// @0x54C490].
	std::function<void()> save_profile;
	std::function<void()> save_game_config;
	// The MainMenu init's read of game.cfg and the Mods reload's save then read; false when the
	// read set mpreset.
	std::function<bool()> read_menu_config;
	std::function<bool()> reload_menu_config;
	// The menu init's profile load [orig: PlayerProfile_LoadAllFromDisk @0x54F4D0].
	std::function<void()> load_profile;
	// The menu scene's cookie jar: loaded at its creation, saved at its destruction
	// [orig: CookieJar_OpenAndLoadFile @0x552630; CUIScene_SaveCookieJarFile @0x54E4AA].
	std::function<void()> load_cookie_jar;
	std::function<void()> save_cookie_jar;
	// The record's binding table and joystick word onto the live controls
	// [orig: Profile_ApplySessionControls @0x563620].
	std::function<void()> apply_controls;
	// --- the menu device ---
	// The options' device copy of the block's words, read again at each menu init.
	std::function<void()> reload_options;
	std::function<void()> show_menu;
	// The mode push's key queue reset [orig: UI_NavHistoryPush @0x52BB30 (Input_ResetKeyQueue)].
	std::function<void()> reset_key_queue;
	// --- the mission load's device ---
	std::function<bool()> load_can_start; // the load coordinator takes a new start
	std::function<void()> stop_lan_browse;
	std::function<void()> stop_music;
	// A single player's start's entry, the SP restart's (the embedder keeps its loader).
	std::function<void()> remember_restart_entry;
	// The join screen (pre.mnu PRE_GAME_MENU) up over the join's wait; false when it cannot open.
	std::function<bool()> open_join_screen;
	// The load's presentation up: the spawn kit staged, the menu left, the loading screen raised.
	std::function<void()> begin_world_load;
	// The load's start (`restart`: no loading screen; `join`: under the join screen). False when
	// the handoff could not start.
	std::function<bool(bool restart, bool join)> begin_load;
	// A load that could not start: the shell's load-failure leg with the reason.
	std::function<void(const std::string &reason)> load_failed;
	// --- the process ---
	// The mpreset exit: the process ends with `code`, nothing saved.
	std::function<void(int code)> exit_process;
};

class Frontend {
public:
	explicit Frontend(FrontendLegs legs = {});

	FrontendLegs &legs() { return legs_; }
	void set_legs(FrontendLegs legs) { legs_ = std::move(legs); }

	// --- the mode table ---
	Mode mode() const { return mode_; }
	// The mission start's load is up (from the start until the reveal or the failure).
	bool loading() const { return loading_; }
	menu::ScreenStack &screens() { return screens_; }
	const menu::ScreenStack &screens() const { return screens_; }
	menu::MissionScreen screen() const { return screens_.current(); }

	// The menus are up: no mission mode (the MainMenu, the Post Menu's route, the loop's ends).
	bool in_menu() const;
	// The mission's play: a Game Loop (or a join's PreMenu, its load under the join screen) with
	// no screen up.
	bool in_world() const;
	// The tooling's state name: "menu", "world", or the screen's (menu::mission_screen_state_name).
	const char *state_name() const;

	// --- the MainMenu ---
	// The MainMenu's init over the embedder's mount: game.cfg read (its mpreset ends the process:
	// false, the exit leg ran), the options' copy, the profile, the first start's intro tail (its
	// +1412 clear and the profile's save), the menu scene's creation with its cookie jar on the
	// first entry, then the menu shown. The mode's init resets the menu bank and its clock.
	// [orig: Menu_InitShellResources @0x552500 - Game_LoadConfig @0x552534, the gamma and the
	//  render settings @0x552543..0x552561, PlayerProfile_LoadAllFromDisk @0x552569, the intro
	//  latch dword_83B3FC @0x55256E..0x55257B (Game_PlayIntroVideos @0x5637A0: the +1412 clear
	//  @0x56385E..0x563876, PlayerProfile_SaveToFiles @0x56387C), the scene's creation
	//  @0x5525A6..0x552668 (CookieJar_OpenAndLoadFile @0x552630); Game_MainLoop @0x52B70F]
	bool enter_main_menu();
	// The Mods list's game switch: the block saved and read again over the new mount (mpreset:
	// false, the exit leg ran), the menu made anew with its cookie jar, the options' copy and the
	// profile again.
	// [orig: Game_ReloadExpansionAndMods @0x552710 - Game_SaveConfig @0x55272A,
	//  Menu_InitShellResources @0x55281A over the destroyed scene]
	bool reload_main_menu();

	// --- mission starts ---
	// Single player's start: the started mission's campaign index into the current record, the
	// block saved, its attribute word stored after the save, the profile saved, then the start.
	// [orig: SinglePlayer_StartMission @0x561AF0 - the campaign index @0x561B99, Game_SaveConfig
	//  @0x561BB2, the attribute store @0x561BB7, Profile_ApplySessionControls @0x561BC1,
	//  PlayerProfile_SaveToFiles @0x561BF5, UI_NavHistoryPush(&g_GameModeGameLoop) @0x561E85]
	bool start_single_player();
	// The common mission start. A session's (`in_session`) runs the PreMenu's init, whose game.cfg
	// save comes at every multiplayer start (a host's PreMenu pushes the Game Loop at once, so the
	// mode goes straight there), and its session apply clamps the record's class bytes; a single
	// player's is remembered as the restart's entry. Then the load: the session's
	// +1460 copy, the record's controls, the presentation, the Game Loop. False when the load
	// coordinator refused the start (nothing changed) or its handoff failed (the failure leg ran).
	// [orig: PreMenu_Init @0x5693E0 - Profile_ApplySessionControls @0x5693EF, Game_SaveConfig
	//  @0x5693F4; Game_ApplySessionSettingsToGlobals - the +1460 copy @0x55163C, the class clamp
	//  @0x5516D0..0x5516EC]
	bool start_mission(bool in_session, bool restart = false);
	// A join's start: the join screen up over the PreMenu's wait (it cannot open: the common
	// start), the menu music stopped, the session's saves, the load under the join screen.
	// [orig: MultiPlayer_JoinSessionStateMachine @0x56A320; PreMenu_Init @0x56944A (pre.mnu)]
	bool start_join();
	// The host started the game: the PreMenu pushes the Game Loop
	// [orig: MultiPlayer_JoinSessionStateMachine - PRE_CONNECTED, the push @0x56A91D].
	void join_game_starting();
	// The launch's own mission (`--mission`, the editor's Play mission), named on the log when its
	// load fails.
	void set_launch_mission(std::string name) { launch_mission_ = std::move(name); }
	// The load's world stands. Its mission start's input reset: a joiner's runs later, at its
	// game-start wait's end. The reveal of the world and the HUD follows at the mission start's
	// tail, a single player's start-mission splash ahead of it holding the world un-ticked until
	// its dismissal (Game_StartMission has not yet returned); for a joiner this is the
	// wire-header world's construction, its reveal the admission's.
	// [orig: LoadingScreen_ReleaseEffect @0x586B80, the final call @0x525D45;
	//  Game_ShowStartMissionSplash @0x520820 ahead of it; the first tick past @0x525D42]
	MissionStarted mission_loaded(bool joiner, bool joined_in_match);
	// The load's presentation ends (the reveal). True when a load was up.
	bool finish_load();
	// The load failed or was aborted: a join with its screen up keeps it (the connection's reason,
	// Cancel alone); every other failure aborts to the menu, the launch's mission named first
	// (take_launch_mission) [orig: Client_CheckDisconnectOrEscDuringLoad @0x520270 - the
	//  "Mission loading aborted" legs; the reason-1 Post Menu push out of Game_StartMission].
	LoadFailure load_failed(bool join_screen_open);
	// The launch's mission while its load has not yet stood (once: it is cleared), else "".
	std::string take_launch_mission();
	bool has_restart_entry() const { return restart_entry_; }

	// --- the mission's exits ---
	// An established session ended without the player asking, or the mission stored its exit:
	// the main frame's route over the stored reason (an unmapped loss leaves as the player's own
	// quit), then the Post Menu's.
	// [orig: CNapiNPConnection_HandleDescriptionPacket @0x621AE0 and the 120000 ms reap
	//  CNapiNetwork_Init @0x4CA4A0, both -> CNapiNetwork_OnDisconnectedFromServer @0x4C63D0; the
	//  captured DPC 33's Input_QueueEvent(3) @0x4C67A4, reason 1 @0x49AF26; Game_ProcessMainFrame
	//  @0x526806..0x526867 (inmatch::main_frame_exit); PostMenu_RouteMissionExit @0x568460 -
	//  every exit reason lands on the same teardown and MainMenu push @0x568654]
	ExitDecision route_exit(const MissionExit &exit) const;
	// The mission teardown's profile step: outside a session only once the round is over, the
	// record's +1460 back, then the profile's save
	// [orig: PlayerProfile_SaveRoundEndProgress @0x54D6A0].
	void record_round_end(bool in_session, bool round_over);
	// The Game Loop's teardown: the screen up closes with the mission
	// [orig: Game_TeardownMission @0x522350 - UI_CloseMenuScreen @0x5225CD].
	void teardown_mission();
	// The mission is left for the menu: the Post Menu's init (the load and the screen go).
	// [orig: PostMenu_Init @0x568360]
	void enter_post_menu();

	// --- the in-mission screens ---
	// A screen's open edge (its presenter put it up): false when another screen is up
	// (UI_OpenMenuScreen's refusal). Every in-mission screen is up over live play: the world
	// keeps ticking under it (the armory leg has no world-stop leg, and a listen host that froze
	// would freeze every peer), only the cursor is released and the player's input idles; only
	// the single player's pause holds the tick. A net session never freezes: its world tick is the
	// session socket's only pump, and a frozen tick sends nothing, so a stock peer drops the
	// connection at its 120 s timeout.
	// [orig: Input_HandleActionBinding_0 case 0xB1 @0x4E0B3F -> UI_OpenMenuScreen("weapon.mnu",
	//  "WEAPON"), the parallel action 218 @0x49B8E3 shipping with no binding row;
	//  cs_dir0.timeout_ms = 120000, CNapiNetwork_Init @0x4CA4A0]
	bool open_screen(menu::MissionScreen screen);
	// A screen's close edge: closed only when it is the one up.
	ScreenClose close_screen(menu::MissionScreen screen);
	// The in-game menu (game.mnu INGAME) over live play; false when play is not live.
	// [orig: Input_HandleActionBinding case 18 - UI_OpenMenuScreen("game.mnu", "INGAME") @0x49B3B6]
	bool pause();
	// INGAME's or WEAPON's close back to play; false when neither is up.
	bool resume();
	EscapeRoute escape(bool join_screen_open, bool round_ended, bool tools_interacting) const;
	// The held USE row's armory and vehicle arms past an update's sample: an open screen (a
	// pass-through one, the drain's) closes instead; over live play the armory opens, the vehicle
	// screen past its team test.
	// [orig: Input_HandleActionBinding_0 @0x4E0420 - UI_IsMenuScreenOpen @0x4E0B35, the close
	//  @0x4E0AC6, the vehicle open @0x4E0AFD, the armory open @0x4E0B49]
	UseItemRoute use_item(bool armory, bool vehicle, bool vehicle_team_matches) const;
	// The weapon-screen toggle (case 40): an open screen closes, else WEAPON opens.
	// [orig: UI_OpenWeaponScreenSinglePlayer @0x424390 - UI_CloseMenuScreen @0x4DDCF2,
	//  UI_OpenMenuScreen("weapon.mnu", "WEAPON") @0x4DDD03]
	UseItemRoute weapon_screen_toggle() const;

	// --- the input frame's gates (`tools_interacting`: the dev tools' interact mode keeps the input) ---
	// Gameplay input is live: play with the round not over.
	bool gameplay_input_active(bool round_ended, bool tools_interacting) const;
	// The keys belong to the input frame's keyboard pass: over play and under a pass-through
	// screen.
	bool keyboard_pass_owns_keys(bool tools_interacting) const;
	// The screen up runs the input frame under it (its pass-through word holds).
	bool screen_input_frame(bool tools_interacting) const;
	// This frame's input frame drains the key queue: play or a passing screen, never a load.
	bool input_frame_drains_keys(bool tools_interacting) const;
	// The chat line owns the keyboard for this frame's samplers.
	bool keyboard_captured(bool chat_capturing, bool tools_interacting) const;
	// The HUD's frame runs: play, or under a screen other than INGAME.
	bool hud_frame_runs() const;

	// --- the main loop's ends ---
	// A quit: the main loop's exit saves game.cfg and the menu's teardown on the way out its cookie
	// jar [orig: Game_Run @0x4A7FFF; Menu_TeardownShellAndCloseBinkVideos @0x54E4AA].
	QuitRequest request_quit();
	bool quit_requested() const { return quit_requested_; }
	// The quit's music drain (a repeat request ends it at once).
	void begin_quit_drain() { quit_drain_pending_ = true; }
	// The drain ended: true when it was still pending (the quit then finishes).
	bool end_quit_drain();
	bool quit_drain_pending() const { return quit_drain_pending_; }
	// The mpreset exit's latch: true the first time (the process then ends with nothing saved).
	bool begin_process_exit();
	bool process_exit_pending() const { return process_exit_pending_; }
	// The main menu's EXIT (or a started updater): the profile and game.cfg saved before the quit
	// [orig: UI_StartupExit_OnPress @0x555760 - PlayerProfile_SaveToFiles @0x555771, Game_SaveConfig
	//  @0x555776, the null mode pushed @0x555787].
	void exit_to_desktop();
	// A mount that failed ends after the main loop's save of game.cfg.
	void fail_boot();
	// The in-game options' Accept: the profile and game.cfg saved [orig: Game_SaveConfig
	// @0x555310].
	void save_options();

	// --- the menu mode's bank ---
	// One menu frame on the main loop's bank: the frame's elapsed time banked (locked when the
	// block's lock_framerate word is set) into the menu updates it drains. Retail runs the
	// MainMenu's update, the PLAYER_INFO portrait's step among its legs, once per drained update.
	// [orig: Game_MainLoop @0x52B630 - the drain's update calls @0x52BA5A; Menu_UpdateFrame
	//  @0x5528A0 - PlayerInfo_UpdatePlayerPreviewAnimation @0x5528F7]
	int bank_menu_frame();
	// The menu updates the last menu frame drained.
	int menu_updates() const { return menu_updates_; }

private:
	void push_mode(Mode mode);
	void begin_load(bool join, bool restart);
	profile::PlayerProfiles *profiles() const;
	inmatch::GameConfigBlock *game_config() const;
	void call(const std::function<void()> &leg) const {
		if (leg) leg();
	}

	FrontendLegs legs_;
	Mode mode_ = Mode::None;
	bool loading_ = false;
	menu::ScreenStack screens_;
	bool intro_pending_ = true;  // dword_83B3FC: the first start's intro step is owed
	bool menu_scene_made_ = false; // g_GameMenu: the menu scene stands
	bool restart_entry_ = false;
	std::string launch_mission_;
	bool quit_requested_ = false;
	bool quit_drain_pending_ = false;
	bool process_exit_pending_ = false;
	inmatch::MainLoopClock menu_clock_;
	world::TickAccumulator menu_bank_;
	int menu_updates_ = 0;
};

} // namespace opennova::frontend
