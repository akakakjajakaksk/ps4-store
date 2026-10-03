TARGET := orbis-store
TITLE := Orbis Store
TITLE_ID := BREW00001
CONTENT_ID := IV0000-BREW00001_00-ORBISSTORE000001
APP_VERSION := 01.00

SOURCES := source/main.cpp
OBJS := $(SOURCES:.cpp=.o)

CXX := clang++
CXXFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -I$(OO_PS4_TOOLCHAIN)/include
LDFLAGS := -fuse-ld=lld -nostdlib -m elf_x86_64 -pie --script $(OO_PS4_TOOLCHAIN)/link.x
LIBS := -L$(OO_PS4_TOOLCHAIN)/lib -lc -lkernel -lScePad -lSDL2

all: eboot.bin

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

eboot.bin: $(TARGET).elf
	$(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$(TARGET).elf -out=eboot.bin

clean:
	rm -f $(OBJS) $(TARGET).elf eboot.bin

.PHONY: all clean
