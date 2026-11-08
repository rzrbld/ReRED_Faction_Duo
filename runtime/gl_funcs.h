/*
 * gl_funcs.h - the GLES 2.0 entry points the renderer uses, resolved at
 * runtime through SDL_GL_GetProcAddress (no link-time dependency on a vendor
 * libGLESv2 path). X(ret, name, args)
 */
#pragma once
#include <SDL2/SDL_opengles2.h>

#define RR_GL_FUNCS(X) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glBlendEquation, (GLenum)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClearDepthf, (GLfloat)) \
    X(void, glClearStencil, (GLint)) \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
    X(void, glCopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
    X(GLuint, glCreateProgram, (void)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glCullFace, (GLenum)) \
    X(void, glDeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) \
    X(void, glDeleteShader, (GLuint)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    X(void, glDepthFunc, (GLenum)) \
    X(void, glDepthMask, (GLboolean)) \
    X(void, glDepthRangef, (GLfloat, GLfloat)) \
    X(void, glDisable, (GLenum)) \
    X(void, glDisableVertexAttribArray, (GLuint)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    X(void, glDrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void *, GLint)) \
    X(void *, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
    X(GLboolean, glUnmapBuffer, (GLenum)) \
    X(void, glEnable, (GLenum)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glFinish, (void)) \
    X(void, glFlush, (void)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glFrontFace, (GLenum)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glGenerateMipmap, (GLenum)) \
    X(GLenum, glGetError, (void)) \
    X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(void, glGetFloatv, (GLenum, GLfloat *)) \
    X(GLboolean, glIsEnabled, (GLenum)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(const GLubyte *, glGetString, (GLenum)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glPolygonOffset, (GLfloat, GLfloat)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glStencilFunc, (GLenum, GLint, GLuint)) \
    X(void, glStencilMask, (GLuint)) \
    X(void, glStencilOp, (GLenum, GLenum, GLenum)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUseProgram, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))

#define RR_GL_DECL(ret, name, args) typedef ret (GL_APIENTRY *PFN_rr_##name) args; extern PFN_rr_##name rr_##name;
RR_GL_FUNCS(RR_GL_DECL)
#undef RR_GL_DECL

/* Renderer code calls gl* names; they resolve to the loaded pointers. */
#ifndef RR_GL_NO_REDIRECT
#define RR_GL_REDIRECT(ret, name, args)
#include "gl_redirect.h"
#endif
