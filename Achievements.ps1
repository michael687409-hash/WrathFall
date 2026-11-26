function Get-AchievementView([string] $ProjectRoot) {
    $metadata = Join-Path $ProjectRoot 'out\achievements'
    $catalog = Get-Content -LiteralPath (Join-Path $metadata 'achievements.toml') -Raw -Encoding UTF8
    $profile = Join-Path $ProjectRoot 'userdata'
    if (!(Test-Path -LiteralPath $profile -PathType Container)) {
        $profile = Join-Path $ProjectRoot 'out\user'
    }
    $progress = Join-Path $profile 'achievements\43430817.toml'
    $unlocked = @{}
    if (Test-Path -LiteralPath $progress -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $progress) {
            if ($line -match '^\[unlocked\.(\d+)\]\s*$') { $unlocked[[int] $Matches[1]] = $true }
        }
    }

    $known = @{}
    $records = $catalog -split '(?m)^\[\[achievements\]\]\s*$', 0
    $rows = @(foreach ($record in $records | Select-Object -Skip 1) {
        $fields = @{}
        foreach ($line in $record -split '\r?\n') {
            if ($line -match '^(id|label|description|unachieved_description|gamerscore|image_id)\s*=\s*(.+?)\s*$') {
                $fields[$Matches[1]] = $Matches[2]
            }
        }
        if ($fields.Count -ne 6) { throw 'The achievement catalog has a missing field.' }
        $id = [int] $fields.id
        $known[$id] = $true
        $earned = $unlocked.ContainsKey($id)
        $label = (ConvertFrom-Json ('{"text":' + $fields.label + '}')).text
        $descriptionKey = if ($earned) { 'description' } else { 'unachieved_description' }
        $description = (ConvertFrom-Json ('{"text":' + $fields[$descriptionKey] + '}')).text
        $icon = Join-Path $metadata ("icons\{0}.png" -f [int] $fields.image_id)
        if (!$earned -and (!$description -or $description -eq '<String Placeholder>')) {
            $label = 'Secret achievement'
            $description = 'Details appear when unlocked.'
            $icon = $null
        } elseif (!(Test-Path -LiteralPath $icon -PathType Leaf)) {
            $icon = $null
        }
        [pscustomobject]@{
            Id = $id
            Title = $label
            Description = $description
            Points = [int] $fields.gamerscore
            Score = "$($fields.gamerscore) G"
            Unlocked = $earned
            State = if ($earned) { 'Unlocked' } else { 'Locked' }
            Icon = $icon
        }
    })
    if (!$rows) { throw 'The imported title does not contain achievement metadata.' }
    $earned = @($rows | Where-Object Unlocked)
    $totalPoints = ($rows | Measure-Object Points -Sum).Sum
    $earnedPoints = ($earned | Measure-Object Points -Sum).Sum
    $summary = '{0} of {1} unlocked   ·   {2} of {3} G' -f $earned.Count, $rows.Count, [int] $earnedPoints, $totalPoints
    foreach ($id in $unlocked.Keys) {
        if ($known.ContainsKey($id)) { continue }
        $rows += [pscustomobject]@{
            Id = $id
            Title = "Achievement $id"
            Description = 'Details are unavailable in the imported title metadata.'
            Score = ''
            Unlocked = $true
            State = 'Unlocked'
            Icon = $null
        }
    }
    [pscustomobject]@{
        Summary = $summary
        Rows = @($rows | Sort-Object @{Expression='Unlocked'; Descending=$true}, Id)
    }
}

function New-AchievementsWindow([string] $ProjectRoot, [Windows.Window] $Owner) {
    $view = Get-AchievementView $ProjectRoot
    $xaml = [xml](Get-Content -LiteralPath (Join-Path $ProjectRoot 'Achievements.xaml') -Raw -Encoding UTF8)
    $dialog = [Windows.Markup.XamlReader]::Load([Xml.XmlNodeReader]::new($xaml))
    if ($Owner) { $dialog.Owner = $Owner }
    $dialog.DataContext = $view
    $dialog
}
