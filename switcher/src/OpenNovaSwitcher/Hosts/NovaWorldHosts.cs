namespace OpenNova.Switcher.Hosts;

/// <summary>
/// The hostnames a stock NovaLogic client looks up to reach NovaWorld and its
/// updater: the one set the switcher maps, all to one address. Witnessed in
/// docs/net/novaworld-net-re.md (NW-L2, NW-L1) and docs/update/updater-re.md (section 3).
/// </summary>
public static class NovaWorldHosts
{
    /// <summary>
    /// <c>gs.novaworld.net</c>, the gate host, UDP 7597. Retail Joint Operations
    /// (JO:CA's Jointops.exe) fills its gate config with it [orig:
    /// CNapiGateManager_InitDefaults @ 0x4d1460, +8; the string @ 0x7cc368], and
    /// dfx2.exe's twin sets the same host [orig: @0x4d1680 (dfx2.exe)]. JO:CA's image
    /// names two other NovaWorld hosts, left alone here: <c>novaworld.net</c>, the base
    /// gate default InitDefaults overwrites before any lookup [orig:
    /// CNapiGateManager_Init @ 0x633f90, +64], and <c>www.novaworld2.com</c>, the patch
    /// URL outside NovaWorld's flow. The gate's reply names the service by address
    /// (POSTIPADDRESS, UDPNOVAWORLD and the STARTUPURL the login, HOSTKEY and NWJoin
    /// pages hang off), so nothing after the gate needs a name; a retail run through the
    /// hosts file is still to be observed (ADR 0054 d2). Also left alone:
    /// <c>assets.novaworld.net</c>, the icon host the menu templates name, which the
    /// OpenNova service does not serve, so the icons come from NovaLogic in every mode.
    /// </summary>
    public const string GateHost = "gs.novaworld.net";

    /// <summary>
    /// The updater's gateways: retail UPDATE.EXE dials <c>ftp.novaworld.com</c>,
    /// <c>ftp2.novaworld.com</c> and <c>ftp4.novaworld.com</c> on its fixed TCP ports
    /// (20044, 20556, 21068, 21580), hardcoded in its state machine [orig:
    /// Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE)]; docs/update/updater-re.md,
    /// section 3. OpenNova's NovaWorld serves the updater at the gate's address (ADR 0061
    /// d1; ADR 0054 as amended by ADR 0064: the update listeners sit behind the same
    /// load balancer as the gate), so the block maps the three gateways to it beside
    /// the gate host, and a data set served the retail way (its SERVER.LST naming
    /// <c>ftp.novaworld.com</c>) updates from OpenNova while the block is on.
    /// </summary>
    public static readonly IReadOnlyList<string> UpdateGateways = new[]
    {
        "ftp.novaworld.com",
        "ftp2.novaworld.com",
        "ftp4.novaworld.com",
    };

    /// <summary>
    /// Every name the managed block maps, in the order the block lists them: the gate
    /// host, then the updater's gateways. One address for all four.
    /// </summary>
    public static IReadOnlyList<string> Names { get; } = new[] { GateHost }.Concat(UpdateGateways).ToArray();
}
