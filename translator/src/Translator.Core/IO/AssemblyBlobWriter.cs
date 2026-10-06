using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using Translator.Core.Loading;

namespace Translator.Core.IO;

internal sealed record AssemblyBlob(string FileName, string Symbol, ReadOnlyMemory<byte> Data, string Comment);

internal static class AssemblyBlobWriter
{
    public static void Write(
        string assemblyPath,
        string blobDirectory,
        string blobReferenceDirectory,
        IReadOnlyList<AssemblyBlob> blobs,
        params string[] headerLines) =>
        Write(TranslationTarget.ObjectFormat, assemblyPath, blobDirectory, blobReferenceDirectory, blobs, headerLines);

    public static void Write(
        NativeObjectFormat format,
        string assemblyPath,
        string blobDirectory,
        string blobReferenceDirectory,
        IReadOnlyList<AssemblyBlob> blobs,
        params string[] headerLines)
    {
        Directory.CreateDirectory(blobDirectory);
        var expectedFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var assembly = new StringBuilder();
        foreach (var header in headerLines) assembly.AppendLine(header);
        // PE/COFF (Windows), Mach-O (macOS), and ELF (Linux) spell a read-only data section
        // differently in GNU-as syntax. Native builds target whichever platform the translator
        // itself runs on (see TranslationTarget). WebAssembly objects need one named section per
        // data symbol instead, emitted with the blob below.
        if (format != NativeObjectFormat.WebAssembly)
        {
            assembly.AppendLine(format switch
            {
                NativeObjectFormat.Coff => ".section .rdata,\"dr\"",
                NativeObjectFormat.MachO => ".section __TEXT,__const",
                _ => ".section .rodata,\"a\",@progbits"
            });
            assembly.AppendLine();
        }

        foreach (var blob in blobs)
        {
            var blobPath = Path.Combine(blobDirectory, blob.FileName);
            expectedFiles.Add(Path.GetFullPath(blobPath));
            FileOutput.WriteBytesIfChanged(blobPath, blob.Data.Span);
            var hash = ChecksumUtilities.Sha256Hex(blob.Data.Span);
            assembly.AppendLine($"// {blob.Comment}; sha256={hash}");
            if (format == NativeObjectFormat.WebAssembly)
                assembly.AppendLine($".section .rodata.{blob.Symbol},\"\",@");
            assembly.AppendLine(".p2align 4");
            // C/C++ external symbols carry a leading underscore in Mach-O,
            // unlike ELF, COFF and WebAssembly. The generated C++ still names the
            // symbol without that ABI decoration, so emit the target's spelling here.
            var assemblySymbol = format == NativeObjectFormat.MachO ? $"_{blob.Symbol}" : blob.Symbol;
            assembly.AppendLine($".globl {assemblySymbol}");
            // A WebAssembly data symbol must be typed and sized to land in a data segment.
            if (format == NativeObjectFormat.WebAssembly)
                assembly.AppendLine($".type {assemblySymbol},@object");
            assembly.AppendLine($"{assemblySymbol}:");
            var referencePath = Path.Combine(blobReferenceDirectory, blob.FileName);
            assembly.AppendLine($".incbin \"{SanitizeAssemblyPath(referencePath)}\"");
            if (format == NativeObjectFormat.WebAssembly)
                assembly.AppendLine($".size {assemblySymbol}, {blob.Data.Length}");
            assembly.AppendLine();
        }

        foreach (var stalePath in Directory.EnumerateFiles(blobDirectory, "*.bin"))
        {
            if (!expectedFiles.Contains(Path.GetFullPath(stalePath))) File.Delete(stalePath);
        }
        if (format == NativeObjectFormat.Elf)
        {
            // Absence of a .note.GNU-stack section makes the linker assume the oldest, most
            // conservative default for this object (an executable stack) and warn about it; this
            // file has no code needing one, so mark it explicitly like every other GNU-as ELF
            // object linked into the binary already does (the norm on modern toolchains, just not
            // producible without an explicit section since this file is hand-assembled, not
            // compiler-emitted).
            assembly.AppendLine(".section .note.GNU-stack,\"\",@progbits");
        }
        FileOutput.WriteTextIfChanged(assemblyPath, assembly.ToString());
    }

    private static string SanitizeAssemblyPath(string path) => Path.GetFullPath(path).Replace('\\', '/');
}
