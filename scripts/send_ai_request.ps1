param(
    [string]$PromptFile = "${PWD}\.vscode\last_crash_ai_request.txt",
    [string]$OutFile   = "${PWD}\.vscode\crash_ai_response.txt",
    [string]$ApiKeyEnv = 'OPENAI_API_KEY',
    [string]$Model    = '',
    [string]$SystemPrompt = 'You are an expert C++ developer and code reviewer. Focus on minimal, safe changes.'
)

function Get-EnvSetting {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    foreach ($scope in @('Process', 'User', 'Machine')) {
        $value = [Environment]::GetEnvironmentVariable($Name, $scope)
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            return $value
        }
    }

    return $null
}

Write-Host "Sending AI request from: $PromptFile" -ForegroundColor Cyan
if (-not (Test-Path $PromptFile)) { Write-Error "Prompt file not found: $PromptFile"; exit 2 }

$apiKey = Get-EnvSetting -Name $ApiKeyEnv
if (-not $apiKey) { Write-Warning "No API key found in environment variable $ApiKeyEnv. Set it before running (will not send)."; exit 1 }

$promptText = Get-Content -Raw -Path $PromptFile -ErrorAction Stop

$resolvedModel = $Model
if ([string]::IsNullOrWhiteSpace($resolvedModel)) {
    $resolvedModel = Get-EnvSetting -Name 'OPENAI_MODEL'
}
if ([string]::IsNullOrWhiteSpace($resolvedModel)) {
    $resolvedModel = 'gpt-5.4'
}

Write-Host "Using model: $resolvedModel" -ForegroundColor Gray

$body = @{
    model = $resolvedModel
    messages = @(
        @{ role = 'system'; content = $SystemPrompt },
        @{ role = 'user'; content = $promptText }
    )
}

$json = $body | ConvertTo-Json -Depth 20

$headers = @{ Authorization = "Bearer $apiKey" }

try {
    Write-Host "Sending request to OpenAI... (this will use network)" -ForegroundColor Yellow
    $resp = Invoke-RestMethod -Uri 'https://api.openai.com/v1/chat/completions' -Method Post -Headers $headers -Body $json -ContentType 'application/json; charset=utf-8'
    # Save the whole response for debugging
    $resp | ConvertTo-Json -Depth 50 | Out-File -FilePath $OutFile -Encoding utf8

    # Extract assistant text answers if present
    if ($resp.choices -and $resp.choices.Count -gt 0) {
        $content = $resp.choices[0].message.content
        if ($content) {
            $content | Out-File -FilePath $OutFile -Encoding utf8
            Write-Host "Saved AI response to: $OutFile" -ForegroundColor Green
        } else { Write-Warning "No text content in response choices." }
    } else { Write-Warning 'Empty choices in API response' }
} catch {
    Write-Error "API request failed: $($_.Exception.Message)"
    exit 1
}

exit 0
