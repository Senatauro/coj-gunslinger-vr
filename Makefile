# Compila o mod (build/d3d9.dll, proxy de 32 bits feito com MinGW) e o instalador do Windows (build/cojvr-setup.exe).
CXX = i686-w64-mingw32-g++
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wextra -fno-exceptions -fno-rtti -Ithird_party/openvr
LDFLAGS = -shared -static -Wl,--enable-stdcall-fixup -lgdi32 -lwinhttp -lshell32
SRC = $(wildcard src/*.cpp)

build/d3d9.dll: $(SRC) $(wildcard src/*.h) src/d3d9.def
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) src/d3d9.def $(LDFLAGS)

build/cojvr-setup.exe: dist/install-windows.cpp
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static -o $@ $<

# make dist: monta o zip da release em build/ (mod, instaladores, DLLs do DXVK e do OpenVR, licenças) e o .sha256 dele.
# DXVK_DLL e OPENVR_DLL usam os mesmos caminhos padrão do tools/install.sh.
# A versão vem do mod.h; o build do GitHub passa VERSION=0.1.N e ela entra na DLL (o aviso de versão nova compara
# com a tag da release).
VERSION ?= $(shell sed -n 's/.*define COJVR_VERSION "\(.*\)"/\1/p' src/mod.h)
CXXFLAGS += -DCOJVR_VERSION=\"$(VERSION)\"
DXVK_DLL ?= $(HOME)/Projetos/Mods/_decomp/downloads/dxvk_3.1.1/dxvk-3.1.1/x32/d3d9.dll
OPENVR_DLL ?= $(HOME)/Projetos/Mods/_decomp/downloads/openvr_2.15.6/openvr-2.15.6/bin/win32/openvr_api.dll
dist: build/d3d9.dll build/cojvr-setup.exe
	rm -rf build/dist && mkdir -p build/dist/cojvr/steamvr build/dist/licenses
	cp build/d3d9.dll build/cojvr-setup.exe dist/install-linux.sh dist/README.md LICENSE build/dist/
	cp $(DXVK_DLL) build/dist/cojvr/dxvk_d3d9.dll
	cp $(OPENVR_DLL) build/dist/cojvr/openvr_api.dll
	cp dist/cojvr.ini cojvr/vr_texts.txt build/dist/cojvr/
	cp cojvr/steamvr/*.json build/dist/cojvr/steamvr/
	cp third_party/dxvk/LICENSE build/dist/licenses/DXVK-zlib.txt
	cp third_party/openvr/LICENSE build/dist/licenses/OpenVR-BSD-3.txt
	cd build/dist && rm -f ../cojvr-$(VERSION).zip && zip -qr ../cojvr-$(VERSION).zip .
	cd build && sha256sum cojvr-$(VERSION).zip > cojvr-$(VERSION).zip.sha256 && cat cojvr-$(VERSION).zip.sha256

install: build/d3d9.dll
	tools/install.sh

uninstall:
	tools/install.sh uninstall

.PHONY: install uninstall dist
