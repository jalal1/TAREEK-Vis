# Move around the map

## Pan and zoom

| To | Do this |
|----|---------|
| Move the map | Drag an empty area of the map with the left mouse button. |
| Zoom in or out | Turn the mouse wheel. The map zooms around the pointer. You can also press Ctrl++ and Ctrl+-. |
| See all of the network | Click **Home > Fit to Network** (F). |
| Move the map without a selection | Click **Home > Pan** (H). |

When you drag from a road, a vehicle or a stop, TAREEK-Vis selects that item
and does not move the map. To prevent this, turn on **Pan**. While Pan is on,
a drag only moves the map, and the selection stays. If the map follows a
vehicle, a drag stops this, but the vehicle keeps its halo. Press H again to
turn Pan off.

## Show or hide layers

Use the **View > Layers** group:

| Button | Shows |
|--------|-------|
| Nodes | The network nodes. Off at the start. |
| Links | The road links. |
| Link Counts | The count stations. Available after you load counts. |
| Info Panel | The Info page of the side panel. |

By default, the gray level and the width of a link show its road type. A
motorway is the widest line, and a residential road is the thinnest. On the
dark map, an important road is lighter. On the OpenStreetMap and Topo maps,
it is darker. The road type comes from the `type` attribute of the link, for
example `motorway` or `primary`, or from `osm:way:highway`. Links with no
road type (usually pt and rail) are thin and dim. See the table in
[Link colors](link-colors.md#road-type).

To color the links by capacity, speed, flow or other values, see
[Link colors](link-colors.md).

## Background map

1. Click **View > Background Map**.
2. To change the map, click **Source**, then select **OpenStreetMap**,
   **Satellite (ESRI)** or **Topo Map**.

- The background map needs the coordinate reference system (CRS) of the
  network, for example `EPSG:26915`. TAREEK-Vis finds the CRS in the
  `coordinateReferenceSystem` attribute of the network file. If the network
  file has no CRS, TAREEK-Vis asks you for the EPSG code (for example
  `EPSG:26915`) when you turn on the background map, and keeps it for the
  run.
- The map tiles come from the internet. TAREEK-Vis keeps up to 300 MB of
  tiles in `%LOCALAPPDATA%\TAREEK-Vis\TAREEK-Vis\cache\tiles`. Areas that you saw before
  load fast, also after a restart.
- Until a tile downloads, a blurred tile shows in its place.

If you publish an image or a video with a background map, give the credit
that the provider requires:

| Source | Credit |
|--------|--------|
| OpenStreetMap | © OpenStreetMap contributors |
| Satellite (ESRI) | Esri, Maxar, Earthstar Geographics, and the GIS User Community |
| Topo Map | © OpenStreetMap contributors, © OpenTopoMap (CC-BY-SA) |
