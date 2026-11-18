// The few OpenGL 1.x functions the emulator itself uses (the plugin finds its own, as it does in X-Plane), loaded at run time so no
// OpenGL headers are needed to build it. The emulator draws with the fixed-function pipeline, in a compatibility context like X-Plane's.
#pragma once
#include <cstddef>

#if defined(_WIN32)
#define EMU_GLAPI __stdcall
#else
#define EMU_GLAPI
#endif

namespace emu {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned char GLubyte;
typedef unsigned int GLbitfield;

enum : GLenum {
    GL_TEXTURE_2D = 0x0DE1, GL_BLEND = 0x0BE2, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_ALPHA_TEST = 0x0BC0,
    GL_DEPTH_TEST = 0x0B71, GL_SCISSOR_TEST = 0x0C11, GL_CULL_FACE = 0x0B44, GL_LIGHTING = 0x0B50, GL_FOG = 0x0B60,
    GL_PROJECTION = 0x1701, GL_MODELVIEW = 0x1700, GL_TEXTURE = 0x1702, GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100,
    GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405,
    GL_TRIANGLES = 0x0004, GL_QUADS = 0x0007, GL_LINE_LOOP = 0x0002, GL_LINES = 0x0001,
    GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_LINEAR = 0x2601, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
    GL_CLAMP_TO_EDGE = 0x812F, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ROW_LENGTH = 0x0CF2,
    GL_VERTEX_ARRAY = 0x8074, GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078, GL_TEXTURE_ENV = 0x2300, GL_TEXTURE_ENV_MODE = 0x2200,
    GL_MODULATE = 0x2100, GL_VIEWPORT = 0x0BA2, GL_VERSION = 0x1F02, GL_RENDERER = 0x1F01, GL_TEXTURE0 = 0x84C0,
};

struct Gl {
    void(EMU_GLAPI* Enable)(GLenum) = nullptr;
    void(EMU_GLAPI* Disable)(GLenum) = nullptr;
    void(EMU_GLAPI* BlendFunc)(GLenum, GLenum) = nullptr;
    void(EMU_GLAPI* Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(EMU_GLAPI* Scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(EMU_GLAPI* ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void(EMU_GLAPI* Clear)(GLbitfield) = nullptr;
    void(EMU_GLAPI* MatrixMode)(GLenum) = nullptr;
    void(EMU_GLAPI* LoadIdentity)(void) = nullptr;
    void(EMU_GLAPI* Ortho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) = nullptr;
    void(EMU_GLAPI* GenTextures)(GLsizei, GLuint*) = nullptr;
    void(EMU_GLAPI* DeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void(EMU_GLAPI* BindTexture)(GLenum, GLuint) = nullptr;
    void(EMU_GLAPI* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
    void(EMU_GLAPI* TexParameteri)(GLenum, GLenum, GLint) = nullptr;
    void(EMU_GLAPI* TexEnvi)(GLenum, GLenum, GLint) = nullptr;
    void(EMU_GLAPI* PixelStorei)(GLenum, GLint) = nullptr;
    void(EMU_GLAPI* EnableClientState)(GLenum) = nullptr;
    void(EMU_GLAPI* DisableClientState)(GLenum) = nullptr;
    void(EMU_GLAPI* VertexPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void(EMU_GLAPI* TexCoordPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void(EMU_GLAPI* ColorPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void(EMU_GLAPI* DrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
    void(EMU_GLAPI* Begin)(GLenum) = nullptr;
    void(EMU_GLAPI* End)(void) = nullptr;
    void(EMU_GLAPI* Vertex2f)(GLfloat, GLfloat) = nullptr;
    void(EMU_GLAPI* Color4f)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void(EMU_GLAPI* LineWidth)(GLfloat) = nullptr;
    void(EMU_GLAPI* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void(EMU_GLAPI* Finish)(void) = nullptr;
    void(EMU_GLAPI* PushAttrib)(GLbitfield) = nullptr;
    void(EMU_GLAPI* PopAttrib)(void) = nullptr;
    const GLubyte*(EMU_GLAPI* GetString)(GLenum) = nullptr;
    void(EMU_GLAPI* ActiveTexture)(GLenum) = nullptr;   // may be missing (then only unit 0)
};
extern Gl gl;

// Loads them with the given lookup (the window library's, or EGL's). False when one is missing.
bool loadGl(void* (*getProc)(const char*));

} // namespace emu
