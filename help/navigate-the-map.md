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

The color of a link shows its road type. The road type comes from the `type`
attribute of the link, for example `motorway` or `primary`. Links without
this attribute are gray.

| Road type | Color |
|-----------|-------|
| Motorway | Red |
| Primary | Orange |
| Secondary | Yellow |
| Tertiary | Light gray |
| Residential | Gray |
| Service | Dark gray |

## Background map

1. Click **View > Background Map**.
2. To change the map, click **Source**, then select **OpenStreetMap**,
   **Satellite (ESRI)** or **Topo Map**.

- The background map needs the coordinate reference system (CRS) of the
  network. The network file must have a `coordinateReferenceSystem`
  attribute, for example `EPSG:26915`.
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
