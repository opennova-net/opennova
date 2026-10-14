namespace OpenNova.Launcher.Hosts;

/// <summary>
/// The addresses the launcher writes into the hosts file. <see cref="System.Net.IPAddress.TryParse(string, out System.Net.IPAddress)"/>
/// also takes "1", "0x7f.1" and IPv6; a hosts line wants the plain dotted quad.
/// </summary>
public static class Ipv4
{
    /// <summary>
    /// A dotted quad: four decimal parts of 0..255, no sign, no leading zero past a
    /// lone 0, nothing around it.
    /// </summary>
    public static bool TryParse(string? text, out byte[] octets)
    {
        octets = Array.Empty<byte>();
        if (string.IsNullOrEmpty(text))
        {
            return false;
        }

        var parts = text.Split('.');
        if (parts.Length != 4)
        {
            return false;
        }

        var parsed = new byte[4];
        for (var i = 0; i < 4; i++)
        {
            var part = parts[i];
            if (part.Length is 0 or > 3 || !part.All(char.IsAsciiDigit) || (part.Length > 1 && part[0] == '0'))
            {
                return false;
            }

            var value = int.Parse(part, System.Globalization.CultureInfo.InvariantCulture);
            if (value > 255)
            {
                return false;
            }

            parsed[i] = (byte)value;
        }

        octets = parsed;
        return true;
    }

    /// <summary>
    /// A dotted quad a hosts line can send a game to: not in 0.0.0.0/8 (the
    /// "this network" block hosts files use to black-hole a name), multicast
    /// (224.0.0.0/4) or reserved and broadcast (240.0.0.0/4). Loopback and the
    /// private ranges are fine: a NovaWorld service on this machine or the LAN.
    /// </summary>
    public static bool IsUsable(string? text)
    {
        return TryParse(text, out var octets) && octets[0] != 0 && octets[0] < 224;
    }
}
