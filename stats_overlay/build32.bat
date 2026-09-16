@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat" || exit /b 1
cd /d "%~dp0"

if not exist obj32 mkdir obj32

cl /nologo /O2 /W3 /MT /EHs- /GS- /I ..\minhook\include ^
  /Foobj32\ /Fdobj32\stats_overlay_32bit.pdb ^
  stats_overlay.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ^
  ..\minhook\src\trampoline.c ..\minhook\src\hde\hde32.c ^
  /link /DLL /MACHINE:X86 /OUT:stats_overlay_32bit.dll /PDB:stats_overlay_32bit.pdb ^
  user32.lib gdi32.lib d3d9.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built stats_overlay_32bit.dll
endlocal
