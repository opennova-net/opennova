namespace OpenNova.Switcher.Hosts;

/// <summary>One hostname to IPv4 line in the switcher's managed block.</summary>
public readonly record struct HostsRedirect(string Hostname, string IPv4);
