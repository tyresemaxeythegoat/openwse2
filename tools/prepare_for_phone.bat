@echo off
rem Double-click to prepare the game for your phone on this PC (see prepare_for_phone.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0prepare_for_phone.ps1" %*
