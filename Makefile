CC      = gcc
WCC     = x86_64-w64-mingw32-gcc

# ── Flags comunes ──────────────────────────────────────────────────────────────
CFLAGS  = -I./include -O2 -Wall -Wextra

# ── Linux: detectar XInput2 en tiempo de compilación ─────────────────────────
#   Si pkg-config encuentra xi, compilamos con captura global X11+XI2.
#   Si no existe (servidor sin X11, Wayland puro, container), cae a evdev solo.
XI2_AVAILABLE := $(shell pkg-config --exists xi 2>/dev/null && echo yes || echo no)

ifeq ($(XI2_AVAILABLE),yes)
    CFLAGS  += -DHAVE_X11_XINPUT2 $(shell pkg-config --cflags xi x11)
    XI2_LIBS = $(shell pkg-config --libs xi x11)
else
    XI2_LIBS =
endif

# ── Libs Linux ────────────────────────────────────────────────────────────────
LDFLAGS = ./lib/libraylib.a -static-libgcc -static-libstdc++ \
          -lX11 -lXi -lGL -lm -lpthread -ldl -lrt \
          $(XI2_LIBS)

# ── Libs Windows (cross-compile desde Linux) ──────────────────────────────────
WINLDFLAGS = -L./lib_mingw-w64 -lraylib -lopengl32 -lgdi32 -lwinmm -static
WIN_CFLAGS = -I./include -O2 -Wall -Wextra

SRC        = mouse_visual.c
TARGET     = mouse_app
WIN_TARGET = mouse_app.exe

# ── Targets ──────────────────────────────────────────────────────────────────
.PHONY: all windows clean setcap help

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LDFLAGS)
	@echo ""
	@echo "  Compilado: $(TARGET)"
	@echo "  Backend X11/XInput2: $(XI2_AVAILABLE)"
	@echo ""
	@echo "  Ejecutar (opción A — setcap, recomendado):"
	@echo "    sudo setcap cap_dac_read_search+eip ./$(TARGET)"
	@echo "    ./$(TARGET)"
	@echo ""
	@echo "  Ejecutar (opción B — sudo + llvmpipe):"
	@echo "    sudo MESA_LOADER_DRIVER_OVERRIDE=llvmpipe ./$(TARGET)"

windows: $(SRC)
	$(WCC) $(SRC) -o $(WIN_TARGET) $(WIN_CFLAGS) $(WINLDFLAGS)
	@echo "  Compilado: $(WIN_TARGET)"

# Aplica setcap para evitar sudo al leer /dev/input
setcap: $(TARGET)
	sudo setcap cap_dac_read_search+eip ./$(TARGET)
	@echo "  setcap aplicado. Ahora puedes ejecutar: ./$(TARGET)"

clean:
	rm -f $(TARGET) $(WIN_TARGET) *.o

help:
	@echo "Targets disponibles:"
	@echo "  make          — compilar para Linux"
	@echo "  make windows  — cross-compilar para Windows"
	@echo "  make setcap   — compilar + aplicar setcap (evita sudo)"
	@echo "  make clean    — limpiar binarios"
