#!/bin/zsh
# Build win32swiftui (PE + unix) and the gallery test, and install them into a
# wine tree laid out like the MNC engine (a wine build tree):
#   <tree>/dlls/win32swiftui/x86_64-windows/win32swiftui.dll
#   <tree>/dlls/win32swiftui/i386-windows/win32swiftui.dll   (32-bit apps, WoW64)
#   <tree>/dlls/win32swiftui/win32swiftui.so                 (serves both)
#
#   ./build.sh [install-tree]    default: $WINE_BUILD
# WINE_SRC and WINE_BUILD point at the wine source and build trees (for the
# map-table check and libwinecrt0.a).
set -e
cd "${0:A:h}"
WINE_SRC=${WINE_SRC:?set WINE_SRC to the MNC wine source tree (branch w2s)}
WINE_BUILD=${WINE_BUILD:?set WINE_BUILD to the x86_64 build directory of that tree}
DEST=${1:-$WINE_BUILD}
OUT=build

echo "=== map -> runtime tables"
python3 tools/gen_runtime_tables.py "$WINE_SRC" | head -1

echo "=== win32swiftui.dll (x86_64 PE)"
mkdir -p $OUT/x86_64-windows
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-unused-parameter -shared -D_WIN32_WINNT=0x0a00 -Iruntime/include \
  -o $OUT/x86_64-windows/win32swiftui.dll \
  runtime/pe/main.c runtime/pe/controls.c runtime/pe/json.c runtime/pe/dialogs.c runtime/pe/taskdialog.c runtime/pe/menus.c \
  runtime/pe/pickers.c runtime/pe/look.c \
  runtime/pe/win32swiftui.def \
  -Wl,--file-alignment=4096 -static-libgcc \
  "$WINE_BUILD/libs/winecrt0/x86_64-windows/libwinecrt0.a" "$WINE_BUILD/dlls/ntdll/x86_64-windows/libntdll.a" \
  -luser32 -lgdi32 -lcomctl32 -luxtheme -lwinspool -lkernel32
"$WINE_BUILD/tools/winebuild/winebuild" --builtin $OUT/x86_64-windows/win32swiftui.dll

# 32-bit apps (installers) run under WoW64: an i386 PE over the same unix
# side. Needs i686-w64-mingw32-gcc and a WoW64 wine build (i386-windows dirs).
I386=0
if [[ -f "$WINE_BUILD/libs/winecrt0/i386-windows/libwinecrt0.a" ]] && whence i686-w64-mingw32-gcc >/dev/null; then
  I386=1
  echo "=== win32swiftui.dll (i386 PE)"
  mkdir -p $OUT/i386-windows
  # stdcall exports keep their plain names (wine's hooks use GetProcAddress)
  i686-w64-mingw32-gcc -O2 -Wall -Wno-unused-parameter -shared -D_WIN32_WINNT=0x0a00 -Iruntime/include \
    -o $OUT/i386-windows/win32swiftui.dll \
    runtime/pe/main.c runtime/pe/controls.c runtime/pe/json.c runtime/pe/dialogs.c runtime/pe/taskdialog.c runtime/pe/menus.c \
    runtime/pe/pickers.c runtime/pe/look.c \
    runtime/pe/win32swiftui.def \
    -Wl,--file-alignment=4096 -Wl,--enable-stdcall-fixup -static-libgcc \
    "$WINE_BUILD/libs/winecrt0/i386-windows/libwinecrt0.a" "$WINE_BUILD/dlls/ntdll/i386-windows/libntdll.a" \
    -luser32 -lgdi32 -lcomctl32 -luxtheme -lwinspool -lkernel32
  "$WINE_BUILD/tools/winebuild/winebuild" --builtin $OUT/i386-windows/win32swiftui.dll
else
  echo "=== win32swiftui.dll (i386 PE): SKIPPED, no i686-w64-mingw32-gcc or no WoW64 wine build"
fi

echo "=== win32swiftui.so (Swift + C, x86_64, macOS 12)"
(cd runtime && swift build -c release --arch x86_64 2>&1 | grep -E "error|warning: .*\.swift|Build complete")
cp runtime/.build/out/Products/Release/libwin32swiftui.dylib $OUT/win32swiftui.so
install_name_tool -id @rpath/win32swiftui.so $OUT/win32swiftui.so
codesign -f -s - $OUT/win32swiftui.so 2>/dev/null

echo "=== gallery.exe"
x86_64-w64-mingw32-gcc -O2 -Wall -municode -mwindows -o $OUT/gallery.exe tests/gallery/gallery.c \
  -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -lwinspool -lgdi32 -luser32
if (( I386 )); then
  echo "=== gallery32.exe (the same self-test from a 32-bit app)"
  i686-w64-mingw32-gcc -O2 -Wall -municode -mwindows -o $OUT/gallery32.exe tests/gallery/gallery.c \
    -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -lwinspool -lgdi32 -luser32
fi

echo "=== install into $DEST"
mkdir -p "$DEST/dlls/win32swiftui/x86_64-windows"
cp $OUT/x86_64-windows/win32swiftui.dll "$DEST/dlls/win32swiftui/x86_64-windows/"
if (( I386 )); then
  mkdir -p "$DEST/dlls/win32swiftui/i386-windows"
  cp $OUT/i386-windows/win32swiftui.dll "$DEST/dlls/win32swiftui/i386-windows/"
fi
cp $OUT/win32swiftui.so "$DEST/dlls/win32swiftui/"
ls -la "$DEST/dlls/win32swiftui" "$DEST/dlls/win32swiftui"/*-windows
