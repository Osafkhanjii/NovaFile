# ============================================
# Nova File - Windows build (C / GCC)
# C only, external lib: lib/stb_sprintf.h
# ============================================

# GCC comes with Dev-Cpp and is not on this machine's PATH.
CC = "C:/Program Files (x86)/Dev-Cpp/MinGW64/bin/gcc.exe"

SRC_DIR = src
INC_DIR = include
BUILD_DIR = build

SOURCES = \
    $(SRC_DIR)/core/memory.c \
    $(SRC_DIR)/core/indexer.c \
    $(SRC_DIR)/platform/window_win.c \
    $(SRC_DIR)/renderer/renderer_gl.c \
    $(SRC_DIR)/ui/ui_framework.c \
    $(SRC_DIR)/app/update.c \
    $(SRC_DIR)/app/main.c

OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# App icon (water-wave circle) embedded via windres; the window class
# loads resource id 1 (src/platform/window_win.c). windres runs the C
# preprocessor itself and chokes on the spaces in "Program Files (x86)",
# so the toolchain dir goes on PATH (export reaches recipes regardless of
# which shell make picks; ";" matches the Windows PATH separator).
WINDRES = "C:/Program Files (x86)/Dev-Cpp/MinGW64/bin/windres.exe"
export PATH := C:\Program Files (x86)\Dev-Cpp\MinGW64\bin;$(PATH)
RES_OBJECT = $(BUILD_DIR)/resource_res.o

INCLUDES = -I$(INC_DIR) -Ilib
CFLAGS = -std=gnu11 -Wall -O2 $(INCLUDES)

# ole32/oleaut32: thumbnail decoding (IPicture)
# shell32: ShellExecuteA, shell folders, shell thumbnails
# user32/gdi32/kernel32: window, input, drawing, files
# msimg32: AlphaBlend (shell thumbnails with transparency)
# comdlg32: GetOpenFileName custom thumbnail picker (was missing here)
# winhttp/bcrypt/crypt32/wintrust: auto-update (HTTPS fetch, SHA-256,
#            Authenticode check) - all four ship with Windows
# -mwindows: GUI subsystem (no console window; also required because the
#            machine's Smart App Control blocks the console build)
# -Map: symbol map - a crash dialog shows the offset, this file names the
#       function (see nova_on_crash in src/app/main.c)
LDFLAGS = -mwindows -Wl,-Map=build/NovaFile.map -lole32 -loleaut32 -lshell32 -luser32 -lkernel32 -lgdi32 -lmsimg32 -lcomdlg32 -lwinhttp -lbcrypt -lcrypt32 -lwintrust
EXECUTABLE = NovaFile.exe

ifdef DEBUG
    CFLAGS += -g -DDEBUG
endif

.PHONY: all run clean rebuild debug

all: $(BUILD_DIR) $(EXECUTABLE)
	@echo "=== Build OK: $(EXECUTABLE) ==="

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(RES_OBJECT): resource.rc NovaFile.ico
	$(WINDRES) --preprocessor=cpp -i resource.rc -O coff -o $@

$(EXECUTABLE): $(OBJECTS) $(RES_OBJECT)
	$(CC) $(CFLAGS) $(OBJECTS) $(RES_OBJECT) -o $(BUILD_DIR)/$(EXECUTABLE) $(LDFLAGS)

run: all
	./$(BUILD_DIR)/$(EXECUTABLE)

debug:
	$(MAKE) DEBUG=1

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean all
