# Third-Party Licenses

TAREEK-Vis uses the following third-party libraries, tools, and online
services:

---

## Qt6

- **License**: LGPL v3.0
- **Website**: https://www.qt.io/
- **Components used**: Widgets, OpenGL, OpenGLWidgets, Concurrent, Network
- **Usage**: Dynamically linked

The Qt Toolkit is Copyright (C) The Qt Company Ltd. and other contributors.
Qt is available under the LGPL v3.0 license. See https://www.gnu.org/licenses/lgpl-3.0.html

---

## PugiXML

- **License**: MIT
- **Version**: 1.15
- **Website**: https://pugixml.org/
- **Copyright**: Copyright (C) 2006-2026 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files, to deal in the Software
without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the
Software, and to permit persons to whom the Software is furnished to do so,
subject to the above copyright notice and this permission notice.

---

## ZLIB

- **License**: Zlib License
- **Website**: https://www.zlib.net/
- **Copyright**: Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software. Permission is granted to anyone to use this software
for any purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented.
2. Altered source versions must be plainly marked as such.
3. This notice may not be removed or altered from any source distribution.

---

## FFmpeg (Optional)

- **License**: LGPL 2.1+ or GPL (depending on build configuration)
- **Website**: https://ffmpeg.org/
- **Usage**: Invoked as external subprocess for video recording (not linked)

FFmpeg is a trademark of Fabrice Bellard. If FFmpeg is bundled with this
application, its license terms apply to the FFmpeg binary only, not to TAREEK-Vis.
Prebuilt releases bundle the gyan.dev "essentials" build, which includes
GPL-licensed components (e.g. libx264); this is compatible with TAREEK-Vis's
own GPLv3 license. An LGPL-only FFmpeg build may be substituted if a less
restrictive redistribution is needed.

---

## OpenGL

- **Usage**: Graphics rendering via system-provided drivers
- **No licensing restrictions** apply to the use of the OpenGL API.

---

## Lucide icons

- **License**: ISC
- **Version**: 0.469.0 (`lucide-static` package)
- **Website**: https://lucide.dev/
- **Copyright**: Copyright (c) for portions of Lucide are held by Cole Bemis
  2013-2022 as part of Feather (MIT). All other copyright (c) for Lucide are
  held by Lucide Contributors 2022.
- **Usage**: Ribbon and info panel button icons. The SVG files are in
  `src/ui/ribbon/icons/svg/` without changes; the app ships PNG files made
  from them (`src/ui/ribbon/icons/render_icons.mjs`). The full license text is
  in `src/ui/ribbon/icons/LICENSE`.

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

---

# Background Map Tile Services

The optional background map layer (View → Background Map) streams raster map
tiles over HTTPS from the third-party services listed below. These tiles are
**not** bundled with or redistributed by TAREEK-Vis; they are requested
directly by the end user's machine when a background layer is enabled. Tiles
are kept in a local disk cache (at most 300 MB) at
`%LOCALAPPDATA%\TAREEK-Vis\TAREEK-Vis\cache\tiles`, so that the same view is
not downloaded again. The installer removes this cache on uninstall.

Each service requires attribution whenever its map imagery is displayed,
reproduced, or published. If you publish figures, screenshots, or recorded
videos made with a background map enabled, credit the corresponding provider
in the caption or credits. Use of these services is subject to each
provider's own terms, which may limit commercial or high-volume use.

---

## OpenStreetMap Standard tiles ("OpenStreetMap" layer)

- **Endpoint**: `https://tile.openstreetmap.org/{z}/{x}/{y}.png`
- **Provider**: OpenStreetMap Foundation (Standard tile layer)
- **Underlying data**: OpenStreetMap, licensed under the
  [Open Database License (ODbL)](https://www.openstreetmap.org/copyright)
- **Terms**: https://operations.osmfoundation.org/policies/tiles/
  (no API key; the client must send an identifying User-Agent and must not
  bulk-download tiles)
- **Required attribution**: © OpenStreetMap contributors

---

## Esri World Imagery ("Satellite" layer)

- **Endpoint**: `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}`
- **Provider**: Esri (ArcGIS Online World Imagery service), aggregating imagery
  from Maxar, Earthstar Geographics, and other sources
- **Terms**: https://www.esri.com/en-us/legal/terms/web-site-service
  — permits noncommercial use with proper attribution to Esri; commercial use
  requires a separate license from Esri.
- **Required attribution**: Esri, Maxar, Earthstar Geographics, and the GIS
  User Community

---

## OpenTopoMap ("Topographic" layer)

- **Endpoint**: `https://tile.opentopomap.org/{z}/{x}/{y}.png`
- **Provider**: OpenTopoMap
- **Underlying data**: OpenStreetMap
  ([ODbL](https://www.openstreetmap.org/copyright)) and SRTM elevation data
- **Cartography license**:
  [CC-BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/)
- **Terms**: https://opentopomap.org/about — high-volume use should be
  arranged with the OpenTopoMap maintainers first.
- **Required attribution**: © OpenStreetMap contributors, © OpenTopoMap
  (CC-BY-SA)
