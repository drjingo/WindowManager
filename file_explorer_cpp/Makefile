# make           (in the MSYS2 UCRT64 / MINGW64 shell)
# make CROSS=x86_64-w64-mingw32-   (cross-compile from Linux)
CROSS ?=
CXX = $(CROSS)g++
WINDRES = $(CROSS)windres
CXXFLAGS = -std=c++17 -O2 -municode -Wall -Wno-unused-parameter -Wno-missing-field-initializers
LIBS = -lshlwapi -lshell32 -lole32 -luuid -lgdiplus -lmsimg32 -ldwmapi -luxtheme -lcomctl32 -loleaut32 -lcomdlg32 -lgdi32
SRC = $(wildcard src/*.cpp)
OBJ = $(patsubst src/%.cpp,build/%.o,$(SRC))

FileExplorer.exe: $(OBJ) build/app_res.o
	$(CXX) -municode -mwindows -static -s $^ -o $@ $(LIBS)

build/%.o: src/%.cpp $(wildcard src/*.h) | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/app_res.o: res/app.rc res/app.manifest res/file_explorer.ico | build
	$(WINDRES) $< -O coff -o $@

build:
	mkdir -p build

clean:
	rm -rf build FileExplorer.exe
