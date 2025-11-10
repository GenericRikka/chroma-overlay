// src/main.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <thread>
#include <iostream>
#include <cctype>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <GL/glx.h>               // GLX, texture_from_pixmap
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xfixes.h> // for input region shaping

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>

// GLX_EXT_texture_from_pixmap entry points
static PFNGLXBINDTEXIMAGEEXTPROC    glXBindTexImageEXT    = nullptr;
static PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT = nullptr;

struct Rect { int x, y, w, h; };

static std::string loadFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { std::perror(path); std::exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string s; s.resize(n);
    fread(s.data(), 1, n, f);
    fclose(f);
    return s;
}

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok=0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len=0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len);
        glGetShaderInfoLog(s, len, nullptr, log.data());
        std::cerr << "Shader compile error: " << log.data() << "\n";
        std::exit(1);
    }
    return s;
}

static GLuint makeProgram(const char* vertPath, const char* fragPath) {
    std::string v = loadFile(vertPath);
    std::string f = loadFile(fragPath);
    GLuint vs = compileShader(GL_VERTEX_SHADER, v.c_str());
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, f.c_str());
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok=0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len=0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len);
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        std::cerr << "Link error: " << log.data() << "\n";
        std::exit(1);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return prog;
}

static Rect scaleToFB(Rect r, float sx, float sy) {
    return { int(r.x * sx), int(r.y * sy), int(r.w * sx), int(r.h * sy) };
}

// --- Frame extents (titlebar/borders) so the quad aligns perfectly ---
static bool getFrameExtents(Display* dpy, Window w, int& left, int& right, int& top, int& bottom) {
    Atom prop = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
    Atom actual; int format; unsigned long nitems, after;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, w, prop, 0, 4, False, XA_CARDINAL,
                           &actual, &format, &nitems, &after, &data) == Success &&
        actual == XA_CARDINAL && format == 32 && nitems == 4 && data) {
        unsigned long* v = reinterpret_cast<unsigned long*>(data);
        left = (int)v[0]; right = (int)v[1]; top = (int)v[2]; bottom = (int)v[3];
        XFree(data);
        return true;
    }
    return false;
}

static void setFullscreen(Display* dpy, Window w) {
    Atom wmState = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fs      = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(dpy, w, wmState, XA_ATOM, 32, PropModeAppend,
                    reinterpret_cast<const unsigned char*>(&fs), 1);
    XFlush(dpy);
}

static Rect getWindowRect(Display* dpy, Window w) {
    XWindowAttributes wa; XGetWindowAttributes(dpy, w, &wa);
    Window child; int rx, ry;
    // This already accounts for the WM frame/titlebar offsets
    XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child);
    return { rx, ry, wa.width, wa.height };
}

static void makeClickThrough(Display* dpy, Window w) {
    XRectangle rect; rect.x = rect.y = 0; rect.width = rect.height = 0;
    XShapeCombineRectangles(dpy, w, ShapeInput, 0, 0, &rect, 1, ShapeSet, Unsorted);
}

static void setAlwaysOnTop(Display* dpy, Window w) {
    Atom wmState = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom above   = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(dpy, w, wmState, XA_ATOM, 32, PropModeAppend,
                    reinterpret_cast<const unsigned char*>(&above), 1);
}

// Set a window's global opacity (0x00000000..0xFFFFFFFF)
static void setWindowOpacity(Display* dpy, Window w, unsigned long argb32) {
    Atom opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    XChangeProperty(dpy, w, opacity, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&argb32), 1);
    XFlush(dpy);
}

// FBConfig supporting RGBA + texture_from_pixmap
static GLXFBConfig chooseFBConfig(Display* dpy) {
    int screen = DefaultScreen(dpy);
    int attrs[] = {
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
        GLX_BIND_TO_TEXTURE_RGBA_EXT, True,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER, False,
        GLX_RED_SIZE,   8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE,  8,
        GLX_ALPHA_SIZE, 8,
        None
    };
    int n = 0;
    GLXFBConfig* cfgs = glXChooseFBConfig(dpy, screen, attrs, &n);
    if (!cfgs || n == 0) {
        std::cerr << "No RGBA-capable FBConfig found.\n";
        std::exit(1);
    }
    GLXFBConfig cfg = cfgs[0];
    XFree(cfgs);
    return cfg;
}

struct Capture {
    Pixmap    xpixmap   = 0;
    GLXPixmap glxpixmap = 0;
    GLuint    texture   = 0;
    int w=0, h=0;
};

static void releaseCapture(Display* dpy, Capture& cap) {
    if (glXReleaseTexImageEXT && cap.glxpixmap)
        glXReleaseTexImageEXT(dpy, cap.glxpixmap, GLX_FRONT_LEFT_EXT);
    if (cap.glxpixmap) { glXDestroyPixmap(dpy, cap.glxpixmap); cap.glxpixmap = 0; }
    if (cap.xpixmap)   { XFreePixmap(dpy, cap.xpixmap);       cap.xpixmap   = 0; }
    if (cap.texture)   { glDeleteTextures(1, &cap.texture);   cap.texture   = 0; }
}

static Capture makeCapture(Display* dpy, Window target, GLXFBConfig fbconf) {
    Capture cap;

    XCompositeRedirectWindow(dpy, target, CompositeRedirectAutomatic);

    XWindowAttributes wa; XGetWindowAttributes(dpy, target, &wa);
    cap.w = wa.width; cap.h = wa.height;

    cap.xpixmap = XCompositeNameWindowPixmap(dpy, target);
    if (!cap.xpixmap) { std::cerr << "XCompositeNameWindowPixmap failed.\n"; std::exit(1); }

    int pixattrs[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGBA_EXT,
        None
    };
    cap.glxpixmap = glXCreatePixmap(dpy, fbconf, cap.xpixmap, pixattrs);
    if (!cap.glxpixmap) { std::cerr << "glXCreatePixmap failed.\n"; std::exit(1); }

    glGenTextures(1, &cap.texture);
    glBindTexture(GL_TEXTURE_2D, cap.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glXBindTexImageEXT(dpy, cap.glxpixmap, GLX_FRONT_LEFT_EXT, nullptr);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "glXBindTexImageEXT GL error: " << (int)err << "\n";
        std::exit(1);
    }
    return cap;
}

static void quadInit(GLuint& vao, GLuint& vbo) {
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*16, nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0); // aPos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1); // aTex
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
}

static void quadUpdate(GLuint vbo, int screenW, int screenH, Rect r) {
    float x0 = (2.0f * r.x / screenW) - 1.0f;
    float y0 = 1.0f - (2.0f * r.y / screenH);
    float x1 = (2.0f * (r.x + r.w) / screenW) - 1.0f;
    float y1 = 1.0f - (2.0f * (r.y + r.h) / screenH);

    float verts[] = {
        // x,  y,   u,  v
         x0, y1, 0.0f, 1.0f,
         x0, y0, 0.0f, 0.0f,
         x1, y1, 1.0f, 1.0f,
         x1, y0, 1.0f, 0.0f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
}

// ---------------------
// WM_CLASS utilities
// ---------------------
static Atom intern(Display* dpy, const char* name) {
    return XInternAtom(dpy, name, False);
}

static std::vector<Window> ewmhClientList(Display* dpy) {
    Atom netClientList = intern(dpy, "_NET_CLIENT_LIST");
    Atom actual_type; int actual_format; unsigned long nitems, bytes_after;
    unsigned char* data = nullptr;
    int status = XGetWindowProperty(
        dpy, DefaultRootWindow(dpy), netClientList,
        0, ~0L, False, XA_WINDOW, &actual_type, &actual_format,
        &nitems, &bytes_after, &data);

    std::vector<Window> wins;
    if (status == Success && actual_type == XA_WINDOW && actual_format == 32 && data) {
        Window* arr = reinterpret_cast<Window*>(data);
        wins.assign(arr, arr + nitems);
        XFree(data);
    }
    return wins;
}

static bool getWMClass(Display* dpy, Window w, std::string& resName, std::string& resClass) {
    XClassHint hint;
    if (XGetClassHint(dpy, w, &hint)) {
        if (hint.res_name)  resName  = hint.res_name;
        if (hint.res_class) resClass = hint.res_class;
        if (hint.res_name)  XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
        return true;
    }
    return false;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = std::tolower((unsigned char)c);
    return s;
}
static bool ciSubstr(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

static std::optional<Window> findWindowByClass(Display* dpy, const std::string& pattern) {
    auto clients = ewmhClientList(dpy);
    for (Window w : clients) {
        std::string rn, rc;
        if (getWMClass(dpy, w, rn, rc)) {
            if (ciSubstr(rn, pattern) || ciSubstr(rc, pattern)) return w;
        }
    }
    return std::nullopt;
}

static unsigned long parseWindowId(const std::string& s) {
    char* end=nullptr;
    unsigned long id = std::strtoul(s.c_str(), &end, 0); // auto base (0x ok)
    if (!id) { std::cerr << "Invalid window id.\n"; std::exit(1); }
    return id;
}

// ---------------------
// Input shaping from chroma (click-through only where transparent)
// ---------------------
static inline bool isGreenPixel(uint8_t r, uint8_t g, uint8_t b) {
    // match shader defaults: g >= 0.60, r <= 0.35, b <= 0.35
    return (g >= 153) && (r <= 89) && (b <= 89);
}

// Build input region = union of opaque (non-green) runs
static void applyOpaqueInputRegion(Display* dpy, Window target, Pixmap srcPixmap, int w, int h, int step = 2) {
    XImage* img = XGetImage(dpy, srcPixmap, 0, 0, (unsigned int)w, (unsigned int)h, AllPlanes, ZPixmap);
    if (!img) return;

    std::vector<XRectangle> rects;
    rects.reserve((h/step + 1) * 16);

    for (int y = 0; y < h; y += step) {
        int runStart = -1;
        for (int x = 0; x < w; x += step) {
            unsigned long px = XGetPixel(img, x, y);
            uint8_t r = (px >> 16) & 0xFF;
            uint8_t g = (px >> 8)  & 0xFF;
            uint8_t b = (px >> 0)  & 0xFF;

            bool opaque = !isGreenPixel(r,g,b);
            if (opaque && runStart < 0) runStart = x;
            if (!opaque && runStart >= 0) {
                XRectangle rc;
                rc.x = (short)runStart; rc.y = (short)y;
                rc.width = (unsigned short)(x - runStart);
                rc.height = (unsigned short)step;
                rects.push_back(rc);
                runStart = -1;
            }
        }
        if (runStart >= 0) {
            XRectangle rc;
            rc.x = (short)runStart; rc.y = (short)y;
            rc.width = (unsigned short)(w - runStart);
            rc.height = (unsigned short)step;
            rects.push_back(rc);
        }
    }
    XDestroyImage(img);

    XserverRegion region = XFixesCreateRegion(dpy, rects.data(), (int)rects.size());
    XFixesSetWindowShapeRegion(dpy, target, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(dpy, region);
    XFlush(dpy);
}

// ---------------------
// CLI args
// ---------------------
struct Args {
    std::string winIdStr;
    std::string classPattern;
};

static void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [--win-id 0xID] [--class CLASS]\n";
}

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if ((s == "--win-id" || s == "-w") && i+1 < argc) a.winIdStr = argv[++i];
        else if ((s == "--class" || s == "-c") && i+1 < argc) a.classPattern = argv[++i];
        else { usage(argv[0]); std::exit(1); }
    }
    if (a.winIdStr.empty() && a.classPattern.empty()) {
        usage(argv[0]); std::exit(1);
    }
    return a;
}

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    if (!glfwInit()) { std::cerr << "glfwInit failed\n"; return 1; }

    // Get Display now to size overlay as the full virtual root
    Display* dpy = glfwGetX11Display();
    if (!dpy) { std::cerr << "Failed to get X11 Display from GLFW\n"; return 1; }
    int screenW = DisplayWidth(dpy, DefaultScreen(dpy));
    int screenH = DisplayHeight(dpy, DefaultScreen(dpy));

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(screenW, screenH, "chroma-overlay", nullptr, nullptr);
    if (!win) { std::cerr << "glfwCreateWindow failed\n"; return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    if (glewInit() != GLEW_OK) { std::cerr << "glewInit failed\n"; return 1; }

    // Verify TFP support
    const char* glxExt = glXQueryExtensionsString(dpy, DefaultScreen(dpy));
    if (!glxExt || !strstr(glxExt, "GLX_EXT_texture_from_pixmap")) {
        std::cerr << "Missing GLX_EXT_texture_from_pixmap\n";
        return 1;
    }
    glXBindTexImageEXT    = (PFNGLXBINDTEXIMAGEEXTPROC)glXGetProcAddress((const GLubyte*)"glXBindTexImageEXT");
    glXReleaseTexImageEXT = (PFNGLXRELEASETEXIMAGEEXTPROC)glXGetProcAddress((const GLubyte*)"glXReleaseTexImageEXT");
    if (!glXBindTexImageEXT || !glXReleaseTexImageEXT) {
        std::cerr << "Failed to load glXBindTexImageEXT/glXReleaseTexImageEXT\n";
        return 1;
    }

    // Make overlay behave: click-through + on-top
    Window overlay = glfwGetX11Window(win);
    makeClickThrough(dpy, overlay);
    setAlwaysOnTop(dpy, overlay);

    setFullscreen(dpy, overlay);

    // make sure it sits at the root origin and covers full root
    XMoveResizeWindow(dpy, overlay, 0, 0, screenW, screenH);
    XFlush(dpy);

    // Ensure compositor treats this window as ARGB + per-pixel alpha
    Atom opacityAtom = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    unsigned long full = 0xFFFFFFFFUL;
    XChangeProperty(dpy, overlay, opacityAtom, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&full), 1);
    XFlush(dpy);

    // Enable blending; clear alpha=0 for transparent background
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    // Resolve target window (by id or by class)
    Window target = 0;
    if (!args.winIdStr.empty()) {
        target = static_cast<Window>(parseWindowId(args.winIdStr));
    } else {
        auto w = findWindowByClass(dpy, args.classPattern);
        if (!w) {
            std::cerr << "No matching window for class pattern: " << args.classPattern << "\n";
            auto clients = ewmhClientList(dpy);
            std::cerr << "Currently managed windows and their WM_CLASS:\n";
            for (auto cw : clients) {
                std::string rn, rc;
                if (getWMClass(dpy, cw, rn, rc)) {
                    std::cerr << "  0x" << std::hex << cw << std::dec
                              << "  name='" << rn << "'  class='" << rc << "'\n";
                }
            }
            return 1;
        }
        target = *w;
        std::cerr << "Matched window 0x" << std::hex << target << std::dec
                  << " for pattern '" << args.classPattern << "'.\n";
    }

    // Hide original window but keep it mapped & interactive
    setWindowOpacity(dpy, target, 0x00000000UL);

    // Choose FBConfig and make capture
    GLXFBConfig fbconf = chooseFBConfig(dpy);
    Capture cap = makeCapture(dpy, target, fbconf);

    // Shaders
    GLuint prog = makeProgram("src/shaders/chromakey.vert", "src/shaders/chromakey.frag");
    glUseProgram(prog);
    GLint locTex = glGetUniformLocation(prog, "uTex");
    GLint locG   = glGetUniformLocation(prog, "uGreenMin");
    GLint locR   = glGetUniformLocation(prog, "uRedMax");
    GLint locB   = glGetUniformLocation(prog, "uBlueMax");
    GLint locF   = glGetUniformLocation(prog, "uFeather");
    glUniform1i(locTex, 0);
    glUniform1f(locG, 0.60f);
    glUniform1f(locR, 0.35f);
    glUniform1f(locB, 0.35f);
    glUniform1f(locF, 0.00f); // hard key initially

    // Quad
    GLuint vao=0, vbo=0;
    quadInit(vao, vbo);

    Rect rect = getWindowRect(dpy, target);

    // Initial input region based on current content
    applyOpaqueInputRegion(dpy, target, cap.xpixmap, cap.w, cap.h, /*step=*/2);
    auto lastMaskUpdate = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        // track target rect in ROOT coordinates
        Rect nowR = getWindowRect(dpy, target);
        if (nowR.w != rect.w || nowR.h != rect.h) {
            glBindTexture(GL_TEXTURE_2D, 0);
            releaseCapture(dpy, cap);
            cap = makeCapture(dpy, target, fbconf);
            applyOpaqueInputRegion(dpy, target, cap.xpixmap, cap.w, cap.h, /*step=*/2);
            lastMaskUpdate = std::chrono::steady_clock::now();
        }
        rect = nowR;

        // NEW: framebuffer size & viewport (GL space)
        int fbW=0, fbH=0;
        glfwGetFramebufferSize(win, &fbW, &fbH);
        if (fbW <= 0 || fbH <= 0) { fbW = screenW; fbH = screenH; } // fallback
        glViewport(0, 0, fbW, fbH);

        // scale root coords into framebuffer coords
        float sx = float(fbW) / float(screenW);
        float sy = float(fbH) / float(screenH);
        Rect fbRect = scaleToFB(rect, sx, sy);

        // Periodic input-mask refresh (unchanged)
        auto nowT = std::chrono::steady_clock::now();
        if (nowT - lastMaskUpdate > std::chrono::milliseconds(100)) {
            applyOpaqueInputRegion(dpy, target, cap.xpixmap, cap.w, cap.h, /*step=*/2);
            lastMaskUpdate = nowT;
        }

        glClear(GL_COLOR_BUFFER_BIT);

        // Use framebuffer size for NDC mapping
        quadUpdate(vbo, fbW, fbH, fbRect);

        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cap.texture);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glfwSwapBuffers(win);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }


    // Restore original window opacity on exit
    setWindowOpacity(dpy, target, 0xFFFFFFFFUL);

    releaseCapture(dpy, cap);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}

