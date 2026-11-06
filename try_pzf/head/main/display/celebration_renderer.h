#pragma once

#include "fola_gfx.h"

namespace fola {

// The session owns the completion lifetime. Both screens use the same art.
class Celebration {
public:
    void draw(const Canvas &canvas, int64_t elapsed_ms, bool repaint);

private:
    int painted_ms_ = -1;
};

} // namespace fola
