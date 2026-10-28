[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("RR", "RO", "OR", "OO")]
    [string] $Topology,

    [Parameter(Mandatory = $true)]
    [string] $Mission,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
    [int] $Port,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(?:auto|[0-9]+)$')]
    [string] $GameType,

    [Parameter(Mandatory = $true)]
    [string] $HostSessionName,

    [Parameter(Mandatory = $true)]
    [ValidateSet("in_match", "deploy_hold")]
    [string] $ReadinessMode,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,54}[A-Za-z0-9])?$')]
    [string] $RunId,

    # Up to five minutes: E5's capture case holds a base for 150 s. The suite
    # refuses a case outside this range when it loads the table.
    [ValidateRange(5, 300)]
    [int] $SteadySeconds = 15,

    [switch] $WireOnly,

    [switch] $AutoDeploy,

    # A mission with deploy zones (the case table's deploy_zones): its joiner
    # joins undeployed, on the deploy screen [orig: Server_OnPlayerJoin
    # @0x51A6E9..0x51A6F2, the slot's bit 0x10 iff SpawnZoneList_GetCount() is
    # nonzero], which the host holds open through the S2C 0x0A flags1 bit 1
    # [orig: NapiNPClientMsg_0x00A @0x42FF7A..0x42FF82, g_DeployScreenActive].
    # An in_match auto-deploy cell deploys a retail joiner with onHook's
    # deploy (SPACE until a local player stands): the screen's SPACE is the
    # auto team pick [orig: Input_HandleSpecialKeys, the gate @0x49C9DA, the
    # SPACE @0x49CA06..0x49CA14: Input_QueueEvent(12, 0xFFFE)], which an
    # OpenNova joiner sends through parity_joiner_ready's auto_deploy. onHook's
    # deploy defaults off for jo, so without it a retail joiner never stands
    # and its in_match never comes (x5's cp04-objcoop-revive RR / RR-aa). A
    # listen host's own slot joins undeployed too [orig: Server_OnPlayerJoin
    # @0x51A6E9..0x51A6F2, +0x15F38 bit 0x10], but a JO host's Person stands
    # from the mission start, so onHook's deploy never presses for it and the
    # host stays on its deploy screen, which drops its look rows (0x0C100425 /
    # 0x0C200425) [orig: Input_HandleActionBinding @0x49AD40, flag 0x8000000 against
    # g_DeployScreenActive @0x49ADC8..0x49ADD7] (p195's revive RR: the medic's
    # first look never turned it). A scenario whose retail host acts deploys it
    # with its prelude's SPACE, as a player does (medic_revive, capture_base).
    [switch] $DeployZones,

    [switch] $ExerciseInput,

    # A network-parity scenario (scripts/net/scenarios/*.json,
    # opennova.scenario.v1; ADR 0050 R5). At the steady-window start its
    # actor (the joiner or the host) plays its prelude, takes the setup pose
    # and plays the scripted steps -- a retail actor through onHook's
    # virtual keyboard, an OpenNova actor through the scenario_play probe --
    # then the run settles for the scenario's settle_seconds. The summary
    # records every step's applied logic tick.
    [string] $Scenario = "",

    # Diagnostic: a JSON file of game probes, [{"name": "<probe>", "args":
    # {...}}], that run in turn on an OpenNova actor's MCP port while its
    # scenario plays (a probe watch such as use_press_trace's watch_handle or
    # body_trace's watch_handles lasts only while its run does, and the game
    # runs one probe at a time): scenario_play starts with wait false, each
    # probe runs to its end, then a scenario_play with neither scenario nor
    # path collects the script's record. Every probe's final status lands in
    # <run>\scenario-probes.json. A retail actor ignores it.
    [string] $ActorScenarioProbes = "",

    # L2-s22's load probe (D-NET-861): --verbose on our joiner's Godot
    # command line, so its godot.log carries the LoadTimeline: lines (each a
    # print_verbose). Nothing else in the cell changes; a retail joiner
    # ignores it.
    [switch] $JoinerVerbose,

    # Round end and rotation (extension E2): the missions after -Mission in
    # the host's rotation, .BMS basenames in play order (a comma-separated
    # list is accepted). A retail host takes them through onhook_host_lan's
    # rotation (the stock host screen's selected-missions list), the OpenNova
    # listen host through --lan-rotation (host_opennova.ps1 -Rotation), each
    # appended at launch option 0.
    [string[]] $Rotation = @(),

    # E2: the round time limit in minutes for this cell. Both hosts read it
    # from the SERVER copy's game.cfg time_limit (retail:
    # Game_ApplySessionSettingsToGlobals @0x551CCC..0x551CD2 copies it to the
    # misnamed g_RoundTimeLimitMinutes, and Game_StartMission @0x525242..0x525251 starts
    # a round that is not co-op with g_RoundTimeRemaining = it * 0xE88 ticks,
    # which Server_CheckWinConditions @0x51AF6E ends at zero; koth_limit is
    # the king-of-the-hill hold, @0x51AF0F..0x51AF18), so the runner writes it
    # there for the cell and restores the file's exact bytes once every
    # process has stopped. 0 keeps the copy's own value and leaves the file
    # alone.
    [ValidateRange(0, 600)]
    [int] $TimeLimitMinutes = 0,

    # E5's permanent death: the SERVER copy's game.cfg mp_permanent_death = 1
    # for this cell, restored with the copy's files after it. Both hosts read
    # it there (retail: g_GameConfigState.permanentDeath_5E4, which becomes
    # g_SessionPermanentDeath on the authority @0x5522AA; ours: host_config's
    # permanent_death), so every topology takes it.
    [switch] $PermanentDeath,

    # D-NET-167's acceptance: the host's side passwords for this cell, the
    # SERVER copy's game.cfg string keys mphostsidepassworda / b (16
    # characters at most; empty keeps the copy's), restored with the copy's
    # files after it. Both hosts read them there (retail's game settings
    # +0x40 / +0x60, which raise the session's side-password bits 0x20 / 0x10
    # and answer a wrong side password with join reject 19 / 20; ours:
    # host_config's side_a_password / side_b_password), so every topology
    # takes them.
    [string] $HostSidePasswordA = "",
    [string] $HostSidePasswordB = "",

    # D-NET-167's acceptance, the joiner half: the side password a RETAIL
    # joiner enters on the join screen (onhook_join_lan side_password,
    # opennova-int #78: it clicks TEAM_PASSWORD, types it and presses Enter),
    # 16 characters at most; the cell then requires the tool's
    # side_password_entered (false: the host never advertised the side
    # bits 0x30, a finding). RR and RR-aa launch their roles one by one for
    # it (onhook_run_lan_pair takes none). RO and OO enter it on our joiner's
    # join screen through the join_screen probe (Enter-OpenNovaJoinerSidePassword),
    # with the same requirement.
    [string] $JoinerSidePassword = "",

    # E5's medic: the host player's class for this cell (5..9; 0 keeps the
    # copy's). The class is the current weapon.sav record's side byte
    # (+0 blue, +0x8006 red), which the session start clamps to 5..9 into
    # g_CharClassTeam1/2 [orig: Game_ApplySessionSettingsToGlobals
    # @0x5516BB..0x551720] and Game_StartMission hands the local player
    # [orig: @0x524EE3..0x524F23 -> PlayerSession_InitFromProfile, the slot
    # class @0x50CB7C]. The runner writes both side bytes, as the PLAYER_INFO
    # dialog's class loop does [orig: PlayerInfo_SaveFromDialog
    # @0x55EE3F..0x55EE6D], in the SERVER copy's active weapon.sav (the
    # expansion's when one is active [orig: PlayerProfile_LoadAllFromDisk
    # @0x54F68C..0x54F6B7]) at game.cfg's player_index; the cell's restore
    # takes the file back. A retail host only: whether the OpenNova listen host
    # takes its class from that record is unchecked, so OR and OO refuse it.
    [ValidateSet(0, 5, 6, 7, 8, 9)]
    [int] $HostPlayerClass = 0,

    # A join after the host's second mission start (L8's admin exercise A13
    # with n >= 2: the S2C 0x02 probe's word 1 counts the authority's mission
    # starts). The host's remote-admin console is armed for the cell (the
    # SERVER copy's game.cfg remote_admin_port = -HostAdminPort and an
    # admin.cfg there with one full-rights user, 127.* only; both host kinds
    # read them there, and the cell's restore takes both files back). Once the
    # host is up, the runner sends MISSION CYCLE, which ends the round with no
    # winner and lingers 620 ticks before the next mission (engine
    # inmatch/admin_console.cpp cycle_tail); the joiner starts once the host's
    # logic tick has restarted and run past 310 ticks with its own player
    # standing again. Every topology.
    [switch] $PreJoinMissionCycle,
    [ValidateRange(1, 65535)]
    [int] $HostAdminPort = 4321,

    # The retail host as retail's Serve Only (dedicated) server
    # (onhook_host_lan serve_only, connection mode 1). Its console line runs
    # the BAN / PUNT verbs only on an authority that is not a session peer
    # [orig: Client_ProcessConsoleCommand @0x520E80 (Server_HandleBanPuntCommand
    # @0x520F63)]; a listen host is a peer and queues the line to its own server
    # as C2S 0x04 [orig: @0x520EFE..0x520F29], whose handler runs NETLOG and STAT
    # only [orig: NapiNPServerMsg_HandleConsoleLine @0x5199D0] and echoes the
    # rest, so E4's kick stages a listen host's SERVER PUNT ALL instead. The
    # OpenNova host (OR, OO) serves only when its game.cfg's server type says so
    # (host_screen_from_block: serve and play iff `dedicated` is 0), so the cell
    # writes the SERVER copy's `dedicated` = 1 (restored after); its readiness
    # takes a loaded world hosting on the port with no own player. Every Serve
    # Only cell saves its host's frame on the round's end (the capture's first
    # S2C 0x1D plus 1 s, or the window's start when the round already ended:
    # serve-only-round-end-frame.png, the dedicated host's round-end overlay)
    # and again at the steady deadline, before the closing checks
    # (serve-only-host-frame.png), so the linger's length shows.
    [switch] $ServeOnly,

    # Which program is a Serve Only cell's OpenNova host (OR, OO): `game`,
    # opennova.exe with the SERVER copy's game.cfg `dedicated` = 1 (above);
    # `serve`, the headless opennova-serve (ADR 0051), the program a hosted
    # server runs, from build\apps\serve\Release. Both run over the SERVER copy
    # as their working directory (its game.cfg, `dedicated` = 1 written for
    # either, admin.cfg, player.sav, weapon.sav, each restored after), so the
    # two hosts' cells differ in the host program alone. opennova-serve takes
    # the cell's map, rotation, mode and player cap through a host file
    # (host\parity.host) where the game takes launch flags, binds -Port, and is
    # ready once its console logs `serving on UDP <port>` (host\serve.log); it
    # has no window, so its frames are not saved, and scripts\net\serve_host.py
    # stops it as its console's Ctrl+Break does (the session's STOP goodbye).
    # The summary records the program (opennova_host_kind).
    [ValidateSet("game", "serve")]
    [string] $OpenNovaHostKind = "game",

    # The pinned SHA-256 of the SERVER copy's game.cfg (the case table's
    # server_game_cfg_sha256): the cell refuses a copy whose game.cfg differs
    # at its start. Empty skips the check (a diagnostic run).
    [ValidatePattern('^(?:|[0-9a-f]{64})$')]
    [string] $ServerGameCfgSha256 = "",

    # Late join (extension E3): a second joiner of the first joiner's kind
    # joins during the steady window, after the first joiner's scripted input,
    # so the host serves a join with a remote player already in the match and
    # the first joiner sees a player arrive. RR and OR: a retail late joiner
    # (onhook_join_lan) from -RetailLateClientRoot (default the CLIENT copy,
    # launched after the first joiner read its onhook.cfg). An OpenNova late
    # joiner (RO, OO) is not wired yet, so those refuse it.
    [switch] $LateJoiner,
    [string] $RetailLateClientRoot = "",
    [string] $LateJoinerCallsign = "ParityLate",

    [ValidatePattern('^[1-9][0-9]{2,4}x[1-9][0-9]{2,4}$')]
    [string] $Resolution = "1920x1080",

    [string] $HostCallsign = "ParityHost",
    [string] $JoinerCallsign = "ParityJoin",

    [string] $SuiteId = "",
    [string] $CorpusFingerprint = "",
    [string] $MissionArtifactPath = "",
    [string] $MissionSha256 = "",

    [Parameter(Mandatory = $true)]
    [string] $RetailServerRoot,

    [Parameter(Mandatory = $true)]
    [string] $RetailClientRoot,

    [Parameter(Mandatory = $true)]
    [string] $OnHookMcpPath,

    [Parameter(Mandatory = $true)]
    [string] $GodotLauncher,

    [ValidateRange(1, 65535)]
    [int] $HostMcpPort = 8975,

    [ValidateRange(1, 65535)]
    [int] $JoinerMcpPort = 8976,

    # The retail expansion both roles load. The pinned reference install
    # (ADR 0050 d2) is JO:CA's jox01.
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,30}$')]
    [string] $Expansion = "jox01",

    # Diagnostic only: accept OpenNova launches through the plain runtime
    # executable (no console-wrapper job). A verdict-bearing run needs the
    # wrapper's ownership proof; the summary records the relaxation.
    [switch] $AllowDirectRuntime,

    # Diagnostic only: launch retail with onHook's behavior-changing hooks as
    # its onhook.cfg defaults them. A verdict-bearing run loads every retail
    # role with StockObserver (retail stock, the hook only observing) and
    # requires the loaded hook to confirm it; the summary records the
    # relaxation.
    [switch] $AllowModifiedRetail,

    # How the OpenNova joiner (RO, OO) reaches the host. `browse` searches as
    # the LAN screen does and joins the row on -Port (`--lan-join
    # browse:<port>`), as onHook's retail joiner does, so its 0x41s are the
    # broadcast announces a retail joiner sends. `direct` dials
    # 127.0.0.1:<port>, whose 0x41 names the dialed host as its EIP: a
    # diagnostic only. The summary records it (joiner_dial).
    [ValidateSet("browse", "direct")]
    [string] $JoinerDial = "browse",

    # The machine-load policy's build lock (a directory whose owner file names
    # the lane building and since when): the cell's machine_load records its
    # owner while the lock is held. Empty: the lock is not looked at.
    [string] $BuildLockPath = "",

    # Diagnostic only: run from a checkout with uncommitted tracked changes.
    # A verdict-bearing run is evidence for its repo_commit alone.
    [switch] $AllowDirtyRepo
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if ([string]::IsNullOrWhiteSpace($HostSessionName)) {
    throw "HostSessionName must contain at least one non-whitespace character."
}
if ($HostSessionName.Length -gt 31 -or $HostSessionName -notmatch '^[\x20-\x7e]+$') {
    throw "HostSessionName must be at most 31 printable ASCII characters."
}
$ServerName = $HostSessionName
if ($ReadinessMode -eq "deploy_hold" -and ($AutoDeploy -or $ExerciseInput)) {
    throw "deploy_hold readiness cannot auto-deploy or exercise input."
}
if ($JoinerSidePassword -and $JoinerSidePassword -cnotmatch '^[\x21\x23-\x7E]{1,16}$') {
    throw "-JoinerSidePassword is 1-16 printable ASCII characters, no space or quote."
}
if ($DeployZones -and -not $AutoDeploy) {
    throw "-DeployZones deploys an in_match auto-deploy cell's retail roles; a deploy_hold cell holds them."
}
if ($WireOnly -and $Topology -ne "OR") {
    throw "WireOnly is an OR-only diagnostic mode."
}
if ($OpenNovaHostKind -eq "serve" -and (-not $ServeOnly -or $Topology -notin @("OR", "OO"))) {
    throw "-OpenNovaHostKind serve is a Serve Only OR / OO cell's host (opennova-serve has no player of its own)."
}
if ($OpenNovaHostKind -eq "serve" -and ($PreJoinMissionCycle -or $WireOnly)) {
    throw "-OpenNovaHostKind serve takes no -PreJoinMissionCycle or -WireOnly."
}
# ConvertFrom-Json yields Int32/Int64 for integers and Double (pwsh) or
# Decimal (Windows PowerShell) for the rest.
function Test-JsonInteger {
    param($Value)
    return $Value -is [int] -or $Value -is [long]
}

function Test-JsonNumber {
    param($Value)
    return (Test-JsonInteger $Value) -or $Value -is [double] -or $Value -is [decimal]
}

# opennova.scenario.v1: the case it is written for (mission, numeric game
# type, actor), an optional prelude stage, an optional setup pose, the steps
# both drivers consume verbatim, and the settle after the last one. The drivers validate each step;
# this binds the scenario to the run's case.
function Read-ParityScenario {
    param([Parameter(Mandatory = $true)] [string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Scenario file is missing: $Path"
    }
    $spec = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $expected = @("schema", "name", "mission", "game_type", "actor", "prelude",
        "setup", "staging", "staging_tick", "steps_tick", "steps", "settle_seconds", "expect", "ends_joiner_session")
    $extra = @($spec.PSObject.Properties.Name | Where-Object { $_ -notin $expected })
    if ($extra.Count -gt 0) {
        throw "Scenario $Path has unknown fields: $($extra -join ', ')"
    }
    if ([string] $spec.schema -cne "opennova.scenario.v1" -or
            [string]::IsNullOrWhiteSpace([string] $spec.name)) {
        throw "Scenario $Path is not a named opennova.scenario.v1 document"
    }
    $missionStem = [IO.Path]::GetFileNameWithoutExtension($Mission)
    $specStem = [IO.Path]::GetFileNameWithoutExtension([string] $spec.mission)
    if ($specStem -ine $missionStem) {
        throw "Scenario $($spec.name) is written for mission $($spec.mission), not $Mission"
    }
    if ([string] $spec.game_type -cne $GameType) {
        throw "Scenario $($spec.name) is written for game type $($spec.game_type), not $GameType"
    }
    if ([string] $spec.actor -cnotin @("joiner", "host")) {
        throw "Scenario $($spec.name): the actor is the joiner or the host (actor '$($spec.actor)')"
    }
    $steps = @($spec.steps)
    if ($steps.Count -lt 1 -or -not ($steps[-1].PSObject.Properties.Name -contains "end")) {
        throw "Scenario $($spec.name) must end with an end step"
    }
    if ($spec.PSObject.Properties.Name -contains "prelude") {
        $prelude = @($spec.prelude)
        if ($prelude.Count -lt 1 -or -not ($prelude[-1].PSObject.Properties.Name -contains "end")) {
            throw "Scenario $($spec.name): the prelude must end with an end step"
        }
        if (@($prelude | Where-Object { $_.PSObject.Properties.Name -contains "aim_at" }).Count -gt 0) {
            throw "Scenario $($spec.name): aim_at plays in the steps, not the prelude"
        }
    }
    # An aim_at step {"tick": t, "aim_at": "0x0019"} turns the actor onto that
    # pool-0 entity: the runner plays the steps before it, reads that side's
    # actor heading and position, the target's position and the actor's mouse
    # sensitivity word, and plays the rest after one look_px computed as
    # retail computes the look yaw (Invoke-ScenarioAimStage,
    # scripts/net/scenario_aim.py). A played part's end releases every held
    # code, so no code is held across an aim_at. The target may also be the
    # other player by role ("joiner" or "host"): its body by DcbId, not by a
    # handle one capture saw (Resolve-ScenarioAim).
    $held = @{}
    $pressReleased = 0
    foreach ($step in $steps) {
        $names = @($step.PSObject.Properties.Name)
        if ($names -contains "down") { $held[[string] $step.down] = $true }
        elseif ($names -contains "up") { $held.Remove([string] $step.up) }
        elseif ($names -contains "press") { $pressReleased = [Math]::Max($pressReleased, [int] $step.tick + 1) }
        elseif ($names -contains "aim_at") {
            $other = if ([string] $spec.actor -eq "host") { "joiner" } else { "host" }
            if ($names.Count -ne 2 -or ([string] $step.aim_at -notmatch '^0x0?[0-9a-fA-F]{1,3}$' -and
                    [string] $step.aim_at -cne $other)) {
                throw "Scenario $($spec.name): an aim_at step is {tick, aim_at} naming a pool-0 handle 0x0000..0x0FFF or the other player's role ($other)"
            }
            $retailActor = ([string] $spec.actor -eq "joiner" -and $Topology -in @("RR", "OR")) -or
                ([string] $spec.actor -eq "host" -and $Topology -in @("RR", "RO"))
            if ([string] $step.aim_at -ceq $other -and -not $retailActor) {
                throw "Scenario $($spec.name): aim_at $other resolves on a retail actor's pool read, and $Topology's $($spec.actor) is OpenNova's"
            }
            if ($held.Count -gt 0 -or $pressReleased -gt [int] $step.tick) {
                throw "Scenario $($spec.name): code(s) $(@($held.Keys) -join ', ') held (or a press released) across the aim_at at tick $($step.tick)"
            }
            if ($ActorScenarioProbes) {
                throw "Scenario $($spec.name) aims (aim_at steps), which plays in parts: no -ActorScenarioProbes"
            }
        }
    }
    $settle = $spec.settle_seconds
    if (-not (Test-JsonInteger $settle) -or $settle -lt 0 -or $settle -gt 300) {
        throw "Scenario $($spec.name): settle_seconds must be an integer from 0 to 300"
    }
    if ($spec.PSObject.Properties.Name -contains "setup" -and $null -ne $spec.setup) {
        $pose = $spec.setup.pose
        $position = @($pose.position_bms)
        if ($position.Count -ne 3 -or
                @($position | Where-Object { -not (Test-JsonNumber $_) }).Count -gt 0 -or
                -not (Test-JsonNumber $pose.yaw) -or -not (Test-JsonNumber $pose.pitch)) {
            throw "Scenario $($spec.name): setup.pose needs position_bms [x, y, z], yaw and pitch numbers"
        }
    }
    # E5's staging: positions the host gives players' slots before the actor
    # plays (a joiner's own pose would not stick, above), on every topology
    # (Invoke-ScenarioStaging). A staged slot takes a position only: its own
    # client puts its heading back within one frame (the E5 pose-stick
    # probe), so a scenario declares none.
    if ($spec.PSObject.Properties.Name -contains "staging") {
        $staging = @($spec.staging)
        if ($staging.Count -lt 1) {
            throw "Scenario $($spec.name): staging lists at least one slot position"
        }
        foreach ($entry in $staging) {
            $position = @($entry.position_bms)
            $fields = @($entry.PSObject.Properties.Name)
            if (-not (Test-JsonInteger $entry.slot) -or $entry.slot -lt 0 -or $entry.slot -gt 254 -or
                    $position.Count -ne 3 -or
                    @($position | Where-Object { -not (Test-JsonNumber $_) }).Count -gt 0 -or
                    @($fields | Where-Object { $_ -notin @("slot", "position_bms") }).Count -gt 0) {
                throw "Scenario $($spec.name): each staging entry is a slot 0..254 and its position_bms [x, y, z], no heading"
            }
        }
    }
    # R-L4-s32-1 / R-LX-s26-1: the host tick the staging lands on, {value,
    # tolerance, why} (Invoke-ScenarioStaging): every topology stages when the
    # host's tick, counted from its mission start, reaches `value`, and a
    # staging that lands more than `tolerance` ticks off it fails the cell (so
    # does a host already past it: the tick never comes back). Both hosts zero
    # the tick at the mission start and step it once a frame, so one value is
    # one condition on both. The wait for it is at most 50 s on either side
    # (Wait-RetailHostTick, the stage_slot probe). A scenario with a prelude
    # stages after it, inside the window (medic_revive, R-LX-s28-2), so its
    # `value` sits past every cell's prelude end.
    if ($spec.PSObject.Properties.Name -contains "staging_tick") {
        $hostTick = $spec.staging_tick
        if (-not ($spec.PSObject.Properties.Name -contains "staging")) {
            throw "Scenario $($spec.name): staging_tick times a staging, and it lists none"
        }
        $fields = @(if ($hostTick -is [pscustomobject]) { $hostTick.PSObject.Properties.Name })
        if ((@($fields | Sort-Object) -join ",") -cne "tolerance,value,why" -or
                -not (Test-JsonInteger $hostTick.tolerance) -or $hostTick.tolerance -lt 1 -or $hostTick.tolerance -gt 64 -or
                -not (Test-JsonInteger $hostTick.value) -or $hostTick.value -le $hostTick.tolerance -or $hostTick.value -gt 65535 -or
                $hostTick.why -isnot [string] -or [string]::IsNullOrWhiteSpace($hostTick.why)) {
            throw "Scenario $($spec.name): staging_tick is {value tolerance+1..65535, tolerance 1..64, why}"
        }
    }
    # R-LX-s29-1 (b'): the host tick the main stage's aim parts play on, {value,
    # tolerance, why} (Invoke-ScenarioAimStage): each part that starts at an aim_at
    # plays its relative tick 0 on host tick value + that aim's tick, so an actor's
    # play keeps one place against the camp AI's think keys as the staging does
    # (the runner's reads and calls between parts take 32..52 ticks and vary). The
    # part before an aim ends one tick past its last step, so the aim's reads fall
    # in the script's gap, and a part whose reads end past its tick fails the cell.
    # An aim_at at tick 0 resolves before the staging, its actor from the setup
    # pose (Resolve-ScenarioFirstAim). The parts play through onhook_play_input on
    # the retail host's own tick, so the actor is the host on RR or RO.
    if ($spec.PSObject.Properties.Name -contains "steps_tick") {
        $partTick = $spec.steps_tick
        $fields = @(if ($partTick -is [pscustomobject]) { $partTick.PSObject.Properties.Name })
        if ((@($fields | Sort-Object) -join ",") -cne "tolerance,value,why" -or
                -not (Test-JsonInteger $partTick.tolerance) -or $partTick.tolerance -lt 1 -or $partTick.tolerance -gt 64 -or
                -not (Test-JsonInteger $partTick.value) -or $partTick.value -le $partTick.tolerance -or $partTick.value -gt 65535 -or
                $partTick.why -isnot [string] -or [string]::IsNullOrWhiteSpace($partTick.why)) {
            throw "Scenario $($spec.name): steps_tick is {value tolerance+1..65535, tolerance 1..64, why}"
        }
        if (@($steps | Where-Object { $_.PSObject.Properties.Name -contains "aim_at" }).Count -eq 0) {
            throw "Scenario $($spec.name): steps_tick times the parts an aim_at starts, and the steps hold none"
        }
        if ([string] $spec.actor -cne "host" -or $Topology -notin @("RR", "RO")) {
            throw "Scenario $($spec.name): steps_tick plays its parts on the retail host's own tick: a host actor on RR or RO, not the $($spec.actor) on $Topology"
        }
        if ($spec.PSObject.Properties.Name -contains "staging_tick" -and
                $partTick.value - $partTick.tolerance -le $spec.staging_tick.value + $spec.staging_tick.tolerance) {
            throw "Scenario $($spec.name): steps_tick's parts follow the staging: its value - tolerance must lie past staging_tick's value + tolerance"
        }
    }
    # A menu click is onHook's (click: opennova-int #75, a named menu widget
    # the menu's own pump clicks). The OpenNova scenario_play driver
    # (ScriptedInput) plays down, up, press, look_px, char (a keystroke in its
    # tick's input frame, as onHook's char: #73) and end, so an OpenNova actor
    # refuses a clicking script.
    $openNovaActor = ([string] $spec.actor -eq "joiner" -and $Topology -in @("RO", "OO")) -or
        ([string] $spec.actor -eq "host" -and $Topology -in @("OR", "OO"))
    $retailOnly = @(@($spec.steps) + @(if ($spec.PSObject.Properties.Name -contains "prelude") { @($spec.prelude) }) |
        Where-Object { $_.PSObject.Properties.Name -contains "click" })
    if ($retailOnly.Count -gt 0 -and $openNovaActor) {
        throw "Scenario $($spec.name) clicks (click steps); the OpenNova $($spec.actor) plays none yet, so a retail $($spec.actor) only"
    }
    foreach ($step in $retailOnly) {
        if ($step.PSObject.Properties.Name -contains "click") {
            $click = $step.click
            $names = @($click.PSObject.Properties.Name)
            if ($names.Count -ne 2 -or "screen" -notin $names -or "control" -notin $names -or
                    [string] $click.screen -notmatch '^[\x21-\x7e]{1,31}$' -or
                    [string] $click.control -notmatch '^[\x21-\x7e]{1,31}$') {
                throw "Scenario $($spec.name): a click names a menu widget as {screen, control}, each 1-31 printable characters"
            }
        }
    }
    # E4's kick: the host's play ends the joiner's session (PUNT ALL,
    # Server_HandleBanPuntCommand @0x50B380), so the steady window's end does
    # not wait for that joiner in match: a retail joiner's closing state is
    # read (RR, OR), an OpenNova joiner's closing readiness skipped (RO, OO).
    # A host actor on an in-match run without a late joiner.
    if ($spec.PSObject.Properties.Name -contains "ends_joiner_session") {
        if ($spec.ends_joiner_session -isnot [bool] -or -not $spec.ends_joiner_session) {
            throw "Scenario $($spec.name): ends_joiner_session is true or absent"
        }
        if ([string] $spec.actor -cne "host" -or $ReadinessMode -ne "in_match" -or $LateJoiner) {
            throw "Scenario $($spec.name) ends the joiner's session: a host actor on an in-match run without a late joiner"
        }
    }
    return $spec
}

# True when the scenario this run plays ends the joiner's session.
function Test-ScenarioEndsJoinerSession {
    return [bool] ($script:ScenarioSpec -and
        $script:ScenarioSpec.PSObject.Properties.Name -contains "ends_joiner_session" -and
        $script:ScenarioSpec.ends_joiner_session)
}

$script:ScenarioSpec = $null
$script:ScenarioWitness = $null
$script:ScenarioEarlySetup = $null
# The previous aim_at's read and look (Resolve-ScenarioAim); none before the first.
$script:LastScenarioAim = $null
$script:DesktopResetBeforeWindow = $false
if ($Scenario) {
    if ($ExerciseInput -or $WireOnly) {
        throw "A scenario plays with neither ExerciseInput nor WireOnly: its steps drive the joiner."
    }
    $script:ScenarioSpec = Read-ParityScenario -Path $Scenario
}
if ($HostMcpPort -eq $JoinerMcpPort) {
    throw "HostMcpPort and JoinerMcpPort must differ."
}
if (-not $WireOnly) {
    [uint32] $wireGameType = 0
    if ($GameType -eq "auto" -or
            -not [uint32]::TryParse($GameType, [ref] $wireGameType)) {
        throw "Verdict-bearing parity runs require an explicit UInt32 GameType; zero is valid."
    }
}
if ($CorpusFingerprint -and $CorpusFingerprint -notmatch '^[0-9a-f]{64}$') {
    throw "CorpusFingerprint must be a lowercase SHA-256 when supplied."
}
if (($SuiteId -and -not $CorpusFingerprint) -or
        ($CorpusFingerprint -and -not $SuiteId)) {
    throw "SuiteId and CorpusFingerprint must be supplied together."
}
if ($MissionSha256 -and $MissionSha256 -notmatch '^[0-9a-f]{64}$') {
    throw "MissionSha256 must be a lowercase SHA-256 when supplied."
}
if (($MissionArtifactPath -and -not $MissionSha256) -or
        ($MissionSha256 -and -not $MissionArtifactPath)) {
    throw "MissionArtifactPath and MissionSha256 must be supplied together."
}
if ($SuiteId -and -not $MissionArtifactPath) {
    throw "A suite-bound run must bind its exact mission artifact."
}
$ResolvedMissionArtifact = ""
if ($MissionArtifactPath) {
    $ResolvedMissionArtifact = (Resolve-Path -LiteralPath $MissionArtifactPath).Path
    if ([System.IO.Path]::GetFileName($ResolvedMissionArtifact) -cne $Mission) {
        throw "Mission artifact leaf does not match Mission: $ResolvedMissionArtifact"
    }
    $actualMissionHash = (Get-FileHash -LiteralPath $ResolvedMissionArtifact `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualMissionHash -ne $MissionSha256) {
        throw "Mission artifact SHA-256 drift: $ResolvedMissionArtifact"
    }
}
# E2's rotation tail, one entry per mission (powershell -File passes a
# comma-separated list as one string).
$RotationList = @($Rotation | ForEach-Object { "$_".Split(',') } |
    ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($entry in $RotationList) {
    if ($entry -notmatch '^[^\\/:*?"<>|,\x00-\x1f]+\.[Bb][Mm][Ss]$') {
        throw "Rotation entry '$entry' is not a .BMS basename."
    }
}
if ($HostPlayerClass -ne 0 -and $Topology -in @("OR", "OO")) {
    throw "-HostPlayerClass needs a retail host (RR or RO): the OpenNova listen host's class from weapon.sav is unchecked."
}
if ($LateJoiner -and $Topology -in @("RO", "OO")) {
    throw "-LateJoiner needs a retail joiner (RR or OR): an OpenNova late joiner is not wired yet."
}
if ($LateJoiner -and $WireOnly) {
    throw "-LateJoiner and -WireOnly do not combine."
}
if ($LateJoiner -and ([string]::IsNullOrWhiteSpace($LateJoinerCallsign) -or
        $LateJoinerCallsign -ceq $JoinerCallsign -or $LateJoinerCallsign -ceq $HostCallsign)) {
    throw "-LateJoinerCallsign must name a third player."
}

$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$ServerDir = (Resolve-Path -LiteralPath $RetailServerRoot).Path
$ClientDir = (Resolve-Path -LiteralPath $RetailClientRoot).Path
$McpExe = (Resolve-Path -LiteralPath $OnHookMcpPath).Path
$GodotLauncher = (Resolve-Path -LiteralPath $GodotLauncher).Path
if (-not (Test-Path -LiteralPath $ServerDir -PathType Container) -or
        -not (Test-Path -LiteralPath $ClientDir -PathType Container)) {
    throw "RetailServerRoot and RetailClientRoot must both be directories."
}
if (-not (Test-Path -LiteralPath $McpExe -PathType Leaf) -or
        -not (Test-Path -LiteralPath $GodotLauncher -PathType Leaf)) {
    throw "OnHookMcpPath and GodotLauncher must both be files."
}
if ([string]::Equals(
        $ServerDir, $ClientDir, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "RetailServerRoot and RetailClientRoot must be distinct installations."
}
$env:GODOT_BIN = $GodotLauncher
# Both role copies must be the same retail executable; the hash rides the
# summary as the run's retail identity.
$RetailServerExeSha256 = (Get-FileHash -LiteralPath (Join-Path $ServerDir "Jointops.exe") `
    -Algorithm SHA256).Hash.ToLowerInvariant()
$RetailClientExeSha256 = (Get-FileHash -LiteralPath (Join-Path $ClientDir "Jointops.exe") `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($RetailServerExeSha256 -ne $RetailClientExeSha256) {
    throw "RetailServerRoot and RetailClientRoot hold different Jointops.exe images."
}
# The jox01 reference is one exact executable (ADR 0050 decision 2); a stock
# jox01 run on any other image is not evidence for it.
$PinnedJox01ExeSha256 = "9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57"
if ($Expansion -eq "jox01" -and -not $AllowModifiedRetail -and
        $RetailServerExeSha256 -ne $PinnedJox01ExeSha256) {
    throw "Jointops.exe $RetailServerExeSha256 is not the pinned jox01 reference ($PinnedJox01ExeSha256)."
}
# What built the OpenNova side: this checkout's commit (and whether it was
# clean), its GDExtension and the Godot binary, recorded with every run.
function Get-FileSha256OrNull {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$RepoCommit = (& git -C $Repo rev-parse HEAD 2>$null)
$RepoDirty = [bool] (& git -C $Repo status --porcelain --untracked-files=no 2>$null)
if ($RepoDirty -and -not $AllowDirtyRepo) {
    throw "The checkout has uncommitted tracked changes; a run is evidence for repo_commit alone (-AllowDirtyRepo for a diagnostic run)."
}
$GdextensionDll = Join-Path $Repo "godot\bin\opennovad.dll"
# A `--path godot` source run loads the editor variant (bin/opennova.gdextension's
# plain windows.debug.x86_64 row), so both variants are recorded.
$GdextensionEditorDll = Join-Path $Repo "godot\bin\oneditord.dll"
# The onHook proxy (binkw32.dll, forwarding to the renamed original) in both
# copies is one build, recorded as the run's hook identity with the MCP exe.
$OnHookProxySha256 = Get-FileSha256OrNull (Join-Path $ServerDir "binkw32.dll")
$clientProxySha256 = Get-FileSha256OrNull (Join-Path $ClientDir "binkw32.dll")
if (-not $OnHookProxySha256 -or $OnHookProxySha256 -ne $clientProxySha256) {
    throw "The SERVER and CLIENT copies do not hold the same onHook proxy binkw32.dll ($OnHookProxySha256 vs $clientProxySha256)."
}
foreach ($copy in @($ServerDir, $ClientDir)) {
    if (-not (Test-Path -LiteralPath (Join-Path $copy "binkw32_.dll") -PathType Leaf)) {
        throw "$copy has no binkw32_.dll, the original Bink DLL the onHook proxy forwards to."
    }
}
# E3's retail late joiner runs from its own copy when given one, else from the
# CLIENT copy; either way the same retail image under the same hook build.
$LateClientDir = $ClientDir
if ($LateJoiner -and $RetailLateClientRoot) {
    $LateClientDir = (Resolve-Path -LiteralPath $RetailLateClientRoot).Path
    if ([string]::Equals($LateClientDir, $ServerDir, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "RetailLateClientRoot must not be the SERVER copy."
    }
    if ((Get-FileSha256OrNull (Join-Path $LateClientDir "Jointops.exe")) -ne $RetailServerExeSha256 -or
            (Get-FileSha256OrNull (Join-Path $LateClientDir "binkw32.dll")) -ne $OnHookProxySha256 -or
            -not (Test-Path -LiteralPath (Join-Path $LateClientDir "binkw32_.dll") -PathType Leaf)) {
        throw "RetailLateClientRoot must hold the same Jointops.exe and onHook proxy pair as the other copies."
    }
}
# A retail LAN host binds only inside its game.cfg mplanserverportmin..max
# (stock JO:CA: 32768..32787), so a port outside the SERVER copy's own range can
# never host a retail cell.
$serverPortBounds = @{}
foreach ($key in @("mplanserverportmin", "mplanserverportmax")) {
    $match = Select-String -LiteralPath (Join-Path $ServerDir "game.cfg") `
        -Pattern ('^\s*' + $key + '\s*=\s*"?(\d+)"?') | Select-Object -First 1
    if ($match) { $serverPortBounds[$key] = [int] $match.Matches[0].Groups[1].Value }
}
if ($serverPortBounds.Count -eq 2 -and ($Port -lt $serverPortBounds["mplanserverportmin"] -or
        $Port -gt $serverPortBounds["mplanserverportmax"])) {
    throw "Port $Port is outside the SERVER copy's LAN host range $($serverPortBounds["mplanserverportmin"])..$($serverPortBounds["mplanserverportmax"]) (game.cfg mplanserverportmin/max)."
}
# A retail host advertises its game.cfg game_name verbatim (S2C 0x2C/0x7B), so
# the session name every cell expects is the SERVER copy's own, exactly.
$serverGameName = Select-String -LiteralPath (Join-Path $ServerDir "game.cfg") `
    -Pattern '^\s*game_name\s*=\s*"([^"]*)"' | Select-Object -First 1
if (-not $serverGameName) {
    throw "RetailServerRoot game.cfg has no game_name."
}
$retailGameName = $serverGameName.Matches[0].Groups[1].Value
if (-not [string]::Equals($ServerName, $retailGameName, [System.StringComparison]::Ordinal)) {
    throw "HostSessionName '$ServerName' must equal the SERVER copy's game.cfg game_name '$retailGameName' exactly."
}
$StockObserver = -not $AllowModifiedRetail
$RunRoot = Join-Path $Repo ".scratch\runs\$RunId"
$CaptureScript = Join-Path $Repo "scripts\net\capture.ps1"
$HostScript = Join-Path $Repo "scripts\net\host_opennova.ps1"
$JoinScript = Join-Path $Repo "scripts\net\join_opennova.ps1"
$NetLib = Join-Path $Repo "scripts\net\lib.ps1"
$ProbeExe = Join-Path $Repo "build\apps\lan_probe\Release\opennova-lan-probe.exe"
$WireReadyScript = Join-Path $Repo "scripts\net\wait_parity_wire_ready.ps1"
$CaptureTagScript = Join-Path $Repo "scripts\net\wait_capture_tag.ps1"
$ServeExe = Join-Path $Repo "build\apps\serve\Release\opennova-serve.exe"
$ServeHostScript = Join-Path $Repo "scripts\net\serve_host.py"
$serveInputs = if ($OpenNovaHostKind -eq "serve") { @($ServeExe, $ServeHostScript) } else { @() }

foreach ($required in @(
    $ServerDir, $ClientDir, $McpExe, $GodotLauncher, $CaptureScript, $HostScript,
    $JoinScript, $NetLib, $ProbeExe,
    $WireReadyScript, $CaptureTagScript
) + $serveInputs) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required parity input is missing: $required"
    }
}
. $NetLib
if (Test-Path -LiteralPath $RunRoot) {
    throw "Run output already exists; choose a create-new RunId: $RunRoot"
}
New-Item -ItemType Directory -Path $RunRoot | Out-Null
# The cell's own console output, errors included, beside its evidence: a suite
# runs cells as child processes whose console it does not keep.
$script:RunnerTranscript = $false
try {
    Start-Transcript -LiteralPath (Join-Path $RunRoot "runner.log") | Out-Null
    $script:RunnerTranscript = $true
}
catch {
    Write-Warning "No runner transcript: $($_.Exception.Message)"
}

# Per-call MCP timing. c2 saw three RO joiner quits acknowledged 43..114 s after
# the request, and endpoint timeouts before the window in the cells with our
# host. Every game MCP message (Send-GameMcpMessage, wrapped here) and every
# onHook call lands in <run>\mcp-calls.jsonl with its duration and outcome.
# Our process's CPU time is taken across the call; a slow call also takes its
# window's responsiveness and its memory. A game MCP call carries where its
# time went: the endpoint's own stamps (X-Mcp-Received / X-Mcp-Responded,
# docs/mcp.md) split it into the wait until the game parsed it, the game's
# time on it and the time from its response to our return, X-Mcp-Frame-Gap
# names the longest frame the game ran meanwhile, and the runner's
# own CPU time across it says whether this side was working. A slow call
# overruns its own long-poll wait by 2 s or more, and it also prints a run note.
$script:McpCallLog = Join-Path $RunRoot "mcp-calls.jsonl"
$script:SendGameMcpMessageUntimed = ${function:Send-GameMcpMessage}

function Get-McpCallProcessCpuMs {
    param([System.Diagnostics.Process] $Process)
    if (-not $Process) { return $null }
    try {
        $Process.Refresh()
        if ($Process.HasExited) { return $null }
        return [long] $Process.TotalProcessorTime.TotalMilliseconds
    }
    catch { return $null }
}

function Write-McpCallTiming {
    param(
        [Parameter(Mandatory = $true)] [string] $Role,
        [Parameter(Mandatory = $true)] [string] $Call,
        [Parameter(Mandatory = $true)] [DateTime] $StartedUtc,
        [Parameter(Mandatory = $true)] [long] $ElapsedMs,
        [Parameter(Mandatory = $true)] [string] $Outcome,
        [long] $WaitMs = 0,
        [System.Diagnostics.Process] $Process = $null,
        $CpuBeforeMs = $null,
        # The endpoint's stamps of the call ($script:GameMcpLastServerTiming).
        $ServerTiming = $null,
        $RunnerCpuMs = $null,
        # onHook calls wait on purpose (a launch, a played script): logged, never noted.
        [switch] $NoNote
    )
    try {
        $slow = $ElapsedMs -ge ($WaitMs + 2000)
        $record = [ordered]@{
            utc = $StartedUtc.ToString("o")
            role = $Role
            call = $Call
            ms = $ElapsedMs
            wait_ms = $WaitMs
            outcome = $Outcome
            in_window = [bool] $script:SteadyStartedUtc -and -not $script:SteadyCompletedUtc
        }
        $detail = ""
        if ($Process) {
            $record.pid = $Process.Id
            $cpuAfter = Get-McpCallProcessCpuMs $Process
            if ($null -ne $CpuBeforeMs -and $null -ne $cpuAfter) {
                $record.cpu_ms = $cpuAfter - [long] $CpuBeforeMs
            }
            if ($slow -and $null -ne $cpuAfter) {
                # Responding is a 5 s SendMessageTimeout on the main window:
                # false means its thread did not pump messages.
                $record.responding = [bool] $Process.Responding
                $record.threads = $Process.Threads.Count
                $record.working_set_mb = [int] ($Process.WorkingSet64 / 1MB)
                $detail = " cpu_ms=$($record.cpu_ms) responding=$($record.responding) threads=$($record.threads) ws_mb=$($record.working_set_mb)"
            }
        }
        if ($ServerTiming) {
            # Unix ms on this machine's one clock: request -> parsed by the game
            # (the transport and the game's next poll of its endpoint, once a
            # frame), parsed -> response written (the game's time on it, a
            # long-poll's wait included), written -> our return (the transport
            # and our decoding).
            $startedMs = ([DateTimeOffset] $StartedUtc).ToUnixTimeMilliseconds()
            $record.to_server_ms = [long] $ServerTiming.received_unix_ms - $startedMs
            $record.server_ms = [long] $ServerTiming.responded_unix_ms - [long] $ServerTiming.received_unix_ms
            $record.from_server_ms = $startedMs + $ElapsedMs - [long] $ServerTiming.responded_unix_ms
            $detail += " to_server_ms=$($record.to_server_ms) server_ms=$($record.server_ms) from_server_ms=$($record.from_server_ms)"
            # The longest frame the game's main thread ran while the request
            # was in (it polls its endpoint once a frame): one that covers the
            # slow part held the call (a mission load), not the endpoint.
            if ($null -ne $ServerTiming.frame_gap_ms) {
                $record.frame_gap_ms = [long] $ServerTiming.frame_gap_ms
                $detail += " frame_gap_ms=$($record.frame_gap_ms)"
            }
        }
        if ($null -ne $RunnerCpuMs) {
            $record.runner_cpu_ms = [long] $RunnerCpuMs
            $detail += " runner_cpu_ms=$($record.runner_cpu_ms)"
        }
        Add-Content -LiteralPath $script:McpCallLog -Value ($record | ConvertTo-Json -Compress)
        if ($slow -and -not $NoNote) {
            Write-Host ("PARITY_RUN_NOTE mcp slow: {0} {1} {2} ms (wait {3} ms) {4}{5}" -f
                $Role, $Call, $ElapsedMs, $WaitMs, $Outcome, $detail)
        }
    }
    catch {
        Write-Host "PARITY_RUN_NOTE mcp timing not recorded: $($_.Exception.Message)"
    }
}

function Send-GameMcpMessage {
    param(
        [Parameter(Mandatory = $true)] [int] $Port,
        [Parameter(Mandatory = $true)] [hashtable] $Message,
        [int] $TimeoutSeconds = 120
    )
    $role = "game:$Port"
    $process = $null
    if ($Port -eq $HostMcpPort) { $role = "host"; $process = $script:OpenNovaHost }
    elseif ($Port -eq $JoinerMcpPort) { $role = "joiner"; $process = $script:OpenNovaJoiner }
    $call = [string] $Message.method
    $waitMs = 0
    if ($Message.ContainsKey("params") -and $Message.params -is [hashtable] -and
            $Message.params.ContainsKey("name")) {
        $call += " " + [string] $Message.params.name
        $arguments = $Message.params.arguments
        if ($arguments -is [hashtable]) {
            foreach ($key in @("op", "name", "action")) {
                if ($arguments.ContainsKey($key)) { $call += " $key=$($arguments[$key])" }
            }
            if ($arguments.ContainsKey("wait_ms")) { $waitMs = [long] $arguments.wait_ms }
        }
    }
    $cpuBefore = Get-McpCallProcessCpuMs $process
    $runner = [System.Diagnostics.Process]::GetCurrentProcess()
    $runnerCpuBefore = $runner.TotalProcessorTime.TotalMilliseconds
    $startedUtc = [DateTime]::UtcNow
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $outcome = "ok"
    try {
        return (& $script:SendGameMcpMessageUntimed -Port $Port -Message $Message -TimeoutSeconds $TimeoutSeconds)
    }
    catch {
        $outcome = if (Test-GameMcpTransportTimeout $_) { "timeout" } else { "error" }
        throw
    }
    finally {
        $watch.Stop()
        $runner.Refresh()
        Write-McpCallTiming -Role $role -Call $call -StartedUtc $startedUtc `
            -ElapsedMs $watch.ElapsedMilliseconds -Outcome $outcome -WaitMs $waitMs `
            -Process $process -CpuBeforeMs $cpuBefore `
            -ServerTiming $script:GameMcpLastServerTiming `
            -RunnerCpuMs ([long] ($runner.TotalProcessorTime.TotalMilliseconds - $runnerCpuBefore))
    }
}

$script:Mcp = $null
$script:McpRequestId = 0
$script:CaptureStarted = $false
$script:CaptureTag = "$RunId-external"
$script:OpenNovaHost = $null
# -OpenNovaHostKind serve: serve_host.py's process (it holds opennova-serve's
# console) and the launch's witness (the exe's hash, the host file, the
# serving line).
$script:ServeWrapper = $null
$script:ServeLaunch = $null
$script:OpenNovaJoiner = $null
$script:OpenNovaLaunchProofs = [ordered]@{}
$script:RetailProcess = $null
$script:RetailHostProcess = $null
$script:RetailJoinerProcess = $null
$script:RetailInstanceId = ""
$script:RetailInstanceIds = [System.Collections.Generic.List[string]]::new()
$script:OwnedRetailRuns = [System.Collections.Generic.List[string]]::new()
$script:ExternalCapture = ""
$script:EvidenceCapture = ""
$script:EvidencePerspective = ""
$script:EvidenceRole = ""
$script:EvidenceInstanceId = ""
$script:Screenshot = ""
$script:ServeOnlyHostScreenshot = ""
$script:ServeOnlyRoundEnd = $null
$script:Acceptance = $null
$script:StopEvidence = @()
$script:CleanupFailure = ""
$script:InputWitness = $null
$script:EffectiveRetailConfigs = [System.Collections.Generic.List[object]]::new()
$script:SteadyStartedUtc = ""
$script:SteadyCompletedUtc = ""
$script:OpenNovaHostStartOwnership = $null
$script:CaptureTailMarker = $null
$script:ExternalCaptureProof = $null
$script:MotionGateWitness = $null
$script:WireReadyWitness = $null
$script:JoinerReadyRunId = ""
$script:JoinerMotionRunId = ""
$script:OpenNovaJoinerShutdownWitness = $null
$script:OpenNovaJoinerCooperativeStopAttempted = $false
$script:OpenNovaJoinerExactFallbackRequired = $false
$script:OpenNovaHostShutdownWitness = $null
$script:OpenNovaHostExactFallbackRequired = $false

function Start-OnHookMcp {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $McpExe
    $start.WorkingDirectory = Split-Path -Parent $McpExe
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $script:Mcp = [System.Diagnostics.Process]::Start($start)
    if (-not $script:Mcp) { throw "Could not launch onhook-mcp.exe" }

    $null = Invoke-OnHookRpc -Method "initialize" -Params @{
        protocolVersion = "2025-03-26"
        capabilities = @{}
        clientInfo = @{ name = "opennova-parity-harness"; version = "1" }
    }
}

function Invoke-OnHookRpc {
    param(
        [Parameter(Mandatory = $true)] [string] $Method,
        [Parameter(Mandatory = $true)] [hashtable] $Params
    )
    if (-not $script:Mcp -or $script:Mcp.HasExited) {
        throw "onhook MCP process is not running"
    }
    $script:McpRequestId++
    $request = @{
        jsonrpc = "2.0"
        id = $script:McpRequestId
        method = $Method
        params = $Params
    } | ConvertTo-Json -Depth 20 -Compress
    $call = $Method
    if ($Params.ContainsKey("name")) { $call += " " + [string] $Params.name }
    $startedUtc = [DateTime]::UtcNow
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $script:Mcp.StandardInput.WriteLine($request)
    $script:Mcp.StandardInput.Flush()
    $line = $script:Mcp.StandardOutput.ReadLine()
    $watch.Stop()
    Write-McpCallTiming -Role "onhook" -Call $call -StartedUtc $startedUtc `
        -ElapsedMs $watch.ElapsedMilliseconds -NoNote `
        -Outcome $(if ([string]::IsNullOrWhiteSpace($line)) { "empty" } else { "ok" })
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "onhook MCP returned no JSON-RPC response"
    }
    $response = $line | ConvertFrom-Json
    if ($response.PSObject.Properties.Name -contains "error") {
        throw "onhook MCP JSON-RPC error: $($response.error | ConvertTo-Json -Compress)"
    }
    return $response.result
}

function Invoke-OnHookTool {
    param(
        [Parameter(Mandatory = $true)] [string] $Name,
        [hashtable] $Arguments = @{},
        [switch] $AllowError
    )
    $result = Invoke-OnHookRpc -Method "tools/call" -Params @{
        name = $Name
        arguments = $Arguments
    }
    if ($result.isError -and -not $AllowError) {
        $message = @($result.content | Where-Object { $_.type -eq "text" } |
            ForEach-Object { $_.text }) -join "`n"
        throw "$Name failed: $message"
    }
    return $result
}

function Stop-ExactProcess {
    param([System.Diagnostics.Process] $Process)
    if (-not $Process) { return }
    if ($Process.PSObject.Properties['OpenNovaLaunchProof']) {
        # The official console wrapper owns a KILL_ON_JOB_CLOSE job. Preserve
        # that lifecycle contract even when its proven runtime has exited.
        Stop-OpenNovaProcess -Process $Process
        return
    }
    try {
        $Process.Refresh()
        if ($Process.HasExited) { return }
        $null = $Process.CloseMainWindow()
        if (-not $Process.WaitForExit(8000)) {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
            $null = $Process.WaitForExit(5000)
        }
    }
    catch {
        Write-Warning "Could not fully close exact PID $($Process.Id): $($_.Exception.Message)"
    }
}

function Add-CleanupFailure {
    param([Parameter(Mandatory = $true)] [string] $Message)
    if ($script:CleanupFailure) {
        $script:CleanupFailure += " | $Message"
    }
    else {
        $script:CleanupFailure = $Message
    }
    Write-Warning $Message
}

function Register-OpenNovaLaunchProof {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("host", "joiner")]
        [string] $Role,
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process] $Process
    )
    # The desktop guard takes the foreground only from this cell's own Godot.
    # The guard job reads this file every 100 ms, and Windows PowerShell's
    # Add-Content cannot open a file another handle is reading, even one that
    # shares read and write: an append that met the guard's read threw "Stream
    # was not readable" (LP-s14's round OO, x11's kick OO: an OO cell, which
    # registers two roles). Both sides open it sharing read and write.
    if ($script:DesktopGuardPids) {
        $guarded = @([string] $Process.Id)
        $launcher = $Process.PSObject.Properties['OpenNovaLauncherProcess']
        if ($launcher -and $launcher.Value) { $guarded += [string] $launcher.Value.Id }
        $stream = [System.IO.File]::Open($script:DesktopGuardPids, [System.IO.FileMode]::Append,
            [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
        try {
            $writer = New-Object System.IO.StreamWriter($stream)
            foreach ($id in $guarded) { $writer.WriteLine($id) }
            $writer.Flush()
        } finally {
            $stream.Dispose()
        }
    }
    if ($script:OpenNovaLaunchProofs.Contains($Role)) {
        throw "Duplicate OpenNova launch proof for role '$Role'."
    }
    $proofProperty = $Process.PSObject.Properties['OpenNovaLaunchProof']
    $launcherProperty = $Process.PSObject.Properties['OpenNovaLauncherProcess']
    if (-not $proofProperty -or -not $proofProperty.Value -or
            -not $launcherProperty -or -not $launcherProperty.Value) {
        throw "OpenNova $Role process lacks centralized launcher ownership proof."
    }
    $proof = $proofProperty.Value
    if ([string] $proof.schema -cne 'opennova.godot-process-ownership.v1' -or
            (-not [bool] $proof.wrapper_used -and -not $AllowDirectRuntime) -or
            [int] $proof.runtime_pid -ne $Process.Id -or
            [int] $proof.launcher_pid -ne $launcherProperty.Value.Id -or
            ([bool] $proof.wrapper_used -and
                [int] $proof.runtime_parent_pid -ne [int] $proof.launcher_pid) -or
            -not [bool] $proof.parent_verified -or
            -not [bool] $proof.argv_verified -or
            -not [bool] $proof.active_execution_verified) {
        throw "OpenNova $Role process returned an invalid or direct-runtime launch proof."
    }
    # Break references to live Process objects. Only the serializable ownership
    # witness is retained for the create-new run summary and manifest.
    $snapshot = $proof | ConvertTo-Json -Depth 10 -Compress | ConvertFrom-Json
    $script:OpenNovaLaunchProofs.Add($Role, $snapshot)
}

function Assert-UdpPortUnowned {
    # Filtering the full table keeps an unbound port as an empty result. The
    # cmdlet's -LocalPort query reports that ordinary state as a terminating
    # CIM "not found" error under -ErrorAction Stop.
    $existing = @(Get-NetUDPEndpoint -ErrorAction Stop | Where-Object {
        [int] $_.LocalPort -eq $Port
    })
    if ($existing.Count -gt 0) {
        $owners = @($existing | ForEach-Object {
            "pid=$($_.OwningProcess) address=$($_.LocalAddress):$($_.LocalPort)"
        }) -join ", "
        throw "Parity port $Port was already bound before endpoint launch: $owners"
    }
}

function Assert-OpenNovaHostOwnsPort {
    if (-not $script:OpenNovaHost) {
        throw "Cannot prove UDP port ownership without an OpenNova host process"
    }
    $script:OpenNovaHost.Refresh()
    if ($script:OpenNovaHost.HasExited) {
        throw "OpenNova host exited before UDP port ownership could be proven"
    }
    $endpoints = @(Get-NetUDPEndpoint -ErrorAction Stop | Where-Object {
        [int] $_.LocalPort -eq $Port
    })
    $owned = @($endpoints | Where-Object {
        [int] $_.OwningProcess -eq $script:OpenNovaHost.Id
    })
    $unexpected = @($endpoints | Where-Object {
        [int] $_.OwningProcess -ne $script:OpenNovaHost.Id
    })
    if ($owned.Count -eq 0) {
        $observed = @($endpoints | ForEach-Object { [int] $_.OwningProcess }) -join ","
        throw "OpenNova host PID $($script:OpenNovaHost.Id) does not own UDP port $Port (observed PIDs: $observed)"
    }
    if ($unexpected.Count -gt 0) {
        $observed = @($unexpected | ForEach-Object { [int] $_.OwningProcess }) -join ","
        throw "Unexpected PID(s) also own OpenNova host UDP port ${Port}: $observed"
    }
    return [pscustomobject]@{
        pid = $script:OpenNovaHost.Id
        port = $Port
        endpoints = @($owned | ForEach-Object {
            [pscustomobject]@{
                local_address = [string] $_.LocalAddress
                local_port = [int] $_.LocalPort
            }
        })
    }
}

function Send-ExternalCaptureTailMarker {
    if ($Topology -ne "OO") {
        throw "External capture tail markers are reserved for OO evidence"
    }
    if ($script:CaptureTailMarker) {
        throw "External capture tail marker was already sent"
    }
    $markerText = "OPENNOVA-PCAPNG-TAIL|$RunId|$([Guid]::NewGuid().ToString('N'))"
    [byte[]] $markerBytes = [System.Text.Encoding]::UTF8.GetBytes($markerText)
    $udp = [System.Net.Sockets.UdpClient]::new(
        [System.Net.Sockets.AddressFamily]::InterNetwork)
    try {
        $sent = $udp.Send($markerBytes, $markerBytes.Length, "127.0.0.1", $Port)
    }
    finally {
        $udp.Dispose()
    }
    if ($sent -ne $markerBytes.Length) {
        throw "UDP tail marker send was short ($sent of $($markerBytes.Length) bytes)"
    }
    $markerHex = ([BitConverter]::ToString($markerBytes)).Replace('-', '').ToLowerInvariant()
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $markerDigest = $sha.ComputeHash($markerBytes) }
    finally { $sha.Dispose() }
    $script:CaptureTailMarker = [pscustomobject]@{
        destination = "127.0.0.1:$Port"
        bytes = $markerBytes.Length
        hex = $markerHex
        sha256 = ([BitConverter]::ToString($markerDigest)).Replace('-', '').ToLowerInvariant()
        sent_utc = [DateTime]::UtcNow.ToString("o")
        flush_wait_ms = 1000
    }
    # No endpoint remains alive after this datagram. Once its complete packet
    # block reaches disk, the capture contains every preceding endpoint packet.
    Start-Sleep -Milliseconds $script:CaptureTailMarker.flush_wait_ms
}

# The retail joiner's motion exercise on exercised RR/OR runs: forward with a
# right turn, strafe right with a left turn, strafe left with half a right turn
# (the catalog codes forward 152, strafe_right 157, strafe_left 156;
# engine/runtime/controls/controls.cpp), played tick-exact through onHook's
# focus-free scripted input (onhook_play_input, bridge 1.9) rather than
# foreground SendInput. Each leg holds its key and spreads its turn over 20
# relative mouse samples.
$script:RetailInputLegs = @(
    [pscustomobject]@{ name = "forward"; code = 152; ticks = 112; turn_pixels = 320 },
    [pscustomobject]@{ name = "strafe"; code = 157; ticks = 75; turn_pixels = -320 },
    [pscustomobject]@{ name = "return"; code = 156; ticks = 56; turn_pixels = 160 }
)

function Get-RetailInputSteps {
    $steps = [System.Collections.Generic.List[object]]::new()
    $tick = 10
    foreach ($leg in $script:RetailInputLegs) {
        $steps.Add([pscustomobject]@{ tick = $tick; down = $leg.code })
        $samples = 20
        $per = [int][Math]::Truncate($leg.turn_pixels / $samples)
        $remainder = $leg.turn_pixels - $per * $samples
        for ($i = 0; $i -lt $samples; ++$i) {
            $dx = $per + $(if ($i -eq $samples - 1) { $remainder } else { 0 })
            $at = $tick + [int][Math]::Floor($i * $leg.ticks / $samples)
            $steps.Add([pscustomobject]@{ tick = $at; look_px = @($dx, 0) })
        }
        $tick += $leg.ticks
        $steps.Add([pscustomobject]@{ tick = $tick; up = $leg.code })
        $tick += 1
    }
    $steps.Add([pscustomobject]@{ tick = $tick + 5; end = $true })
    return $steps.ToArray()
}

function Invoke-RetailInputExercise {
    param(
        [Parameter(Mandatory = $true)] [string] $InstanceId,
        [Parameter(Mandatory = $true)] [int] $ProcessId
    )
    if (-not $ExerciseInput -or $Topology -notin @("RR", "OR")) {
        throw "Retail input witnesses are valid only for exercised RR/OR runs"
    }
    $path = Join-Path $RunRoot "retail-input.json"
    if (Test-Path -LiteralPath $path) {
        throw "Retail input witness must be create-new: $path"
    }
    $steps = Get-RetailInputSteps
    $started = [DateTime]::UtcNow
    $played = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_play_input" -Arguments @{
        instance_id = $InstanceId
        steps = $steps
        wait = $true
        # Generous: two retail windows on one machine tick well below 62 Hz.
        timeout_ms = 60000
    })
    $unapplied = @($played.steps | Where-Object {
        [string] $_.kind -ne "end" -and $null -eq $_.applied_logic_tick
    })
    if ([string] $played.state -ne "done" -or $unapplied.Count -gt 0) {
        throw "Retail input exercise did not play on instance ${InstanceId}: state=$($played.state) result=$($played.result) unapplied=$($unapplied.Count)"
    }
    [pscustomobject]@{
        schema = "opennova.retail-input.v2"
        run_id = $RunId
        topology = $Topology
        instance_id = $InstanceId
        pid = $ProcessId
        driver = "onhook_play_input"
        started_utc = $started.ToString("o")
        completed_utc = [DateTime]::UtcNow.ToString("o")
        legs = $script:RetailInputLegs
        steps = $steps
        record = $played
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $path -Encoding UTF8
    $script:InputWitness = [pscustomobject]@{
        path = $path
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        pid = $ProcessId
        instance_id = $InstanceId
        state = [string] $played.state
        applied_steps = @($played.steps).Count
    }
}

function Bind-EffectiveRetailConfig {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("server", "client", "late_client")]
        [string] $Role,
        [Parameter(Mandatory = $true)]
        [string] $Source
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Effective retail $Role cfg is missing: $Source"
    }
    if (@($script:EffectiveRetailConfigs | Where-Object { $_.role -eq $Role }).Count -ne 0) {
        throw "Effective retail cfg role is already bound: $Role"
    }
    $owned = Join-Path $RunRoot "effective-$Role.onhook.cfg"
    if (Test-Path -LiteralPath $owned) {
        throw "Effective retail cfg snapshot must be create-new: $owned"
    }
    Copy-Item -LiteralPath $Source -Destination $owned
    $script:EffectiveRetailConfigs.Add([pscustomobject]@{
        role = $Role
        path = $owned
        sha256 = (Get-FileHash -LiteralPath $owned -Algorithm SHA256).Hash.ToLowerInvariant()
    })
}

# The desktop the game windows share. Retail centres the cursor on its own
# window every frame while it holds focus (Game_ProcessMainFrame
# @0x526401..0x526458 -> Input_PumpAndCenterCursor @0x7616D0: SetCursorPos to
# the client origin + 320,240, then the delta is the last WM_MOUSEMOVE minus
# 320,240), and confines the cursor system-wide to its window rect whenever it
# leaves a menu, with no focus check (UI_CloseMenuScreen ->
# Game_ClipCursorToGameWindow @0x4983C0, ClipCursor @0x4983D8). A clip another
# window left (a joiner still at its small launch size) clamps the focused
# window's centring, so it reads one constant delta every frame: its body's
# yaw steps every frame and its pitch pins at the clamp (the R1 rows of i1 and
# c1, reproduced by parity-20261020-r1probe2). The runner releases any clip as
# the steady window opens and records the desktop at both ends of the window;
# a cell whose focused retail window could not centre is marked unclean.
$script:DesktopWitness = [ordered]@{}
function Initialize-DesktopProbe {
    if ('ParityDesktop' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ParityDesktop {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClipCursor(out RECT r);
    [DllImport("user32.dll", EntryPoint = "ClipCursor")] public static extern bool ReleaseClip(IntPtr r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
    public static List<IntPtr> VisibleWindowsOf(uint pid) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == pid && IsWindowVisible(h)) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
}

function Get-DesktopState {
    Initialize-DesktopProbe
    $foreground = [ParityDesktop]::GetForegroundWindow()
    $foregroundPid = [uint32]0
    $null = [ParityDesktop]::GetWindowThreadProcessId($foreground, [ref] $foregroundPid)
    $clip = New-Object ParityDesktop+RECT
    $null = [ParityDesktop]::GetClipCursor([ref] $clip)
    $roles = @(
        @{ role = "retail_host"; process = $script:RetailHostProcess; retail = $true },
        @{ role = "retail_joiner"; process = $script:RetailJoinerProcess; retail = $true },
        @{ role = "retail_late_joiner"; process = $script:RetailLateJoinerProcess; retail = $true },
        @{ role = "opennova_host"; process = $script:OpenNovaHost; retail = $false },
        @{ role = "opennova_joiner"; process = $script:OpenNovaJoiner; retail = $false }
    )
    # A retail process the cell holds no handle for (a path that keeps only the
    # tool's pid) still centres and clips: every Jointops.exe counts.
    $known = @($roles | Where-Object { $_.process } | ForEach-Object { [int] $_.process.Id })
    foreach ($other in @(Get-Process -Name "Jointops" -ErrorAction SilentlyContinue)) {
        if ($known -notcontains [int] $other.Id) {
            $roles += @{ role = "retail"; process = $other; retail = $true }
        }
    }
    $windows = @()
    $clean = $true
    foreach ($entry in $roles) {
        if (-not $entry.process) { continue }
        foreach ($window in [ParityDesktop]::VisibleWindowsOf([uint32] $entry.process.Id)) {
            $rect = New-Object ParityDesktop+RECT
            $null = [ParityDesktop]::GetWindowRect($window, [ref] $rect)
            $origin = New-Object ParityDesktop+POINT
            $null = [ParityDesktop]::ClientToScreen($window, [ref] $origin)
            $focused = [uint32] $entry.process.Id -eq $foregroundPid
            # Retail's centring point; only the focused retail window centres.
            $warp = if ($entry.retail) { @(($origin.X + 320), ($origin.Y + 240)) } else { $null }
            $warpInClip = if ($warp) {
                $warp[0] -ge $clip.L -and $warp[0] -lt $clip.R -and $warp[1] -ge $clip.T -and $warp[1] -lt $clip.B
            } else { $null }
            if ($entry.retail -and $focused -and -not $warpInClip) { $clean = $false }
            $windows += [pscustomobject][ordered]@{
                role = $entry.role
                hwnd = [int64] $window
                retail = [bool] $entry.retail
                pid = $entry.process.Id
                rect = @($rect.L, $rect.T, $rect.R, $rect.B)
                client_origin = @($origin.X, $origin.Y)
                warp = $warp
                warp_in_clip = $warpInClip
                focused = $focused
            }
        }
    }
    return [pscustomobject][ordered]@{
        utc = [DateTime]::UtcNow.ToString("o")
        foreground_pid = $foregroundPid
        clip = @($clip.L, $clip.T, $clip.R, $clip.B)
        windows = $windows
        clean = $clean
    }
}

# As the steady window opens: what the desktop held, then any clip released
# (ClipCursor(NULL), the desktop's state, not the game's code), and every
# retail window told it has no focus (WM_KILLFOCUS, retail's focus flag
# cleared @0x762670): a window the guard could not take the foreground from,
# or one that took focus again while it loaded, centres no more.
function Reset-DesktopCursor {
    $before = Get-DesktopState
    $released = [ParityDesktop]::ReleaseClip([IntPtr]::Zero)
    $unfocused = 0
    foreach ($window in @($before.windows | Where-Object { $_.retail })) {
        if ([ParityDesktop]::PostMessage([IntPtr] $window.hwnd, 0x0008, [IntPtr]::Zero, [IntPtr]::Zero)) { $unfocused++ }
    }
    $after = Get-DesktopState
    $script:DesktopWitness.steady_start = [pscustomobject][ordered]@{
        found = $before
        released = [bool] $released
        retail_windows_unfocused = $unfocused
        after = $after
    }
    Write-Host ("PARITY_RUN_NOTE desktop at the steady start: clip {0} -> {1}, foreground pid {2}" -f
        ($before.clip -join ","), ($after.clip -join ","), $after.foreground_pid)
}

# The desktop guard: for the whole cell no game window takes desk input or
# keeps the foreground. Each game window is disabled (EnableWindow FALSE) as it
# appears. A click on a window not in front is otherwise delivered while
# Windows activates it (WM_MOUSEACTIVATE MA_ACTIVATE, then WM_LBUTTONDOWN):
# retail's WM_SETFOCUS sets its focus flag again (@0x762697), opening its input
# gate, and ours fires off Godot's polled button state, which has no focus gate
# (ControlsModel::is_token_pressed / is_token_fire_signed,
# godot/src/mnu/controls_model.cpp). x1's capture OO took one desk click as a
# round. A disabled window gets no mouse or keyboard input and cannot be
# activated by a click, and it still renders and ticks. The scripted inputs
# never pass through window messages: onHook's at Input_ProcessFrame and the
# menu pump, ours at the ControlsModel seam.
# Without real focus a retail window neither centres nor reads a cursor delta
# (the gate above); scripted input brings its own virtual focus and zeroes
# the real delta when the window is not really focused. So a clip any window
# leaves cannot reach a body's look. The guard hands the foreground to the
# shell's desktop window (GetShellWindow) whenever a Jointops.exe window or one
# of this cell's own Godot processes (desktop-guard.pids, written as each
# OpenNova role launches) holds it, and logs each hand-off. Another session's
# Godot window (a lane's windowed GUT run) is left alone.
$script:DesktopGuard = $null
$script:DesktopGuardLog = ""
$script:DesktopGuardPids = ""
function Start-DesktopGuard {
    $script:DesktopGuardLog = Join-Path $RunRoot "desktop-guard.log"
    $script:DesktopGuardPids = Join-Path $RunRoot "desktop-guard.pids"
    $script:DesktopGuard = Start-Job -ArgumentList $script:DesktopGuardLog, $script:DesktopGuardPids -ScriptBlock {
        param([string] $Log, [string] $PidFile)
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ParityDesktopGuard {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnableWindow(IntPtr h, bool enable);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    // The visible, still enabled top-level windows of the given processes.
    public static System.Collections.Generic.List<IntPtr> EnabledWindows(uint[] pids) {
        var found = new System.Collections.Generic.List<IntPtr>();
        EnumWindows((h, l) => {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (Array.IndexOf(pids, owner) >= 0 && IsWindowVisible(h) && IsWindowEnabled(h))
                found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }
    // A background process may not take the foreground once a launched game
    // has claimed it: joined to the foreground thread's input, it may.
    public static bool HandOff(IntPtr foreground, IntPtr target) {
        uint owner;
        uint thread = GetWindowThreadProcessId(foreground, out owner);
        uint self = GetCurrentThreadId();
        bool attached = thread != 0 && thread != self && AttachThreadInput(self, thread, true);
        bool ok = SetForegroundWindow(target);
        if (attached) AttachThreadInput(self, thread, false);
        return ok;
    }
}
'@
        $desktop = [ParityDesktopGuard]::GetShellWindow()
        Add-Content -LiteralPath $Log -Value "$([DateTime]::UtcNow.ToString('o')) guard start desktop=$desktop"
        # Once per foreground spell: hand the foreground to the desktop; when
        # Windows refuses (a launched game keeps it), tell the window it lost
        # focus (WM_KILLFOCUS: retail clears its focus flag, @0x762670), as an
        # alt-tab away would.
        $handled = [IntPtr]::Zero
        $own = @()
        while ($true) {
            # Read sharing read and write, as the runner's append does
            # (Register-OpenNovaLaunchProof); a failed read keeps the last list.
            if (Test-Path -LiteralPath $PidFile) {
                try {
                    $stream = [System.IO.File]::Open($PidFile, [System.IO.FileMode]::Open,
                        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                    try {
                        $own = @((New-Object System.IO.StreamReader($stream)).ReadToEnd() -split "\r?\n")
                    } finally {
                        $stream.Dispose()
                    }
                } catch {
                }
            }
            $games = @(@(Get-Process -Name Jointops -ErrorAction SilentlyContinue | ForEach-Object { [uint32] $_.Id }) +
                @($own | Where-Object { $_ } | ForEach-Object { [uint32] $_ }))
            if ($games.Count -gt 0) {
                foreach ($window in [ParityDesktopGuard]::EnabledWindows([uint32[]] $games)) {
                    $owner = [uint32]0
                    $null = [ParityDesktopGuard]::GetWindowThreadProcessId($window, [ref] $owner)
                    $name = try { (Get-Process -Id $owner -ErrorAction Stop).ProcessName } catch { "" }
                    $null = [ParityDesktopGuard]::EnableWindow($window, $false)
                    Add-Content -LiteralPath $Log -Value ("{0} input-lock {1} pid={2} hwnd={3} disabled={4}" -f
                        [DateTime]::UtcNow.ToString('o'), $name, $owner, $window,
                        (-not [ParityDesktopGuard]::IsWindowEnabled($window)))
                }
            }
            $foreground = [ParityDesktopGuard]::GetForegroundWindow()
            if ($foreground -ne $handled) {
                $owner = [uint32]0
                $null = [ParityDesktopGuard]::GetWindowThreadProcessId($foreground, [ref] $owner)
                $name = try { (Get-Process -Id $owner -ErrorAction Stop).ProcessName } catch { "" }
                if ($name -eq "Jointops" -or ($name -like "Godot*" -and $own -contains [string] $owner)) {
                    $ok = [ParityDesktopGuard]::HandOff($foreground, $desktop)
                    Start-Sleep -Milliseconds 50
                    $still = [ParityDesktopGuard]::GetForegroundWindow() -eq $foreground
                    $killed = $false
                    if ($still) {
                        $killed = [ParityDesktopGuard]::PostMessage($foreground, 0x0008, [IntPtr]::Zero, [IntPtr]::Zero)
                    }
                    Add-Content -LiteralPath $Log -Value ("{0} foreground {1} pid={2} hwnd={3} -> desktop ok={4} kept={5} killfocus={6}" -f
                        [DateTime]::UtcNow.ToString('o'), $name, $owner, $foreground, $ok, $still, $killed)
                    $handled = $foreground
                }
            }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Stop-DesktopGuard {
    if (-not $script:DesktopGuard) { return }
    Stop-Job -Job $script:DesktopGuard -ErrorAction SilentlyContinue
    Remove-Job -Job $script:DesktopGuard -Force -ErrorAction SilentlyContinue
    $script:DesktopGuard = $null
}

function Complete-DesktopWitness {
    $state = Get-DesktopState
    $script:DesktopWitness.steady_end = $state
    if (-not $state.clean) {
        Write-Host ("PARITY_RUN_NOTE desktop UNCLEAN at the steady end: the focused retail window's centre lies outside the cursor clip {0}" -f ($state.clip -join ","))
    }
}

# The machine load through the steady window (the maintainer's machine-load
# policy): a timing-class row from a loaded cell is suspect until it is rerun
# on a quiet machine. A sampler job reads the total CPU % and the available
# RAM every 2 s; the processes' CPU time is taken at both ends, and its top
# five by CPU are kept; the build lock's owner is read at both ends. Every step
# is best effort: a failure only leaves a note.
$script:MachineLoadJob = $null
$script:MachineLoadStart = $null
$script:MachineLoad = $null
function Get-BuildLockOwner {
    if (-not $BuildLockPath -or -not (Test-Path -LiteralPath $BuildLockPath)) { return $null }
    $owner = Join-Path $BuildLockPath "owner"
    if (Test-Path -LiteralPath $owner) { return ((Get-Content -LiteralPath $owner -Raw) -replace '\s+$', '') }
    return "held (no owner file)"
}

function Start-MachineLoadSampler {
    try {
        $script:MachineLoadStart = [pscustomobject]@{ at = [DateTime]::UtcNow; lock = Get-BuildLockOwner }
        # One line every 2 s: the total CPU %, the available MB, and the busiest
        # processes then (per-core %, an instance's #n suffix dropped), which also
        # catches short-lived compiler processes a start/end snapshot would miss.
        $script:MachineLoadJob = Start-Job -ScriptBlock {
            while ($true) {
                try {
                    $cpu = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
                    $free = [int] ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)
                    $busy = @(Get-CimInstance Win32_PerfFormattedData_PerfProc_Process |
                        Where-Object { $_.Name -notin @("_Total", "Idle") -and $_.PercentProcessorTime -gt 0 } |
                        ForEach-Object { "{0}={1}" -f ($_.Name -replace '#\d+$', ''), $_.PercentProcessorTime }) -join ";"
                    "{0}|{1}|{2}|{3}" -f [DateTime]::UtcNow.ToString("o"), $cpu, $free, $busy
                }
                catch { }
                Start-Sleep -Seconds 2
            }
        }
    }
    catch { Write-Host "PARITY_RUN_NOTE machine load sampler did not start: $($_.Exception.Message)" }
}

function Complete-MachineLoadSampler {
    if (-not $script:MachineLoadStart) { return }
    try {
        $samples = @()
        if ($script:MachineLoadJob) {
            $samples = @(Receive-Job -Job $script:MachineLoadJob -ErrorAction SilentlyContinue)
            Stop-Job -Job $script:MachineLoadJob -ErrorAction SilentlyContinue
            Remove-Job -Job $script:MachineLoadJob -Force -ErrorAction SilentlyContinue
            $script:MachineLoadJob = $null
        }
        $cores = [Environment]::ProcessorCount
        $cpu = @(); $free = @(); $byName = @{}
        foreach ($line in $samples) {
            $parts = "$line".Split('|')
            if ($parts.Count -lt 4) { continue }
            $cpu += [double] $parts[1]
            $free += [int] $parts[2]
            foreach ($pair in @($parts[3].Split(';') | Where-Object { $_ })) {
                $kv = $pair.Split('=')
                if ($kv.Count -ne 2) { continue }
                $prior = if ($byName.ContainsKey($kv[0])) { [double] $byName[$kv[0]] } else { 0.0 }
                $byName[$kv[0]] = $prior + [double] $kv[1]
            }
        }
        $n = [Math]::Max(1, $cpu.Count)
        $top = @($byName.GetEnumerator() | ForEach-Object {
                [pscustomobject][ordered]@{ name = $_.Key; machine_pct = [Math]::Round($_.Value / $n / $cores, 1) }
            } | Sort-Object machine_pct -Descending | Select-Object -First 5)
        $sorted = @($cpu | Sort-Object)
        $script:MachineLoad = [pscustomobject][ordered]@{
            schema = "opennova.parity-machine-load.v1"
            samples = $cpu.Count
            cpu_pct_mean = $(if ($cpu.Count) { [Math]::Round(($cpu | Measure-Object -Average).Average, 1) } else { $null })
            cpu_pct_p90 = $(if ($sorted.Count) { $sorted[[Math]::Min($sorted.Count - 1, [int] [Math]::Floor($sorted.Count * 0.9))] } else { $null })
            cpu_pct_max = $(if ($cpu.Count) { ($cpu | Measure-Object -Maximum).Maximum } else { $null })
            available_mb_min = $(if ($free.Count) { ($free | Measure-Object -Minimum).Minimum } else { $null })
            logical_cores = $cores
            top_processes = $top
            build_lock_at_start = $script:MachineLoadStart.lock
            build_lock_at_end = Get-BuildLockOwner
        }
        Write-Host ("PARITY_RUN_NOTE machine load: cpu mean {0}% p90 {1}% max {2}%, available {3} MB min, top {4}, build lock {5}/{6}" -f
            $script:MachineLoad.cpu_pct_mean, $script:MachineLoad.cpu_pct_p90, $script:MachineLoad.cpu_pct_max,
            $script:MachineLoad.available_mb_min, (($top | ForEach-Object { "$($_.name) $($_.machine_pct)%" }) -join ", "),
            $script:MachineLoad.build_lock_at_start, $script:MachineLoad.build_lock_at_end)
    }
    catch { Write-Host "PARITY_RUN_NOTE machine load not recorded: $($_.Exception.Message)" }
}

function Begin-SteadyWindow {
    if ($script:SteadyStartedUtc) { throw "Steady window was already started" }
    if (-not $script:DesktopResetBeforeWindow) {
        try { Reset-DesktopCursor }
        catch { Write-Host "PARITY_RUN_NOTE desktop probe failed at the steady start: $($_.Exception.Message)" }
    }
    $script:SteadyStartedUtc = [DateTime]::UtcNow.ToString("o")
    if (-not $script:MachineLoadStart) { Start-MachineLoadSampler }
}

function Publish-MotionGate {
    if (-not $ExerciseInput -or $Topology -notin @("RO", "OO")) {
        throw "Motion gates are valid only for exercised RO/OO runs"
    }
    if (-not $script:SteadyStartedUtc -or $script:SteadyCompletedUtc) {
        throw "Motion gate publication must immediately follow steady-window start"
    }
    if ($script:JoinerMotionRunId) {
        throw "Motion gate must be create-new for run $RunId"
    }
    $script:OpenNovaJoiner.Refresh()
    if ($script:OpenNovaJoiner.HasExited) {
        throw "OpenNova joiner exited before motion-gate publication"
    }
    $started = [DateTimeOffset]::Parse(
        $script:SteadyStartedUtc,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind
    ).ToUniversalTime()
    $requestedUtc = [DateTimeOffset]::UtcNow
    if ($requestedUtc -lt $started -or ($requestedUtc - $started).TotalMilliseconds -gt 2000.0) {
        throw "Motion gate was not requested immediately after steady-window start"
    }
    # The exercise runs inside the joiner (walk/strafe/turn through its real
    # input path) and blocks until complete; its witness is the verdict data.
    $status = Invoke-GameProbe -Port $JoinerMcpPort -Name "parity_joiner_motion" -Process $script:OpenNovaJoiner `
        -TimeoutSeconds 60 -PollMs 500 -Arguments @{
            run_id = $RunId
            topology = $Topology
            steady_started_utc = $script:SteadyStartedUtc
            readiness_mode = $ReadinessMode
            auto_deploy = [bool] $AutoDeploy
        }
    $script:JoinerMotionRunId = [string] $status.run_id
    if ([string] $status.state -ne "passed" -or -not $status.verdict -or
            -not [bool] $status.verdict.ok) {
        $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
        throw "OpenNova joiner motion exercise did not complete: state=$($status.state) $why"
    }
    $witness = $status.verdict.data
    if (-not [bool] $witness.motion_complete -or -not [bool] $witness.motion_gate_observed -or
            [string] $witness.motion_gate_run_id -cne $RunId -or
            [string] $witness.motion_gate_steady_started_utc -cne $script:SteadyStartedUtc -or
            [int] $witness.process_id -ne $script:OpenNovaJoiner.Id) {
        throw "OpenNova joiner motion witness does not bind this run"
    }
    $created = ConvertFrom-RunnerStrictUtcTimestamp ([string] $witness.motion_gate_created_utc)
    if ($created -lt $started -or ($created - $started).TotalMilliseconds -gt 2000.0) {
        throw "Motion gate was not created immediately after steady-window start"
    }
    $script:MotionGateWitness = [pscustomobject]@{
        schema = "opennova.parity-motion-gate.v2"
        run_id = $RunId
        topology = $Topology
        joiner_pid = $script:OpenNovaJoiner.Id
        joiner_mcp_port = $JoinerMcpPort
        probe_run_id = $script:JoinerMotionRunId
        steady_started_utc = $script:SteadyStartedUtc
        created_utc = [string] $witness.motion_gate_created_utc
        pre_roll_ms = [int] $witness.motion_pre_roll_ms
        inter_phase_gap_ms = [int] $witness.motion_inter_phase_gap_ms
        look_samples_per_phase = [int] $witness.look_samples_per_phase
        started_ticks_msec = [int64] $witness.motion_started_ticks_msec
        completed_ticks_msec = [int64] $witness.motion_completed_ticks_msec
        forward_ms = 1800
        forward_look = 320
        strafe_ms = 1200
        strafe_look = -320
        return_ms = 900
        return_look = 160
        detail = ConvertTo-OpenNovaJoinerReadinessSnapshot $witness
    }
}

# The SERVER copy's mutable files. Both hosts run in that copy: retail's
# Jointops.exe in its install, ours through --working-dir. Each reads game.cfg
# and player.sav at its start and writes them, and weapon.sav (the root's and
# the expansion's), at its save points: retail saves at its exit
# (Game_SaveConfig from Game_Run). Every cell starts from the same bytes:
# game.cfg's are the case table's pin (-ServerGameCfgSha256), and the cleanup
# writes each file's start bytes back once every process has stopped (and
# deletes one the cell created).
$script:ServerCopyFiles = @()
function Get-BytesSha256([byte[]] $Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes)) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Save-ServerCopyFiles {
    $relative = @("game.cfg", "player.sav", "weapon.sav", "admin.cfg")
    if ($Expansion) { $relative += (Join-Path (Join-Path "expansion" $Expansion) "weapon.sav") }
    $script:ServerCopyFiles = @(foreach ($rel in $relative) {
        $path = Join-Path $ServerDir $rel
        $exists = Test-Path -LiteralPath $path -PathType Leaf
        $bytes = if ($exists) { [IO.File]::ReadAllBytes($path) } else { $null }
        $sha = if ($exists) { Get-BytesSha256 $bytes } else { "" }
        [pscustomobject][ordered]@{
            file = $rel
            path = $path
            bytes = $bytes
            sha256 = $sha
            effective_sha256 = $sha
            written_during_cell = $false
            restored = $false
        }
    })
    $cfg = $script:ServerCopyFiles | Where-Object { $_.file -eq "game.cfg" }
    if (-not $cfg.sha256) { throw "The SERVER copy has no game.cfg: $($cfg.path)" }
    if ($ServerGameCfgSha256 -and $cfg.sha256 -cne $ServerGameCfgSha256) {
        throw "SERVER game.cfg is $($cfg.sha256), not the pinned $ServerGameCfgSha256 (a cell left it changed, or the copy moved on: restore it, or re-pin the case table)."
    }
}

function Restore-ServerCopyFiles {
    if (-not $script:ServerCopyFiles.Count) { return }
    foreach ($process in @($script:RetailHostProcess, $script:RetailProcess, $script:OpenNovaHost)) {
        if ($process) {
            try { $null = $process.WaitForExit(15000) } catch {}
        }
    }
    $failures = @()
    foreach ($entry in $script:ServerCopyFiles) {
        $present = Test-Path -LiteralPath $entry.path -PathType Leaf
        $now = if ($present) { (Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash.ToLowerInvariant() } else { "" }
        $entry.written_during_cell = $now -ne $entry.effective_sha256
        if ($now -eq $entry.sha256) { $entry.restored = $true; continue }
        if ($null -eq $entry.bytes) {
            Remove-Item -LiteralPath $entry.path -Force
        } else {
            [IO.File]::WriteAllBytes($entry.path, $entry.bytes)
        }
        $after = if (Test-Path -LiteralPath $entry.path -PathType Leaf) { (Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash.ToLowerInvariant() } else { "" }
        if ($after -ne $entry.sha256) {
            $failures += "$($entry.file) left $after, not the cell's start $($entry.sha256)"
        } else {
            $entry.restored = $true
        }
    }
    foreach ($override in $script:ServerGameCfgOverrides) {
        $override.restored = [bool] ($script:ServerCopyFiles | Where-Object { $_.file -eq "game.cfg" }).restored
    }
    if ($script:ServerPlayerClassOverride) {
        $script:ServerPlayerClassOverride.restored = [bool] ($script:ServerCopyFiles |
            Where-Object { $_.file -eq $script:ServerPlayerClassOverride.file }).restored
    }
    if ($failures.Count) { throw ("SERVER copy restore: " + ($failures -join "; ")) }
}

# The cell's writes into the SERVER copy's game.cfg, one integer key each:
# E2's time_limit and E5's mp_permanent_death (and D-NET-167's two side
# passwords, Set-ServerGameCfgString below). The retail host reads the file
# at its start and the OpenNova host runs in the same copy; each write starts
# from the file as the previous one left it, and the cleanup's restore takes
# the copy back to the cell's start bytes (the pin).
$script:ServerGameCfgOverrides = [System.Collections.Generic.List[object]]::new()
function Set-ServerGameCfgValue {
    param([Parameter(Mandatory = $true)] [string] $Key, [Parameter(Mandatory = $true)] [int] $Value)
    $cfg = $script:ServerCopyFiles | Where-Object { $_.file -eq "game.cfg" }
    $path = $cfg.path
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $before = [IO.File]::ReadAllBytes($path)
    $text = $latin1.GetString($before)
    $found = [regex]::Matches($text, '(?m)^[ \t]*' + [regex]::Escape($Key) + '[ \t]*=[ \t]*"?(\d+)"?')
    if ($found.Count -ne 1) {
        throw "SERVER game.cfg holds $($found.Count) $Key lines; the cell's write needs exactly one."
    }
    $digits = $found[0].Groups[1]
    $updated = $text.Substring(0, $digits.Index) + [string] $Value +
        $text.Substring($digits.Index + $digits.Length)
    $bytes = $latin1.GetBytes($updated)
    $override = [pscustomobject][ordered]@{
        path = $path
        key = $Key
        value = $Value
        original_value = [int] $digits.Value
        original_sha256 = $cfg.sha256
        effective_sha256 = Get-BytesSha256 $bytes
        restored = $false
    }
    $script:ServerGameCfgOverrides.Add($override)
    $cfg.effective_sha256 = $override.effective_sha256
    [IO.File]::WriteAllBytes($path, $bytes)
    Write-Host "PARITY_RUN_NOTE SERVER game.cfg $Key $($digits.Value) -> $Value for this cell"
}

# A quoted string key's write (D-NET-167's side passwords), as
# Set-ServerGameCfgValue writes an integer one: exactly one line, the value
# between its quotes replaced, the restore the copy's.
function Set-ServerGameCfgString {
    param([Parameter(Mandatory = $true)] [string] $Key, [Parameter(Mandatory = $true)] [string] $Value)
    $cfg = $script:ServerCopyFiles | Where-Object { $_.file -eq "game.cfg" }
    $path = $cfg.path
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $before = [IO.File]::ReadAllBytes($path)
    $text = $latin1.GetString($before)
    $found = [regex]::Matches($text, '(?m)^[ \t]*' + [regex]::Escape($Key) + '[ \t]*=[ \t]*"([^"\r\n]*)"')
    if ($found.Count -ne 1) {
        throw "SERVER game.cfg holds $($found.Count) quoted $Key lines; the cell's write needs exactly one."
    }
    $quoted = $found[0].Groups[1]
    $updated = $text.Substring(0, $quoted.Index) + $Value + $text.Substring($quoted.Index + $quoted.Length)
    $bytes = $latin1.GetBytes($updated)
    $override = [pscustomobject][ordered]@{
        path = $path
        key = $Key
        value = $Value
        original_value = [string] $quoted.Value
        original_sha256 = $cfg.sha256
        effective_sha256 = Get-BytesSha256 $bytes
        restored = $false
    }
    $script:ServerGameCfgOverrides.Add($override)
    $cfg.effective_sha256 = $override.effective_sha256
    [IO.File]::WriteAllBytes($path, $bytes)
    Write-Host "PARITY_RUN_NOTE SERVER game.cfg $Key '$($quoted.Value)' -> '$Value' for this cell"
}

# E5's medic: the host player's class into the SERVER copy's active weapon.sav
# record (game.cfg's player_index), both side bytes. weapon.sav is a 16-byte
# header ("FPBC", "0211") then five 0x1080C-byte records; a record's blue side
# block starts at +0 and its red at +0x8006, each with the class byte first
# [orig: PlayerProfile_LoadAllFromDisk @0x54F4D0, the record loop; the
# session start's reads @0x5516BB..0x551720]. The cleanup's restore writes the
# file's start bytes back.
$script:ServerPlayerClassOverride = $null
function Set-ServerHostPlayerClass {
    $cfgText = [Text.Encoding]::GetEncoding(28591).GetString(
        [IO.File]::ReadAllBytes(($script:ServerCopyFiles | Where-Object { $_.file -eq "game.cfg" }).path))
    $index = [regex]::Matches($cfgText, '(?m)^[ \t]*player_index[ \t]*=[ \t]*"?(\d+)"?')
    if ($index.Count -ne 1) {
        throw "SERVER game.cfg holds $($index.Count) player_index lines; the class write needs exactly one."
    }
    $slot = [int] $index[0].Groups[1].Value
    if ($slot -gt 4) { throw "SERVER game.cfg player_index $slot names no weapon.sav record (0..4)." }
    $rel = if ($Expansion) { Join-Path (Join-Path "expansion" $Expansion) "weapon.sav" } else { "weapon.sav" }
    $entry = $script:ServerCopyFiles | Where-Object { $_.file -eq $rel }
    if (-not $entry -or $null -eq $entry.bytes) { throw "The SERVER copy has no $rel to write the host's class into." }
    $bytes = [byte[]] $entry.bytes.Clone()
    $record = 16 + $slot * 0x1080C
    $blue = $record
    $red = $record + 0x8006
    $magic = [Text.Encoding]::ASCII.GetString($bytes, 0, 8)
    if ($magic -cne "FPBC0211" -or $bytes.Length -lt $record + 0x1080C) {
        throw "SERVER $rel is not a weapon.sav with record $slot (header '$magic', $($bytes.Length) bytes)."
    }
    $original = @([int] $bytes[$blue], [int] $bytes[$red])
    $bytes[$blue] = [byte] $HostPlayerClass
    $bytes[$red] = [byte] $HostPlayerClass
    $script:ServerPlayerClassOverride = [pscustomobject][ordered]@{
        file = $rel
        record = $slot
        offsets = @($blue, $red)
        value = $HostPlayerClass
        original_values = $original
        original_sha256 = $entry.sha256
        effective_sha256 = Get-BytesSha256 $bytes
        restored = $false
    }
    $entry.effective_sha256 = $script:ServerPlayerClassOverride.effective_sha256
    [IO.File]::WriteAllBytes($entry.path, $bytes)
    Write-Host "PARITY_RUN_NOTE SERVER $rel record $slot class $($original -join '/') -> $HostPlayerClass for this cell"
}

# -PreJoinMissionCycle: the host's admin console for the cell, its MISSION
# CYCLE, and the wait for the next mission (the param's comment). The witness
# rides the summary's pre_join_mission_cycle.
$script:HostAdminUser = "parity"
$script:HostAdminPassword = "parity"
$script:PreJoinCycleWitness = $null
$AdminClientScript = Join-Path $Repo "scripts\net\admin_client.py"

function Enable-HostAdminConsole {
    Set-ServerGameCfgValue -Key "remote_admin_port" -Value $HostAdminPort
    $entry = $script:ServerCopyFiles | Where-Object { $_.file -eq "admin.cfg" }
    $text = "// run_parity_topology.ps1 -PreJoinMissionCycle: the host's admin console for one cell; its restore takes this file back`r`n" +
        "ip_restrict = 127.*`r`n$($script:HostAdminUser) $($script:HostAdminPassword) FFFFFFFF`r`n"
    $bytes = [Text.Encoding]::ASCII.GetBytes($text)
    $entry.effective_sha256 = Get-BytesSha256 $bytes
    [IO.File]::WriteAllBytes($entry.path, $bytes)
    Write-Host "PARITY_RUN_NOTE SERVER admin.cfg: one full-rights user, 127.* only, for this cell"
}

function Invoke-HostAdmin {
    param([Parameter(Mandatory = $true)] [string[]] $Arguments)
    $lines = @(& python -I $AdminClientScript @Arguments)
    return ($lines -join "`n") | ConvertFrom-Json
}

# The plaintext QUERY's status on the host's console, or $null while it does
# not answer (a host in its mission load).
function Get-HostAdminStatus {
    $reply = Invoke-HostAdmin -Arguments @("query", "127.0.0.1", [string] $HostAdminPort, "--timeout", "5")
    if ($reply.PSObject.Properties.Name -contains "error") { return $null }
    $text = (@($reply.replies) | Where-Object { $_.PSObject.Properties.Name -contains "text" } |
        ForEach-Object { [string] $_.text }) -join ""
    if (-not $text) { return $null }
    return [pscustomobject][ordered]@{ utc = [DateTime]::UtcNow.ToString("o"); text = $text }
}

# One look at the host: its logic tick (a retail host's through
# onhook_pool_entities, the tick its frame callback observed; ours through
# game_state's runtime), whether it is loading a mission, and whether it
# stands in a mission with its own player. A read that fails (a host in its
# mission load) leaves the tick $null.
function Get-HostCycleSample {
    param([string] $RetailHostInstanceId = "")
    $sample = [ordered]@{ utc = [DateTime]::UtcNow.ToString("o"); tick = $null; loading = $false; ready = $false }
    try {
        if ($RetailHostInstanceId) {
            $result = Invoke-OnHookTool -Name "onhook_pool_entities" -AllowError -Arguments @{
                instance_id = $RetailHostInstanceId
                pool = 0
                max = 1
            }
            if (-not $result.isError) {
                $pool = Get-StructuredResult $result
                if ($pool.PSObject.Properties.Name -contains "logic_tick") { $sample.tick = [long] $pool.logic_tick }
            }
            $instance = @((Get-StructuredResult (Invoke-OnHookTool -Name "onhook_instances")).instances) |
                Where-Object { [string] $_.instance_id -eq $RetailHostInstanceId } | Select-Object -First 1
            if ($instance) {
                $sample.loading = [string] $instance.run_state.phase -ne "in_match"
                $sample.ready = [string] $instance.run_state.phase -eq "in_match" -and [bool] $instance.run_state.local_player
            }
        }
        else {
            $state = Get-OpenNovaHostState
            if ($state.runtime.PSObject.Properties.Name -contains "logic_tick") { $sample.tick = [long] $state.runtime.logic_tick }
            $sample.loading = [bool] $state.shell.world_loading
            $sample.ready = [bool] $state.shell.world_loaded -and [bool] $state.player.present
        }
    }
    catch {
        Write-Host "PARITY_RUN_NOTE pre-join cycle: the host read failed: $($_.Exception.Message)"
    }
    return [pscustomobject] $sample
}

function Invoke-PreJoinMissionCycle {
    param([int] $RetailHostProcessId = 0, [int] $TimeoutSeconds = 240)
    if (-not $PreJoinMissionCycle) { return }
    $retailInstanceId = ""
    if ($RetailHostProcessId -ne 0) {
        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        while (-not $retailInstanceId) {
            $instance = @((Get-StructuredResult (Invoke-OnHookTool -Name "onhook_instances")).instances) |
                Where-Object { [int] $_.pid -eq $RetailHostProcessId } | Select-Object -First 1
            if ($instance) { $retailInstanceId = [string] $instance.instance_id; break }
            if ([DateTime]::UtcNow -ge $deadline) { throw "Pre-join cycle: no onHook instance for the retail host PID $RetailHostProcessId" }
            Start-Sleep -Milliseconds 500
        }
    }
    $witness = [ordered]@{
        admin_port = $HostAdminPort
        host = $(if ($RetailHostProcessId -ne 0) { "retail" } else { "opennova" })
        status_before = $null
        tick_before = $null
        cycle = $null
        ticks = [System.Collections.Generic.List[object]]::new()
        restart_seen_utc = $null
        ready_utc = $null
        status_after = $null
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while (-not $witness.status_before) {
        $witness.status_before = Get-HostAdminStatus
        if ($witness.status_before) { break }
        if ([DateTime]::UtcNow -ge $deadline) { throw "Pre-join cycle: the host's admin console on TCP $HostAdminPort never answered QUERY" }
        Start-Sleep -Seconds 1
    }
    $before = Get-HostCycleSample -RetailHostInstanceId $retailInstanceId
    $witness.tick_before = $before.tick
    if ($null -eq $before.tick -or -not $before.ready) {
        throw "Pre-join cycle: the host is not in its first mission before the cycle: $($before | ConvertTo-Json -Compress)"
    }
    $cycle = Invoke-HostAdmin -Arguments @("run", "127.0.0.1", [string] $HostAdminPort,
        $script:HostAdminUser, $script:HostAdminPassword, "MISSION CYCLE")
    $witness.cycle = $cycle
    $replies = @()
    if ($cycle.PSObject.Properties.Name -contains "steps") {
        $replies = @($cycle.steps | ForEach-Object { @($_.replies) } |
            Where-Object { $_.PSObject.Properties.Name -contains "text" } | ForEach-Object { [string] $_.text })
    }
    if ($replies -notcontains "OK - Server is cycling...") {
        throw "Pre-join cycle: MISSION CYCLE was not taken: $($cycle | ConvertTo-Json -Depth 6 -Compress)"
    }
    Write-Host "PARITY_RUN_NOTE pre-join cycle: MISSION CYCLE taken at host tick $($witness.tick_before)"
    # The restart: the host's tick falls back, or it is seen loading. The
    # joiner then waits for the host's own player and 310 ticks (5 s) of the
    # next mission, past the 62nd server tick the probe's word 1 counts from.
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $last = [long] $witness.tick_before
    $restartAt = $null
    while (-not $witness.ready_utc) {
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Pre-join cycle: the host's next mission did not run within $TimeoutSeconds s (restart seen: $([bool] $witness.restart_seen_utc))"
        }
        Start-Sleep -Milliseconds 500
        $sample = Get-HostCycleSample -RetailHostInstanceId $retailInstanceId
        $witness.ticks.Add($sample)
        if (-not $witness.restart_seen_utc -and
                ($sample.loading -or ($null -ne $sample.tick -and $sample.tick -lt $last))) {
            $witness.restart_seen_utc = $sample.utc
            $restartAt = [DateTime]::UtcNow
        }
        if ($null -ne $sample.tick) { $last = $sample.tick }
        if ($restartAt -and $sample.ready -and -not $sample.loading -and $null -ne $sample.tick -and
                $sample.tick -ge 310 -and ([DateTime]::UtcNow - $restartAt).TotalSeconds -ge 5) {
            $witness.ready_utc = $sample.utc
        }
    }
    $witness.status_after = Get-HostAdminStatus
    $script:PreJoinCycleWitness = [pscustomobject] $witness
    Write-Host "PARITY_RUN_NOTE pre-join cycle: the next mission runs (restart $($witness.restart_seen_utc), ready $($witness.ready_utc)); the joiner starts"
}

# The first record of one S2C tag on the rolling external capture, at or after
# -AfterUtc when given (wait_capture_tag.ps1, the wire-readiness gate's
# snapshot decode). With -OutPath it runs in the background and returns its
# process (the caller reads the file with ConvertFrom-CaptureTagLine); without,
# the call blocks and returns the record (capture frame, ts_ns) or $null on its
# timeout.
function Start-CaptureTagWait {
    param(
        [Parameter(Mandatory = $true)] [string] $Tag,
        [Parameter(Mandatory = $true)] [int] $TimeoutSeconds,
        [DateTime] $AfterUtc,
        [string] $OutPath = ""
    )
    if (-not $script:ExternalCapture) { throw "A capture tag wait needs the rolling external capture." }
    $itemsPath = Join-Path (Split-Path -Parent (Split-Path -Parent $ResolvedMissionArtifact)) "items.def"
    $afterNs = if ($PSBoundParameters.ContainsKey("AfterUtc")) {
        ([DateTimeOffset] $AfterUtc).ToUnixTimeMilliseconds() * 1000000
    } else { 0 }
    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $CaptureTagScript,
        "-Capture", $script:ExternalCapture, "-Items", $itemsPath, "-Direction", "S", "-Tag", $Tag,
        "-AfterUnixNs", [string] $afterNs, "-TimeoutSeconds", [string] $TimeoutSeconds)
    if ($OutPath) {
        return Start-Process -FilePath powershell.exe -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $OutPath -RedirectStandardError "$OutPath.err" `
            -ArgumentList @($arguments | ForEach-Object { if ("$_" -match '\s') { "`"$_`"" } else { "$_" } })
    }
    return ConvertFrom-CaptureTagLine @(& powershell.exe @arguments)
}

function ConvertFrom-CaptureTagLine {
    param([object[]] $Lines)
    foreach ($line in @($Lines)) {
        if ("$line" -match '^PARITY_CAPTURE_TAG frame=(\d+) ts_ns=(\d+)') {
            return [pscustomobject]@{ frame = [int64] $Matches[1]; ts_ns = [int64] $Matches[2] }
        }
    }
    return $null
}

# -ServeOnlyRoundEnd (a Serve Only cell): while the window runs, the rolling
# external capture is read for its first S2C 0x1D (the round's end), and the
# host's frame is saved 1 s after it, or at once when the round ended before
# the window opened (serve-only-round-end-frame.png; -RetailInstanceId names a
# retail host). x8's retail Serve Only RR: the 0x1D came 2.4 s before the
# window, nine 5 s countdowns ran in it, and its S2C 0x25 came at 42 s, so the
# deadline frame showed the next mission's load.
function Wait-UntilSteadyDeadline {
    param([switch] $ServeOnlyRoundEnd, [string] $RetailInstanceId = "")
    if (-not $script:SteadyStartedUtc -or $script:SteadyCompletedUtc) {
        throw "Steady deadline wait is out of order"
    }
    $started = [DateTime]::Parse(
        $script:SteadyStartedUtc,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind
    ).ToUniversalTime()
    $deadline = $started.AddSeconds($SteadySeconds)
    $watch = $null
    $watchOut = Join-Path $RunRoot "serve-only-round-end.txt"
    $roundEnd = $null
    if ($ServeOnly -and $ServeOnlyRoundEnd) {
        $watch = Start-CaptureTagWait -Tag "0x1d" -TimeoutSeconds ($SteadySeconds + 5) -OutPath $watchOut
        $script:ServeOnlyRoundEnd = [ordered]@{ watch_started_utc = [DateTime]::UtcNow.ToString("o"); tag = "S2C 0x1d" }
    }
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($watch -and -not $script:ServeOnlyRoundEnd.Contains("frame_path")) {
            if (-not $roundEnd -and (Test-Path -LiteralPath $watchOut)) {
                $roundEnd = ConvertFrom-CaptureTagLine @(Get-Content -LiteralPath $watchOut -ErrorAction SilentlyContinue)
                if ($roundEnd) {
                    $script:ServeOnlyRoundEnd.capture_frame = $roundEnd.frame
                    $script:ServeOnlyRoundEnd.capture_ts_ns = $roundEnd.ts_ns
                    $script:ServeOnlyRoundEnd.seen_utc = [DateTime]::UtcNow.ToString("o")
                }
            }
            if ($roundEnd -and [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() -ge [int64] ($roundEnd.ts_ns / 1000000) + 1000) {
                $script:ServeOnlyRoundEnd.frame_path = Save-ServeOnlyHostFrame -RetailInstanceId $RetailInstanceId `
                    -Name "serve-only-round-end-frame.png"
                $script:ServeOnlyRoundEnd.frame_utc = [DateTime]::UtcNow.ToString("o")
            }
        }
        $remainingMs = [int][Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        Start-Sleep -Milliseconds ([Math]::Min(250, [Math]::Max(1, $remainingMs)))
    }
    if ($watch) {
        if (-not $watch.HasExited) { Stop-Process -Id $watch.Id -Force -ErrorAction SilentlyContinue }
        $script:ServeOnlyRoundEnd.watch_exit = $(if ($watch.HasExited) { $watch.ExitCode } else { $null })
        if (-not $script:ServeOnlyRoundEnd.Contains("frame_path")) {
            Write-Host "PARITY_RUN_NOTE Serve Only: no S2C 0x1d in the window's capture, so no round-end frame"
        }
        $script:ServeOnlyRoundEnd = [pscustomobject] $script:ServeOnlyRoundEnd
    }
}

# One scripted stage on the joiner, waited to its end: the setup pose first
# when one is given, then the steps. A retail joiner takes the pose through
# onhook_load_debug_snapshot and the steps through onhook_play_input
# (retail's own key and mouse handlers at the Input_ProcessFrame seams); an
# OpenNova joiner takes both through the scenario_play probe (ScriptedInput
# at the ControlsModel device seam). Both report each step's applied tick.
# One opennova.debug_snapshot.v1 file for a scenario pose, create-new.
function Write-ScenarioPoseSnapshot {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [Parameter(Mandatory = $true)] $Pose
    )
    if (Test-Path -LiteralPath $Path) {
        throw "Scenario pose snapshot must be create-new: $Path"
    }
    $position = @($Pose.position_bms)
    [pscustomobject]@{
        schema = "opennova.debug_snapshot.v1"
        player = [pscustomobject]@{
            position_bms = [pscustomobject]@{
                x = $position[0]; y = $position[1]; z = $position[2]
            }
            orientation_mission_deg = [pscustomobject]@{
                yaw = $Pose.yaw; pitch = $Pose.pitch
            }
        }
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Path -Encoding ASCII
}

# E5: the scenario's staging positions, given by the host to each listed slot
# in order: just before the window after the actor's setup pose (a scenario
# with no prelude), or after the prelude and before the setup and steps. A
# retail host applies them through onhook_load_debug_snapshot target_slot
# (opennova-int #74); an OpenNova host through the stage_slot probe
# (godot/probes/net/stage_slot_probe.gd: the authority's three Position words
# of the body the slot holds, inmatch::stage_slot_position), which answers a
# result per entry, and a refused entry fails the cell. A position sticks
# only when it is more than two units from where that player stands: nearer,
# its own uplink pulls it back; past two units its client snaps to the
# host's [orig: Entity_UpdateInfantryPlayerBody @0x4B42C1..0x4B434A]. The
# snapshot format needs an orientation, so the retail leg writes yaw 0 and
# pitch 0; the player's own client puts its heading back on the next frame.
#
# A scenario's staging_tick (R-L4-s32-1, R-LX-s26-1) stages at one absolute
# host tick on every topology: a camp AI's think key is the host's
# g_CurrentTick + 36 x DcbId [orig: Entity_UpdateInfantryAI @0x4B9948..0x4B9953],
# its perception runs when the key & 0x1F is 0 with its range staged by
# (key >> 5) & 3 [orig: @0x4BBE3E..0x4BBEB3], and its aim's pitch error phase is
# ((key >> 9) + (key >> 2)) & 0x3F [orig: Entity_UpdateInfantryAI @0x4B9910
# (`sar ebx, 9; add ebx, eax; and ebx, 3Fh` @0x4BC61E..0x4BC623)], so which AI
# sees, turns on and hits a staged player first is set by the tick the staging
# lands on, its high bits included (L4-s32: one aim class gives the same shot
# on both hosts). Both hosts count that tick from the mission start: retail
# zeroes g_CurrentTick on every role, past every load path [orig:
# Game_StartMission @0x524360 (`mov g_CurrentTick, ebx` @0x525B9F)], and steps
# it once a main frame [orig: Game_ProcessMainFrame @0x5263F0 (`add
# g_CurrentTick, ebx` @0x5265B4)]; ours is World::logic_tick
# (MissionKernel::begin_mission_start's zero, the step in
# World::run_entity_pass). An OpenNova host stages inside the stage_slot probe
# on the first frame whose logic tick reaches the value (the probe reports it).
# A retail host's tick is onhook_pool_entities' logic_tick, g_CurrentTick at
# its frame callback, and it steps every 16 ms of GetTickCount time, 62.5 a
# second: the main loop banks 16 units a millisecond, drains 64 a pass and
# calls the mode's update (in a mission g_GameModeGameLoop's
# Game_ProcessMainFrame) on every fourth pass [orig: Game_MainLoop @0x52B630
# (`shl ecx, 4` @0x52B7AA, `add esi, ecx` @0x52B7B2, `sub esi, 40h` @0x52BA21,
# `test byte ptr g_TickPhase, 3` @0x52BA40, `call eax` @0x52BA58)]. The runner
# sleeps through the gap at that rate but its last 30 ticks (a sleep and the
# next read come back late, never early: x10's ran 31..77 ms over), reads
# those back to back (a read every 3 to 5 ticks) and stages on the first read
# at most 6 ticks ahead, so that read lands 2 to 6 ticks ahead. The pose lands
# inside onhook_load_debug_snapshot, on the frame callback that serves it, 3
# to 5 ticks after the read's sample (x10: 33..83 ms calls), so the landing
# centres on the value as the probe's does; a read after the last entry
# bounds it from above (x10: 6 to 8 ticks after the read). A staging outside
# value +- tolerance fails the cell.
function Read-RetailHostTick {
    param([Parameter(Mandatory = $true)] [string] $RetailHostInstanceId)
    $pool = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_pool_entities" -Arguments @{
        instance_id = $RetailHostInstanceId
        pool = 0
        max = 1
    })
    if (-not ($pool.PSObject.Properties.Name -contains "logic_tick") -or $null -eq $pool.logic_tick) {
        throw "Staging tick: the retail host's pool read carries no logic_tick (its g_CurrentTick did not resolve)"
    }
    return [long] $pool.logic_tick
}

function Wait-RetailHostTick {
    param([Parameter(Mandatory = $true)] [string] $RetailHostInstanceId, [Parameter(Mandatory = $true)] $HostTick,
        [string] $Label = "Staging tick")
    $leadTicks = 6
    $value = [long] $HostTick.value
    $started = [DateTime]::UtcNow
    $deadline = $started.AddSeconds(50)
    $reads = 0
    $firstTick = $null
    while ($true) {
        $tick = Read-RetailHostTick -RetailHostInstanceId $RetailHostInstanceId
        ++$reads
        if ($null -eq $firstTick) { $firstTick = $tick }
        $ahead = $value - $tick
        if ($ahead -lt -[long] $HostTick.tolerance) {
            throw "${Label}: the retail host was already at tick $tick, past $value + $($HostTick.tolerance), when it was due ($reads reads)"
        }
        if ($ahead -le $leadTicks) {
            return [pscustomobject][ordered]@{
                tick = $tick
                ahead = $ahead
                lead_ticks = $leadTicks
                reads = $reads
                first_tick = $firstTick
                waited_ms = [int] ([DateTime]::UtcNow - $started).TotalMilliseconds
            }
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "${Label}: the retail host's tick never came within $leadTicks of $value in 50 s ($reads reads, last $tick)"
        }
        # Sleep at retail's 16 ms a tick through all but the gap's last 30
        # ticks; those are read back to back.
        $sleepTicks = $ahead - $leadTicks - 24
        if ($sleepTicks -gt 0) { Start-Sleep -Milliseconds ([int] ($sleepTicks * 16)) }
    }
}

function Invoke-ScenarioStaging {
    param([string] $RetailHostInstanceId = "")
    $spec = $script:ScenarioSpec
    if (-not ($spec.PSObject.Properties.Name -contains "staging")) { return $null }
    $hostTick = if ($spec.PSObject.Properties.Name -contains "staging_tick") { $spec.staging_tick } else { $null }
    if ($Topology -in @("OR", "OO")) {
        $staging = @(@($spec.staging) | ForEach-Object {
            @{ slot = [int] $_.slot; position_bms = @($_.position_bms) }
        })
        $probeArgs = @{ staging = $staging }
        if ($hostTick) {
            $probeArgs.stage_tick = [int] $hostTick.value
            $probeArgs.stage_tolerance = [int] $hostTick.tolerance
        }
        $status = Invoke-GameProbe -Port $HostMcpPort -Name "stage_slot" -Process $script:OpenNovaHost `
            -TimeoutSeconds 120 -PollMs 500 -Arguments $probeArgs
        $data = if ($status.verdict) { $status.verdict.data } else { $null }
        $entries = @(if ($data -and $data.PSObject.Properties["entries"]) { $data.entries })
        $refused = @($entries | Where-Object { [string] $_.result -ne "staged" } |
            ForEach-Object { "entry $($_.index): $($_.result)" })
        if ([string] $status.state -ne "passed" -or -not [bool] $status.verdict.ok -or
                $entries.Count -ne $staging.Count -or $refused.Count -gt 0) {
            $why = if ($refused.Count -gt 0) { $refused -join "; " }
                elseif ($status.verdict) { [string] $status.verdict.summary }
                else { [string] $status.error }
            throw "OpenNova host did not stage scenario $($spec.name): state=$($status.state) $why"
        }
        $stagingTick = $null
        if ($hostTick) {
            $offset = [long] $data.logic_tick - [long] $hostTick.value
            if ([Math]::Abs($offset) -gt [long] $hostTick.tolerance) {
                throw "OpenNova host staged scenario $($spec.name) at tick $($data.logic_tick), $offset off $($hostTick.value) (tolerance $($hostTick.tolerance))"
            }
            $stagingTick = [pscustomobject][ordered]@{
                value = [int] $hostTick.value
                tolerance = [int] $hostTick.tolerance
                staged_tick = [long] $data.logic_tick
                offset = $offset
                waited_ms = $(if ($data.PSObject.Properties["staging_tick"]) { $data.staging_tick.waited_ms } else { $null })
            }
        }
        return [pscustomobject]@{ driver = "stage_slot"; logic_tick = $data.logic_tick; entries = $entries; staging_tick = $stagingTick }
    }
    if (-not $RetailHostInstanceId) {
        throw "Scenario staging on $Topology needs the retail host's instance id"
    }
    # The snapshots are written first, so the tick read leads straight into
    # the first call.
    $paths = @(for ($index = 0; $index -lt @($spec.staging).Count; ++$index) {
        $path = Join-Path $RunRoot "scenario-staging-$index.debug_snapshot.json"
        Write-ScenarioPoseSnapshot -Path $path -Pose ([pscustomobject]@{
            position_bms = @(@($spec.staging)[$index].position_bms); yaw = 0; pitch = 0
        })
        $path
    })
    $before = if ($hostTick) { Wait-RetailHostTick -RetailHostInstanceId $RetailHostInstanceId -HostTick $hostTick } else { $null }
    $applied = [System.Collections.Generic.List[object]]::new()
    $index = 0
    foreach ($entry in @($spec.staging)) {
        $applied.Add((Get-StructuredResult (Invoke-OnHookTool -Name "onhook_load_debug_snapshot" -Arguments @{
            instance_id = $RetailHostInstanceId
            path = $paths[$index]
            target_slot = [int] $entry.slot
        })))
        ++$index
    }
    $stagingTick = $null
    if ($hostTick) {
        $after = Read-RetailHostTick -RetailHostInstanceId $RetailHostInstanceId
        $stagingTick = [pscustomobject][ordered]@{
            value = [int] $hostTick.value
            tolerance = [int] $hostTick.tolerance
            read_before = $before
            tick_after = $after
            offset_before = [long] $before.tick - [long] $hostTick.value
            offset_after = [long] $after - [long] $hostTick.value
        }
        if ([Math]::Abs($stagingTick.offset_after) -gt [long] $hostTick.tolerance) {
            throw "Retail host staged scenario $($spec.name) between ticks $($before.tick) and $after, past $($hostTick.value) + $($hostTick.tolerance)"
        }
    }
    return [pscustomobject]@{ driver = "onhook_load_debug_snapshot"; entries = $applied.ToArray(); staging_tick = $stagingTick }
}

# One read of a player's own body for the window-open wait: a retail process's
# pool-0 record whose DcbId is 10000 + the slot (the host 0, the pair's joiner 1)
# [orig: PlayerClass_InitEntity, the slot byte +0x154 plus 0x2710
# @0x4B1149..0x4B1155 stored to +0x7C @0x4B1173], its Q16 Position words and
# +0x10 heading; an OpenNova process's scenario_play place_read (the local
# player's mission position and heading BAM).
function Read-ScenarioBody {
    param(
        [Parameter(Mandatory = $true)] [ValidateSet("host", "joiner")] [string] $Role,
        [string] $RetailHostInstanceId = "",
        [string] $RetailJoinerInstanceId = ""
    )
    $slot = if ($Role -eq "host") { 0 } else { 1 }
    $retail = if ($Role -eq "host") { $Topology -in @("RR", "RO") } else { $Topology -in @("RR", "OR") }
    if ($retail) {
        $instance = if ($Role -eq "host") { $RetailHostInstanceId } else { $RetailJoinerInstanceId }
        if (-not $instance) {
            throw "Window-open wait on $Topology needs the retail $Role's instance id"
        }
        $pool = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_pool_entities" -Arguments @{
            instance_id = $instance
            pool = 0
            max = 4096
        })
        $self = @(@($pool.entities) | Where-Object { [int] $_.net_id -eq 10000 + $slot })
        if ($self.Count -ne 1) {
            throw "Window-open wait: the retail $Role's pool 0 holds $($self.Count) body record(s) with DcbId $(10000 + $slot)"
        }
        return [pscustomobject][ordered]@{
            driver = "onhook_pool_entities"
            logic_tick = $(if ($pool.PSObject.Properties.Name -contains "logic_tick") { $pool.logic_tick } else { $null })
            q16 = @([long] $self[0].pos[0], [long] $self[0].pos[1], [long] $self[0].pos[2])
            heading_bam = [long] $self[0].yaw_bam
        }
    }
    $port = if ($Role -eq "host") { $HostMcpPort } else { $JoinerMcpPort }
    $process = if ($Role -eq "host") { $script:OpenNovaHost } else { $script:OpenNovaJoiner }
    $status = Invoke-GameProbe -Port $port -Name "scenario_play" -Process $process `
        -TimeoutSeconds 30 -PollMs 250 -Arguments @{ place_read = $true }
    if ([string] $status.state -ne "passed" -or -not $status.verdict -or -not [bool] $status.verdict.ok) {
        $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
        throw "Window-open wait: the OpenNova $Role's place read failed: state=$($status.state) $why"
    }
    $data = $status.verdict.data
    return [pscustomobject][ordered]@{
        driver = "scenario_play"
        logic_tick = $data.logic_tick
        q16 = @($data.position_bms | ForEach-Object { [long] [Math]::Round(([double] $_) * 65536.0) })
        heading_bam = [long] $data.heading_bam
    }
}

# The window opens on the setup's bodies at rest, one rule on every topology:
# each body the setup or the staging posed stands at its spot, and the pair's
# joiner, posed or not, stands still. Every read is recorded, so the triage sees
# what the wait skipped (setup_before_window.at_rest: per body its rule, spot,
# waited ms and logic ticks, and every read's tick, Q16 position, heading BAM
# and distance). Each body throws after 15 s.
# - A posed or staged body (the actor's setup pose, a staged slot 0 or 1) is read
#   until two reads in a row stand within two units (x, y) of its spot; z settles
#   onto the ground. A retail pose lands on the game's next frame, after the
#   onhook_load_debug_snapshot call returns: x7's hostkill RR window opened on
#   one host-body record mid-pose (x 1036.9, heading 0) and the posed (64, 247)
#   followed 0.19 s later, where RR-aa and RO opened on it (and their last
#   pre-window frames still carried the pre-pose body). A staged joiner snaps
#   back to the host's pre-staging frames (its setup pose's position undone, its
#   heading kept) and on to the staged spot only from the first frame after the
#   staging, past two units [orig: Entity_UpdateInfantryPlayerBody
#   @0x4B42C1..0x4B434A, the 0x20000 compare @0x4B431F; the heading copy skipped
#   for the local player @0x4B434D]: x7's coop_ai_kill took that first frame
#   0.14..0.19 s into the window on RR, RR-aa and OR. One read can still be the
#   joiner's own setup write that the last pre-staging frame is about to undo.
# - The pair's joiner with no spot is read until it stands at one planar Q16
#   position across two send periods, 24 logic ticks (LAN mode 1's holdoff of 12
#   ticks [orig: NapiNPServer_GetSendHoldoffTicks @0x4C4ADB], the 0.19 s period
#   of every x7 capture): its own uplink then the host frame echoing it carry the
#   rest. x7's 03tr-dm: the two players spawn on one point and push apart for
#   about 14 s, and the host's pose lands beside the joiner and pushes it again;
#   RR's joiner moved -0.339 -> -0.361 inside the window (the push), RR-aa's
#   -5.28 -> -5.48 (the slide's end), OR's and OO's stopped just before it, so
#   the frame anchor and the uplink read 0 against RR's 8433..2083.
# The host's own body is read first: its pose is what pushes the joiner.
function Wait-ScenarioBodiesAtRest {
    param(
        [string] $RetailJoinerInstanceId = "",
        [string] $RetailHostInstanceId = ""
    )
    $spec = $script:ScenarioSpec
    $names = @($spec.PSObject.Properties.Name)
    $setupAt = $null
    if ($names -contains "setup" -and $null -ne $spec.setup -and
            @($spec.setup.PSObject.Properties.Name) -contains "pose" -and $null -ne $spec.setup.pose) {
        $setupAt = @($spec.setup.pose.position_bms | ForEach-Object { [double] $_ })
    }
    $staged = @{}
    if ($names -contains "staging") {
        foreach ($entry in @($spec.staging)) {
            $slot = [int] $entry.slot
            if ($slot -notin @(0, 1)) {
                throw "Window-open wait: staging slot $slot is neither the host (0) nor the pair's joiner (1)"
            }
            $staged[$slot] = @($entry.position_bms | ForEach-Object { [double] $_ })
        }
    }
    $actor = [string] $spec.actor
    $bodies = [System.Collections.Generic.List[object]]::new()
    $hostSpot = if ($staged.ContainsKey(0)) { $staged[0] } elseif ($actor -eq "host") { $setupAt } else { $null }
    if ($null -ne $hostSpot) {
        $bodies.Add([pscustomobject]@{ slot = 0; role = "host"; spot = $hostSpot })
    }
    $joinerSpot = if ($staged.ContainsKey(1)) { $staged[1] } elseif ($actor -eq "joiner") { $setupAt } else { $null }
    $bodies.Add([pscustomobject]@{ slot = 1; role = "joiner"; spot = $joinerSpot })
    $stillTicks = 24
    $records = foreach ($body in $bodies) {
        $reads = [System.Collections.Generic.List[object]]::new()
        $started = [DateTime]::UtcNow
        $deadline = $started.AddSeconds(15)
        $firstTick = $null
        $inPlace = 0
        $restKey = $null
        $restTick = $null
        $restUtc = $null
        $lastTick = $null
        while ($true) {
            $read = Read-ScenarioBody -Role $body.role -RetailHostInstanceId $RetailHostInstanceId `
                -RetailJoinerInstanceId $RetailJoinerInstanceId
            $now = [DateTime]::UtcNow
            if ($null -eq $firstTick) { $firstTick = $read.logic_tick }
            $lastTick = $read.logic_tick
            $at = @($read.q16 | ForEach-Object { ([double] $_) / 65536.0 })
            $distance = $null
            if ($null -ne $body.spot) {
                $distance = [Math]::Sqrt([Math]::Pow($at[0] - $body.spot[0], 2) + [Math]::Pow($at[1] - $body.spot[1], 2))
            }
            $reads.Add([pscustomobject][ordered]@{
                utc = $now.ToString("o")
                logic_tick = $read.logic_tick
                q16 = $read.q16
                position_bms = $at
                heading_bam = $read.heading_bam
                distance = $(if ($null -ne $distance) { [Math]::Round($distance, 3) } else { $null })
            })
            if ($null -ne $body.spot) {
                if ($distance -le 2.0) { ++$inPlace } else { $inPlace = 0 }
                if ($inPlace -ge 2) { break }
            } else {
                $key = "$($read.q16[0]),$($read.q16[1])"
                if ($key -ne $restKey) {
                    $restKey = $key
                    $restTick = $read.logic_tick
                    $restUtc = $now
                } else {
                    # Ticks from the reads' own logic ticks; a read without one counts
                    # wall time at 62 ticks a second.
                    $held = if ($null -ne $restTick -and $null -ne $read.logic_tick) {
                        [long] $read.logic_tick - [long] $restTick
                    } else {
                        ($now - $restUtc).TotalSeconds * 62.0
                    }
                    if ($held -ge $stillTicks) { break }
                }
            }
            if ($now -ge $deadline) {
                $why = if ($null -ne $body.spot) { "stand within two units of its spot (last read $([Math]::Round($distance, 2)) units off)" }
                    else { "stand still for $stillTicks logic ticks" }
                throw "Window-open wait: the $($body.role) did not $why within 15 s ($($reads.Count) reads)"
            }
            Start-Sleep -Milliseconds 100
        }
        [pscustomobject][ordered]@{
            slot = $body.slot
            role = $body.role
            driver = $read.driver
            rule = $(if ($null -ne $body.spot) { "in_place" } else { "still" })
            spot_bms = $body.spot
            waited_ms = [int] ([DateTime]::UtcNow - $started).TotalMilliseconds
            waited_ticks = $(if ($null -ne $firstTick -and $null -ne $lastTick) { [long] $lastTick - [long] $firstTick } else { $null })
            reads = $reads.ToArray()
        }
    }
    # One record or two: kept an array for the witness.
    return ,@($records)
}

function Invoke-ScenarioStage {
    param(
        [Parameter(Mandatory = $true)] [object[]] $Steps,
        $Setup = $null,
        [string] $RetailInstanceId = "",
        # The setup pose alone (Invoke-ScenarioSetupBeforeWindow): a retail
        # actor plays no input; an OpenNova actor plays -Steps, the lone end.
        [switch] $SetupOnly,
        # steps_tick's host tick for this part's relative tick 0 (Invoke-ScenarioAimStage).
        $DueTick = $null
    )
    if (-not $SetupOnly -and
            @($Steps | Where-Object { $_.PSObject.Properties.Name -contains "aim_at" }).Count -gt 0) {
        return Invoke-ScenarioAimStage -Steps $Steps -Setup $Setup -RetailInstanceId $RetailInstanceId
    }
    $hasPose = $null -ne $Setup -and $null -ne $Setup.pose
    $actor = [string] $script:ScenarioSpec.actor
    $retailActor = ($actor -eq "joiner" -and $Topology -in @("RR", "OR")) -or
        ($actor -eq "host" -and $Topology -in @("RR", "RO"))
    # Generous: two retail windows on one machine tick well below 62 Hz.
    $timeoutSeconds = [int] ($Steps[-1].tick / 10.0 + 60)
    if ($retailActor) {
        if (-not $RetailInstanceId) {
            throw "Scenario play on $Topology needs the retail $actor's instance id"
        }
        $setupResult = $null
        if ($hasPose) {
            $snapshotPath = Join-Path $RunRoot "scenario-setup.debug_snapshot.json"
            Write-ScenarioPoseSnapshot -Path $snapshotPath -Pose $Setup.pose
            $setupResult = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_load_debug_snapshot" -Arguments @{
                instance_id = $RetailInstanceId
                path = $snapshotPath
            })
        }
        if ($SetupOnly) {
            return [pscustomobject]@{ driver = "onhook_load_debug_snapshot"; setup = $setupResult; record = $null }
        }
        # steps_tick (R-LX-s29-1 (b')): the play arms on the frame callback that
        # serves the call and starts on the next tick (start_logic_tick = armed + 1),
        # so the part waits for its due tick less one as the staging waits for its
        # own (Wait-RetailHostTick: a read 2 to 6 ticks ahead, the arm 3 to 5 after).
        $partTick = $null
        if ($null -ne $DueTick) {
            $partTick = $script:ScenarioSpec.steps_tick
            $dueRead = Wait-RetailHostTick -RetailHostInstanceId $RetailInstanceId -Label "Part tick" `
                -HostTick ([pscustomobject]@{ value = [long] $DueTick - 1; tolerance = [long] $partTick.tolerance })
        }
        $played = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_play_input" -Arguments @{
            instance_id = $RetailInstanceId
            steps = $Steps
            wait = $true
            timeout_ms = $timeoutSeconds * 1000
        })
        # Retail runs no input frame while a menu that does not pass input
        # through is up; a run that waits there past its end step expires
        # rather than finishing. Every step before the end must still apply.
        $unapplied = @($played.steps | Where-Object {
            [string] $_.kind -ne "end" -and $null -eq $_.applied_logic_tick
        })
        if ([string] $played.state -notin @("done", "expired") -or $unapplied.Count -gt 0) {
            throw "Retail scenario $($script:ScenarioSpec.name) did not play: state=$($played.state) result=$($played.result) unapplied=$($unapplied.Count)"
        }
        # A click whose widget was not on screen on its tick pressed nothing
        # (its screen not the current one, or the control hidden or disabled).
        $missed = @($played.steps | Where-Object {
            [string] $_.kind -eq "click" -and [bool] $_.widget_missed
        })
        if ($missed.Count -gt 0) {
            throw "Retail scenario $($script:ScenarioSpec.name) did not play: $($missed.Count) click(s) found no widget on screen (steps $(@($missed.index) -join ','))"
        }
        if ($null -ne $partTick) {
            $offset = [long] $played.start_logic_tick - [long] $DueTick
            $played | Add-Member -NotePropertyName steps_tick -Force -NotePropertyValue ([pscustomobject][ordered]@{
                due_tick = [long] $DueTick
                tolerance = [int] $partTick.tolerance
                read_before = $dueRead
                start_logic_tick = [long] $played.start_logic_tick
                offset = $offset
            })
            if ([Math]::Abs($offset) -gt [long] $partTick.tolerance) {
                throw "Retail scenario $($script:ScenarioSpec.name): a part started on host tick $($played.start_logic_tick), $offset off its steps_tick $DueTick (tolerance $($partTick.tolerance))"
            }
        }
        return [pscustomobject]@{ driver = "onhook_play_input"; setup = $setupResult; record = $played }
    }
    $stage = @{ steps = $Steps }
    if ($null -ne $Setup) { $stage.setup = $Setup }
    $port = if ($actor -eq "host") { $HostMcpPort } else { $JoinerMcpPort }
    $process = if ($actor -eq "host") { $script:OpenNovaHost } else { $script:OpenNovaJoiner }
    $probes = @()
    if ($ActorScenarioProbes -and -not $SetupOnly) {
        # Windows PowerShell's ConvertFrom-Json hands a JSON array on as ONE
        # object; @() alone wraps it whole, so each probe's args went missing
        # (LP-s7's drive probe cells). The ForEach-Object unrolls it.
        $probes = @((Get-Content -LiteralPath $ActorScenarioProbes -Raw | ConvertFrom-Json) | ForEach-Object { $_ })
    }
    $status = Invoke-GameProbe -Port $port -Name "scenario_play" -Process $process `
        -TimeoutSeconds $timeoutSeconds -PollMs 500 -Arguments @{
            scenario = $stage
            wait = ($probes.Count -eq 0)
        }
    if ($probes.Count -gt 0) {
        if ([string] $status.state -ne "passed") {
            throw "OpenNova scenario $($script:ScenarioSpec.name) did not start for its probes: state=$($status.state)"
        }
        $probeStatus = @(foreach ($probe in $probes) {
            $probeArgs = @{}
            if ($probe.PSObject.Properties.Name -contains "args") {
                foreach ($field in $probe.args.PSObject.Properties) { $probeArgs[$field.Name] = $field.Value }
            }
            $result = Invoke-GameProbe -Port $port -Name ([string] $probe.name) -Process $process `
                -TimeoutSeconds ($timeoutSeconds + 60) -PollMs 500 -Arguments $probeArgs
            Write-Host "PARITY_RUN_NOTE scenario probe $($probe.name): state=$($result.state)"
            [pscustomobject]@{ name = [string] $probe.name; args = $probe.args; status = $result }
        })
        $probeStatus | ConvertTo-Json -Depth 40 |
            Set-Content -LiteralPath (Join-Path $RunRoot "scenario-probes.json") -Encoding UTF8
        $status = Invoke-GameProbe -Port $port -Name "scenario_play" -Process $process `
            -TimeoutSeconds $timeoutSeconds -PollMs 500 -Arguments @{ wait = $true }
    }
    if ([string] $status.state -ne "passed" -or -not $status.verdict -or
            -not [bool] $status.verdict.ok) {
        $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
        throw "OpenNova scenario $($script:ScenarioSpec.name) did not play: state=$($status.state) $why"
    }
    $record = $status.verdict.data
    return [pscustomobject]@{ driver = "scenario_play"; setup = $record.setup; record = $record }
}

# The aim_at step's arithmetic (scripts/net/scenario_aim.py, unit-tested in
# test_scenario_aim.py); one JSON object back.
$ScenarioAimScript = Join-Path $Repo "scripts\net\scenario_aim.py"
function Invoke-ScenarioAimTool {
    param([Parameter(Mandatory = $true)] [string[]] $Arguments)
    $lines = @(& python -I $ScenarioAimScript @Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "scenario_aim.py $($Arguments -join ' ') failed (exit $LASTEXITCODE)"
    }
    return ($lines -join "`n") | ConvertFrom-Json
}

# One aim_at: this side's reads, then the look that turns the actor onto the
# target. A retail actor's reads come from onhook_pool_entities (pool 0) on its
# own instance: its body is the record whose DcbId is 10000 + its slot (the
# host 0, the pair's joiner 1) [orig: PlayerClass_InitEntity, the slot byte
# +0x154 plus 0x2710 @0x4B1149..0x4B1155 stored to +0x7C @0x4B1173], its
# heading the record's yaw_bam (+0x10), and both positions the Q16 Position
# words; its mouse sensitivity is its copy's current profile record's +0x590
# word (scenario_aim.py profile-word). An OpenNova actor's come from the
# scenario_play probe's aim_read: the local player's heading BAM, its and the
# target's mission positions, and the session's mouse sensitivity word. A
# target named by role ("joiner" or "host") is the other player's body, found
# the same way by its DcbId (10000 + its slot) in the retail actor's pool read,
# so no captured handle rides the scenario; an OpenNova actor's aim_read takes
# a handle, so the role form plays on a retail actor.
function Resolve-ScenarioAim {
    param(
        [Parameter(Mandatory = $true)] [string] $Handle,
        [string] $RetailInstanceId = "",
        # Resolve-ScenarioFirstAim: the actor's heading and position words are its setup
        # pose's, as the drivers write them (scenario_aim.py pose); the target and the
        # sensitivity word are read live.
        $ActorPose = $null
    )
    $byRole = $Handle -in @("joiner", "host")
    $target = if ($byRole) { -1 } else { [Convert]::ToInt32($Handle.Substring(2), 16) }
    $actor = [string] $script:ScenarioSpec.actor
    $retailActor = ($actor -eq "joiner" -and $Topology -in @("RR", "OR")) -or
        ($actor -eq "host" -and $Topology -in @("RR", "RO"))
    $invariant = [Globalization.CultureInfo]::InvariantCulture
    if ($byRole -and -not $retailActor) {
        throw "aim_at ${Handle}: a target named by role resolves on a retail actor's pool read; the OpenNova $actor on $Topology takes a handle"
    }
    if ($retailActor) {
        $slot = if ($actor -eq "host") { 0 } else { 1 }
        $pool = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_pool_entities" -Arguments @{
            instance_id = $RetailInstanceId
            pool = 0
            max = 4096
        })
        $entities = @($pool.entities)
        $self = @($entities | Where-Object { [int] $_.net_id -eq 10000 + $slot })
        # @() around the if: a statement's one-element array output unrolls to the element,
        # which has no Count under strict mode.
        $targetSlot = if ($Handle -eq "host") { 0 } else { 1 }
        $mark = @(if ($byRole) {
            $entities | Where-Object { [int] $_.net_id -eq 10000 + $targetSlot }
        } else {
            $entities | Where-Object { [int] $_.handle -eq $target }
        })
        if ($self.Count -ne 1 -or $mark.Count -ne 1) {
            throw "aim_at ${Handle}: the retail $actor's pool 0 holds $($self.Count) body record(s) with DcbId $(10000 + $slot) and $($mark.Count) live entity record(s) for the target"
        }
        if ($byRole) {
            Write-Host "PARITY_RUN_NOTE aim_at ${Handle}: the body with DcbId $(10000 + $targetSlot) is handle 0x$(([int] $mark[0].handle).ToString('x4'))"
        }
        $copy = if ($actor -eq "host") { $ServerDir } else { $ClientDir }
        $word = Invoke-ScenarioAimTool -Arguments @("profile-word", "--copy=$copy")
        $read = [pscustomobject][ordered]@{
            driver = "onhook_pool_entities"
            logic_tick = $(if ($pool.PSObject.Properties.Name -contains "logic_tick") { $pool.logic_tick } else { $null })
            heading_bam = [long] $self[0].yaw_bam
            actor_q16 = @($self[0].pos)
            # The look's gate words on the actor's record: +0x24 (the dispatcher's
            # flag-0x01 operand), health, team, parent slot.
            actor_flags = [long] $self[0].flags
            actor_health = $self[0].health
            actor_team = $self[0].team
            actor_parent_slot = $self[0].parent_slot
            target_handle = [int] $mark[0].handle
            target_q16 = @($mark[0].pos)
            mouse_sensitivity = [int] $word.mouse_sensitivity
            profile_slot = [int] $word.slot
        }
        $actorXY = "$([long] $self[0].pos[0]),$([long] $self[0].pos[1])"
        $targetXY = "$([long] $mark[0].pos[0]),$([long] $mark[0].pos[1])"
        if ($null -ne $ActorPose) {
            $position = @($ActorPose.position_bms)
            $words = Invoke-ScenarioAimTool -Arguments @("pose",
                "--x=$(([double] $position[0]).ToString('R', $invariant))",
                "--y=$(([double] $position[1]).ToString('R', $invariant))",
                "--yaw-deg=$(([double] $ActorPose.yaw).ToString('R', $invariant))")
            $read | Add-Member -NotePropertyName actor_from -NotePropertyValue "setup pose"
            $read.heading_bam = [long] $words.heading_bam
            $read.actor_q16 = @([long] $words.x_q16, [long] $words.y_q16, $self[0].pos[2])
            $actorXY = "$([long] $words.x_q16),$([long] $words.y_q16)"
        }
    } else {
        if ($null -ne $ActorPose) {
            throw "aim_at ${Handle}: an aim resolved from the setup pose reads a retail actor's pool (steps_tick plays on a retail host)"
        }
        $port = if ($actor -eq "host") { $HostMcpPort } else { $JoinerMcpPort }
        $process = if ($actor -eq "host") { $script:OpenNovaHost } else { $script:OpenNovaJoiner }
        $status = Invoke-GameProbe -Port $port -Name "scenario_play" -Process $process `
            -TimeoutSeconds 30 -PollMs 250 -Arguments @{ aim_read = $target }
        if ([string] $status.state -ne "passed" -or -not $status.verdict -or -not [bool] $status.verdict.ok) {
            $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
            throw "aim_at ${Handle}: the OpenNova $actor's aim read failed: state=$($status.state) $why"
        }
        $data = $status.verdict.data
        $read = [pscustomobject][ordered]@{
            driver = "scenario_play"
            logic_tick = $data.logic_tick
            heading_bam = [long] $data.heading_bam
            actor_bms = @($data.position_bms)
            target_bms = @($data.target.position_bms)
            mouse_sensitivity = [int] $data.mouse_sensitivity
        }
        $actorXY = "$(([double] $data.position_bms[0]).ToString('R', $invariant)),$(([double] $data.position_bms[1]).ToString('R', $invariant))"
        $targetXY = "$(([double] $data.target.position_bms[0]).ToString('R', $invariant)),$(([double] $data.target.position_bms[1]).ToString('R', $invariant))"
    }
    # The previous aim's look must have turned the actor before this aim reads on
    # from it, on either driver. The look's only writes are the local player's
    # +0x10 heading and g_LocalPlayerLookYaw [orig: Input_HandleActionBinding_0
    # @0x4E0420 - g_LocalPlayerEntity @0x4E0434, case 166 @0x4E10A4..0x4E10AA,
    # case 167 @0x4E1102..0x4E110F], the word this read returns, and the event
    # reaches them only past the dispatcher's head, which drops a look row (flags
    # 0x0C100425 / 0x0C200425) under the pre-round timer (0x400 @0x49ADB3..0x49ADC2),
    # the deploy screen (0x8000000 @0x49ADC8..0x49ADD7), the local player's +0x24 bit
    # 2 (0x1 @0x49AD76..0x49ADA0) or the round's end (@0x49ADEE..0x49AE16)
    # [orig: Input_HandleActionBinding @0x49AD40]. x7's revive pre-flight (push 167,
    # RR) read the posed heading 0x40000000 on all three aims, so aims two and
    # three turned from a heading the actor no longer had in mind. Case 166 subtracts
    # the tick's whole turn in one store, so the check asks for at least half the
    # look's turn in its direction (scenario_aim.py turned), not a changed word: x9's
    # revive pre-flight (push 203, RR) passed aim 3 on a dead medic's -10,035 BAM
    # drift against aim 2's -379,584,512 BAM look.
    $previous = $script:LastScenarioAim
    $turn = $null
    if ($null -ne $previous -and [int] $previous.look.look_px -ne 0) {
        $turn = Invoke-ScenarioAimTool -Arguments @("turned",
            "--before-bam=$([long] $previous.read.heading_bam)", "--after-bam=$([long] $read.heading_bam)",
            "--expected-bam=$([long] $previous.look.heading_after_bam)")
    }
    if ($null -ne $turn -and -not [bool] $turn.ok) {
        # The actor's own screen at that moment shows which gate holds (a deploy
        # screen or an open menu); a failed shot does not hide the finding.
        if ($retailActor) {
            try {
                $null = Invoke-OnHookTool -Name "onhook_screenshot" -Arguments @{
                    instance_id = $RetailInstanceId
                    path = (Join-Path $RunRoot "aim-gated-$actor.png")
                    preview_max_dim = 256
                }
            } catch { Write-Host "PARITY_RUN_NOTE aim_at ${Handle}: the gated actor's screenshot failed: $($_.Exception.Message)" }
        }
        # A retail read's +0x24 bit 0x2 is the dead body the dispatcher's head drops the
        # look for (@0x49AD76..0x49ADA0).
        $dead = if ($retailActor -and ([long] $read.actor_flags -band 2) -ne 0) { " (the $actor is dead: +0x24 bit 0x2)" } else { "" }
        throw "aim_at ${Handle}: the previous look ($($previous.look.look_px) px for $($previous.aim_at)) did not turn the $actor$dead (turned $($turn.turned_bam) BAM of the look's $($turn.expected_bam); heading $($read.heading_bam), expected $($previous.look.heading_after_bam)): its look rows are gated; read $(ConvertTo-Json $read -Compress)"
    }
    $look = Invoke-ScenarioAimTool -Arguments @("look", "--heading-bam=$($read.heading_bam)",
        "--actor=$actorXY", "--target=$targetXY", "--sensitivity=$($read.mouse_sensitivity)")
    Write-Host "PARITY_RUN_NOTE aim_at ${Handle}: heading $($read.heading_bam) -> look_px $($look.look_px) (S $($read.mouse_sensitivity), residual $($look.residual_bam) BAM)"
    $aim = [pscustomobject][ordered]@{ aim_at = $Handle; read = $read; look = $look }
    $script:LastScenarioAim = $aim
    return $aim
}

# R-LX-s29-1 (b'): with steps_tick, an aim_at at the main stage's tick 0 resolves
# before the staging, so after the staging only its after-read and the setup pose
# stand between it and the first part (LX-s29's read of LP-s21's revive cells: the
# aim's reads after the setup took 48..52 ticks, a pool read of 53 ms behind the
# setup's 165 ms play and about 600 ms of decode and two python runs, and put the
# medic's first tap past 0x0019's retarget). The actor's heading and position words
# are the setup pose's, the target's position and the sensitivity word read live:
# the camp AI hold their posts until a threat comes.
$script:ScenarioFirstAim = $null
function Resolve-ScenarioFirstAim {
    param([string] $RetailInstanceId = "")
    $spec = $script:ScenarioSpec
    $script:ScenarioFirstAim = $null
    if (-not ($spec.PSObject.Properties.Name -contains "steps_tick")) { return }
    $first = @($spec.steps)[0]
    if (-not ($first.PSObject.Properties.Name -contains "aim_at") -or [int] $first.tick -ne 0) { return }
    $pose = if ($spec.PSObject.Properties.Name -contains "setup" -and $null -ne $spec.setup) { $spec.setup.pose } else { $null }
    $script:ScenarioFirstAim = Resolve-ScenarioAim -Handle ([string] $first.aim_at) `
        -RetailInstanceId $RetailInstanceId -ActorPose $pose
}

# A stage with aim_at steps plays in parts: the steps before each aim_at (the
# setup pose with the first part) up to an end on the aim_at's tick, then the
# aim's reads and look, then the next part from that tick on, its ticks
# relative to it and its first step the look (one look_px per same-tick step
# scenario_aim.py gives; the steps of one tick sum into one axis word on both
# drivers, so it rounds once). The witness records each part's record and
# each aim. With steps_tick each part an aim starts plays its tick 0 on host
# tick value + the aim's tick (Invoke-ScenarioStage -DueTick), and the part
# before an aim ends one tick past its last step, so the aim's reads fall in
# the script's gap.
function Invoke-ScenarioAimStage {
    param(
        [Parameter(Mandatory = $true)] [object[]] $Steps,
        $Setup = $null,
        [string] $RetailInstanceId = ""
    )
    $spec = $script:ScenarioSpec
    $partTick = if ($spec.PSObject.Properties.Name -contains "steps_tick") { $spec.steps_tick } else { $null }
    $parts = [System.Collections.Generic.List[object]]::new()
    $aims = [System.Collections.Generic.List[object]]::new()
    $pending = [System.Collections.Generic.List[object]]::new()
    $partSetup = $Setup
    $origin = 0
    $due = $null
    foreach ($step in $Steps) {
        if ($step.PSObject.Properties.Name -contains "aim_at") {
            $at = [int] $step.tick
            if ($pending.Count -gt 0 -or $null -ne $partSetup -or $at -gt $origin) {
                $end = $at - $origin
                if ($partTick -and $pending.Count -gt 0) {
                    $end = [int] ($pending | Measure-Object -Property tick -Maximum).Maximum + 1
                }
                $pending.Add([pscustomobject]@{ tick = $end; end = $true })
                $parts.Add((Invoke-ScenarioStage -Steps $pending.ToArray() -Setup $partSetup `
                    -RetailInstanceId $RetailInstanceId -DueTick $due))
                $partSetup = $null
            }
            $aim = if ($aims.Count -eq 0 -and $at -eq 0 -and $script:ScenarioFirstAim) {
                $script:ScenarioFirstAim
            } else {
                Resolve-ScenarioAim -Handle ([string] $step.aim_at) -RetailInstanceId $RetailInstanceId
            }
            $aims.Add($aim)
            if ($partTick) { $due = [long] $partTick.value + $at }
            $pending = [System.Collections.Generic.List[object]]::new()
            foreach ($px in @($aim.look.look_steps)) {
                $pending.Add([pscustomobject]@{ tick = 0; look_px = @([int] $px, 0) })
            }
            $origin = $at
            continue
        }
        $copy = [ordered]@{}
        foreach ($field in $step.PSObject.Properties) { $copy[$field.Name] = $field.Value }
        $copy.tick = [int] $step.tick - $origin
        $pending.Add([pscustomobject] $copy)
    }
    $parts.Add((Invoke-ScenarioStage -Steps $pending.ToArray() -Setup $partSetup `
        -RetailInstanceId $RetailInstanceId -DueTick $due))
    return [pscustomobject]@{
        driver = $parts[0].driver
        setup = $parts[0].setup
        record = [pscustomobject][ordered]@{
            parts = @($parts | ForEach-Object { $_.record })
            aims = $aims.ToArray()
        }
    }
}

# A scenario with no prelude takes its setup pose and its staging BEFORE the
# steady window opens, so every run's window starts on the posed state.
# Applied inside the window, the pose landed 0.03..0.44 s in, and the S2C frame
# period (about 190 ms) decided whether one pre-pose frame fell inside the
# window, and the pose jump with it. c2 03TR read entity_P orientation range 64
# on RR, RR-aa and OR against 5 on RO and OO. The window then waits for the
# setup's bodies at rest (Wait-ScenarioBodiesAtRest). A prelude (the deploy key) must
# come before the pose, so a scenario with one keeps its whole play in the window.
function Test-ScenarioSetupBeforeWindow {
    $spec = $script:ScenarioSpec
    if (-not $spec) { return $false }
    $names = @($spec.PSObject.Properties.Name)
    if ($names -contains "prelude") { return $false }
    return ($names -contains "staging") -or ($names -contains "setup" -and $null -ne $spec.setup)
}

function Invoke-ScenarioSetupBeforeWindow {
    param(
        [string] $RetailJoinerInstanceId = "",
        [string] $RetailHostInstanceId = ""
    )
    if (-not (Test-ScenarioSetupBeforeWindow)) { return }
    if ($script:SteadyStartedUtc) {
        throw "Scenario setup must precede the steady-window start"
    }
    if ($script:ScenarioEarlySetup) {
        throw "Scenario setup was already applied in run $RunId"
    }
    $spec = $script:ScenarioSpec
    $retailInstanceId = if ([string] $spec.actor -eq "host") {
        $RetailHostInstanceId
    } else {
        $RetailJoinerInstanceId
    }
    # The machine-load sampler starts here, ahead of the setup, not at the
    # window's start: its Start-Job spins up a child PowerShell, which took
    # 0.1..0.2 s on x5's aikill cells and 1.3 s on its OO cell, and in between
    # the staging and the first step a staged actor in a fight has none of that
    # to spare (x5 aikill OO: the camp's AI killed the joiner 1.31 s into the
    # window, and its steps began at 1.32 s; the other four fired at
    # 0.21..0.43 s).
    Start-MachineLoadSampler
    # The steady start's desktop reset (two window enumerations, about 1 s) runs
    # before the setup, so the window opens and the steps follow the pose as
    # closely as they did inside one stage: a posed actor in a fight (x1's
    # coop_ai_kill, the AI firing from the pose on) gets no extra second.
    try { Reset-DesktopCursor }
    catch { Write-Host "PARITY_RUN_NOTE desktop probe failed at the steady start: $($_.Exception.Message)" }
    $script:DesktopResetBeforeWindow = $true
    $startedUtc = [DateTime]::UtcNow.ToString("o")
    # The actor's setup pose first, the staging last, then the wait for the
    # bodies at rest, so the window opens on the posed state. A client
    # keeps its own heading through the host's snap
    # (the copy skipped for the local player @0x4B434D..0x4B4353), so a staged
    # joiner actor turns while still at its spawn and is exposed only from the
    # staging on (x1's coop_ai_kill: the AI killed the joiner 0.55 s after it).
    # A joiner actor staged at its own setup position lands there from either
    # side of the two-unit snap: past it the client snaps to the host's copy
    # (@0x4B42C1..0x4B434A), within it the client glides to it over 48 or 512
    # ticks (+0x27E @0x4B4490 / @0x4B449E), and its own pose already put it
    # there (x1's drive: a joiner pushed 3.9 units off its spawn never reached
    # the buggy; RO's joiner glided back 0.6 units).
    $setup = $null
    if ($spec.PSObject.Properties.Name -contains "setup" -and $null -ne $spec.setup) {
        $setup = (Invoke-ScenarioStage -Steps @([pscustomobject]@{ tick = 0; end = $true }) `
            -Setup $spec.setup -RetailInstanceId $retailInstanceId -SetupOnly).setup
    }
    Resolve-ScenarioFirstAim -RetailInstanceId $retailInstanceId
    $staging = Invoke-ScenarioStaging -RetailHostInstanceId $RetailHostInstanceId
    $stagedUtc = [DateTime]::UtcNow.ToString("o")
    # The window opens on the setup's bodies at rest (Wait-ScenarioBodiesAtRest).
    $atRest = Wait-ScenarioBodiesAtRest -RetailJoinerInstanceId $RetailJoinerInstanceId `
        -RetailHostInstanceId $RetailHostInstanceId
    $script:ScenarioEarlySetup = [pscustomobject]@{
        started_utc = $startedUtc
        staged_utc = $stagedUtc
        applied_utc = [DateTime]::UtcNow.ToString("o")
        at_rest = $atRest
        staging = $staging
        setup = $setup
    }
}

# Plays -Scenario on its actor (the joiner or the host) right after the
# steady window starts: its prelude stage when it has one (e.g. the deploy
# key on a deploy_hold run), then the setup pose and the steps, then the
# settle; a scenario with no prelude took its setup pose and staging just
# before the window (Invoke-ScenarioSetupBeforeWindow). A pose sticks only on
# the authority's own player: a client snaps
# its players to the host's replicated pose past 2 units
# [orig: Entity_UpdateInfantryPlayerBody @0x4B42C1..0x4B434A, skipped on the
#  authority @0x4B432A], so a posed scenario's actor is the host.
function Invoke-ScenarioIfRequested {
    param(
        [string] $RetailJoinerInstanceId = "",
        [string] $RetailHostInstanceId = ""
    )
    if (-not $script:ScenarioSpec) { return }
    if (-not $script:SteadyStartedUtc -or $script:SteadyCompletedUtc) {
        throw "Scenario play must follow the steady-window start"
    }
    if ($script:ScenarioWitness) {
        throw "Scenario was already played in run $RunId"
    }
    $spec = $script:ScenarioSpec
    $retailInstanceId = if ([string] $spec.actor -eq "host") {
        $RetailHostInstanceId
    } else {
        $RetailJoinerInstanceId
    }
    $startedUtc = [DateTime]::UtcNow.ToString("o")
    $prelude = $null
    if ($spec.PSObject.Properties.Name -contains "prelude") {
        $prelude = Invoke-ScenarioStage -Steps @($spec.prelude) `
            -RetailInstanceId $retailInstanceId
    }
    $early = $script:ScenarioEarlySetup
    if (Test-ScenarioSetupBeforeWindow) {
        if (-not $early) {
            throw "Scenario $($spec.name) has no prelude: its setup and staging must land before the steady window"
        }
        $staging = $early.staging
        $setup = $null
    } else {
        Resolve-ScenarioFirstAim -RetailInstanceId $retailInstanceId
        $staging = Invoke-ScenarioStaging -RetailHostInstanceId $RetailHostInstanceId
        $setup = if ($spec.PSObject.Properties.Name -contains "setup") { $spec.setup } else { $null }
    }
    $main = Invoke-ScenarioStage -Steps @($spec.steps) -Setup $setup `
        -RetailInstanceId $retailInstanceId
    $playedUtc = [DateTime]::UtcNow.ToString("o")
    Start-Sleep -Seconds ([int] $spec.settle_seconds)
    # The run keeps the exact script it played; the comparator checks every
    # run played the same steps and reads the expectations from the source.
    $playedCopy = Join-Path $RunRoot "scenario.json"
    if (Test-Path -LiteralPath $playedCopy) {
        throw "Played scenario copy must be create-new: $playedCopy"
    }
    Copy-Item -LiteralPath $Scenario -Destination $playedCopy
    $script:ScenarioWitness = [pscustomobject]@{
        schema = "opennova.parity-scenario.v1"
        name = [string] $spec.name
        source = (Resolve-Path -LiteralPath $Scenario).Path
        path = $playedCopy
        sha256 = (Get-FileHash -LiteralPath $playedCopy -Algorithm SHA256).Hash.ToLowerInvariant()
        actor = [string] $spec.actor
        driver = $main.driver
        started_utc = $startedUtc
        played_utc = $playedUtc
        settled_utc = [DateTime]::UtcNow.ToString("o")
        settle_seconds = [int] $spec.settle_seconds
        prelude = $(if ($prelude) { $prelude.record } else { $null })
        staging = $staging
        setup = $(if ($early) { $early.setup } else { $main.setup })
        setup_before_window = $(if ($early) {
            [pscustomobject][ordered]@{
                started_utc = $early.started_utc
                staged_utc = $early.staged_utc
                applied_utc = $early.applied_utc
                at_rest = $early.at_rest
            }
        } else { $null })
        record = $main.record
    }
}

function Complete-SteadyWindow {
    if (-not $script:SteadyStartedUtc -or $script:SteadyCompletedUtc) {
        throw "Steady window completion is out of order"
    }
    $script:SteadyCompletedUtc = [DateTime]::UtcNow.ToString("o")
    try { Complete-DesktopWitness }
    catch { Write-Host "PARITY_RUN_NOTE desktop probe failed at the steady end: $($_.Exception.Message)" }
    Complete-MachineLoadSampler
}

function Stop-OwnedRetailRun {
    param([Parameter(Mandatory = $true)] [string] $OwnedRunId)
    $result = Invoke-OnHookTool -Name "onhook_stop_run" -Arguments @{
        run_id = $OwnedRunId
        wait_timeout_ms = 15000
    } -AllowError
    $structured = if ($result.PSObject.Properties.Name -contains "structuredContent") {
        $result.structuredContent
    } else { $null }
    $script:StopEvidence += [pscustomobject]@{
        run_id = $OwnedRunId
        is_error = [bool] $result.isError
        detail = $structured
    }
    if ($result.isError -or -not $structured -or
            -not [bool] $structured.capture_complete) {
        $message = @($result.content | Where-Object { $_.type -eq "text" } |
            ForEach-Object { $_.text }) -join "`n"
        throw "Retail run $OwnedRunId did not produce complete capture evidence: $message"
    }
    $null = $script:OwnedRetailRuns.Remove($OwnedRunId)
}

# A rotation cell's closing checks (E2): the host has moved the session to the
# rotation's next mission inside the steady window, so its players stand at that
# mission's deploy screen, not in the first mission's match (x3's round RR and
# RR-aa joiners never again reported a local player). After a rotation, a live
# run that still sees its peer is the closing state; the run state is recorded.
function Test-RotationCell { return $RotationList.Count -gt 0 }

function Wait-RetailRunForPeer {
    param(
        [Parameter(Mandatory = $true)] [string] $OwnedRunId,
        [int] $TimeoutSeconds = 90,
        [switch] $AfterRotation,
        # The run is this cell's Serve Only host: no local player stands.
        [switch] $ServeOnlyHost
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $status = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_run_status" -Arguments @{
            run_id = $OwnedRunId
        })
        $state = $status.instance.run_state
        if ($state.failure -ne "none") {
            throw "Retail run $OwnedRunId failed in phase $($state.phase): $($state.failure)"
        }
        if ($state.phase -eq "in_match" -and $state.peer -and
                $state.local_player -and
                $state.capture_counters.packets -ge 40) {
            return $status
        }
        # A retail Serve Only host's run state never raises peer (nor counts a
        # player) with a joiner in its session: x7's admin pass read phase
        # in_match, authority, responder_ready, serve_only, connection mode 1
        # and 1709 captured packets, but peer false and player_count 0. Its
        # joiner's own readiness proves the session.
        if ($ServeOnlyHost -and $state.authority -and $state.serve_only -and
                $state.capture_counters.packets -ge 40) {
            return $status
        }
        if ($AfterRotation -and $state.peer) {
            Write-Host "PARITY_RUN_NOTE rotation: retail run $OwnedRunId closes in phase $($state.phase) with its peer (local_player=$($state.local_player))"
            return $status
        }
        Start-Sleep -Milliseconds 1000
    } while ((Get-Date) -lt $deadline)
    # The last run state names the flag that never held (LP-s7: a retail
    # Serve Only host under our joiner played 154 uplinks yet timed out here).
    throw "Timed out waiting for retail run $OwnedRunId to observe a peer; last run_state: $($state | ConvertTo-Json -Depth 4 -Compress)"
}

function Assert-RetailEndpointState {
    param(
        [Parameter(Mandatory = $true)] $Instance,
        [Parameter(Mandatory = $true)] [ValidateSet("host", "joiner")] [string] $Role,
        [switch] $RequireInMatch,
        # After a scenario played, a deploy_hold joiner may have deployed.
        [switch] $AllowScenarioDeploy
    )
    $state = $Instance.run_state
    if ([string] $state.failure -ne "none") {
        throw "Retail $Role PID $($Instance.pid) failed in phase $($state.phase): $($state.failure)"
    }
    foreach ($counter in @("dropped", "truncated", "write_errors", "unsupported_async")) {
        if ([int64] $state.capture_counters.$counter -ne 0) {
            throw "Retail $Role PID $($Instance.pid) has nonzero capture counter ${counter}=$($state.capture_counters.$counter)"
        }
    }
    # A retail Serve Only host never raises peer (Wait-RetailRunForPeer's note).
    $servingOnly = $ServeOnly -and $Role -eq "host"
    if (-not [bool] $state.active -or (-not [bool] $state.peer -and -not $servingOnly) -or
            [string] $state.capture_state -ne "recording" -or
            [int] $state.network_type -ne 2 -or
            [int] $state.requested_port -ne $Port -or
            [int64] $state.capture_counters.packets -lt 40) {
        return $false
    }
    if ($Role -eq "host") {
        # A Serve Only host takes host-only connection mode 1 (onhook_host_lan
        # serve_only) and reports retail's live Serve Only flag.
        $hostMode = if ($ServeOnly) { 1 } else { 3 }
        if (-not [bool] $state.authority -or -not [bool] $state.responder_ready -or
                [int] $state.socket_mode -ne 3 -or [int] $state.connection_mode -ne $hostMode -or
                [int] $state.local_port -ne $Port -or ($ServeOnly -and -not [bool] $state.serve_only)) {
            return $false
        }
    }
    else {
        if ([bool] $state.authority -or [int] $state.socket_mode -ne 2 -or
                [int] $state.connection_mode -ne 2 -or [int] $state.local_port -le 0) {
            return $false
        }
    }
    if ($RequireInMatch -and
            ([string] $state.phase -ne "in_match" -or
                (-not [bool] $state.local_player -and -not ($ServeOnly -and $Role -eq "host")))) {
        return $false
    }
    if (-not $RequireInMatch -and $ReadinessMode -eq "deploy_hold" -and
            $Role -eq "joiner" -and
            ([string] $state.phase -ne "loading" -or [bool] $state.local_player) -and
            -not ($AllowScenarioDeploy -and [string] $state.phase -eq "in_match" -and
                [bool] $state.local_player)) {
        return $false
    }
    return $true
}

function Wait-RetailProcessForPeer {
    param(
        [Parameter(Mandatory = $true)] [int] $ProcessId,
        [Parameter(Mandatory = $true)] [ValidateSet("host", "joiner")] [string] $Role,
        [int] $TimeoutSeconds = 240,
        [switch] $RequireInMatch,
        [switch] $AllowScenarioDeploy
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $ownedProcess = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
        if (-not $ownedProcess -or $ownedProcess.HasExited) {
            throw "Retail $Role PID $ProcessId exited before satisfying readiness_mode=$ReadinessMode"
        }
        $instances = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_instances")
        $instance = @($instances.instances) |
            Where-Object { [int] $_.pid -eq $ProcessId } |
            Select-Object -First 1
        if ($instance) {
            $observedInstanceId = [string] $instance.instance_id
            if ($observedInstanceId -and
                    -not $script:RetailInstanceIds.Contains($observedInstanceId)) {
                $script:RetailInstanceIds.Add($observedInstanceId)
            }
            if (Assert-RetailEndpointState -Instance $instance -Role $Role `
                    -RequireInMatch:$RequireInMatch -AllowScenarioDeploy:$AllowScenarioDeploy) {
                return $instance
            }
        }
        Start-Sleep -Milliseconds 1000
    } while ((Get-Date) -lt $deadline)
    throw "Timed out waiting for retail $Role PID $ProcessId to satisfy readiness_mode=$ReadinessMode; last run_state: $(if ($instance) { $instance.run_state | ConvertTo-Json -Depth 4 -Compress } else { 'no instance' })"
}

function Assert-RetailHostOwnsPort {
    param([Parameter(Mandatory = $true)] [int] $ProcessId)
    $owners = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
    if ($owners.Count -eq 0 -or
            @($owners | Where-Object { [int] $_.OwningProcess -ne $ProcessId }).Count -ne 0 -or
            @($owners | Where-Object { [int] $_.OwningProcess -eq $ProcessId }).Count -eq 0) {
        throw "Retail host PID $ProcessId is not the exclusive owner of UDP $Port"
    }
    return [pscustomobject]@{
        pid = $ProcessId
        port = $Port
        endpoints = @($owners | ForEach-Object {
            [pscustomobject]@{
                local_address = [string] $_.LocalAddress
                local_port = [int] $_.LocalPort
            }
        })
    }
}

function Stop-RetailProcessCapture {
    param(
        [Parameter(Mandatory = $true)] [string] $InstanceId,
        [Parameter(Mandatory = $true)] [string] $RunLabel
    )
    $result = Invoke-OnHookTool -Name "onhook_stop_capture" -Arguments @{
        instance_id = $InstanceId
    } -AllowError
    $structured = if ($result.PSObject.Properties.Name -contains "structuredContent") {
        $result.structuredContent
    } else { $null }
    $script:StopEvidence += [pscustomobject]@{
        run_id = $RunLabel
        is_error = [bool] $result.isError
        detail = $structured
    }
    if ($result.isError -or -not $structured -or
            -not [bool] $structured.capture_complete) {
        $message = @($result.content | Where-Object { $_.type -eq "text" } |
            ForEach-Object { $_.text }) -join "`n"
        throw "Retail capture $RunLabel did not drain cleanly: $message"
    }
    $null = $script:RetailInstanceIds.Remove($InstanceId)
}

# Retail is launched only through onhook-mcp (bridge protocol 1.8 or later):
# its role tools return the exact process (pid + instance_id), render the
# role's onhook.cfg from their arguments, and report whether the loaded hook
# runs retail stock. A launch missing any of that stops the cell instead of
# guessing at a process or a configuration.
function Assert-RetailStock {
    param(
        [Parameter(Mandatory = $true)] [string] $Tool,
        [Parameter(Mandatory = $true)] $Reported
    )
    if ($StockObserver -and -not ($Reported -eq $true)) {
        throw "$Tool did not confirm stock_observer; deploy onhook (bridge protocol 1.8+) to both retail copies, or pass -AllowModifiedRetail for a diagnostic run."
    }
}

function Assert-OnHookRoleLaunch {
    param(
        [Parameter(Mandatory = $true)] [string] $Tool,
        [Parameter(Mandatory = $true)] $Result
    )
    foreach ($name in @("pid", "instance_id", "run_id", "capture_path", "hook_config_path")) {
        if (-not ($Result.PSObject.Properties.Name -contains $name) -or
                [string]::IsNullOrWhiteSpace([string] $Result.$name)) {
            throw "onhook-mcp $Tool returned no '$name'; deploy onhook-mcp from bridge protocol 1.8 or later."
        }
    }
    if ([int] $Result.pid -le 0) {
        throw "onhook-mcp $Tool returned pid $($Result.pid)."
    }
    $reported = if ($Result.PSObject.Properties.Name -contains "stock_observer") {
        $Result.stock_observer
    } else { $null }
    Assert-RetailStock -Tool $Tool -Reported $reported
}

# onhook_host_lan's arguments for this cell's retail host; the rotation key
# only when the cell has a tail, so every other cell's call is unchanged.
function Get-RetailHostArguments {
    param([Parameter(Mandatory = $true)] [string] $OutputDir)
    $arguments = @{
        mission = $Mission
        game_dir = $ServerDir
        expansion = $Expansion
        port = $Port
        callsign = $HostCallsign
        game_type = [string] $GameType
        max_players = 4
        run_id = "$RunId-host"
        output_dir = $OutputDir
        capture = $true
        windowed = $true
        allow_many = $true
        stock_observer = $StockObserver
        deploy = [bool] $DeployZones
        wait_timeout_ms = 240000
    }
    if ($RotationList.Count -gt 0) { $arguments.rotation = @($RotationList) }
    if ($ServeOnly) { $arguments.serve_only = $true }
    return $arguments
}

# A Serve Only cell's host must report retail's Serve Only flag (the hook's
# serve_only echo).
function Assert-RetailHostServeOnly {
    param([Parameter(Mandatory = $true)] $Result)
    if (-not $ServeOnly) { return }
    $echo = ($Result.PSObject.Properties.Name -contains "serve_only") -and [bool] $Result.serve_only
    if (-not $echo) {
        throw "onhook_host_lan did not host as Serve Only; deploy an onHook pair with serve_only (opennova-int #71)."
    }
}

# The hook echoes the rotation it built; a hook pair without it (before
# opennova-int's host rotation) cannot host an E2 cell.
function Assert-RetailHostRotation {
    param([Parameter(Mandatory = $true)] $Result)
    if ($RotationList.Count -eq 0) { return }
    $echo = if ($Result.PSObject.Properties.Name -contains "rotation") {
        @($Result.rotation | ForEach-Object { [string] $_ })
    } else { $null }
    if ($null -eq $echo -or ($echo -join ',') -cne ($RotationList -join ',')) {
        throw "onhook_host_lan did not build the requested rotation ($($RotationList -join ',')); deploy an onHook pair with host rotation."
    }
}

# E3: the retail late joiner, launched during the steady window and owned by
# run id like the pair's roles (its capture drains and its process stops
# through onhook_stop_run; the cleanup stops it with every owned run).
$script:LateJoinerWitness = $null
$script:RetailLateJoinerProcess = $null
function Start-RetailLateJoiner {
    if (-not $LateJoiner) { return }
    $lateDir = Join-Path $RunRoot "late"
    New-Item -ItemType Directory -Path $lateDir -Force | Out-Null
    $requested = [DateTime]::UtcNow.ToString("o")
    $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_join_lan" -Arguments @{
        game_dir = $LateClientDir
        expansion = $Expansion
        join_address = "127.0.0.1"
        port = $Port
        callsign = $LateJoinerCallsign
        run_id = "$RunId-late"
        output_dir = $lateDir
        capture = $true
        windowed = $true
        allow_many = $true
        stock_observer = $StockObserver
        deploy = [bool] $DeployZones
        readiness = $(if ($ReadinessMode -eq "in_match") { "in_match" } else { "peer" })
        wait_timeout_ms = 240000
    })
    Assert-OnHookRoleLaunch -Tool "onhook_join_lan (late joiner)" -Result $result
    $script:OwnedRetailRuns.Add([string] $result.run_id)
    $script:RetailLateJoinerProcess = Get-Process -Id ([int] $result.pid) -ErrorAction Stop
    Bind-EffectiveRetailConfig -Role late_client -Source (Join-Path $LateClientDir "onhook.cfg")
    $status = if ($ReadinessMode -eq "in_match") {
        Wait-RetailRunForPeer -OwnedRunId ([string] $result.run_id)
    } else {
        Get-StructuredResult (Invoke-OnHookTool -Name "onhook_run_status" -Arguments @{
            run_id = [string] $result.run_id
        })
    }
    $state = $status.instance.run_state
    if ([string] $state.failure -ne "none" -or -not [bool] $state.peer -or
            [int] $state.local_port -le 0) {
        throw "Retail late joiner $($result.run_id) is not a connected peer: phase $($state.phase), failure $($state.failure)"
    }
    $script:LateJoinerWitness = [pscustomobject][ordered]@{
        kind = "retail"
        callsign = $LateJoinerCallsign
        run_id = [string] $result.run_id
        pid = [int] $result.pid
        instance_id = [string] $result.instance_id
        game_dir = $LateClientDir
        capture_path = [string] $result.capture_path
        # The comparator's session selector: the late joiner's client port.
        client_port = [int] $state.local_port
        launch_requested_utc = $requested
        ready_utc = [DateTime]::UtcNow.ToString("o")
        ready_state = $state
        final_state = $null
    }
}

function Complete-RetailLateJoiner {
    if (-not $script:LateJoinerWitness) { return }
    $script:RetailLateJoinerProcess.Refresh()
    if ($script:RetailLateJoinerProcess.HasExited) {
        throw "The retail late joiner exited during the steady window"
    }
    $runId = [string] $script:LateJoinerWitness.run_id
    $status = if ($ReadinessMode -eq "in_match") {
        Wait-RetailRunForPeer -OwnedRunId $runId
    } else {
        Get-StructuredResult (Invoke-OnHookTool -Name "onhook_run_status" -Arguments @{
            run_id = $runId
        })
    }
    $script:LateJoinerWitness.final_state = $status.instance.run_state
    Stop-OwnedRetailRun -OwnedRunId $runId
    $script:RetailLateJoinerProcess = $null
}

function Start-RetailHostExact {
    $hostDir = Join-Path $RunRoot "host"
    New-Item -ItemType Directory -Path $hostDir -Force | Out-Null
    $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_host_lan" `
        -Arguments (Get-RetailHostArguments -OutputDir $hostDir))
    Assert-OnHookRoleLaunch -Tool "onhook_host_lan" -Result $result
    $script:OwnedRetailRuns.Add([string] $result.run_id)
    Assert-RetailHostRotation -Result $result
    Assert-RetailHostServeOnly -Result $result
    $script:RetailHostProcess = Get-Process -Id ([int] $result.pid) -ErrorAction Stop
    Bind-EffectiveRetailConfig -Role server -Source (Join-Path $ServerDir "onhook.cfg")
    $probeOutput = @(& $ProbeExe --host 127.0.0.1 --port $Port --timeout-ms 240000 `
        --interval-ms 1000 --expect-name $ServerName)
    if ($LASTEXITCODE -ne 0) {
        throw "Retail host PID $($script:RetailHostProcess.Id) did not answer matching LAN discovery: $($probeOutput -join ' | ')"
    }
    $probeOutput | ForEach-Object { Write-Host $_ }
    $ownership = Assert-RetailHostOwnsPort -ProcessId $script:RetailHostProcess.Id
    return [pscustomobject]@{
        process = $script:RetailHostProcess
        capture_path = [string] $result.capture_path
        port_ownership = $ownership
    }
}

# D-NET-167: a retail joiner's side password into its onhook_join_lan
# arguments, and the tool's witness that it was entered.
function Add-JoinerSidePassword {
    param([Parameter(Mandatory = $true)] [hashtable] $Arguments)
    if ($JoinerSidePassword) { $Arguments.side_password = $JoinerSidePassword }
    return $Arguments
}

function Assert-JoinerSidePasswordEntered {
    param([Parameter(Mandatory = $true)] $Result)
    if (-not $JoinerSidePassword) { return }
    $entered = ($Result.PSObject.Properties.Name -contains "side_password_entered") -and [bool] $Result.side_password_entered
    $script:JoinerSidePasswordEntered = $entered
    if (-not $entered) {
        throw "onhook_join_lan never showed the team-password panel (side_password_entered false): the host did not advertise its side passwords (P2 0x30)"
    }
}
$script:JoinerSidePasswordEntered = $null

# D-NET-167: our joiner's side password, entered on its join screen as a
# player does, through the join_screen probe. The --lan-join dial answers no
# role (no --spectator), so a host advertising its side passwords (P2 0x30)
# opens the team-password panel. The joiner types the password and presses
# ACCEPT, which closes the choice; the join's load reopens the screen on its
# progress. The side check is the retail joiner's: the panel showed and took
# the password, else the cell fails.
function Enter-OpenNovaJoinerSidePassword {
    $script:JoinerSidePasswordEntered = $false
    $steps = @(
        @{ fails = "never showed the team-password panel: the host did not advertise its side passwords (P2 0x30)"
           arguments = @{ op = "state"; until = "team_password"; wait_ms = 120000 } },
        @{ fails = "did not take the side password"
           arguments = @{ op = "set_text"; control = "TEAM_PASSWORD"; text = $JoinerSidePassword } },
        @{ fails = "did not leave the team-password panel for the join's progress"
           arguments = @{ op = "press"; control = "ACCEPT"; until = "progress"; wait_ms = 10000 } }
    )
    foreach ($step in $steps) {
        $status = Invoke-GameProbe -Port $JoinerMcpPort -Name "join_screen" -Process $script:OpenNovaJoiner `
            -TimeoutSeconds 180 -PollMs 1000 -Arguments $step.arguments
        $ok = [string] $status.state -eq "passed" -and $status.verdict -and [bool] $status.verdict.ok
        if ($ok -and $step.arguments.op -eq "set_text") {
            $ok = [string] $status.verdict.data.team_password -ceq $JoinerSidePassword
        }
        if (-not $ok) {
            $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
            throw "Our joiner's join screen $($step.fails) ($($step.arguments.op): $why)"
        }
    }
    $script:JoinerSidePasswordEntered = $true
}

function Start-RetailJoinerExact {
    $joinerDir = Join-Path $RunRoot "joiner"
    New-Item -ItemType Directory -Path $joinerDir -Force | Out-Null
    $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_join_lan" -Arguments (Add-JoinerSidePassword @{
        game_dir = $ClientDir
        expansion = $Expansion
        join_address = "127.0.0.1"
        port = $Port
        callsign = $JoinerCallsign
        run_id = "$RunId-joiner"
        output_dir = $joinerDir
        capture = $true
        windowed = $true
        allow_many = $true
        stock_observer = $StockObserver
        deploy = [bool] $DeployZones
        # deploy_hold returns once the session is connected: an A&S joiner
        # holds on the deploy screen with no local Person, which the
        # in_match readiness waits for.
        readiness = $(if ($ReadinessMode -eq "in_match") { "in_match" } else { "peer" })
        wait_timeout_ms = 240000
    }))
    Assert-OnHookRoleLaunch -Tool "onhook_join_lan" -Result $result
    Assert-JoinerSidePasswordEntered -Result $result
    $script:OwnedRetailRuns.Add([string] $result.run_id)
    $script:RetailJoinerProcess = Get-Process -Id ([int] $result.pid) -ErrorAction Stop
    Bind-EffectiveRetailConfig -Role client -Source (Join-Path $ClientDir "onhook.cfg")
    $instance = Wait-RetailProcessForPeer -ProcessId $script:RetailJoinerProcess.Id `
        -Role joiner -RequireInMatch:($ReadinessMode -eq "in_match")
    $instanceId = [string] $instance.instance_id
    if (-not $script:RetailInstanceIds.Contains($instanceId)) {
        $script:RetailInstanceIds.Add($instanceId)
    }
    return [pscustomobject]@{
        process = $script:RetailJoinerProcess
        instance = $instance
        instance_id = $instanceId
        capture_path = [string] $result.capture_path
    }
}

function Wait-ParityWireReady {
    if (-not $ResolvedMissionArtifact) {
        throw "Wire readiness requires a hash-bound mission artifact and corpus."
    }
    $corpusRoot = Split-Path -Parent (Split-Path -Parent $ResolvedMissionArtifact)
    $itemsPath = Join-Path $corpusRoot "items.def"
    if (-not (Test-Path -LiteralPath $itemsPath -PathType Leaf)) {
        throw "Wire readiness items.def is missing: $itemsPath"
    }
    if (-not $script:EvidenceCapture) {
        throw "Wire readiness has no canonical perspective capture."
    }
    # onHook's in-process pcap writer holds traffic.pcap exclusively until
    # capture drain.  The simultaneously owned dumpcap stream is rolling and
    # snapshot-readable, so it is the pre-steady gate artifact.  Cleanup copies
    # that exact stream into the run root; the completed retail capture remains
    # the verdict-bearing perspective capture for RR/RO/OR comparisons.
    if (-not $script:ExternalCapture) {
        throw "Wire readiness has no rolling external capture."
    }
    $witnessPath = Join-Path $RunRoot "wire-ready.json"
    $arguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $WireReadyScript,
        "-Capture", $script:ExternalCapture,
        "-Items", $itemsPath,
        "-Mission", $Mission,
        "-MissionArtifact", $ResolvedMissionArtifact,
        "-GameType", [string] $GameType,
        "-HostSessionName", $ServerName,
        "-HostCallsign", $HostCallsign,
        "-JoinerCallsign", $JoinerCallsign,
        "-Map", $Mission,
        "-Expansion", $Expansion,
        "-Output", $witnessPath,
        "-TimeoutSeconds", "240"
    )
    if ($ServeOnly) { $arguments += "-NoHostPlayer" }
    $output = @(& powershell.exe @arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "wait_parity_wire_ready.ps1 failed: $($output -join ' | ')"
    }
    $line = @($output | Where-Object { "$_" -like "PARITY_WIRE_READY_JSON=*" } |
        Select-Object -Last 1)
    if ($line.Count -ne 1) {
        throw "wait_parity_wire_ready.ps1 returned no unique witness."
    }
    $script:WireReadyWitness = "$($line[0])".Substring(
        "PARITY_WIRE_READY_JSON=".Length) | ConvertFrom-Json
    if ([string] $script:WireReadyWitness.schema -ne
            "opennova.parity-wire-readiness.v2") {
        throw "Wire readiness witness schema drift."
    }
    return $script:WireReadyWitness
}

# A Serve Only cell's host frame: at the steady deadline, before the closing
# checks (serve-only-host-frame.png), and on the round's end (-Name
# serve-only-round-end-frame.png, from Wait-UntilSteadyDeadline): a retail
# host's through onhook_screenshot, ours through the game MCP's
# game_screenshot. Returns the saved path.
function Save-ServeOnlyHostFrame {
    param([string] $RetailInstanceId = "", [string] $Name = "serve-only-host-frame.png")
    if (-not $ServeOnly) { return }
    if (-not $RetailInstanceId -and $OpenNovaHostKind -eq "serve") {
        # opennova-serve draws no frame (its status page is off in a cell).
        Write-Host "PARITY_RUN_NOTE Serve Only: opennova-serve has no window, so no $Name"
        return ""
    }
    $path = Join-Path $RunRoot $Name
    if ($RetailInstanceId) {
        $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_screenshot" -Arguments @{
            instance_id = $RetailInstanceId
            path = $path
            preview_max_dim = 256
        })
        $path = [string] $result.path
    } else {
        $result = Invoke-GameTool -Port $HostMcpPort -Name "game_screenshot" -Arguments @{
            format = "png"
            max_dim = 4096
        }
        $image = @($result.content | Where-Object { $_.type -eq "image" }) | Select-Object -First 1
        if (-not $image) { throw "game_screenshot returned no image from the OpenNova host" }
        [IO.File]::WriteAllBytes($path, [Convert]::FromBase64String([string] $image.data))
    }
    if ($Name -eq "serve-only-host-frame.png") { $script:ServeOnlyHostScreenshot = $path }
    return $path
}

function Save-RetailScreenshot {
    param([Parameter(Mandatory = $true)] [string] $InstanceId)
    $path = Join-Path $RunRoot "retail-frame.png"
    $result = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_screenshot" -Arguments @{
        instance_id = $InstanceId
        path = $path
        preview_max_dim = 256
    })
    $script:Screenshot = $result.path
    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Image]::FromFile($script:Screenshot)
    try {
        $actual = "$($image.Width)x$($image.Height)"
        if ($actual -ne $Resolution) {
            throw "Retail viewport $actual does not match pinned matrix resolution $Resolution"
        }
    }
    finally { $image.Dispose() }
}

function ConvertFrom-RunnerStrictUtcTimestamp {
    param([Parameter(Mandatory = $true)] [string] $Value)
    if ($Value -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,7})?Z$') {
        throw "Timestamp is not strict ISO-8601 UTC: $Value"
    }
    [string[]] $formats = @("yyyy-MM-dd'T'HH:mm:ss'Z'")
    foreach ($digits in 1..7) {
        $formats += "yyyy-MM-dd'T'HH:mm:ss.$('f' * $digits)'Z'"
    }
    $styles = [Globalization.DateTimeStyles]::AssumeUniversal -bor
        [Globalization.DateTimeStyles]::AdjustToUniversal
    return [DateTimeOffset]::ParseExact(
        $Value, $formats, [Globalization.CultureInfo]::InvariantCulture, $styles)
}

function Assert-RunnerExactJsonProperties {
    param($Value, [string[]] $Expected, [string] $Context)
    if (-not $Value) { throw "$Context is missing" }
    $actual = @($Value.PSObject.Properties.Name | Sort-Object)
    $wanted = @($Expected | Sort-Object)
    if (($actual -join ',') -cne ($wanted -join ',')) {
        throw "$Context property set drift: actual=$($actual -join ',') expected=$($wanted -join ',')"
    }
}

function Stop-OpenNovaJoinerCooperatively {
    if ($Topology -notin @("RO", "OO")) {
        throw "Cooperative OpenNova joiner shutdown is valid only for RO/OO"
    }
    if ($script:OpenNovaJoinerShutdownWitness) {
        return $script:OpenNovaJoinerShutdownWitness
    }
    if (-not $script:OpenNovaJoiner) { return $null }
    if ($script:OpenNovaJoinerCooperativeStopAttempted) {
        throw "The cooperative OpenNova joiner shutdown attempt already failed"
    }
    $script:OpenNovaJoinerCooperativeStopAttempted = $true
    $process = $script:OpenNovaJoiner
    $process.Refresh()
    if ($process.HasExited) {
        throw "OpenNova joiner exited before a cooperative stop could be requested"
    }
    # Cancel any probe, ask the game to quit through its endpoint, and give it
    # 20 seconds; the process-shutdown evidence then proves the clean exit.
    $requestedUtc = [DateTime]::UtcNow.ToString("o")
    $quitAcknowledged = Stop-GameViaMcp -Port $JoinerMcpPort -Process $process -TimeoutSeconds 20
    $quitAcknowledgedUtc = [DateTime]::UtcNow.ToString("o")
    if (-not $quitAcknowledged) {
        throw "OpenNova joiner did not exit within 20 seconds of game_control quit"
    }
    $shutdown = Stop-OpenNovaProcess -Process $process `
        -GracefulTimeoutMs 0 -PassThruShutdownEvidence -QuitRequested
    if ([bool] $shutdown.exact_fallback_required -or $null -eq $shutdown.exit_code -or
            [int] $shutdown.exit_code -ne 0) {
        throw "OpenNova joiner cooperative shutdown was not clean: exact_fallback_required=$($shutdown.exact_fallback_required) exit_code=$($shutdown.exit_code)"
    }
    $requested = ConvertFrom-RunnerStrictUtcTimestamp $requestedUtc
    $acknowledged = ConvertFrom-RunnerStrictUtcTimestamp $quitAcknowledgedUtc
    $completed = ConvertFrom-RunnerStrictUtcTimestamp ([string] $shutdown.completed_utc)
    $processStarted = [DateTimeOffset]::new($process.StartTime.ToUniversalTime())
    # Each leg within its own budget: the quit leg is Stop-GameViaMcp's two
    # endpoint calls (10 s each) and its 20 s exit wait; the evidence leg is
    # Stop-OpenNovaProcess's two launcher waits (ForceTimeoutMs, 5 s each).
    if ($requested -lt $processStarted -or $acknowledged -lt $requested -or
            $acknowledged -gt $requested.AddSeconds(40) -or $completed -lt $acknowledged -or
            $completed -gt $acknowledged.AddSeconds(10)) {
        throw "OpenNova joiner shutdown timestamps drifted: requested $requestedUtc, quit acknowledged $quitAcknowledgedUtc, completed $($shutdown.completed_utc)"
    }
    $script:OpenNovaJoinerShutdownWitness = [pscustomobject][ordered]@{
        schema = "opennova.parity-joiner-shutdown.v2"
        run_id = $RunId
        topology = $Topology
        process_id = [int] $process.Id
        mcp_port = $JoinerMcpPort
        requested_utc = $requestedUtc
        quit_acknowledged_utc = $quitAcknowledgedUtc
        completed_utc = [string] $shutdown.completed_utc
        quit_acknowledged = $true
        exit_code = [int] $shutdown.exit_code
        clean = $true
    }
    return $script:OpenNovaJoinerShutdownWitness
}

function Stop-OpenNovaHostCleanly {
    if ($Topology -notin @("OR", "OO")) {
        throw "Clean OpenNova host shutdown proof is valid only for OR/OO"
    }
    if ($script:OpenNovaHostShutdownWitness) {
        return $script:OpenNovaHostShutdownWitness
    }
    if ($OpenNovaHostKind -eq "serve") { return Stop-OpenNovaServeHostCleanly }
    if (-not $script:OpenNovaHost) {
        throw "OpenNova host is missing before clean shutdown proof"
    }

    $process = $script:OpenNovaHost
    $logPath = Join-Path $RunRoot "host\godot.log"
    # A cooperative quit through the host's endpoint first; the window close
    # inside Stop-OpenNovaProcess is the witnessed fallback.
    $hostQuit = Stop-GameViaMcp -Port $HostMcpPort -Process $process -TimeoutSeconds 20
    $shutdown = Stop-OpenNovaProcess -Process $process `
        -PassThruShutdownEvidence -RequireCleanExit -QuitRequested:$hostQuit
    Assert-RunnerExactJsonProperties $shutdown @(
        "schema", "process_id", "requested_utc", "completed_utc",
        "close_requested", "exact_fallback_required", "launcher_killed",
        "runtime_killed", "exit_code", "clean"
    ) "OpenNova host process shutdown evidence"
    $script:OpenNovaHostExactFallbackRequired =
        [bool] $shutdown.exact_fallback_required

    $requested = ConvertFrom-RunnerStrictUtcTimestamp ([string] $shutdown.requested_utc)
    $completed = ConvertFrom-RunnerStrictUtcTimestamp ([string] $shutdown.completed_utc)
    $steadyCompleted = ConvertFrom-RunnerStrictUtcTimestamp $script:SteadyCompletedUtc
    $processStarted = [DateTimeOffset]::new($process.StartTime.ToUniversalTime())
    if ([string] $shutdown.schema -cne "opennova.process-shutdown.v1" -or
            [int] $shutdown.process_id -ne $process.Id -or
            $shutdown.close_requested -isnot [bool] -or
            -not [bool] $shutdown.close_requested -or
            $shutdown.exact_fallback_required -isnot [bool] -or
            [bool] $shutdown.exact_fallback_required -or
            $shutdown.launcher_killed -isnot [bool] -or
            [bool] $shutdown.launcher_killed -or
            $shutdown.runtime_killed -isnot [bool] -or
            [bool] $shutdown.runtime_killed -or
            -not ($shutdown.exit_code -is [int] -or $shutdown.exit_code -is [long]) -or
            [int] $shutdown.exit_code -ne 0 -or
            $shutdown.clean -isnot [bool] -or -not [bool] $shutdown.clean -or
            $requested -lt $processStarted -or $requested -lt $steadyCompleted -or
            $completed -lt $requested -or $completed -gt $requested.AddSeconds(20)) {
        throw "OpenNova host process shutdown evidence did not prove a clean post-steady exit"
    }

    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) {
        throw "OpenNova host shutdown log is missing: $logPath"
    }
    $resolvedLogPath = (Resolve-Path -LiteralPath $logPath -ErrorAction Stop).Path
    $expectedLogPath = [IO.Path]::GetFullPath((Join-Path $RunRoot "host\godot.log"))
    if (-not $resolvedLogPath.Equals(
            $expectedLogPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "OpenNova host shutdown log escaped its exact run path: $resolvedLogPath"
    }
    [byte[]] $logRaw = [IO.File]::ReadAllBytes($resolvedLogPath)
    $hasBom = ($logRaw.Length -ge 3 -and
            $logRaw[0] -eq 0xef -and $logRaw[1] -eq 0xbb -and
            $logRaw[2] -eq 0xbf) -or
        ($logRaw.Length -ge 2 -and
            (($logRaw[0] -eq 0xff -and $logRaw[1] -eq 0xfe) -or
             ($logRaw[0] -eq 0xfe -and $logRaw[1] -eq 0xff))) -or
        ($logRaw.Length -ge 4 -and
            $logRaw[0] -eq 0x00 -and $logRaw[1] -eq 0x00 -and
            $logRaw[2] -eq 0xfe -and $logRaw[3] -eq 0xff)
    if ($logRaw.Length -eq 0 -or $hasBom -or
            [Array]::IndexOf($logRaw, [byte] 0) -ge 0) {
        throw "OpenNova host log is empty, BOM-prefixed, or contains NUL bytes"
    }
    try { $logText = [Text.UTF8Encoding]::new($false, $true).GetString($logRaw) }
    catch { throw "OpenNova host log is not strict BOM-free UTF-8: $($_.Exception.Message)" }
    if ([string]::IsNullOrWhiteSpace($logText)) {
        throw "OpenNova host log contains no diagnostic text"
    }
    $forbiddenDiagnostics = @(
        [pscustomobject]@{ Name = "ObjectDB leak"; Pattern = '(?im)\bObjectDB\b[^\r\n]*\bleak(?:ed|s)?\b' },
        [pscustomobject]@{ Name = "resource leak"; Pattern = '(?im)\bResources?\b[^\r\n]*(?:\bstill in use\b|\bleak(?:ed|s)?\b)' },
        [pscustomobject]@{ Name = "RenderingServer leak"; Pattern = '(?im)\bRenderingServer\b[^\r\n]*(?:\bstill in use\b|\bleak(?:ed|s)?\b|get_singleton\(\))' },
        [pscustomobject]@{ Name = "RID leak"; Pattern = '(?im)\bRIDs?\b[^\r\n]*\bleak(?:ed|s)?\b' }
    )
    $detected = @($forbiddenDiagnostics | Where-Object {
        $logText -match $_.Pattern
    } | ForEach-Object { $_.Name })
    if ($detected.Count -ne 0) {
        throw "OpenNova host emitted forbidden shutdown diagnostics: $($detected -join ', ')"
    }

    $script:OpenNovaHostShutdownWitness = [pscustomobject][ordered]@{
        schema = "opennova.parity-host-shutdown.v1"
        run_id = $RunId
        topology = $Topology
        process_id = [int] $shutdown.process_id
        requested_utc = [string] $shutdown.requested_utc
        completed_utc = [string] $shutdown.completed_utc
        close_requested = [bool] $shutdown.close_requested
        exit_code = [int] $shutdown.exit_code
        clean = [bool] $shutdown.clean
        log_path = $resolvedLogPath
        log_sha256 = (Get-FileHash -LiteralPath $resolvedLogPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        forbidden_diagnostics_absent = $true
    }
    return $script:OpenNovaHostShutdownWitness
}

# -OpenNovaHostKind serve: opennova-serve over the SERVER copy (its working
# directory, as the game host's), its host file the game host's launch flags:
# the player cap (-MaxPlayers 4), the mode and the map list, the starting map
# last, as the host file reads it (apps/serve/README.md "The host file"). The
# file names no GameName, so the SERVER copy's game_name stands, and LanMode
# comes from that game.cfg's lanmode (1, the game host's -LanMode 1).
function Start-OpenNovaServeHostExact {
    $hostDir = Join-Path $RunRoot "host"
    New-Item -ItemType Directory -Path $hostDir -Force | Out-Null
    $hostFile = Join-Path $hostDir "parity.host"
    $lines = @(
        "// run_parity_topology.ps1 ${RunId}: the cell's host screen for opennova-serve.",
        "MaxPlayers 4",
        "GameType $GameType"
    )
    foreach ($entry in @($RotationList)) { $lines += "Mission $entry 0" }
    $lines += "Mission $Mission 0"
    [IO.File]::WriteAllText($hostFile, (($lines -join "`r`n") + "`r`n"), [Text.Encoding]::ASCII)
    $log = Join-Path $hostDir "serve.log"
    $pidFile = Join-Path $hostDir "serve.pid"
    $stopFile = Join-Path $hostDir "serve.stop"
    $tokens = @("-I", $ServeHostScript, "--exe", $ServeExe, "--cwd", $ServerDir, "--log", $log,
        "--pid-file", $pidFile, "--stop-file", $stopFile, "--",
        "--resource-dir", $ServerDir, "/HOST", $hostFile, "/exp", $Expansion,
        "--lan-port", [string] $Port, "--status-page", "off")
    $argumentLine = ($tokens | ForEach-Object {
        $value = [string] $_
        if ($value -match '\s') { '"{0}"' -f $value } else { $value }
    }) -join ' '
    $script:ServeWrapper = Start-Process -FilePath "python" -ArgumentList $argumentLine `
        -WorkingDirectory $hostDir -WindowStyle Hidden -PassThru
    $null = $script:ServeWrapper.Handle # keeps ExitCode readable once it exits
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) {
        $script:ServeWrapper.Refresh()
        if ($script:ServeWrapper.HasExited) {
            throw "serve_host.py exited ($($script:ServeWrapper.ExitCode)) before opennova-serve started"
        }
        if ([DateTime]::UtcNow -ge $deadline) { throw "serve_host.py wrote no pid in 30 s" }
        Start-Sleep -Milliseconds 200
    }
    $servePid = [int] (Get-Content -LiteralPath $pidFile -Raw).Trim()
    $script:OpenNovaHost = Get-Process -Id $servePid -ErrorAction Stop
    $null = $script:OpenNovaHost.Handle
    # Ready once the mission is up and the socket serves (the console's last start
    # line); discovery and the port's owner are checked as for the game host.
    $serving = "opennova-serve: serving on UDP $Port "
    $deadline = [DateTime]::UtcNow.AddSeconds(240)
    $servingLine = ""
    while (-not $servingLine) {
        $script:OpenNovaHost.Refresh()
        if ($script:OpenNovaHost.HasExited) {
            $tail = if (Test-Path -LiteralPath $log) { (Get-Content -LiteralPath $log -Tail 20) -join ' | ' } else { "" }
            throw "opennova-serve exited ($($script:OpenNovaHost.ExitCode)) before serving: $tail"
        }
        if (Test-Path -LiteralPath $log) {
            $servingLine = [string] (@(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue |
                Where-Object { "$_".Contains($serving) }) | Select-Object -First 1)
        }
        if (-not $servingLine) {
            if ([DateTime]::UtcNow -ge $deadline) {
                throw "opennova-serve did not log serving on UDP $Port within 240 s"
            }
            Start-Sleep -Milliseconds 500
        }
    }
    & $ProbeExe --host 127.0.0.1 --port $Port --timeout-ms 60000 `
        --interval-ms 1000 --expect-name $ServerName
    if ($LASTEXITCODE -ne 0) {
        throw "opennova-serve PID $servePid did not answer LAN discovery"
    }
    $script:OpenNovaHostStartOwnership = Assert-OpenNovaHostOwnsPort
    $script:ServeLaunch = [pscustomobject][ordered]@{
        schema = "opennova.parity-serve-launch.v1"
        exe = $ServeExe
        exe_sha256 = (Get-FileHash -LiteralPath $ServeExe -Algorithm SHA256).Hash.ToLowerInvariant()
        wrapper_pid = $script:ServeWrapper.Id
        serve_pid = $servePid
        working_dir = $ServerDir
        host_file = $hostFile
        host_file_lines = $lines
        log_path = $log
        serving_line = $servingLine
    }
    Write-Host "PARITY_RUN_NOTE opennova-serve PID $servePid serving: $servingLine"
}

# The game_state stand-in for opennova-serve (no MCP): its process and serving line.
function Get-OpenNovaServeHostState {
    $script:OpenNovaHost.Refresh()
    return [pscustomobject][ordered]@{
        kind = "serve"
        pid = $script:OpenNovaHost.Id
        running = -not $script:OpenNovaHost.HasExited
        serving_line = $(if ($script:ServeLaunch) { $script:ServeLaunch.serving_line } else { "" })
    }
}

# opennova-serve's clean stop: serve_host.py sends its console Ctrl+Break, whose
# handler stops the session (every joiner's round reset and STOP goodbye, the
# socket closed) and logs `opennova-serve: stopped`; the exit code must be 0.
function Stop-OpenNovaServeHostCleanly {
    if (-not $script:OpenNovaHost -or -not $script:ServeWrapper) {
        throw "opennova-serve is missing before clean shutdown proof"
    }
    $process = $script:OpenNovaHost
    $log = Join-Path $RunRoot "host\serve.log"
    $requestedUtc = [DateTime]::UtcNow.ToString("o")
    New-Item -ItemType File -Path (Join-Path $RunRoot "host\serve.stop") -Force | Out-Null
    if (-not $script:ServeWrapper.WaitForExit(30000)) {
        throw "opennova-serve did not stop within 30 s of its Ctrl+Break"
    }
    $process.Refresh()
    if (-not $process.HasExited -and -not $process.WaitForExit(5000)) {
        throw "opennova-serve outlived serve_host.py"
    }
    $completedUtc = [DateTime]::UtcNow.ToString("o")
    $exitCode = [int] $script:ServeWrapper.ExitCode
    $logText = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { "" }
    $stoppedLine = $logText -match '(?m)opennova-serve: stopped\s*$'
    if ($exitCode -ne 0 -or -not $stoppedLine) {
        throw "opennova-serve did not stop cleanly (exit $exitCode, stopped line $stoppedLine)"
    }
    $script:OpenNovaHostShutdownWitness = [pscustomobject][ordered]@{
        schema = "opennova.parity-host-shutdown.v1"
        run_id = $RunId
        topology = $Topology
        process_id = $process.Id
        requested_utc = $requestedUtc
        completed_utc = $completedUtc
        close_requested = $true
        exit_code = $exitCode
        clean = $true
        log_path = $log
        log_sha256 = (Get-FileHash -LiteralPath $log -Algorithm SHA256).Hash.ToLowerInvariant()
        forbidden_diagnostics_absent = $true
    }
    $script:ServeWrapper = $null
    return $script:OpenNovaHostShutdownWitness
}

function Start-OpenNovaHostExact {
    if ($OpenNovaHostKind -eq "serve") { Start-OpenNovaServeHostExact; return }
    $hostDir = Join-Path $RunRoot "host"
    New-Item -ItemType Directory -Path $hostDir -Force | Out-Null
    $script:OpenNovaHost = & $HostScript `
        -Mission $Mission -Name $HostCallsign -Port $Port -LanMode 1 `
        -MaxPlayers 4 -GameType ([string] $GameType) -Rotation $RotationList `
        -ResourceDir $ServerDir -WorkingDir $ServerDir -Expansion $Expansion `
        -LogFile (Join-Path $hostDir "godot.log") `
        -Resolution $Resolution -Windowed -McpPort $HostMcpPort `
        -SkipReadyCheck -PassThru
    if (-not $script:OpenNovaHost) { throw "OpenNova host launcher returned no process" }
    Register-OpenNovaLaunchProof -Role host -Process $script:OpenNovaHost
    & $ProbeExe --host 127.0.0.1 --port $Port --timeout-ms 240000 `
        --interval-ms 1000 --expect-name $ServerName
    if ($LASTEXITCODE -ne 0) {
        throw "OpenNova host PID $($script:OpenNovaHost.Id) did not answer LAN discovery"
    }
    $script:OpenNovaHostStartOwnership = Assert-OpenNovaHostOwnsPort
    # Discovery answers while the host is still loading its world; a joiner
    # dialing before the load completes times out on preload admission. The
    # endpoint's own account gates the joiner launch.
    $null = Wait-OpenNovaHostLoaded -TimeoutSeconds 240
}

function Get-OpenNovaHostState {
    if (-not $script:OpenNovaHost) { throw "No OpenNova host to query" }
    if ($OpenNovaHostKind -eq "serve") { return Get-OpenNovaServeHostState }
    return Get-StructuredResult (Invoke-GameTool -Port $HostMcpPort -Name "game_state")
}

function Wait-OpenNovaHostLoaded {
    param([int] $TimeoutSeconds = 240)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        $script:OpenNovaHost.Refresh()
        if ($script:OpenNovaHost.HasExited) {
            throw "OpenNova host exited before its world loaded"
        }
        $state = Get-OpenNovaHostState
        $network = $null
        if ($state.runtime.PSObject.Properties['network']) { $network = $state.runtime.network }
        # The boot is complete once the host's own player has spawned (the
        # listen server auto-spawns it at the end of the mission boot); the
        # world node exists earlier, while the load still runs. A Serve Only
        # host has no own player: its loaded world hosting on the port.
        if ([bool] $state.shell.world_loaded -and ($ServeOnly -or [bool] $state.player.present) -and $network -and
                [string] $network.role -eq "host" -and [int] $network.port -eq $Port) {
            return $state
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "OpenNova host did not report a loaded world hosting on port $Port within $TimeoutSeconds s"
        }
        Start-Sleep -Milliseconds 500
    }
}

function Start-OpenNovaJoinerExact {
    $joinerDir = Join-Path $RunRoot "joiner"
    New-Item -ItemType Directory -Path $joinerDir -Force | Out-Null
    $joinTarget = if ($JoinerDial -eq "browse") { "browse:$Port" } else { "127.0.0.1:$Port" }
    $godotArguments = @(
        "--windowed", "--resolution", $Resolution,
        "--log-file", (Join-Path $joinerDir "godot.log")
    )
    if ($JoinerVerbose) { $godotArguments += "--verbose" }
    $script:OpenNovaJoiner = Start-OpenNovaProcess `
        -GodotArguments $godotArguments `
        -GameArguments (@(
            "--lan-join", $joinTarget,
            "--callsign", $JoinerCallsign
        ) + @(
            "--resource-dir", $ClientDir, "/exp", $Expansion
        )) `
        -McpPort $JoinerMcpPort
    if (-not $script:OpenNovaJoiner) { throw "OpenNova joiner launcher returned no process" }
    Register-OpenNovaLaunchProof -Role joiner -Process $script:OpenNovaJoiner
    if ($JoinerSidePassword) { Enter-OpenNovaJoinerSidePassword }
}

# The joiner's readiness witness. The first call runs parity_joiner_ready in
# the joiner (blocking until the requested readiness, sending the default
# deployment pick when -AutoDeploy); every later call takes one live snapshot
# through parity_joiner_state. Both carry the same witness the readiness
# classifiers in lib.ps1 consume unchanged.
# Our joiner after a rotation (E2): wait up to 60 s for the next mission's match
# (an in-match or post-death-redeploy class); a joiner still at that mission's
# deploy screen closes the cell when it is alive and connected: its heartbeat
# advances, it keeps its self handle, and it has no join error or lost session.
# x3's round RO and OO joiners failed the first mission's in-match check there.
# The wait starts only once the rotation is on the wire: the host's S2C 0x25 (the
# next mission's load) at or after the window's start in the rolling external
# capture. Before it, a snapshot still reads the old mission's match (x8's Serve
# Only RO: the first snapshot read in_match 0.19 s before the retail host's 0x25,
# the cell quit our joiner, and no reload head reached the capture).
function Wait-OpenNovaJoinerAfterRotation {
    param([int] $TimeoutSeconds = 60)
    $steadyStarted = [DateTime]::Parse(
        $script:SteadyStartedUtc,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind
    ).ToUniversalTime()
    $reload = Start-CaptureTagWait -Tag "0x25" -TimeoutSeconds $TimeoutSeconds -AfterUtc $steadyStarted
    if (-not $reload) {
        throw "The rotation's S2C 0x25 (the next mission's load) did not reach the capture within $TimeoutSeconds s"
    }
    Write-Host "PARITY_RUN_NOTE rotation: the host's S2C 0x25 at capture frame $($reload.frame); the OpenNova joiner's wait starts"
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $first = $null
    while ($true) {
        $script:OpenNovaJoiner.Refresh()
        if ($script:OpenNovaJoiner.HasExited) {
            throw "OpenNova joiner exited after the rotation"
        }
        $status = Invoke-GameProbe -Port $JoinerMcpPort -Name "parity_joiner_state" -Process $script:OpenNovaJoiner `
            -TimeoutSeconds 30 -PollMs 250 -Arguments @{
                readiness_mode = $ReadinessMode
                auto_deploy = [bool] $AutoDeploy
                exercise_motion = [bool] $ExerciseInput
            }
        # At the 0x25 our joiner unloads the old mission; until the next one's
        # load builds a simulation the snapshot has nothing to witness. That is
        # the reload in progress, not a failure (p192's Serve Only RO).
        if ([string] $status.state -eq "failed" -and $status.verdict -and
                [string] $status.verdict.summary -eq "no simulation to witness") {
            if ([DateTime]::UtcNow -ge $deadline) {
                throw "OpenNova joiner had no simulation for $TimeoutSeconds s after the rotation's S2C 0x25"
            }
            Start-Sleep -Milliseconds 1000
            continue
        }
        if ([string] $status.state -ne "passed" -or -not $status.verdict) {
            $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
            throw "OpenNova joiner state snapshot failed after the rotation: state=$($status.state) $why"
        }
        $state = $status.verdict.data
        if (-not $first) { $first = $state }
        $class = Get-OpenNovaJoinerReadinessClass -State $state -ReadinessMode $ReadinessMode `
            -AllowPostDeathRedeploy
        $live = [int] $state.self_handle -gt 0 -and [string]::IsNullOrEmpty([string] $state.join_error) -and
            -not [bool] $state.session_lost -and
            [int64] $state.heartbeat_sequence -gt [int64] $first.heartbeat_sequence
        if ($class -ne 'invalid' -or ([DateTime]::UtcNow -ge $deadline -and $live)) {
            Write-Host ("PARITY_RUN_NOTE rotation: OpenNova joiner closes as {0} (in_match={1} local_player={2} deploy_presented={3} phase={4} stage='{5}')" -f
                $(if ($class -ne 'invalid') { $class } else { 'connected_after_rotation' }), $state.in_match,
                $state.local_player, $state.deploy_presented, $state.joiner_phase, $state.join_admission_stage)
            $state | Add-Member -NotePropertyName after_rotation_class -NotePropertyValue $(if ($class -ne 'invalid') { $class } else { 'connected_after_rotation' }) -Force
            $state | Add-Member -NotePropertyName observed_utc -NotePropertyValue ([DateTime]::UtcNow.ToString("o")) -Force
            $state | Add-Member -NotePropertyName rotation_reload -NotePropertyValue $reload -Force
            return $state
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw (Write-JoinerReadinessRefusal -State $state `
                -Reason "OpenNova joiner is neither in the rotation's next match nor live after $TimeoutSeconds s")
        }
        Start-Sleep -Milliseconds 1000
    }
}

# A refused joiner readiness state names the witness fields it turned on, in a
# PARITY_RUN_NOTE and in the text the caller throws.
function Write-JoinerReadinessRefusal {
    param([Parameter(Mandatory = $true)] [string] $Reason, $State)
    $message = "$Reason ($(Format-OpenNovaJoinerReadinessFields -State $State))"
    Write-Host "PARITY_RUN_NOTE joiner readiness refused: $message"
    return $message
}

function Wait-OpenNovaJoinerReady {
    param(
        [int] $TimeoutSeconds = 240,
        [long] $MinimumTicksMsec = -1,
        [switch] $RequireMotionComplete,
        [switch] $AllowPostDeathRedeploy,
        [switch] $AllowScenarioDeploy
    )
    $script:OpenNovaJoiner.Refresh()
    if ($script:OpenNovaJoiner.HasExited) {
        throw "OpenNova joiner exited before reporting $ReadinessMode readiness"
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        if (-not $script:JoinerReadyRunId) {
            $status = Invoke-GameProbe -Port $JoinerMcpPort -Name "parity_joiner_ready" -Process $script:OpenNovaJoiner `
                -TimeoutSeconds $TimeoutSeconds -PollMs 1000 -Arguments @{
                    readiness_mode = $ReadinessMode
                    auto_deploy = [bool] $AutoDeploy
                    exercise_motion = [bool] $ExerciseInput
                    run_id = $RunId
                    topology = $Topology
                }
            $script:JoinerReadyRunId = [string] $status.run_id
            if ([string] $status.state -ne "passed" -or -not $status.verdict -or
                    -not [bool] $status.verdict.ok) {
                $why = if ($status.verdict) { [string] $status.verdict.summary } else { [string] $status.error }
                throw "OpenNova joiner did not reach $ReadinessMode readiness: state=$($status.state) $why"
            }
            $ready = $status.verdict.data
        }
        else {
            $status = Invoke-GameProbe -Port $JoinerMcpPort -Name "parity_joiner_state" -Process $script:OpenNovaJoiner `
                -TimeoutSeconds 30 -PollMs 250 -Arguments @{
                    readiness_mode = $ReadinessMode
                    auto_deploy = [bool] $AutoDeploy
                    exercise_motion = [bool] $ExerciseInput
                }
            if ([string] $status.state -ne "passed" -or -not $status.verdict) {
                throw "OpenNova joiner state snapshot failed: state=$($status.state) $($status.error)"
            }
            $ready = $status.verdict.data
        }
        if ([int] $ready.process_id -ne $script:OpenNovaJoiner.Id) {
            throw "OpenNova readiness witness PID does not match the owned joiner process"
        }
        if ([string] $ready.readiness_mode -ne $ReadinessMode) {
            throw "OpenNova joiner witness readiness mode does not match the run"
        }
        $readinessClass = Get-OpenNovaJoinerReadinessClass `
            -State $ready -ReadinessMode $ReadinessMode `
            -AllowPostDeathRedeploy:$AllowPostDeathRedeploy `
            -AllowScenarioDeploy:$AllowScenarioDeploy
        if ($readinessClass -eq 'invalid') {
            throw (Write-JoinerReadinessRefusal -State $ready `
                -Reason "OpenNova joiner witness reports an invalid admission/liveness state")
        }
        $ticksMsec = [long] $ready.ticks_msec
        if ($ticksMsec -gt $MinimumTicksMsec -and
                (-not $RequireMotionComplete -or [bool] $ready.motion_complete)) {
            $ready | Add-Member -NotePropertyName observed_utc `
                -NotePropertyValue ([DateTime]::UtcNow.ToString("o")) -Force
            $ready | Add-Member -NotePropertyName probe_run_id `
                -NotePropertyValue ([string] $status.run_id) -Force
            return $ready
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Timed out waiting for OpenNova joiner $ReadinessMode readiness witness"
        }
        Start-Sleep -Milliseconds 250
    }
}

try {
    Write-Host "PARITY_RUN_BEGIN topology=$Topology mission=$Mission game_type=$GameType port=$Port resolution=$Resolution joiner_dial=$JoinerDial run=$RunId"
    Assert-UdpPortUnowned
    Save-ServerCopyFiles
    Start-DesktopGuard
    if ($TimeLimitMinutes -gt 0) { Set-ServerGameCfgValue -Key "time_limit" -Value $TimeLimitMinutes }
    if ($PermanentDeath) { Set-ServerGameCfgValue -Key "mp_permanent_death" -Value 1 }
    if ($HostSidePasswordA) { Set-ServerGameCfgString -Key "mphostsidepassworda" -Value $HostSidePasswordA }
    if ($HostSidePasswordB) { Set-ServerGameCfgString -Key "mphostsidepasswordb" -Value $HostSidePasswordB }
    if ($HostPlayerClass -ne 0) { Set-ServerHostPlayerClass }
    if ($PreJoinMissionCycle) { Enable-HostAdminConsole }
    if ($ServeOnly -and $Topology -in @("OR", "OO")) { Set-ServerGameCfgValue -Key "dedicated" -Value 1 }
    Start-OnHookMcp

    # Exercise the tracked capture wrapper used by the runbook so every suite
    # also covers its scalar-interface path.
    # Worst-case discovery, endpoint readiness, and decoded wire readiness can
    # run sequentially before the 120-second maximum steady window. Keep the
    # safety cap beyond that whole
    # interval; normal completion still stops the exact capture in finally,
    # after both endpoints have shut down.
    $captureStart = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $CaptureScript -Action start -Tag $script:CaptureTag `
        -Filter "udp and port $Port" -DurationSeconds 1500)
    if ($LASTEXITCODE -ne 0) { throw "capture.ps1 start failed: $LASTEXITCODE" }
    $capturePathLine = $captureStart | Where-Object { "$_" -like "CAPTURE_FILE=*" } |
        Select-Object -Last 1
    if (-not $capturePathLine) { throw "capture.ps1 did not report CAPTURE_FILE" }
    $script:ExternalCapture = "$capturePathLine".Substring("CAPTURE_FILE=".Length)
    $script:CaptureStarted = $true

    switch ($Topology) {
        "RR" {
            # onhook_run_lan_pair takes no serve_only: a Serve Only RR launches its
            # host (onhook_host_lan serve_only) and its joiner one by one, as
            # does a cell whose host cycles its mission before the join.
            if ($ReadinessMode -eq "deploy_hold" -or $ServeOnly -or $PreJoinMissionCycle -or $JoinerSidePassword) {
                $retailHost = Start-RetailHostExact
                Invoke-PreJoinMissionCycle -RetailHostProcessId $script:RetailHostProcess.Id
                $retailJoiner = Start-RetailJoinerExact
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $hostInstanceId = [string] $hostInstance.instance_id
                if (-not $script:RetailInstanceIds.Contains($hostInstanceId)) {
                    $script:RetailInstanceIds.Add($hostInstanceId)
                }
                $script:EvidenceCapture = [string] $retailJoiner.capture_path
                $script:EvidencePerspective = "retail-client"
                $script:EvidenceRole = "joiner"
                $script:EvidenceInstanceId = [string] $retailJoiner.instance_id
                $script:RetailInstanceId = [string] $retailJoiner.instance_id
                $null = Wait-ParityWireReady
                $script:Acceptance = [pscustomobject]@{
                    host = $hostInstance.run_state
                    joiner = $retailJoiner.instance.run_state
                    host_pid = $script:RetailHostProcess.Id
                    joiner_pid = $script:RetailJoinerProcess.Id
                    wire_ready = $script:WireReadyWitness
                }
                Invoke-ScenarioSetupBeforeWindow -RetailJoinerInstanceId ([string] $retailJoiner.instance_id) `
                    -RetailHostInstanceId $hostInstanceId
                Begin-SteadyWindow
                Invoke-ScenarioIfRequested -RetailJoinerInstanceId ([string] $retailJoiner.instance_id) `
                    -RetailHostInstanceId $hostInstanceId
                Start-RetailLateJoiner
                Wait-UntilSteadyDeadline -ServeOnlyRoundEnd -RetailInstanceId $hostInstanceId
                $null = Save-ServeOnlyHostFrame -RetailInstanceId $hostInstanceId
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $joinerInstance = if (Test-ScenarioEndsJoinerSession) {
                    # The scenario ends the joiner's session (E4's kick): its
                    # closing state is read, not waited for.
                    @(Get-StructuredResult (Invoke-OnHookTool -Name "onhook_instances")).instances |
                        Where-Object { [int] $_.pid -eq $script:RetailJoinerProcess.Id } | Select-Object -First 1
                } else {
                    Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailJoinerProcess.Id -Role joiner `
                    -RequireInMatch:($ReadinessMode -eq "in_match") `
                    -AllowScenarioDeploy:([bool] $script:ScenarioSpec)
                }
                $script:Acceptance = [pscustomobject]@{
                    host = $hostInstance.run_state
                    joiner = $(if ($joinerInstance) { $joinerInstance.run_state } else { $null })
                    host_pid = $script:RetailHostProcess.Id
                    joiner_pid = $script:RetailJoinerProcess.Id
                    wire_ready = $script:WireReadyWitness
                }
                Complete-RetailLateJoiner
                Complete-SteadyWindow
                Stop-RetailProcessCapture -InstanceId ([string] $retailJoiner.instance_id) `
                    -RunLabel "$RunId-joiner"
                Stop-RetailProcessCapture -InstanceId $hostInstanceId `
                    -RunLabel "$RunId-host"
                Stop-ExactProcess -Process $script:RetailJoinerProcess
                $script:RetailJoinerProcess = $null
                Stop-ExactProcess -Process $script:RetailHostProcess
                $script:RetailHostProcess = $null
                break
            }
            $pairArguments = @{
                mission = $Mission
                host_game_dir = $ServerDir
                joiner_game_dir = $ClientDir
                expansion = $Expansion
                join_address = "127.0.0.1"
                port = $Port
                host_callsign = $HostCallsign
                joiner_callsign = $JoinerCallsign
                game_type = [string] $GameType
                max_players = 4
                run_id = $RunId
                output_dir = $RunRoot
                capture = $true
                windowed = $true
                allow_many = $true
                stock_observer = $StockObserver
                # The pair's deploy is both roles' (-DeployZones).
                deploy = [bool] $DeployZones
                wait_timeout_ms = 240000
            }
            if ($RotationList.Count -gt 0) { $pairArguments.rotation = @($RotationList) }
            $pairResult = Invoke-OnHookTool -Name "onhook_run_lan_pair" -Arguments $pairArguments
            $pair = Get-StructuredResult $pairResult
            $script:OwnedRetailRuns.Add([string] $pair.host.run_id)
            $script:OwnedRetailRuns.Add([string] $pair.joiner.run_id)
            Assert-RetailHostRotation -Result $pair
            Assert-RetailStock -Tool "onhook_run_lan_pair host" `
                -Reported $pair.host.instance.stock_observer
            Assert-RetailStock -Tool "onhook_run_lan_pair joiner" `
                -Reported $pair.joiner.instance.stock_observer
            Bind-EffectiveRetailConfig -Role server -Source (Join-Path $ServerDir "onhook.cfg")
            Bind-EffectiveRetailConfig -Role client -Source (Join-Path $ClientDir "onhook.cfg")
            $hostStatus = Wait-RetailRunForPeer -OwnedRunId ([string] $pair.host.run_id)
            $joinerStatus = Wait-RetailRunForPeer -OwnedRunId ([string] $pair.joiner.run_id)
            $script:EvidenceCapture = [string] $pair.joiner.capture_path
            $script:EvidencePerspective = "retail-client"
            $script:EvidenceRole = "joiner"
            $script:EvidenceInstanceId = [string] $pair.joiner.instance_id
            $script:Acceptance = [pscustomobject]@{
                host = $hostStatus.instance.run_state
                joiner = $joinerStatus.instance.run_state
                host_pid = [int] $pair.host.pid
                joiner_pid = [int] $pair.joiner.pid
            }
            $null = Wait-ParityWireReady
            $script:Acceptance | Add-Member -NotePropertyName wire_ready `
                -NotePropertyValue $script:WireReadyWitness -Force
            Invoke-ScenarioSetupBeforeWindow -RetailJoinerInstanceId ([string] $pair.joiner.instance_id) `
                -RetailHostInstanceId ([string] $pair.host.instance_id)
            Begin-SteadyWindow
            Invoke-ScenarioIfRequested -RetailJoinerInstanceId ([string] $pair.joiner.instance_id) `
                -RetailHostInstanceId ([string] $pair.host.instance_id)
            if ($ExerciseInput) {
                Invoke-RetailInputExercise -InstanceId ([string] $pair.joiner.instance_id) `
                    -ProcessId ([int] $pair.joiner.pid)
            }
            Start-RetailLateJoiner
            Wait-UntilSteadyDeadline
            $hostStatus = Wait-RetailRunForPeer -OwnedRunId ([string] $pair.host.run_id) `
                -AfterRotation:(Test-RotationCell)
            $joinerStatus = if (Test-ScenarioEndsJoinerSession) {
                Get-StructuredResult (Invoke-OnHookTool -Name "onhook_run_status" -Arguments @{
                    run_id = [string] $pair.joiner.run_id
                })
            } else {
                Wait-RetailRunForPeer -OwnedRunId ([string] $pair.joiner.run_id) `
                    -AfterRotation:(Test-RotationCell)
            }
            $script:Acceptance = [pscustomobject]@{
                host = $hostStatus.instance.run_state
                joiner = $joinerStatus.instance.run_state
                host_pid = [int] $pair.host.pid
                joiner_pid = [int] $pair.joiner.pid
                wire_ready = $script:WireReadyWitness
            }
            Complete-RetailLateJoiner
            Complete-SteadyWindow
            Save-RetailScreenshot -InstanceId ([string] $pair.joiner.instance_id)
            Stop-OwnedRetailRun -OwnedRunId ([string] $pair.joiner.run_id)
            Stop-OwnedRetailRun -OwnedRunId ([string] $pair.host.run_id)
        }
        "RO" {
            if ($ReadinessMode -eq "deploy_hold") {
                $retailHost = Start-RetailHostExact
                $script:EvidenceCapture = [string] $retailHost.capture_path
                $script:EvidencePerspective = "retail-host"
                $script:EvidenceRole = "host"
                Invoke-PreJoinMissionCycle -RetailHostProcessId $script:RetailHostProcess.Id
                Start-OpenNovaJoinerExact
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $hostInstanceId = [string] $hostInstance.instance_id
                $script:RetailInstanceId = $hostInstanceId
                $script:EvidenceInstanceId = $hostInstanceId
                if (-not $script:RetailInstanceIds.Contains($hostInstanceId)) {
                    $script:RetailInstanceIds.Add($hostInstanceId)
                }
                $joinerAcceptance = Wait-OpenNovaJoinerReady
                $initialJoinerAcceptance = $joinerAcceptance
                $initialJoinerTicks = [long] $joinerAcceptance.ticks_msec
                $null = Wait-ParityWireReady
                $script:Acceptance = [pscustomobject]@{
                    retail_host = [pscustomobject]@{
                        instance = $hostInstance
                        pid = $script:RetailHostProcess.Id
                        port_ownership = $retailHost.port_ownership
                    }
                    opennova_joiner = $joinerAcceptance
                    wire_ready = $script:WireReadyWitness
                }
                Invoke-ScenarioSetupBeforeWindow -RetailHostInstanceId $hostInstanceId
                Begin-SteadyWindow
                Invoke-ScenarioIfRequested -RetailHostInstanceId $hostInstanceId
                Wait-UntilSteadyDeadline
                $script:OpenNovaJoiner.Refresh()
                if ($script:OpenNovaJoiner.HasExited) {
                    throw "OpenNova joiner exited during the RO deploy-hold steady window"
                }
                $hostInstance = Wait-RetailProcessForPeer `
                    -ProcessId $script:RetailHostProcess.Id -Role host
                $joinerAcceptance = Wait-OpenNovaJoinerReady `
                    -MinimumTicksMsec $initialJoinerTicks `
                    -AllowScenarioDeploy:([bool] $script:ScenarioSpec)
                if (-not (Test-OpenNovaJoinerReadinessTransition `
                        -Initial $initialJoinerAcceptance -Final $joinerAcceptance `
                        -ReadinessMode $ReadinessMode `
                        -AllowScenarioDeploy:([bool] $script:ScenarioSpec))) {
                    throw (Write-JoinerReadinessRefusal -State $joinerAcceptance `
                        -Reason "OpenNova joiner deploy-hold heartbeat transition is invalid")
                }
                $script:Acceptance = [pscustomobject]@{
                    retail_host = [pscustomobject]@{
                        instance = $hostInstance
                        pid = $script:RetailHostProcess.Id
                        port_ownership = (Assert-RetailHostOwnsPort `
                            -ProcessId $script:RetailHostProcess.Id)
                    }
                    opennova_joiner_initial = $initialJoinerAcceptance
                    opennova_joiner = $joinerAcceptance
                    wire_ready = $script:WireReadyWitness
                }
                Complete-SteadyWindow
                $null = Stop-OpenNovaJoinerCooperatively
                $script:OpenNovaJoiner = $null
                Stop-RetailProcessCapture -InstanceId $hostInstanceId `
                    -RunLabel "$RunId-host"
                Stop-ExactProcess -Process $script:RetailHostProcess
                $script:RetailHostProcess = $null
                break
            }
            $hostResult = Invoke-OnHookTool -Name "onhook_host_lan" `
                -Arguments (Get-RetailHostArguments -OutputDir $RunRoot)
            $retailHost = Get-StructuredResult $hostResult
            $script:OwnedRetailRuns.Add([string] $retailHost.run_id)
            Assert-OnHookRoleLaunch -Tool "onhook_host_lan" -Result $retailHost
            Assert-RetailHostRotation -Result $retailHost
            Assert-RetailHostServeOnly -Result $retailHost
            Bind-EffectiveRetailConfig -Role server -Source (Join-Path $ServerDir "onhook.cfg")
            $script:EvidenceCapture = [string] $retailHost.capture_path
            $script:EvidencePerspective = "retail-host"
            $script:EvidenceRole = "host"
            $script:EvidenceInstanceId = [string] $retailHost.instance_id
            Invoke-PreJoinMissionCycle -RetailHostProcessId ([int] $retailHost.pid)
            Start-OpenNovaJoinerExact
            $retailAcceptance = Wait-RetailRunForPeer -OwnedRunId ([string] $retailHost.run_id) -ServeOnlyHost:$ServeOnly
            $joinerAcceptance = Wait-OpenNovaJoinerReady
            $initialJoinerAcceptance = $joinerAcceptance
            $initialJoinerTicks = [long] $joinerAcceptance.ticks_msec
            $script:Acceptance = [pscustomobject]@{
                retail_host = $retailAcceptance
                opennova_joiner = $joinerAcceptance
            }
            $null = Wait-ParityWireReady
            $script:Acceptance | Add-Member -NotePropertyName wire_ready `
                -NotePropertyValue $script:WireReadyWitness -Force
            Invoke-ScenarioSetupBeforeWindow -RetailHostInstanceId ([string] $retailHost.instance_id)
            Begin-SteadyWindow
            Invoke-ScenarioIfRequested -RetailHostInstanceId ([string] $retailHost.instance_id)
            if ($ExerciseInput) {
                Publish-MotionGate
            }
            Wait-UntilSteadyDeadline -ServeOnlyRoundEnd -RetailInstanceId ([string] $retailHost.instance_id)
            $null = Save-ServeOnlyHostFrame -RetailInstanceId ([string] $retailHost.instance_id)
            $script:OpenNovaJoiner.Refresh()
            if ($script:OpenNovaJoiner.HasExited) {
                throw "OpenNova joiner exited during the RO steady window"
            }
            $retailAcceptance = Wait-RetailRunForPeer -OwnedRunId ([string] $retailHost.run_id) -ServeOnlyHost:$ServeOnly `
                -AfterRotation:(Test-RotationCell)
            $joinerAcceptance = $null
            if (Test-RotationCell) {
                $joinerAcceptance = Wait-OpenNovaJoinerAfterRotation
            }
            elseif (-not (Test-ScenarioEndsJoinerSession)) {
                $joinerAcceptance = Wait-OpenNovaJoinerReady `
                    -MinimumTicksMsec $initialJoinerTicks -RequireMotionComplete `
                    -AllowPostDeathRedeploy
                if (-not (Test-OpenNovaJoinerReadinessTransition `
                        -Initial $initialJoinerAcceptance -Final $joinerAcceptance `
                        -ReadinessMode $ReadinessMode -AllowPostDeathRedeploy `
                        -RequireMotionComplete)) {
                    throw (Write-JoinerReadinessRefusal -State $joinerAcceptance `
                        -Reason "OpenNova joiner in-match/post-death heartbeat transition is invalid")
                }
            }
            $script:Acceptance = [pscustomobject]@{
                retail_host = $retailAcceptance
                opennova_joiner_initial = $initialJoinerAcceptance
                opennova_joiner = $joinerAcceptance
                wire_ready = $script:WireReadyWitness
            }
            Complete-SteadyWindow
            Save-RetailScreenshot -InstanceId ([string] $retailHost.instance_id)
            $null = Stop-OpenNovaJoinerCooperatively
            $script:OpenNovaJoiner = $null
            Stop-OwnedRetailRun -OwnedRunId ([string] $retailHost.run_id)
        }
        "OR" {
            Start-OpenNovaHostExact
            Invoke-PreJoinMissionCycle
            $joinerDir = Join-Path $RunRoot "joiner"
            New-Item -ItemType Directory -Path $joinerDir -Force | Out-Null
            $retailJoinLaunch = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_join_lan" -Arguments (Add-JoinerSidePassword @{
                game_dir = $ClientDir
                expansion = $Expansion
                join_address = "127.0.0.1"
                port = $Port
                callsign = $JoinerCallsign
                run_id = "$RunId-joiner"
                output_dir = $joinerDir
                capture = $true
                windowed = $true
                allow_many = $true
                stock_observer = $StockObserver
                deploy = [bool] $DeployZones
                readiness = $(if ($ReadinessMode -eq "in_match") { "in_match" } else { "peer" })
                wait_timeout_ms = 240000
            }))
            Assert-OnHookRoleLaunch -Tool "onhook_join_lan" -Result $retailJoinLaunch
            Assert-JoinerSidePasswordEntered -Result $retailJoinLaunch
            $script:OwnedRetailRuns.Add([string] $retailJoinLaunch.run_id)
            $script:RetailProcess = Get-Process -Id ([int] $retailJoinLaunch.pid) -ErrorAction Stop
            $script:RetailJoinerProcess = $script:RetailProcess
            $clientCfg = Join-Path $ClientDir "onhook.cfg"
            $cfgText = Get-Content -LiteralPath $clientCfg -Raw
            # The managed keys onhook_join_lan renders for this case (the
            # expansion travels as its launch argument, not in the cfg).
            foreach ($expected in @(
                "LanJoinAddress 127.0.0.1",
                "LanJoinPort $Port",
                "LanJoinCallsign $JoinerCallsign",
                "StockObserver $(if ($StockObserver) { 1 } else { 0 })"
            )) {
                if ($cfgText -notmatch [regex]::Escape($expected)) {
                    throw "CLIENT cfg is not pinned to the current matrix case: missing '$expected'"
                }
            }
            Bind-EffectiveRetailConfig -Role client -Source $clientCfg
            $script:EvidenceCapture = [string] $retailJoinLaunch.capture_path
            $script:EvidencePerspective = "retail-client"
            $script:EvidenceRole = "joiner"
            if ($WireOnly) {
                Begin-SteadyWindow
                Wait-UntilSteadyDeadline
                $script:RetailProcess.Refresh()
                if ($script:RetailProcess.HasExited) {
                    throw "Retail joiner exited during the OR wire-only steady window"
                }
                $endOwnership = Assert-OpenNovaHostOwnsPort
                $instances = Get-StructuredResult (Invoke-OnHookTool -Name "onhook_instances")
                $instance = @($instances.instances) |
                    Where-Object { [int] $_.pid -eq $script:RetailProcess.Id } |
                    Select-Object -First 1
                if ($instance) {
                    $script:RetailInstanceId = [string] $instance.instance_id
                    $script:Acceptance = [pscustomobject]@{
                        retail_joiner = $instance.run_state
                        opennova_host = [pscustomobject]@{
                            pid = $script:OpenNovaHost.Id
                            port_ownership = $endOwnership
                        }
                    }
                    if ($instance.capture_ready) {
                        Save-RetailScreenshot -InstanceId $script:RetailInstanceId
                    }
                } else {
                    $script:Acceptance = [pscustomobject]@{
                        wire_only = $true
                        retail_pid = $script:RetailProcess.Id
                        sustained_seconds = $SteadySeconds
                        opennova_host = [pscustomobject]@{
                            pid = $script:OpenNovaHost.Id
                            port_ownership = $endOwnership
                        }
                    }
                }
                Complete-SteadyWindow
            } else {
                $instance = Wait-RetailProcessForPeer -ProcessId $script:RetailProcess.Id `
                    -Role joiner -RequireInMatch:($ReadinessMode -eq "in_match")
                $script:RetailInstanceId = [string] $instance.instance_id
                $script:EvidenceInstanceId = $script:RetailInstanceId
                if (-not $script:RetailInstanceIds.Contains($script:RetailInstanceId)) {
                    $script:RetailInstanceIds.Add($script:RetailInstanceId)
                }
                $null = Wait-ParityWireReady
                Invoke-ScenarioSetupBeforeWindow -RetailJoinerInstanceId $script:RetailInstanceId
                Begin-SteadyWindow
                Invoke-ScenarioIfRequested -RetailJoinerInstanceId $script:RetailInstanceId
                if ($ExerciseInput) {
                    Invoke-RetailInputExercise -InstanceId $script:RetailInstanceId `
                        -ProcessId $script:RetailProcess.Id
                }
                Start-RetailLateJoiner
                Wait-UntilSteadyDeadline -ServeOnlyRoundEnd
                $null = Save-ServeOnlyHostFrame
                $script:RetailProcess.Refresh()
                $script:OpenNovaHost.Refresh()
                if ($script:RetailProcess.HasExited -or $script:OpenNovaHost.HasExited) {
                    throw "An OR endpoint exited during the steady window"
                }
                $instance = if (Test-ScenarioEndsJoinerSession) {
                    # The scenario ends the joiner's session (E4's kick): its
                    # closing state is read, not waited for (as RR).
                    @(Get-StructuredResult (Invoke-OnHookTool -Name "onhook_instances")).instances |
                        Where-Object { [int] $_.pid -eq $script:RetailProcess.Id } | Select-Object -First 1
                } else {
                    Wait-RetailProcessForPeer -ProcessId $script:RetailProcess.Id `
                    -Role joiner -RequireInMatch:($ReadinessMode -eq "in_match") `
                    -AllowScenarioDeploy:([bool] $script:ScenarioSpec)
                }
                & $ProbeExe --host 127.0.0.1 --port $Port --timeout-ms 5000 `
                    --interval-ms 250 --expect-name $ServerName
                if ($LASTEXITCODE -ne 0) {
                    throw "OpenNova host stopped answering matching discovery during OR"
                }
                $endOwnership = Assert-OpenNovaHostOwnsPort
                $script:Acceptance = [pscustomobject]@{
                    retail_joiner = $(if ($instance) { $instance.run_state } else { $null })
                    retail_pid = $script:RetailProcess.Id
                    wire_ready = $script:WireReadyWitness
                    opennova_host = [pscustomobject]@{
                        pid = $script:OpenNovaHost.Id
                        mcp_port = $(if ($OpenNovaHostKind -eq "serve") { $null } else { $HostMcpPort })
                        discovery_ready = $true
                        port_ownership = $endOwnership
                        state = Get-OpenNovaHostState
                    }
                }
                Complete-RetailLateJoiner
                Complete-SteadyWindow
                if ($ReadinessMode -eq "in_match") {
                    Save-RetailScreenshot -InstanceId $script:RetailInstanceId
                }
            }
            if ($script:RetailInstanceId) {
                Stop-RetailProcessCapture -InstanceId $script:RetailInstanceId `
                    -RunLabel "$RunId-joiner"
            }
            Stop-ExactProcess -Process $script:RetailProcess
            $script:RetailProcess = $null
            $script:RetailJoinerProcess = $null
            $null = Stop-OpenNovaHostCleanly
            $script:OpenNovaHost = $null
        }
        "OO" {
            Start-OpenNovaHostExact
            Invoke-PreJoinMissionCycle
            Start-OpenNovaJoinerExact
            $joinerAcceptance = Wait-OpenNovaJoinerReady
            $initialJoinerAcceptance = $joinerAcceptance
            $initialJoinerTicks = [long] $joinerAcceptance.ticks_msec
            $script:EvidenceCapture = $script:ExternalCapture
            $script:EvidencePerspective = "external"
            $script:EvidenceRole = "external"
            $null = Wait-ParityWireReady
            Invoke-ScenarioSetupBeforeWindow
            Begin-SteadyWindow
            Invoke-ScenarioIfRequested
            if ($ExerciseInput) {
                Publish-MotionGate
            }
            Wait-UntilSteadyDeadline -ServeOnlyRoundEnd
            $null = Save-ServeOnlyHostFrame
            $script:OpenNovaHost.Refresh()
            $script:OpenNovaJoiner.Refresh()
            if ($script:OpenNovaHost.HasExited -or $script:OpenNovaJoiner.HasExited) {
                throw "An OpenNova endpoint exited before sustained capture completed"
            }
            & $ProbeExe --host 127.0.0.1 --port $Port --timeout-ms 5000 `
                --interval-ms 250 --expect-name $ServerName
            if ($LASTEXITCODE -ne 0) {
                throw "OpenNova host stopped answering matching discovery during OO"
            }
            $endOwnership = Assert-OpenNovaHostOwnsPort
            # A scenario that ends the joiner's session (E4's kick) skips the
            # OpenNova joiner's closing readiness (as RO).
            $joinerAcceptance = if (Test-RotationCell) { Wait-OpenNovaJoinerAfterRotation }
                elseif (Test-ScenarioEndsJoinerSession) { $null } else {
                Wait-OpenNovaJoinerReady `
                -MinimumTicksMsec $initialJoinerTicks -RequireMotionComplete `
                -AllowPostDeathRedeploy:($ReadinessMode -eq 'in_match') `
                -AllowScenarioDeploy:([bool] $script:ScenarioSpec) }
            if (-not (Test-RotationCell) -and -not (Test-ScenarioEndsJoinerSession) -and
                    -not (Test-OpenNovaJoinerReadinessTransition `
                    -Initial $initialJoinerAcceptance -Final $joinerAcceptance `
                    -ReadinessMode $ReadinessMode `
                    -AllowPostDeathRedeploy:($ReadinessMode -eq 'in_match') `
                    -AllowScenarioDeploy:([bool] $script:ScenarioSpec) `
                    -RequireMotionComplete)) {
                throw (Write-JoinerReadinessRefusal -State $joinerAcceptance `
                    -Reason "OpenNova joiner steady-window heartbeat transition is invalid")
            }
            $script:Acceptance = [pscustomobject]@{
                host_pid = $script:OpenNovaHost.Id
                joiner_pid = $script:OpenNovaJoiner.Id
                sustained_seconds = $SteadySeconds
                wire_ready = $script:WireReadyWitness
                opennova_host = [pscustomobject]@{
                    pid = $script:OpenNovaHost.Id
                    mcp_port = $(if ($OpenNovaHostKind -eq "serve") { $null } else { $HostMcpPort })
                    discovery_ready = $true
                    port_ownership = $endOwnership
                    state = Get-OpenNovaHostState
                }
                opennova_joiner_initial = $initialJoinerAcceptance
                opennova_joiner = $joinerAcceptance
            }
            Complete-SteadyWindow
            $stoppedJoiner = $script:OpenNovaJoiner
            $stoppedHost = $script:OpenNovaHost
            $null = Stop-OpenNovaJoinerCooperatively
            $script:OpenNovaJoiner = $null
            $null = Stop-OpenNovaHostCleanly
            $script:OpenNovaHost = $null
            $stoppedJoiner.Refresh()
            $stoppedHost.Refresh()
            if (-not $stoppedJoiner.HasExited -or -not $stoppedHost.HasExited) {
                throw "An OO endpoint remained alive before the capture tail marker"
            }
            Assert-UdpPortUnowned
            Send-ExternalCaptureTailMarker
        }
    }
}
finally {
    try { Stop-DesktopGuard }
    catch { Add-CleanupFailure "Desktop guard stop failed: $($_.Exception.Message)" }
    if ($script:OpenNovaJoiner) {
        try {
            $null = Stop-OpenNovaJoinerCooperatively
            $script:OpenNovaJoiner = $null
        }
        catch {
            $cooperativeError = $_.Exception.Message
            $script:OpenNovaJoinerExactFallbackRequired = $true
            try { Stop-ExactProcess -Process $script:OpenNovaJoiner }
            catch { Add-CleanupFailure "OpenNova joiner exact fallback failed: $($_.Exception.Message)" }
            $script:OpenNovaJoiner = $null
            Add-CleanupFailure "OpenNova joiner cooperative cleanup failed before exact fallback: $cooperativeError"
        }
    }
    if ($script:ServeWrapper) {
        # A cell that failed before its clean stop: opennova-serve's Ctrl+Break first.
        try {
            New-Item -ItemType File -Path (Join-Path $RunRoot "host\serve.stop") -Force | Out-Null
            if (-not $script:ServeWrapper.WaitForExit(30000)) {
                Stop-Process -Id $script:ServeWrapper.Id -Force -ErrorAction SilentlyContinue
                Add-CleanupFailure "serve_host.py did not stop opennova-serve within 30 s"
            }
        }
        catch { Add-CleanupFailure "opennova-serve cleanup failed: $($_.Exception.Message)" }
        $script:ServeWrapper = $null
    }
    if ($script:OpenNovaHost) {
        try {
            $script:OpenNovaHost.Refresh()
            if ($Topology -in @("OR", "OO") -and
                    -not $script:OpenNovaHost.HasExited) {
                $script:OpenNovaHostExactFallbackRequired = $true
            }
            Stop-ExactProcess -Process $script:OpenNovaHost
        }
        catch { Add-CleanupFailure "OpenNova host cleanup failed: $($_.Exception.Message)" }
    }

    if ($script:Mcp -and -not $script:Mcp.HasExited) {
        $cleanupInstances = @($script:RetailInstanceIds.ToArray())
        [array]::Reverse($cleanupInstances)
        foreach ($instanceId in $cleanupInstances) {
            try {
                Stop-RetailProcessCapture -InstanceId $instanceId `
                    -RunLabel "$RunId-cleanup-$instanceId"
            }
            catch { Add-CleanupFailure "Retail capture cleanup failed for ${instanceId}: $($_.Exception.Message)" }
        }
    }
    try { Stop-ExactProcess -Process $script:RetailLateJoinerProcess }
    catch { Add-CleanupFailure "Retail late joiner cleanup failed: $($_.Exception.Message)" }
    try { Stop-ExactProcess -Process $script:RetailJoinerProcess }
    catch { Add-CleanupFailure "Retail joiner cleanup failed: $($_.Exception.Message)" }
    try { Stop-ExactProcess -Process $script:RetailHostProcess }
    catch { Add-CleanupFailure "Retail host cleanup failed: $($_.Exception.Message)" }
    try { Stop-ExactProcess -Process $script:RetailProcess }
    catch { Add-CleanupFailure "Retail process cleanup failed: $($_.Exception.Message)" }

    if ($script:Mcp -and -not $script:Mcp.HasExited) {
        $cleanupRuns = @($script:OwnedRetailRuns.ToArray())
        [array]::Reverse($cleanupRuns)
        foreach ($owned in $cleanupRuns) {
            try { Stop-OwnedRetailRun -OwnedRunId $owned }
            catch { Write-Warning "Cleanup failed for retail run ${owned}: $($_.Exception.Message)" }
        }
    }

    if ($script:CaptureStarted) {
        try {
            $captureStopArguments = @(
                "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", $CaptureScript,
                "-Action", "stop",
                "-Tag", $script:CaptureTag
            )
            if ($script:CaptureTailMarker) {
                $captureStopArguments += @(
                    "-TailMarkerHex", [string] $script:CaptureTailMarker.hex)
            }
            $captureStop = @(& powershell.exe @captureStopArguments)
            if ($LASTEXITCODE -ne 0) { throw "capture.ps1 stop failed: $LASTEXITCODE" }
            $proofLine = $captureStop | Where-Object {
                "$_" -like "CAPTURE_PROOF_JSON=*"
            } | Select-Object -Last 1
            if (-not $proofLine) {
                throw "capture.ps1 stop did not report CAPTURE_PROOF_JSON"
            }
            $validation = "$proofLine".Substring("CAPTURE_PROOF_JSON=".Length) |
                ConvertFrom-Json
            if (-not [bool] $validation.valid -or -not [bool] $validation.exact_eof -or
                    [int] $validation.packet_block_count -le 0) {
                throw "capture.ps1 returned an invalid pcapng completion proof"
            }
            if ($Topology -eq "OO") {
                if (-not $script:CaptureTailMarker) {
                    throw "Successful OO capture has no post-endpoint tail marker"
                }
                if (-not [bool] $validation.tail_marker_required -or
                        -not [bool] $validation.tail_marker_found -or
                        [string] $validation.tail_marker_sha256 -ne
                            [string] $script:CaptureTailMarker.sha256) {
                    throw "OO capture tail marker proof does not match the sent marker"
                }
            }
            if (-not (Test-Path -LiteralPath $script:ExternalCapture) -or
                    (Get-Item -LiteralPath $script:ExternalCapture).Length -le 0) {
                throw "External capture is empty or missing: $($script:ExternalCapture)"
            }
            $captureSource = $script:ExternalCapture
            $sourceHash = (Get-FileHash -LiteralPath $captureSource `
                -Algorithm SHA256).Hash.ToLowerInvariant()
            $runCapture = Join-Path $RunRoot "external.pcapng"
            Copy-Item -LiteralPath $captureSource -Destination $runCapture
            $artifactHash = (Get-FileHash -LiteralPath $runCapture `
                -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($artifactHash -ne $sourceHash) {
                throw "External capture artifact copy does not match validated source"
            }
            $script:ExternalCapture = $runCapture
            $script:ExternalCaptureProof = [pscustomobject]@{
                validated_source = $captureSource
                artifact_path = $runCapture
                artifact_sha256 = $artifactHash
                validation = $validation
                tail_marker = $script:CaptureTailMarker
            }
            if ($Topology -eq "OO") {
                $script:EvidenceCapture = $runCapture
            }
        }
        catch {
            Add-CleanupFailure "External capture cleanup failed: $($_.Exception.Message)"
        }
        $script:CaptureStarted = $false
    }

    if ($script:Mcp -and -not $script:Mcp.HasExited) {
        try { $script:Mcp.StandardInput.Close() } catch {}
        if (-not $script:Mcp.WaitForExit(5000)) {
            Stop-Process -Id $script:Mcp.Id -Force -ErrorAction SilentlyContinue
        }
    }

    if ($script:ServerCopyFiles.Count) {
        try { Restore-ServerCopyFiles }
        catch { Add-CleanupFailure "SERVER copy restore failed: $($_.Exception.Message)" }
    }
}

if ($script:CleanupFailure) {
    throw $script:CleanupFailure
}

$expectsHostShutdownWitness = $Topology -in @("OR", "OO")
if ($expectsHostShutdownWitness) {
    if (-not $script:OpenNovaHostShutdownWitness -or
            $script:OpenNovaHostExactFallbackRequired) {
        throw "Verdict-bearing $Topology run lacks a clean OpenNova host shutdown witness"
    }
}
elseif ($script:OpenNovaHostShutdownWitness -or
        $script:OpenNovaHostExactFallbackRequired) {
    throw "Unexpected OpenNova host shutdown evidence for topology $Topology"
}

$expectsJoinerShutdownWitness = $Topology -in @("RO", "OO")
if ($expectsJoinerShutdownWitness) {
    if (-not $script:OpenNovaJoinerShutdownWitness -or
            $script:OpenNovaJoinerExactFallbackRequired) {
        throw "Verdict-bearing $Topology run lacks a clean cooperative OpenNova joiner shutdown witness"
    }
}
elseif ($script:OpenNovaJoinerShutdownWitness -or
        $script:OpenNovaJoinerExactFallbackRequired) {
    throw "Unexpected OpenNova joiner shutdown evidence for topology $Topology"
}

$expectedLaunchProofRoles = switch ($Topology) {
    "RR" { @() }
    "RO" { @("joiner") }
    "OR" { @("host") }
    "OO" { @("host", "joiner") }
}
# Godot's launch proofs: opennova-serve is no Godot process (opennova_serve_launch witnesses it).
if ($OpenNovaHostKind -eq "serve") {
    if (-not $script:ServeLaunch) { throw "Verdict-bearing $Topology run lacks opennova-serve's launch witness" }
    $expectedLaunchProofRoles = @($expectedLaunchProofRoles | Where-Object { $_ -ne "host" })
}
$actualLaunchProofRoles = @($script:OpenNovaLaunchProofs.Keys |
    ForEach-Object { [string] $_ } | Sort-Object)
if (($actualLaunchProofRoles -join ',') -cne
        (@($expectedLaunchProofRoles | Sort-Object) -join ',')) {
    throw "OpenNova launch-proof role set drift: topology=$Topology expected=$($expectedLaunchProofRoles -join ',') actual=$($actualLaunchProofRoles -join ',')"
}

$summary = [pscustomobject]@{
    topology = $Topology
    mission = $Mission
    mission_path = $ResolvedMissionArtifact
    mission_sha256 = $MissionSha256
    game_type = $GameType
    port = $Port
    resolution = $Resolution
    expansion = $Expansion
    host_session_name = $HostSessionName
    server_name = $ServerName
    host_callsign = $HostCallsign
    joiner_callsign = $JoinerCallsign
    readiness_mode = $ReadinessMode
    steady_seconds = $SteadySeconds
    steady_started_utc = $script:SteadyStartedUtc
    steady_completed_utc = $script:SteadyCompletedUtc
    retail_exe_sha256 = $RetailServerExeSha256
    retail_stock_observer = [bool] $StockObserver
    onhook_mcp_sha256 = (Get-FileHash -LiteralPath $McpExe -Algorithm SHA256).Hash.ToLowerInvariant()
    onhook_proxy_sha256 = $OnHookProxySha256
    # A listen host (a playing host), or a Serve Only host (-ServeOnly): retail's, or
    # ours as opennova.exe or opennova-serve (opennova_host_kind).
    host_kind =$(if ($ServeOnly) { "serve_only" } else { "listen" })
    # The OpenNova host's program (OR, OO): game (opennova.exe) or serve (opennova-serve), with
    # opennova-serve's launch witness (its exe hash, host file and serving line).
    opennova_host_kind = $(if ($Topology -in @("OR", "OO")) { $OpenNovaHostKind } else { $null })
    opennova_serve_launch = $script:ServeLaunch
    # E2: the host's rotation tail (empty: one mission) and the cell's round
    # time limit (0: the copy's own game.cfg value), with the game.cfg write
    # and its restore.
    rotation = @($RotationList)
    time_limit_minutes = $TimeLimitMinutes
    # E5: permanent death and the host player's class (0: the copy's own),
    # with every game.cfg and weapon.sav write and its restore.
    permanent_death = [bool] $PermanentDeath
    host_player_class = $HostPlayerClass
    # D-NET-167: the host's side passwords ("" keeps the copy's).
    host_side_password_a = $HostSidePasswordA
    host_side_password_b = $HostSidePasswordB
    # D-NET-167: the retail joiner's side password and the tool's witness.
    joiner_side_password = $JoinerSidePassword
    joiner_side_password_entered = $script:JoinerSidePasswordEntered
    # The case's deploy zones: onHook deployed the retail roles.
    deploy_zones = [bool] $DeployZones
    server_game_cfg_overrides = @($script:ServerGameCfgOverrides)
    server_player_class_override = $script:ServerPlayerClassOverride
    # -PreJoinMissionCycle: the host's QUERY before and after, the MISSION
    # CYCLE exchange, the host's ticks until its next mission ran ($null when
    # the cell joined the host's first mission).
    pre_join_mission_cycle = $script:PreJoinCycleWitness
    # The SERVER copy's mutable files: each one's start hash (game.cfg's is
    # the case table's pin), whether the cell's processes wrote it, and the
    # restore of its start bytes.
    server_copy_files = @($script:ServerCopyFiles | ForEach-Object {
        [pscustomobject][ordered]@{
            file = $_.file
            sha256 = $_.sha256
            written_during_cell = $_.written_during_cell
            restored = $_.restored
        }
    })
    # The shared desktop at the steady window's start (the cursor clip found
    # and released) and end (clean: the focused retail window could centre).
    desktop_witness = $(if ($script:DesktopWitness.Count) { [pscustomobject] $script:DesktopWitness } else { $null })
    machine_load = $script:MachineLoad
    # The guard's hand-offs of the foreground from a game window to the desktop.
    desktop_guard_handoffs = $(if ($script:DesktopGuardLog -and (Test-Path -LiteralPath $script:DesktopGuardLog)) { @(Get-Content -LiteralPath $script:DesktopGuardLog | Where-Object { $_ -like '* foreground *' }).Count } else { $null })
    # The OpenNova host's working directory (OR, OO): the SERVER copy, as
    # retail's host runs in its install.
    opennova_host_working_dir = $(if ($Topology -in @("OR", "OO")) { $ServerDir } else { $null })
    # E3: the late joiner (null without -LateJoiner): its run, capture and
    # client port (the comparator's session selector), ready and final state.
    late_joiner = $script:LateJoinerWitness
    joiner_dial =$(if ($Topology -in @("RO", "OO")) { $JoinerDial } else { $null })
    # L2-s22's load probe: our joiner ran under --verbose (-JoinerVerbose).
    joiner_verbose = [bool] ($JoinerVerbose -and $Topology -in @("RO", "OO"))
    repo_commit = [string] $RepoCommit
    repo_dirty = $RepoDirty
    allow_dirty_repo = [bool] $AllowDirtyRepo
    gdextension_sha256 = Get-FileSha256OrNull $GdextensionDll
    gdextension_editor_sha256 = Get-FileSha256OrNull $GdextensionEditorDll
    godot_launcher_sha256 = Get-FileSha256OrNull $GodotLauncher
    run_id = $RunId
    suite_id = $SuiteId
    corpus_fingerprint = $CorpusFingerprint
    wire_only = [bool] $WireOnly
    exercise_input = [bool] $ExerciseInput
    run_root = $RunRoot
    external_capture = $script:ExternalCapture
    external_capture_proof = $script:ExternalCaptureProof
    evidence_capture = $script:EvidenceCapture
    evidence_perspective = $script:EvidencePerspective
    evidence_role = $script:EvidenceRole
    evidence_instance_id = $script:EvidenceInstanceId
    screenshot = $script:Screenshot
    # A Serve Only cell's host frames: at the steady deadline, and on the round's end
    # (the window's first S2C 0x1D plus 1 s: its capture frame and time, the frame's path).
    serve_only_host_screenshot = $script:ServeOnlyHostScreenshot
    serve_only_round_end = $script:ServeOnlyRoundEnd
    acceptance = $script:Acceptance
    opennova_launch_proofs = [pscustomobject] $script:OpenNovaLaunchProofs
    direct_runtime_launch = [bool] $AllowDirectRuntime
    opennova_host_mcp_port = $(if ($Topology -in @("OR", "OO") -and $OpenNovaHostKind -ne "serve") { $HostMcpPort } else { $null })
    opennova_joiner_mcp_port = $(if ($Topology -in @("RO", "OO")) { $JoinerMcpPort } else { $null })
    joiner_ready_run_id = $script:JoinerReadyRunId
    joiner_motion_run_id = $script:JoinerMotionRunId
    opennova_joiner_shutdown_witness = $script:OpenNovaJoinerShutdownWitness
    opennova_joiner_exact_fallback_required = [bool] $script:OpenNovaJoinerExactFallbackRequired
    stop_evidence = $script:StopEvidence
    retail_input_witness = $script:InputWitness
    motion_gate_witness = $script:MotionGateWitness
    scenario_witness = $script:ScenarioWitness
    wire_ready_witness = $script:WireReadyWitness
    effective_retail_configs = @($script:EffectiveRetailConfigs.ToArray())
}
if ($expectsHostShutdownWitness) {
    $summary | Add-Member -NotePropertyName opennova_host_shutdown_witness `
        -NotePropertyValue $script:OpenNovaHostShutdownWitness
    $summary | Add-Member -NotePropertyName opennova_host_exact_fallback_required `
        -NotePropertyValue ([bool] $script:OpenNovaHostExactFallbackRequired)
}
$summaryJson = $summary | ConvertTo-Json -Depth 30
$summaryJson | Set-Content -LiteralPath (Join-Path $RunRoot "run-summary.json") -Encoding UTF8
Write-Host ("PARITY_SUMMARY_JSON=" + ($summary | ConvertTo-Json -Depth 30 -Compress))
Write-Host "PARITY_RUN_COMPLETE topology=$Topology mission=$Mission run=$RunId"
if ($script:RunnerTranscript) { Stop-Transcript | Out-Null }
