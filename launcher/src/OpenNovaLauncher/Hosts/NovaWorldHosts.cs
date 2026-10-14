namespace OpenNova.Launcher.Hosts;

/// <summary>
/// The hostnames a stock NovaLogic client looks up to reach NovaWorld: the one
/// set the launcher maps. Witnessed in docs/net/novaworld-net-re.md (NW-L2, NW-L1).
/// </summary>
public static class NovaWorldHosts
{
    /// <summary>
    /// <c>gs.novaworld.net</c>, the gate host, UDP 7597. Retail Joint Operations
    /// (JO:CA's Jointops.exe) fills its gate config with it [orig:
    /// CNapiGateManager_InitDefaults @ 0x4d1460, +8; the string @ 0x7cc368], and
    /// dfx2.exe's twin sets the same host [orig: @0x4d1680 (dfx2.exe)]. The gate's
    /// reply names the service by address (POSTIPADDRESS, UDPNOVAWORLD and the
    /// STARTUPURL the login, HOSTKEY and NWJoin pages hang off), so nothing after
    /// the gate needs a name. Not here: <c>novaworld.net</c>, the base gate
    /// default InitDefaults overwrites before any lookup [orig:
    /// CNapiGateManager_Init @ 0x633f90, +64]; <c>www.novaworld2.com</c>, the patch
    /// URL outside NovaWorld's flow; <c>assets.novaworld.net</c>, the icon host the
    /// menu templates name, which the OpenNova service does not serve.
    /// </summary>
    public const string GateHost = "gs.novaworld.net";

    public static IReadOnlyList<string> Names { get; } = new[] { GateHost };
}
