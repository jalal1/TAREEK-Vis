# Troubleshooting

## "Required Files Not Found"

TAREEK-Vis did not find a network file or an events file in the folder. The
file names must contain `network` and `events`. If the files are in
different folders, use **Home > Open Files**. See
[Open a simulation run](open-a-run.md).

## The background map does not show

- If TAREEK-Vis says "CRS not detected", the network file has no
  `coordinateReferenceSystem` attribute. Add it, then click
  **Home > Rebuild Cache**.
- The tiles come from the internet. Make sure that the computer is
  connected.

## The map shows old data

The run files changed, but TAREEK-Vis did not see the change. Click
**Home > Rebuild Cache**.

## "No valid count locations found"

The `loc_id` values in the counts file do not match the link IDs of the
network. Make sure that the counts file is for this network.

## The hourly volume chart does not show

Just after a run opens, TAREEK-Vis counts the volumes in the background.
Wait some seconds. The chart then shows.

## The heatmap takes a long time

On a large network, one map can take some minutes. You can continue to use
the window. The map is saved, and the next time it opens immediately. See
[Activity heatmap](heatmap.md).

## "Failed to start recording"

TAREEK-Vis did not find FFmpeg. See [Export](export.md).

## Windows says "Windows protected your PC"

The installer is not code-signed. Click **More info**, then **Run anyway**.

## Report a problem

1. Click **Help > Report a Problem**. The GitHub issue page opens.
2. Tell us what you did, what you expected and what occurred.
3. Attach the log file, `simvis.log`. It is in the `.simvis_cache` folder of
   the run. Before a run opens, the log is in your `%TEMP%` folder.

The log contains the full paths of your files. Read it before you attach it.
See [Privacy and security](privacy.md).
