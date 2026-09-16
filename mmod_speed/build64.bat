@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"

if not exist obj64 mkdir obj64

cl /nologo /O2 /W3 /MT /EHs- /GS- /I ..\minhook\include ^
  /Foobj64\ /Fdobj64\mmod_speed_64bit.pdb ^
  mmod_speed.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ^
  ..\minhook\src\trampoline.c ..\minhook\src\hde\hde64.c ^
  /link /DLL /MACHINE:X64 /OUT:mmod_speed_64bit.dll /PDB:mmod_speed_64bit.pdb ^
  user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built mmod_speed_64bit.dll
endlocal
