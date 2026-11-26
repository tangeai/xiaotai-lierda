param([Parameter(Mandatory = $true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$ackOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($ackOutput) | Out-Null
$ackVoices = @(
    @{ Key = 'female'; Name = 'Microsoft Huihui Desktop'; Gender = 'Female' },
    @{ Key = 'male'; Name = 'Microsoft Kangkang'; Gender = 'Male' }
)
$ackFormat = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
    16000,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono
)
$ackReport = @()
foreach ($ackVoice in $ackVoices) {
    $ackSynth = New-Object System.Speech.Synthesis.SpeechSynthesizer
    try {
        $ackInstalled = $ackSynth.GetInstalledVoices() | Where-Object {
            $_.Enabled -and $_.VoiceInfo.Name -eq $ackVoice.Name -and
            $_.VoiceInfo.Culture.Name -eq 'zh-CN' -and
            [string]$_.VoiceInfo.Gender -eq $ackVoice.Gender
        }
        if (@($ackInstalled).Count -ne 1) { throw "Required voice unavailable: $($ackVoice.Name)" }
        $ackSynth.SelectVoice($ackVoice.Name)
        $ackSynth.Rate = 0
        $ackSynth.Volume = 100
        $ackWavePath = Join-Path $ackOutput ($ackVoice.Key + '.wav')
        # File output is selected before Speak; no computer speaker is used.
        $ackSynth.SetOutputToWaveFile($ackWavePath, $ackFormat)
        $ackSynth.Speak('我在')
        $ackSynth.SetOutputToNull()
        $ackReport += @{
            key = $ackVoice.Key; name = $ackSynth.Voice.Name
            culture = $ackSynth.Voice.Culture.Name; gender = [string]$ackSynth.Voice.Gender
            engine_id = $ackSynth.Voice.Id; rate = 0; volume = 100
        }
    } finally { $ackSynth.Dispose() }
}
$ackJson = $ackReport | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText((Join-Path $ackOutput 'voices.json'), $ackJson,
    (New-Object System.Text.UTF8Encoding($false)))
