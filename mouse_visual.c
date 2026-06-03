/* ============================================================
 *  mouse_visual.c  —  Mouse Pixel Art con captura global
 *
 *  Linux  : X11 + XInput2  (botones y rueda globales)
 *           Fallback evdev  (/dev/input/eventN) si no hay X11
 *  Windows: WinAPI GetAsyncKeyState + GetMouseState
 *
 *  Compilar (Linux → Linux):
 *    make
 *  Compilar (Linux → Windows):
 *    make windows
 *
 *  Ejecutar sin sudo (Linux):
 *    sudo setcap cap_dac_read_search+eip ./mouse_app
 *    ./mouse_app
 *  O con llvmpipe:
 *    sudo MESA_LOADER_DRIVER_OVERRIDE=llvmpipe ./mouse_app
 * ============================================================ */

/* ── Constantes de la aplicación ─────────────────────────── */
#define BODY_HEIGHT 300
#define BODY_WIDTH 200
#define BUTTON_HEIGHT 100
#define BUTTON_WIDTH 85
#define CABLE_HEIGHT 100
#define CABLE_WIDTH 10
#define ERROR_TEXT "ERROR: No se pudo abrir mouse global"
#define HELP_TEXT "Clic L / R / M  |  Mueve la Rueda"
#define MOUSE_APP_TITLE "Mouse Pixel Art"
#define OUTLINE_WIDTH 6
#define SCREEN_HEIGHT 600
#define SCREEN_WIDTH 500
#define SCROLL_AMPLIFY 15.0f
#define SCROLL_CENTER_DECAY 1.5f
#define SCROLL_MAX 25.0f
#define SCROLL_MIN -25.0f
#define SCROLL_NEAR_ZERO 0.1f
#define TEXT_BOTTOM_SIZE 18
#define TEXT_TOP_SIZE 20
#define WHEEL_HEIGHT 60
#define WHEEL_WIDTH 30
#define WHEEL_Y_OFFSET 20

/* ── Estado del mouse global ─────────────────────────────── */
typedef struct {
  int left; /* 1 = presionado */
  int right;
  int middle;
  float wheel;   /* acumulador de rueda */
  int available; /* 1 = backend activo */
} GlobalMouseState;

#include "include/raylib.h"

/* ================================================================
 *  BACKEND WINDOWS  (WinAPI)
 * ================================================================ */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define Rectangle WIN_Rectangle
#define CloseWindow WIN_CloseWindow
#define ShowCursor WIN_ShowCursor
#define DrawText WIN_DrawText
#include <windows.h>
#undef DrawText
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#undef NOGDI

static GlobalMouseState gms = {0};

void global_mouse_init(void) { gms.available = 1; }
void global_mouse_close(void) {}

void global_mouse_poll(GlobalMouseState *out) {
  out->left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
  out->right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
  out->middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 1 : 0;
  out->wheel = gms.wheel;
  out->available = 1;
  gms.wheel = 0.0f; /* consumir tras leer */
}

/* Hook de rueda (opcional: sin hook, la rueda solo funciona en foco) */
static HHOOK _wheel_hook = NULL;

static LRESULT CALLBACK _wheel_proc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode >= 0 && wParam == WM_MOUSEWHEEL) {
    MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;
    short delta = (short)HIWORD(ms->mouseData);
    gms.wheel += (float)delta / WHEEL_DELTA;
  }
  return CallNextHookEx(_wheel_hook, nCode, wParam, lParam);
}

void global_mouse_hook_install(void) {
  _wheel_hook = SetWindowsHookEx(WH_MOUSE_LL, _wheel_proc, NULL, 0);
}

void global_mouse_hook_pump(void) {
  MSG msg;
  while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

/* ================================================================
 *  BACKEND LINUX
 * ================================================================ */
#elif defined(__linux__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Intento X11 + XInput2 ─────────────────────────────────── */
#ifdef HAVE_X11_XINPUT2

#define Font X11_Font
#include <X11/Xlib.h>
#undef Font
#include <X11/extensions/XInput2.h>

static Display *_dpy = NULL;
static int _xi_opcode = 0;
static pthread_t _xi_thread;
static pthread_mutex_t _xi_lock = PTHREAD_MUTEX_INITIALIZER;

static GlobalMouseState _xi_state = {0};

static void *_xi_loop(void *arg) {
  (void)arg;
  XIEventMask mask;
  unsigned char bits[4] = {0};
  mask.deviceid = XIAllMasterDevices;
  mask.mask_len = sizeof(bits);
  mask.mask = bits;
  XISetMask(bits, XI_RawButtonPress);
  XISetMask(bits, XI_RawButtonRelease);
  XISetMask(bits, XI_RawMotion);

  Window root = DefaultRootWindow(_dpy);
  XISelectEvents(_dpy, root, &mask, 1);
  XFlush(_dpy);

  while (1) {
    XEvent ev;
    XNextEvent(_dpy, &ev);
    if (ev.xcookie.type != GenericEvent)
      continue;
    if (!XGetEventData(_dpy, &ev.xcookie))
      continue;
    if (ev.xcookie.extension != _xi_opcode) {
      XFreeEventData(_dpy, &ev.xcookie);
      continue;
    }

    XIRawEvent *re = (XIRawEvent *)ev.xcookie.data;
    pthread_mutex_lock(&_xi_lock);

    if (ev.xcookie.evtype == XI_RawButtonPress ||
        ev.xcookie.evtype == XI_RawButtonRelease) {
      int pressed = (ev.xcookie.evtype == XI_RawButtonPress);
      switch (re->detail) {
      case 1:
        _xi_state.left = pressed;
        break;
      case 2:
        _xi_state.middle = pressed;
        break;
      case 3:
        _xi_state.right = pressed;
        break;
      case 4:
        if (pressed)
          _xi_state.wheel += 1.0f;
        break;
      case 5:
        if (pressed)
          _xi_state.wheel -= 1.0f;
        break;
      }
    }

    pthread_mutex_unlock(&_xi_lock);
    XFreeEventData(_dpy, &ev.xcookie);
  }
  return NULL;
}

static int _xi_init(void) {
  _dpy = XOpenDisplay(NULL);
  if (!_dpy)
    return 0;

  int event, error;
  if (!XQueryExtension(_dpy, "XInputExtension", &_xi_opcode, &event, &error)) {
    XCloseDisplay(_dpy);
    _dpy = NULL;
    return 0;
  }
  int maj = 2, min = 0;
  if (XIQueryVersion(_dpy, &maj, &min) != Success) {
    XCloseDisplay(_dpy);
    _dpy = NULL;
    return 0;
  }

  pthread_create(&_xi_thread, NULL, _xi_loop, NULL);
  return 1;
}

static void _xi_poll(GlobalMouseState *out) {
  Window root = DefaultRootWindow(_dpy);
  Window root_return, child_return;
  int root_x, root_y, win_x, win_y;
  unsigned int mask = 0;

  if (XQueryPointer(_dpy, root, &root_return, &child_return, &root_x, &root_y,
                    &win_x, &win_y, &mask)) {
    out->left = (mask & Button1Mask) ? 1 : 0;
    out->middle = (mask & Button2Mask) ? 1 : 0;
    out->right = (mask & Button3Mask) ? 1 : 0;
  } else {
    out->left = 0;
    out->middle = 0;
    out->right = 0;
  }

  pthread_mutex_lock(&_xi_lock);
  out->wheel = _xi_state.wheel;
  _xi_state.wheel = 0.0f;
  pthread_mutex_unlock(&_xi_lock);
  out->available = 1;
}

static void _xi_close(void) {
  if (_dpy) {
    XCloseDisplay(_dpy);
    _dpy = NULL;
  }
}

#endif /* HAVE_X11_XINPUT2 */

/* ── Fallback evdev (/dev/input) ───────────────────────────── */
#include <linux/input.h>
#include <sys/inotify.h>

#define MAX_EVDEV 8

typedef struct {
  int fd;
  char path[512];
} EvdevDev;

static EvdevDev _evdev[MAX_EVDEV];
static int _evdev_count = 0;
static pthread_t _evdev_thread;
static pthread_mutex_t _evdev_lock = PTHREAD_MUTEX_INITIALIZER;
static GlobalMouseState _evdev_state = {0};
static volatile int _evdev_running = 0;

static int _evdev_is_mouse(int fd) {
  unsigned long evbits = 0;
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0)
    return 0;
  /* Debe tener EV_KEY y EV_REL */
  return (evbits & (1 << EV_KEY)) && (evbits & (1 << EV_REL));
}

static void _evdev_open_all(void) {
  _evdev_count = 0;
  DIR *dir = opendir("/dev/input");
  if (!dir)
    return;
  struct dirent *de;
  while ((de = readdir(dir)) && _evdev_count < MAX_EVDEV) {
    if (strncmp(de->d_name, "event", 5) != 0)
      continue;
    char path[512];
    snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;
    if (_evdev_is_mouse(fd)) {
      _evdev[_evdev_count].fd = fd;
      snprintf(_evdev[_evdev_count].path, sizeof(_evdev[_evdev_count].path), "%s", path);
      _evdev_count++;
    } else {
      close(fd);
    }
  }
  closedir(dir);
}

static void *_evdev_loop(void *arg) {
  (void)arg;
  struct input_event ev;

  /* El hilo solo acumula la rueda. Los botones se leen con EVIOCGKEY
     en _evdev_poll() para evitar estados "pegados" cuando se pierde
     un evento EV_KEY release (p.ej. focus-change, evento descartado). */
  while (_evdev_running) {
    for (int i = 0; i < _evdev_count; i++) {
      ssize_t n = read(_evdev[i].fd, &ev, sizeof(ev));
      if (n != sizeof(ev))
        continue;
      if (ev.type == EV_REL && ev.code == REL_WHEEL) {
        pthread_mutex_lock(&_evdev_lock);
        _evdev_state.wheel += (float)ev.value;
        pthread_mutex_unlock(&_evdev_lock);
      }
    }
    usleep(1000); /* ~1 kHz polling */
  }
  return NULL;
}

static int _evdev_init(void) {
  _evdev_open_all();
  if (_evdev_count == 0)
    return 0;
  _evdev_running = 1;
  pthread_create(&_evdev_thread, NULL, _evdev_loop, NULL);
  return 1;
}

/* Máscara de bits para EVIOCGKEY: cubre hasta KEY_MAX */
#define EVKEY_WORDS                                                            \
  ((KEY_MAX + 1 + (8 * (int)sizeof(unsigned long) - 1)) /                      \
   (8 * (int)sizeof(unsigned long)))

static void _evdev_poll(GlobalMouseState *out) {
  /* Leer botones con EVIOCGKEY — fuente de verdad del kernel,
     nunca se "pega" aunque se pierda un evento de release. */
  int any_left = 0, any_right = 0, any_middle = 0;
  unsigned long keybits[EVKEY_WORDS];

  for (int i = 0; i < _evdev_count; i++) {
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(_evdev[i].fd, EVIOCGKEY(sizeof(keybits)), keybits) < 0)
      continue;
#define BIT_TEST(arr, bit)                                                     \
  (((arr)[(bit) / (8 * (int)sizeof(unsigned long))]) >>                        \
       ((bit) % (8 * (int)sizeof(unsigned long))) &                            \
   1UL)
    if (BIT_TEST(keybits, BTN_LEFT))
      any_left = 1;
    if (BIT_TEST(keybits, BTN_RIGHT))
      any_right = 1;
    if (BIT_TEST(keybits, BTN_MIDDLE))
      any_middle = 1;
#undef BIT_TEST
  }

  pthread_mutex_lock(&_evdev_lock);
  out->left = any_left;
  out->right = any_right;
  out->middle = any_middle;
  out->wheel = _evdev_state.wheel;
  _evdev_state.wheel = 0.0f;
  pthread_mutex_unlock(&_evdev_lock);

  out->available = 1;
}

static void _evdev_close(void) {
  _evdev_running = 0;
  pthread_join(_evdev_thread, NULL);
  for (int i = 0; i < _evdev_count; i++)
    close(_evdev[i].fd);
  _evdev_count = 0;
}

/* ── API pública Linux ─────────────────────────────────────── */
typedef enum { BACKEND_NONE, BACKEND_XI2, BACKEND_EVDEV } LinuxBackend;
static LinuxBackend _active_backend = BACKEND_NONE;

void global_mouse_init(void) {
#ifdef HAVE_X11_XINPUT2
  if (_xi_init()) {
    _active_backend = BACKEND_XI2;
    return;
  }
#endif
  if (_evdev_init()) {
    _active_backend = BACKEND_EVDEV;
    return;
  }
  _active_backend = BACKEND_NONE;
}

void global_mouse_poll(GlobalMouseState *out) {
  memset(out, 0, sizeof(*out));
  switch (_active_backend) {
#ifdef HAVE_X11_XINPUT2
  case BACKEND_XI2:
    _xi_poll(out);
    break;
#endif
  case BACKEND_EVDEV:
    _evdev_poll(out);
    break;
  default:
    out->available = 0;
    break;
  }
}

void global_mouse_close(void) {
  switch (_active_backend) {
#ifdef HAVE_X11_XINPUT2
  case BACKEND_XI2:
    _xi_close();
    break;
#endif
  case BACKEND_EVDEV:
    _evdev_close();
    break;
  default:
    break;
  }
}

/* Sin WinAPI en Linux, estas funciones son no-op */
void global_mouse_hook_install(void) {}
void global_mouse_hook_pump(void) {}

/* ================================================================
 *  PLATAFORMA DESCONOCIDA  (compilación mínima sin captura global)
 * ================================================================ */
#else

#include <string.h>

void global_mouse_init(void) {}
void global_mouse_close(void) {}
void global_mouse_poll(GlobalMouseState *out) {
  memset(out, 0, sizeof(*out));
  out->available = 0;
}
void global_mouse_hook_install(void) {}
void global_mouse_hook_pump(void) {}

#endif /* plataformas */

/* ================================================================
 *  APLICACIÓN  (raylib)
 * ================================================================ */

int main(void) {
  const int screenWidth = SCREEN_WIDTH;
  const int screenHeight = SCREEN_HEIGHT;
  const int mouseWidth = BODY_WIDTH;
  const int mouseHeight = BODY_HEIGHT;
  const int mouseX = (screenWidth / 2) - (mouseWidth / 2);
  const int mouseY = (screenHeight / 2) - (mouseHeight / 2);

  global_mouse_init();
  global_mouse_hook_install(); /* no-op en Linux/otros */

  InitWindow(screenWidth, screenHeight, MOUSE_APP_TITLE);
  SetTargetFPS(60);

  float scrollOffset = 0.0f;
  float scrollTarget = 0.0f;

  /* Cadena de estado del backend */
  const char *backendLabel =
#ifdef _WIN32
      "Backend: WinAPI";
#elif defined(__linux__)
#ifdef HAVE_X11_XINPUT2
      "Backend: X11/XInput2 + evdev";
#else
      "Backend: evdev";
#endif
#else
      "Backend: ninguno";
#endif

  while (!WindowShouldClose()) {
    /* ── Bombear mensajes Windows ── */
    global_mouse_hook_pump();

    /* ── Leer estado global ── */
    GlobalMouseState gms;
    global_mouse_poll(&gms);

    /* ── Rueda: global tiene prioridad, si no usar raylib ── */
    float wheelMove =
        (gms.available && gms.wheel != 0.0f) ? gms.wheel : GetMouseWheelMove();

    if (wheelMove != 0.0f) {
      scrollTarget -= wheelMove * SCROLL_AMPLIFY;
    }

    /* Centrado suave */
    if (scrollTarget > 0.0f) {
      scrollTarget -= SCROLL_CENTER_DECAY;
      if (scrollTarget < 0.0f)
        scrollTarget = 0.0f;
    } else if (scrollTarget < 0.0f) {
      scrollTarget += SCROLL_CENTER_DECAY;
      if (scrollTarget > 0.0f)
        scrollTarget = 0.0f;
    }
    if (scrollTarget > SCROLL_MAX)
      scrollTarget = SCROLL_MAX;
    if (scrollTarget < SCROLL_MIN)
      scrollTarget = SCROLL_MIN;

    scrollOffset += (scrollTarget - scrollOffset) * 0.2f;
    if (scrollOffset < SCROLL_NEAR_ZERO && scrollOffset > -SCROLL_NEAR_ZERO)
      scrollOffset = 0.0f;

    /* ── Colores: global tiene prioridad sobre raylib ── */
    int leftDown =
        gms.available ? gms.left : IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    int rightDown =
        gms.available ? gms.right : IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    int middleDown =
        gms.available ? gms.middle : IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);

    Color bodyColor = LIGHTGRAY;
    Color leftColor = leftDown ? RED : GRAY;
    Color rightColor = rightDown ? BLUE : GRAY;
    Color wheelColor = middleDown ? GREEN : DARKGRAY;

    /* ── Geometría ── */
    int wheelX = mouseX + (mouseWidth / 2) - (WHEEL_WIDTH / 2);
    int wheelY = mouseY + WHEEL_Y_OFFSET;

    Rectangle bodyRect = {mouseX, mouseY, mouseWidth, mouseHeight};
    Rectangle leftButtonRect = {mouseX, mouseY, BUTTON_WIDTH, BUTTON_HEIGHT};
    Rectangle rightButtonRect = {mouseX + (mouseWidth / 2) + 15, mouseY,
                                 BUTTON_WIDTH, BUTTON_HEIGHT};
    Rectangle wheelHoleRect = {wheelX, wheelY, WHEEL_WIDTH, WHEEL_HEIGHT};
    Rectangle wheelRect = {wheelX, wheelY + (int)scrollOffset, WHEEL_WIDTH,
                           WHEEL_HEIGHT};
    Vector2 wl1s = {wheelX + 4, wheelY + 15};
    Vector2 wl1e = {wheelX + WHEEL_WIDTH - 4, wheelY + 15};
    Vector2 wl2s = {wheelX + 4, wheelY + 30};
    Vector2 wl2e = {wheelX + WHEEL_WIDTH - 4, wheelY + 30};
    Vector2 wl3s = {wheelX + 4, wheelY + 45};
    Vector2 wl3e = {wheelX + WHEEL_WIDTH - 4, wheelY + 45};
    Rectangle cableRect = {mouseX + (mouseWidth / 2) - (CABLE_WIDTH / 2),
                           mouseY - CABLE_HEIGHT, CABLE_WIDTH, CABLE_HEIGHT};

    /* ── Dibujo ── */
    BeginDrawing();
    ClearBackground(RAYWHITE);

    DrawRectangleRec(bodyRect, bodyColor);
    DrawRectangleLinesEx(bodyRect, OUTLINE_WIDTH, BLACK);

    DrawRectangleRec(leftButtonRect, leftColor);
    DrawRectangleLinesEx(leftButtonRect, OUTLINE_WIDTH, BLACK);
    DrawRectangleRec(rightButtonRect, rightColor);
    DrawRectangleLinesEx(rightButtonRect, OUTLINE_WIDTH, BLACK);

    DrawRectangleRec(wheelHoleRect, BLACK);
    DrawRectangleRec(wheelRect, wheelColor);
    DrawRectangleLinesEx(wheelRect, 4, BLACK);

    DrawLineEx(wl1s, wl1e, 4, BLACK);
    DrawLineEx(wl2s, wl2e, 4, BLACK);
    DrawLineEx(wl3s, wl3e, 4, BLACK);
    DrawRectangleRec(cableRect, BLACK);

    DrawText("INTERFAZ DE PRUEBA", screenWidth / 2 - 110, 40, TEXT_TOP_SIZE,
             DARKGRAY);
    DrawText(HELP_TEXT, screenWidth / 2 - 150, screenHeight - 60,
             TEXT_BOTTOM_SIZE, GRAY);

    /* Estado del backend */
    Color statusColor = gms.available ? DARKGREEN : RED;
    DrawText(backendLabel, 10, screenHeight - 30, 14, statusColor);
    if (!gms.available)
      DrawText(ERROR_TEXT, 10, screenHeight - 50, 13, RED);

    EndDrawing();
  }

  CloseWindow();
  global_mouse_close();
  return 0;
}
