using module .\cfxBuildContext.psm1
using module .\cfxBuildTools.psm1

function Invoke-RunMSBuild {
    param(
        [CfxBuildContext] $Context,
        [CfxBuildTools] $Tools
    )

    # Private tooling setup
    $sogBefore = "C:\f\shadesofgray_before.ps1"
    $sogAfter = "C:\f\shadesofgray_after.ps1"
    $useSog = $false

    if (!$Context.IsDryRun -and $Context.IsPublicBuild) {
        $someToolIsMissing = ($sogBefore, "C:\f\shadesofgray.cmd", $sogAfter | Test-Path) -contains $false
        if ($someToolIsMissing) {
            throw "Private tooling not found or incomplete, but is required for release build"
        }

        $env:SOG_DISCRIMINATOR = [System.Guid]::NewGuid().ToString()
        
        $useSog = $true
    }

    # setup errors log file
    $errorsLogPath = $Context.getPathInBuildCache("MSBuild.Errors.log")

    if (Test-Path $errorsLogPath) {
        Remove-Item -Force -ErrorAction Ignore $errorsLogPath
    }

    # set the env vars for MSBuild
    $env:UseMultiToolTask = "true"
    $env:TargetPlatformVersion = "10.0.15063.0"
    $env:EnforceProcessCountAcrossBuilds = "true"

    if (Test-Path Env:\platform) {
        Remove-Item Env:\platform
    }

    if ($useSog) {
        Start-Process powershell -ArgumentList "-ExecutionPolicy unrestricted $sogBefore" -NoNewWindow -Wait
    }

    # CFX_MSBUILD_PROJECTS (linux-native fork): only these projects and what they depend on,
    # instead of the whole solution (a component build, for quick iterations)
    $inputs = @($Context.MSBuildSolution)
    $projectArgs = @()
    if ($env:CFX_MSBUILD_PROJECTS) {
        $solutionDir = Split-Path -Parent $Context.MSBuildSolution
        # CitiCore first: it generates the IDL headers (fxcore.h...) other projects include without
        # referencing it (the whole solution builds it early)
        $names = @('CitiCore') + @($env:CFX_MSBUILD_PROJECTS -split '[,; ]+' | Where-Object { $_ -and $_ -ne 'CitiCore' })
        $inputs = @($names | ForEach-Object { Join-Path $solutionDir "$_.vcxproj" })
        $projectArgs = @("-p:Platform=x64", "-p:SolutionDir=$solutionDir\")
        Write-Output "Building only: $($inputs -join ', ')"
    }

    try {
      foreach ($input in $inputs) {
        msbuild @(
            $input
            $projectArgs
            "-m"        # Use all CPU cores
            "-v:q"      # Quiet verbosity level
            "-nr:False" # Don't keep MSBuild nodes alive
            "-r"        # Run restore before build
            "-nologo"   # Disable MSBuild banner output
            # In addition to the default console logger, use file logger to store all errors during the build
            "-fl1"
            "-flp1:errorsonly;logfile={0}" -f $errorsLogPath
            "-p:Configuration=release"
            "-p:RestorePackagesConfig=true"
            "-p:PreferredToolArchitecture=x64"
        )

        Test-LastExitCode "MSBuild failed"
      }
    }
    catch {
        $msbuildException = $_

        if (Test-Path $errorsLogPath) {
            $esc = $([char]27)

            $red = "$esc[31m"
            $reset = "$esc[0m"

            Write-Host "${red}MSBuild Errors:"
            Write-Host (Get-Content -Raw $errorsLogPath)
            Write-Host $reset
        }

        # rethrow to break the invoker flow
        throw $msbuildException
    }
    finally {
        if ($useSog) {
            Start-Process powershell -ArgumentList "-ExecutionPolicy unrestricted $sogAfter" -NoNewWindow -Wait
        }
    }
}
