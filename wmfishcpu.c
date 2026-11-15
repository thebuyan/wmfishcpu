#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <zlib.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "fish_frames.h"

#define SIDE 64
#define SOURCE_FPS FISH_FPS
#define SAMPLE_INTERVAL 0.5

typedef struct { uint64_t total, idle; } CpuSample;
static volatile sig_atomic_t running = 1;
static void stop(int signal_number) { (void)signal_number; running = 0; }

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool parse_cpu(const char *line, CpuSample *sample) {
    unsigned long long v[8] = {0};
    int count = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (count < 4) return false;
    sample->total = 0;
    for (int i = 0; i < count; ++i) sample->total += v[i];
    /* guest/guest_nice are already included in user/nice: never add twice. */
    sample->idle = v[3] + v[4];
    return true;
}

static bool read_cpu(CpuSample *sample) {
    FILE *file = fopen("/proc/stat", "r");
    if (!file) return false;
    char line[512];
    bool ok = fgets(line, sizeof(line), file) && parse_cpu(line, sample);
    fclose(file);
    return ok;
}

static double cpu_usage(CpuSample previous, CpuSample current) {
    if (current.total <= previous.total || current.idle < previous.idle) return 0;
    uint64_t total = current.total - previous.total;
    uint64_t idle = current.idle - previous.idle;
    if (idle >= total) return 0;
    return 100.0 * (double)(total - idle) / (double)total;
}

static double advance(double phase, double seconds, double cpu, double speed) {
    return fmod(phase + seconds * SOURCE_FPS * speed * cpu / 100.0, FISH_COUNT);
}

static void self_test(void) {
    CpuSample a, b;
    assert(parse_cpu("cpu 10 20 30 40 5 6 7 8 999 999", &a));
    assert(a.total == 126 && a.idle == 45);
    assert(!parse_cpu("cpu invalid", &b));
    a = (CpuSample){100, 50};
    assert(cpu_usage(a, (CpuSample){200, 150}) == 0);
    assert(cpu_usage(a, (CpuSample){200, 50}) == 100);
    assert(cpu_usage(a, (CpuSample){200, 100}) == 50);
    assert(cpu_usage(a, a) == 0);
    assert(cpu_usage(a, (CpuSample){90, 40}) == 0);
    assert(advance(7, 600, 0, 3) == 7);
    assert(fabs(advance(7, 0.1, 100, 3) - (7 + 0.1 * SOURCE_FPS * 3)) < 1e-9);
    assert(fabs((advance(7, 0.1, 50, 3) - 7) * 2 -
                (advance(7, 0.1, 100, 3) - 7)) < 1e-9);
    unsigned char *rgb = malloc(FISH_BYTES);
    assert(rgb);
    uLongf length = FISH_BYTES;
    assert(uncompress(rgb, &length, fish_compressed, sizeof(fish_compressed)) == Z_OK);
    assert(length == FISH_BYTES && length == FISH_COUNT * FISH_WIDTH * FISH_HEIGHT * 3UL);
    free(rgb);
    puts("OK: CPU 0/50/100%, idle/iowait/guest, counters, linear speed, animation data.");
}

static unsigned long channel(unsigned char value, unsigned long mask) {
    unsigned int shift = 0;
    if (!mask) return 0;
    while (!(mask & 1)) { ++shift; mask >>= 1; }
    return (((unsigned long)value * mask + 127) / 255) << shift;
}

static unsigned long color(Visual *visual, unsigned int rgb) {
    return channel((rgb >> 16) & 255, visual->red_mask) |
           channel((rgb >> 8) & 255, visual->green_mask) |
           channel(rgb & 255, visual->blue_mask);
}

static XImage **load_frames(Display *display, Visual *visual, int depth) {
    unsigned char *rgb = malloc(FISH_BYTES);
    XImage **frames = calloc(FISH_COUNT, sizeof(*frames));
    uLongf length = FISH_BYTES;
    if (!rgb || !frames || uncompress(rgb, &length, fish_compressed,
            sizeof(fish_compressed)) != Z_OK || length != FISH_BYTES) {
        fprintf(stderr, "Could not load the animation.\n"); exit(1);
    }
    for (int f = 0; f < FISH_COUNT; ++f) {
        XImage *im = XCreateImage(display, visual, (unsigned)depth, ZPixmap,
            0, NULL, FISH_WIDTH, FISH_HEIGHT, 32, 0);
        if (!im || !(im->data = calloc((size_t)im->bytes_per_line, FISH_HEIGHT))) {
            fprintf(stderr, "Out of memory.\n"); exit(1);
        }
        for (int y = 0; y < FISH_HEIGHT; ++y) {
            for (int x = 0; x < FISH_WIDTH; ++x) {
                size_t p = ((size_t)f * FISH_WIDTH * FISH_HEIGHT + y * FISH_WIDTH + x) * 3;
                unsigned long pixel = channel(rgb[p], visual->red_mask) |
                    channel(rgb[p+1], visual->green_mask) | channel(rgb[p+2], visual->blue_mask);
                XPutPixel(im, x, y, pixel);
            }
        }
        frames[f] = im;
    }
    free(rgb);
    return frames;
}

static void draw(Display *d, Window w, Pixmap buffer, GC gc, Visual *visual,
                 XImage *frame, double cpu, bool valid, XFontStruct *font) {
    XSetForeground(d, gc, color(visual, 0x242b33));
    XFillRectangle(d, buffer, gc, 0, 0, SIDE, SIDE);
    XSetForeground(d, gc, color(visual, 0x7a8795));
    XDrawLine(d, buffer, gc, 0, 0, 63, 0);
    XDrawLine(d, buffer, gc, 0, 0, 0, 63);
    XSetForeground(d, gc, color(visual, 0x10151c));
    XDrawLine(d, buffer, gc, 63, 0, 63, 63);
    XDrawLine(d, buffer, gc, 0, 63, 63, 63);
    XPutImage(d, buffer, gc, frame, 0, 0, 2, 2, FISH_WIDTH, FISH_HEIGHT);
    char text[20];
    if (valid) {
        /* Avoid showing 0% while a nonzero CPU reading still advances. */
        if (cpu > 0 && cpu < 1) snprintf(text, sizeof(text), "CPU <1%%");
        else snprintf(text, sizeof(text), "CPU %d%%", (int)cpu);
    } else snprintf(text, sizeof(text), "CPU --");
    int width = font ? XTextWidth(font, text, (int)strlen(text)) : (int)strlen(text) * 6;
    XSetForeground(d, gc, color(visual, 0xe5edf4));
    XDrawString(d, buffer, gc, (SIDE - width) / 2, 59, text, (int)strlen(text));
    XCopyArea(d, buffer, w, gc, 0, 0, SIDE, SIDE, 0, 0);
    XFlush(d);
}

static double number(const char *text, const char *option, double min, double max) {
    char *end;
    errno = 0;
    double value = strtod(text, &end);
    if (errno || end == text || *end || !isfinite(value) || value < min || value > max) {
        fprintf(stderr, "%s requires a number between %g and %g.\n", option, min, max);
        exit(2);
    }
    return value;
}

int main(int argc, char **argv) {
    bool windowed = false, trace = false;
    double speed = 3, fixed_cpu = -1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--window")) windowed = true;
        else if (!strcmp(argv[i], "--trace")) trace = true;
        else if (!strcmp(argv[i], "--self-test")) { self_test(); return 0; }
        else if (!strcmp(argv[i], "--max-speed") && i + 1 < argc)
            speed = number(argv[++i], "--max-speed", 0.1, 20);
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc)
            fixed_cpu = number(argv[++i], "--cpu", 0, 100);
        else if (!strcmp(argv[i], "--help")) {
            puts("wmfishcpu: spinning fish driven by total CPU usage.\n"
                 "  --window         Use a regular window instead of a dockapp\n"
                 "  --max-speed N    Speed at 100% CPU (default: 3; 1 = original speed)\n"
                 "  --cpu N          Simulate a CPU percentage between 0 and 100\n"
                 "  --trace          Print CPU usage and animation frame every 0.5 seconds\n"
                 "  --self-test      Check CPU math and animation data without opening a window\n"
                 "  --help           Show this help\n"
                 "In window mode, press Escape or Q to quit.");
            return 0;
        } else { fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]); return 2; }
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "Could not open the X11 display.\n"); return 1; }
    int screen = DefaultScreen(d), depth = DefaultDepth(d, screen);
    Visual *visual = DefaultVisual(d, screen);
    if (visual->class != TrueColor) {
        fprintf(stderr, "A TrueColor X11 display is required.\n"); XCloseDisplay(d); return 1;
    }
    Window root = RootWindow(d, screen);
    Window main_window = XCreateSimpleWindow(d, root, 0, 0, SIDE, SIDE, 0, 0, 0);
    Window icon = windowed ? main_window : XCreateSimpleWindow(d, root, 0, 0, SIDE, SIDE, 0, 0, 0);
    XClassHint class_hint = {"wmfishcpu", "DockApp"};
    XSetClassHint(d, main_window, &class_hint);
    XSetClassHint(d, icon, &class_hint);
    XStoreName(d, main_window, "Fish CPU");
    XStoreName(d, icon, "Fish CPU");
    /* Window Maker must be able to relaunch the icon from any directory. */
    char executable[4096];
    ssize_t path_length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    char **command = calloc((size_t)argc, sizeof(*command));
    if (command && path_length > 0) {
        executable[path_length] = '\0';
        for (int i = 0; i < argc; ++i) command[i] = argv[i];
        command[0] = executable;
        XSetCommand(d, main_window, command, argc);
    } else XSetCommand(d, main_window, argv, argc);
    free(command);
    XWMHints hints = {0};
    hints.flags = StateHint | WindowGroupHint | InputHint;
    hints.initial_state = windowed ? NormalState : WithdrawnState;
    hints.window_group = main_window;
    hints.input = windowed;
    if (!windowed) { hints.flags |= IconWindowHint; hints.icon_window = icon; }
    XSetWMHints(d, main_window, &hints);
    XSizeHints size = {0};
    size.flags = PMinSize | PMaxSize;
    size.min_width = size.max_width = SIDE;
    size.min_height = size.max_height = SIDE;
    XSetWMNormalHints(d, main_window, &size);
    Atom delete_window = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d, main_window, &delete_window, 1);
    XSetWMProtocols(d, icon, &delete_window, 1);
    XSelectInput(d, icon, ExposureMask | StructureNotifyMask | KeyPressMask);
    XImage **frames = load_frames(d, visual, depth);
    GC gc = XCreateGC(d, icon, 0, NULL);
    XFontStruct *font = XLoadQueryFont(d, "6x10");
    if (!font) font = XLoadQueryFont(d, "fixed");
    if (font) XSetFont(d, gc, font->fid);
    Pixmap buffer = XCreatePixmap(d, icon, SIDE, SIDE, (unsigned)depth);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    CpuSample previous = {0};
    bool baseline = read_cpu(&previous), valid = fixed_cpu >= 0;
    double cpu = valid ? fixed_cpu : 0;
    double phase = FISH_COUNT > 7 ? 7 : 0, last_time = now(), last_sample = last_time;
    int old_frame = -1, old_label = -2;
    bool redraw = true;
    XMapWindow(d, main_window);
    while (running) {
        while (XPending(d)) {
            XEvent event;
            XNextEvent(d, &event);
            if (event.type == Expose) redraw = true;
            if (event.type == DestroyNotify || (event.type == ClientMessage &&
                event.xclient.message_type == XInternAtom(d, "WM_PROTOCOLS", False) &&
                (Atom)event.xclient.data.l[0] == delete_window)) running = 0;
            if (event.type == KeyPress && windowed) {
                KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape || key == XK_q || key == XK_Q) running = 0;
            }
        }
        if (!running) break;
        double current_time = now();
        /* Integrate the previous measurement over its own interval. */
        phase = advance(phase, current_time - last_time, cpu, speed);
        last_time = current_time;
        if (current_time - last_sample >= SAMPLE_INTERVAL) {
            if (fixed_cpu < 0) {
                CpuSample current;
                if (read_cpu(&current)) {
                    valid = baseline;
                    cpu = baseline ? cpu_usage(previous, current) : 0;
                    previous = current;
                    baseline = true;
                } else { valid = false; baseline = false; cpu = 0; }
            }
            if (trace) {
                printf("cpu=%.3f frame=%d phase=%.6f fps=%.3f\n", cpu, (int)phase,
                       phase, SOURCE_FPS * speed * cpu / 100.0);
                fflush(stdout);
            }
            last_sample = current_time;
        }
        int frame = (int)phase;
        int label = valid ? ((cpu > 0 && cpu < 1) ? -3 : (int)cpu) : -1;
        if (redraw || frame != old_frame || label != old_label) {
            draw(d, icon, buffer, gc, visual, frames[frame], cpu, valid, font);
            old_frame = frame; old_label = label; redraw = false;
        }
        fd_set input;
        FD_ZERO(&input);
        FD_SET(ConnectionNumber(d), &input);
        struct timeval timeout = {0, cpu > 0 ? 16667 : 100000};
        int result = select(ConnectionNumber(d) + 1, &input, NULL, NULL, &timeout);
        if (result < 0 && errno != EINTR) { perror("select"); break; }
    }
    for (int f = 0; f < FISH_COUNT; ++f) XDestroyImage(frames[f]);
    free(frames);
    if (font) XFreeFont(d, font);
    XFreePixmap(d, buffer);
    XFreeGC(d, gc);
    XCloseDisplay(d);
    return 0;
}
