// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/types.h"
#include <QOpenGLFunctions_3_3_Core>
#include <QMatrix4x4>
#include <memory>

namespace simvis {

class NetworkIndex;

// Base OpenGL renderer with common functionality
class GLRenderer : protected QOpenGLFunctions_3_3_Core {
public:
    GLRenderer();
    virtual ~GLRenderer();

    // Initialize OpenGL resources
    virtual bool initialize();

    // Cleanup OpenGL resources
    virtual void cleanup();

    // Set view/projection matrices
    void setProjectionMatrix(const QMatrix4x4& proj);
    void setViewMatrix(const QMatrix4x4& view);

    // Get current viewport bounds in world coordinates
    BoundingBox getViewportBounds() const;

    // Set viewport size
    void setViewportSize(int width, int height);

protected:
    // Compile shader program
    GLuint compileShader(const char* vertexSource, const char* fragmentSource);

    QMatrix4x4 projectionMatrix_;
    QMatrix4x4 viewMatrix_;
    int viewportWidth_ = 800;
    int viewportHeight_ = 600;

    bool initialized_ = false;
};

} // namespace simvis
