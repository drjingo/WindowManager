@echo off
REM Builds Window Manager into "dist":
REM   dist\WindowManager.exe       - the app (no console window), with its icon
REM   dist\window-manager-cli.exe  - command-line tool, e.g.  window-manager-cli decrypt
setlocal
cd /d "%~dp0"

python -m pip install --upgrade pyinstaller cryptography || goto :error
python window_manager.py icon window_manager.ico || goto :error
python -m PyInstaller --noconfirm --onefile --windowed --icon window_manager.ico --name WindowManager window_manager.py || goto :error
python -m PyInstaller --noconfirm --onefile --console --icon window_manager.ico --name window-manager-cli window_manager.py || goto :error

echo.
echo Done:  dist\WindowManager.exe and dist\window-manager-cli.exe
echo Copy your data folder and settings.ini next to WindowManager.exe to keep your notes and shortcuts
echo (or point [data] data_folder in settings.ini to your data folder).
if /i not "%~1"=="nopause" pause
exit /b 0

:error
echo.
echo Window Manager build failed - see the messages above.
if /i not "%~1"=="nopause" pause
exit /b 1
