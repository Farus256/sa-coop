@echo off
chcp 65001 >nul
rem SA:COOP: installs the ASI loader when the game has none. Run it from the game folder.
cd /d "%~dp0"
rem the game's own files, not its exe: some builds name it otherwise (gta-sa.exe), issue #54
if not exist models\gta3.img (
    echo Здесь нет файлов игры. Распакуйте архив SA-COOP прямо в папку игры и запустите install.bat оттуда.
    pause
    exit /b 1
)
if not exist coop.asi (
    echo Не хватает coop.asi. Распакуйте архив SA-COOP в папку игры целиком.
    pause
    exit /b 1
)
if exist vorbisHooked.dll (
    echo Загрузчик ASI уже установлен. Ничего менять не нужно.
    echo Можно запускать игру.
    pause
    exit /b 0
)
if not exist vorbisFile.dll (
    echo В папке игры нет vorbisFile.dll. Похоже, это не GTA San Andreas 1.0.
    pause
    exit /b 1
)
ren vorbisFile.dll vorbisHooked.dll
copy /y "coop\loader\vorbisFile.dll" vorbisFile.dll >nul
if errorlevel 1 (
    ren vorbisHooked.dll vorbisFile.dll
    echo Не получилось скопировать загрузчик. Если игра лежит в Program Files, запустите install.bat от имени администратора.
    pause
    exit /b 1
)
echo Загрузчик ASI установлен. Оригинальный vorbisFile.dll сохранён как vorbisHooked.dll.
echo Можно запускать игру.
pause
