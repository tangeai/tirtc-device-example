param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\media")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Speech

$encoderSource = @'
using System;
using System.Collections.Generic;
using System.IO;

public static class G711PromptEncoder
{
    private static readonly int[] SegmentEnds = {
        0x00ff, 0x01ff, 0x03ff, 0x07ff,
        0x0fff, 0x1fff, 0x3fff, 0x7fff
    };

    private static byte LinearToALaw(short sample)
    {
        int value = sample >> 3;
        int mask;
        if (value >= 0) {
            mask = 0xd5;
        } else {
            mask = 0x55;
            value = -value - 1;
        }

        int segment = 0;
        while (segment < SegmentEnds.Length &&
               value > SegmentEnds[segment]) {
            segment++;
        }
        if (segment >= SegmentEnds.Length) {
            return (byte)(0x7f ^ mask);
        }

        int encoded = segment << 4;
        encoded |= segment < 2
            ? (value >> 1) & 0x0f
            : (value >> segment) & 0x0f;
        return (byte)(encoded ^ mask);
    }

    public static void ConvertWave(string wavePath, string outputPath)
    {
        byte[] pcm = ReadWavePcm(wavePath);
        const int leadingSilenceSamples = 1600;
        const int trailingSilenceSamples = 4000;
        var output = new List<byte>(
            leadingSilenceSamples + pcm.Length / 2 +
            trailingSilenceSamples + 160);
        byte silence = LinearToALaw(0);
        for (int i = 0; i < leadingSilenceSamples; ++i) {
            output.Add(silence);
        }
        for (int offset = 0; offset < pcm.Length; offset += 2) {
            output.Add(LinearToALaw(
                BitConverter.ToInt16(pcm, offset)));
        }
        for (int i = 0; i < trailingSilenceSamples; ++i) {
            output.Add(silence);
        }
        while ((output.Count % 160) != 0) {
            output.Add(silence);
        }
        File.WriteAllBytes(outputPath, output.ToArray());
    }

    private static byte[] ReadWavePcm(string path)
    {
        using (var stream = File.OpenRead(path))
        using (var reader = new BinaryReader(stream)) {
            if (new string(reader.ReadChars(4)) != "RIFF") {
                throw new InvalidDataException("missing RIFF header");
            }
            reader.ReadUInt32();
            if (new string(reader.ReadChars(4)) != "WAVE") {
                throw new InvalidDataException("missing WAVE header");
            }

            bool formatSeen = false;
            while (stream.Position + 8 <= stream.Length) {
                string id = new string(reader.ReadChars(4));
                uint size = reader.ReadUInt32();
                long next = stream.Position + size + (size & 1);
                if (next > stream.Length) {
                    throw new InvalidDataException("invalid WAVE chunk");
                }
                if (id == "fmt ") {
                    ushort format = reader.ReadUInt16();
                    ushort channels = reader.ReadUInt16();
                    uint sampleRate = reader.ReadUInt32();
                    reader.ReadUInt32();
                    reader.ReadUInt16();
                    ushort bits = reader.ReadUInt16();
                    if (format != 1 || channels != 1 ||
                        sampleRate != 8000 || bits != 16) {
                        throw new InvalidDataException(
                            "expected PCM 8 kHz 16-bit mono");
                    }
                    formatSeen = true;
                } else if (id == "data") {
                    if (!formatSeen || size == 0 || (size & 1) != 0) {
                        throw new InvalidDataException("invalid PCM data");
                    }
                    return reader.ReadBytes((int)size);
                }
                stream.Position = next;
            }
        }
        throw new InvalidDataException("WAVE data chunk not found");
    }
}
'@
Add-Type -TypeDefinition $encoderSource -Language CSharp

$prompts = [ordered]@{
    "ai_story.g711a" = [regex]::Unescape(
        "\u8bb2\u4e2a\u767e\u5b57\u5185\u7684\u6545\u4e8b\u3002"
    )
    "ai_joke.g711a" = [regex]::Unescape(
        "\u8bb2\u4e2a\u7b80\u77ed\u7b11\u8bdd\u3002"
    )
    "ai_weather.g711a" = [regex]::Unescape(
        "\u4eca\u5929\u5929\u6c14\u600e\u4e48\u6837\uff1f"
    )
    "ai_call_xiaozhang.g711a" = [regex]::Unescape(
        "\u547c\u53eb\u5c0f\u5f20\u3002"
    )
    "ai_call_xiaoli.g711a" = [regex]::Unescape(
        "\u547c\u53eb\u5c0f\u674e\u3002"
    )
}

$output = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($output) | Out-Null
$temporary = Join-Path ([System.IO.Path]::GetTempPath()) (
    "tirtc-ai-prompts-" + [Guid]::NewGuid().ToString("N")
)
[System.IO.Directory]::CreateDirectory($temporary) | Out-Null

try {
    foreach ($entry in $prompts.GetEnumerator()) {
        $wavePath = Join-Path $temporary ($entry.Key + ".wav")
        $targetPath = Join-Path $output $entry.Key
        $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
            8000,
            [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
            [System.Speech.AudioFormat.AudioChannel]::Mono
        )
        $synthesizer =
            New-Object System.Speech.Synthesis.SpeechSynthesizer
        try {
            $synthesizer.SelectVoice("Microsoft Huihui Desktop")
            $synthesizer.Rate = 0
            $synthesizer.Volume = 100
            $synthesizer.SetOutputToWaveFile($wavePath, $format)
            $synthesizer.Speak([string]$entry.Value)
        } finally {
            $synthesizer.Dispose()
        }
        [G711PromptEncoder]::ConvertWave($wavePath, $targetPath)
        $item = Get-Item -LiteralPath $targetPath
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $targetPath).Hash
        Write-Output (
            "{0}`t{1}`t{2}`t{3}" -f
            $item.Name, $item.Length, $hash.ToLowerInvariant(), $entry.Value
        )
    }
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
