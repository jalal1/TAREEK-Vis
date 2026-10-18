# Export images, charts and videos

## Save the map as an image

1. Set the map as you want it in the image.
2. Click **Export > PNG** (Ctrl+Shift+P) or **Export > PDF** (Ctrl+P).
3. Select a resolution: 1x (screen), 2x (recommended), 4x or 8x.
4. Type a file name.

- The image shows the map as it is on the screen, with the heatmap legend.
- Playback pauses during the export.
- If the map shows a background map, give the credit that the provider
  requires. See [Move around the map](navigate-the-map.md).

## Save a chart

To save the hourly volume chart of a link, see
[Road links and hourly volume](road-links.md).

## Record a video

1. Set the map as you want it in the video.
2. Click **Export > Record** (Ctrl+R).
3. Set the video:

   | Setting | Choices |
   |---------|---------|
   | Resolution | 720p, 1080p or 4K |
   | Frame Rate | 30 fps or 60 fps |
   | Speed | 1x, 10x, 30x or 60x. At 30x, 30 simulation seconds become 1 second of video. |
   | Start Time, End Time | The part of the simulation to record. |
   | Output File | An `.mp4` file. |

   The window shows the video duration and an estimate of the file size.
4. Click **Start Recording**.

The recording stops at the end time. To stop it before, click
**Export > Stop** (Ctrl+Shift+R). You can minimize the window while it
records.

TAREEK-Vis uses FFmpeg to write the video. The installer includes FFmpeg. If
you built TAREEK-Vis yourself, put `ffmpeg.exe` in an `ffmpeg` folder next to
`TAREEK-Vis.exe`, or add FFmpeg to the PATH.

## Save a scenario file

See [What-if scenarios](scenarios.md).
