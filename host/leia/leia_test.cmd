@echo off
rem Leia SR test run. Pick "Leia SR" as the 3D mode in the game's Graphics settings (it is
rem remembered), then play. Everything the game logs goes to leia-log.txt next to this file:
rem whether conker_leia_sr.dll loaded, whether the SR runtime and display were found, the
rem first weave, and a [perf] line every two seconds. Send leia-log.txt back with any report.
rem
rem   leia_test.cmd            normal run
rem   leia_test.cmd noweave    present the plain side-by-side pair instead of weaving, to
rem                            tell a weave problem from anything else
cd /d "%~dp0"
set CONKER_PERF_LOG=1
if /i "%~1"=="noweave" set CONKER_DISABLE_LEIA_WEAVE=1
echo Logging to %~dp0leia-log.txt
ConkerRecomp.exe 2> leia-log.txt
