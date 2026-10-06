using Translator.Core.IO;

namespace Translator.Tests;

public sealed class AssemblyBlobWriterTests
{
    // The format is passed explicitly: TranslationTarget.ObjectFormat is process state, and test
    // classes run in parallel.
    private static string WriteBlobs(NativeObjectFormat format, string root)
    {
        var assemblyPath = Path.Combine(root, "blobs.S");
        var blobDirectory = Path.Combine(root, "blobs");
        AssemblyBlobWriter.Write(
            format,
            assemblyPath,
            blobDirectory,
            blobDirectory,
            new[]
            {
                new AssemblyBlob("first.bin", "kFirst", new byte[] { 1, 2, 3 }, "first"),
                new AssemblyBlob("second.bin", "kSecond", new byte[17], "second"),
            },
            "// header");
        return File.ReadAllText(assemblyPath);
    }

    [Fact]
    public void WebAssemblyGivesEachBlobATypedSizedSection()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkw-blob-wasm-" + Guid.NewGuid().ToString("N"));
        try
        {
            var assembly = WriteBlobs(NativeObjectFormat.WebAssembly, root);
            Assert.Contains(".section .rodata.kFirst,\"\",@", assembly, StringComparison.Ordinal);
            Assert.Contains(".section .rodata.kSecond,\"\",@", assembly, StringComparison.Ordinal);
            Assert.Contains(".type kFirst,@object", assembly, StringComparison.Ordinal);
            Assert.Contains(".size kFirst, 3", assembly, StringComparison.Ordinal);
            Assert.Contains(".size kSecond, 17", assembly, StringComparison.Ordinal);
            Assert.Contains(".globl kFirst", assembly, StringComparison.Ordinal);
            Assert.DoesNotContain(".globl _kFirst", assembly, StringComparison.Ordinal);
            Assert.DoesNotContain("@progbits", assembly, StringComparison.Ordinal);
            Assert.DoesNotContain(".rdata", assembly, StringComparison.Ordinal);
            Assert.DoesNotContain("__TEXT", assembly, StringComparison.Ordinal);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Theory]
    [InlineData(NativeObjectFormat.Elf, ".section .rodata,\"a\",@progbits", "kFirst")]
    [InlineData(NativeObjectFormat.Coff, ".section .rdata,\"dr\"", "kFirst")]
    [InlineData(NativeObjectFormat.MachO, ".section __TEXT,__const", "_kFirst")]
    public void NativeFormatsKeepOneSharedReadOnlySection(NativeObjectFormat format, string section, string symbol)
    {
        var root = Path.Combine(Path.GetTempPath(), "mkw-blob-native-" + Guid.NewGuid().ToString("N"));
        try
        {
            var assembly = WriteBlobs(format, root);
            Assert.Contains(section, assembly, StringComparison.Ordinal);
            Assert.Contains($".globl {symbol}", assembly, StringComparison.Ordinal);
            Assert.DoesNotContain(".type ", assembly, StringComparison.Ordinal);
            Assert.DoesNotContain(".size ", assembly, StringComparison.Ordinal);
            Assert.Equal(format == NativeObjectFormat.Elf, assembly.Contains(".note.GNU-stack", StringComparison.Ordinal));
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }
}
