// SPDX-License-Identifier: GPL-3.0-or-later

#include "gl_renderer.h"
#include "core/logger.h"

namespace simvis {

GLRenderer::GLRenderer() = default;

GLRenderer::~GLRenderer() {
    cleanup();
}

bool GLRenderer::initialize() {
    if (initialized_) return true;

    if (!initializeOpenGLFunctions()) {
        LOG_ERROR("GLRenderer: failed to initialize OpenGL functions");
        return false;
    }

    initialized_ = true;
    return true;
}

void GLRenderer::cleanup() {
    initialized_ = false;
}

void GLRenderer::setProjectionMatrix(const QMatrix4x4& proj) {
    projectionMatrix_ = proj;
}

void GLRenderer::setViewMatrix(const QMatrix4x4& view) {
    viewMatrix_ = view;
}

void GLRenderer::setViewportSize(int width, int height) {
    viewportWidth_ = width;
    viewportHeight_ = height;
}

BoundingBox GLRenderer::getViewportBounds() const {
    // Transform viewport corners to world space
    QMatrix4x4 invPV = (projectionMatrix_ * viewMatrix_).inverted();

    auto unproject = [&](float ndcX, float ndcY) -> Point2D {
        QVector4D ndc(ndcX, ndcY, 0.0f, 1.0f);
        QVector4D world = invPV * ndc;
        if (world.w() != 0) {
            world /= world.w();
        }
        return Point2D(world.x(), world.y());
    };

    Point2D bottomLeft = unproject(-1, -1);
    Point2D topRight = unproject(1, 1);

    BoundingBox bounds;
    bounds.expand(bottomLeft);
    bounds.expand(topRight);
    return bounds;
}

GLuint GLRenderer::compileShader(const char* vertexSource, const char* fragmentSource) {
    // Compile vertex shader
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexSource, nullptr);
    glCompileShader(vertexShader);

    GLint success;
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
        LOG_ERROR(QString("GLRenderer: vertex shader compilation failed: %1").arg(infoLog));
        return 0;
    }

    // Compile fragment shader
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentSource, nullptr);
    glCompileShader(fragmentShader);

    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
        LOG_ERROR(QString("GLRenderer: fragment shader compilation failed: %1").arg(infoLog));
        glDeleteShader(vertexShader);
        return 0;
    }

    // Link program
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        LOG_ERROR(QString("GLRenderer: shader program linking failed: %1").arg(infoLog));
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return 0;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return program;
}

} // namespace simvis
