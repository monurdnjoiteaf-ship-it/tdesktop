using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

if (args.Length == 0)
{
    Console.WriteLine("Usage: KeyGen init <private.pem> <public.pem> | issue <private.pem> <machine-id> <days|permanent>");
    return;
}

if (args[0] == "init" && args.Length == 3)
{
    using var rsa = RSA.Create(3072);
    File.WriteAllText(args[1], rsa.ExportPkcs8PrivateKeyPem());
    File.WriteAllText(args[2], rsa.ExportSubjectPublicKeyInfoPem());
    Console.WriteLine("Keys created. KEEP PRIVATE KEY SECRET; never commit it to GitHub.");
    return;
}

if (args[0] == "issue" && args.Length == 4)
{
    var machine = args[2].Trim();
    if (machine.Length < 8 || machine.Length > 256)
        throw new ArgumentException("Invalid machine ID.");
    DateTimeOffset? expires = null;
    if (!args[3].Equals("permanent", StringComparison.OrdinalIgnoreCase))
    {
        if (!int.TryParse(args[3], out var days) || !new[] { 1, 7, 30, 365 }.Contains(days))
            throw new ArgumentException("Days must be 1, 7, 30, 365 or permanent.");
        expires = DateTimeOffset.UtcNow.AddDays(days);
    }
    using var rsa = RSA.Create();
    rsa.ImportFromPem(File.ReadAllText(args[1]));
    var payload = JsonSerializer.SerializeToUtf8Bytes(new
    {
        version = 1,
        machine,
        issuedUtc = DateTimeOffset.UtcNow.ToString("O"),
        expiresUtc = expires?.ToString("O")
    });
    var signature = rsa.SignData(payload, HashAlgorithmName.SHA256, RSASignaturePadding.Pss);
    Console.WriteLine(Convert.ToBase64String(payload) + "." + Convert.ToBase64String(signature));
    return;
}
throw new ArgumentException("Unknown command or invalid arguments.");
