@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"

if not exist obj64 mkdir obj64

cl /nologo /O2 /W3 /MT /EHs- /GS- /I ..\minhook\include ^
  /Foobj64\ /Fdobj64\menu_fps_64bit.pdb ^
  menu_fps.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ^
  ..\minhook\src\trampoline.c ..\minhook\src\hde\hde64.c ^
  /link /DLL /MACHINE:X64 /OUT:menu_fps_64bit.dll /PDB:menu_fps_64bit.pdb ^
  user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built menu_fps_64bit.dll
endlocal
