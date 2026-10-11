#pragma once

#include "gl_renderer.h"
#include <vector>

namespace simvis {

// Renders one person's full-day route as a thick yellow overlay.
// Vehicle legs follow the network link-by-link; teleported legs (walk/bike)
// are drawn as straight connectors between their endpoints.
// Thickness is quad-based (6 verts per segment) since glLineWidth is clamped
// to 1px in the GL Core profile.
class PersonRouteRenderer : public GLRenderer {
public:
    struct Segment {
        float x1, y1, x2, y2;
        bool teleported;  // straight connector (drawn slightly transparent)
    };

    PersonRouteRenderer();
    ~PersonRouteRenderer() override;

    bool initialize() override;
    void cleanup() override;

    // Replace the route geometry (world coordinates). Empty clears the overlay.
    void setRoute(const std::vector<Segment>& segments);
    void clearRoute() { setRoute({}); }
    bool hasRoute() const { return !segments_.empty(); }

    void setVisible(bool visible) { visible_ = visible; }
    bool visible() const { return visible_; }

    // World-units half-width of the route band (scaled with zoom by caller if desired)
    void setHalfWidth(float halfWidth) { halfWidth_ = halfWidth; buffersNeedUpdate_ = true; }

    // Band color, 0..1 per channel. Yellow unless set.
    void setColor(float r, float g, float b) { colorR_ = r; colorG_ = g; colorB_ = b; }

    void render();

private:
    void buildBuffers();

    std::vector<Segment> segments_;

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint program_ = 0;
    size_t vertexCount_ = 0;

    bool visible_ = false;
    bool buffersNeedUpdate_ = false;
    float halfWidth_ = 6.0f;  // meters (world units)
    float colorR_ = 1.0f, colorG_ = 0.85f, colorB_ = 0.1f;
};

} // namespace simvis
