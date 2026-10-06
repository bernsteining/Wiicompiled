using System;

namespace Translator.Core.IO;

/// <summary>
/// Object-file format the generated assembly (binary data blobs) is written for. Generated C++ is
/// target-neutral; only hand-written assembly has to spell sections and symbols per format.
/// </summary>
public enum NativeObjectFormat
{
    Coff,
    MachO,
    Elf,
    WebAssembly
}

/// <summary>
/// The compilation target for this process. Native builds target the OS the translator runs on
/// (there is no native cross-compilation), so that is the default; the CLI's <c>--target web</c>
/// selects WebAssembly, which is produced on a native host.
/// </summary>
public static class TranslationTarget
{
    public static NativeObjectFormat HostObjectFormat { get; } =
        OperatingSystem.IsWindows() ? NativeObjectFormat.Coff
        : OperatingSystem.IsMacOS() ? NativeObjectFormat.MachO
        : NativeObjectFormat.Elf;

    /// <summary>Set once from the command line, before any output is written.</summary>
    public static NativeObjectFormat ObjectFormat { get; set; } = HostObjectFormat;
}
