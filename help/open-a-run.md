# Open a simulation run

TAREEK-Vis reads the output folder of a MATSim run. It needs a network file
and an events file. A transit schedule is optional. You load traffic counts
after the run is open (see [Compare with traffic counts](counts.md)).

## Open a folder

1. Click **Home > Open Folder** (Ctrl+O).
2. Select the folder that contains the output files.

TAREEK-Vis finds the files by their names:

| File | Name | Required |
|------|------|----------|
| Network | `*network*.xml` or `*network*.xml.gz` | Yes |
| Events | `*events*.xml` or `*events*.xml.gz` | Yes |
| Transit schedule | `*transitSchedule*.xml` or `.xml.gz` | No |

- TAREEK-Vis ignores `networkChangeEvents` files.
- If two or more files match, TAREEK-Vis uses the file that starts with
  `output_`. If this does not find one file, TAREEK-Vis asks you to select
  one.

You can also open a run in these ways:

- Drag the folder, or one `.xml` or `.xml.gz` file in it, onto the window.
- Click **Home > Recent Folders**. The list keeps the last 5 folders. Point
  to an item to see its full path.
- Start TAREEK-Vis from a command line: `TAREEK-Vis.exe <folder>`.

## Open files from different folders

1. Click **Home > Open Files**.
2. Select the network file.
3. Select the events file.

TAREEK-Vis looks for the transit schedule in the folder of the network file.

## The cache

The first time that you open a run, TAREEK-Vis reads the XML files and
writes a cache. A window shows each step. Large runs can take some minutes.
Click **Cancel** or press Esc to stop.

- The cache is in the `.simvis_cache` folder, in the folder of the network
  file.
- Later, TAREEK-Vis reads the cache. This is much faster.
- TAREEK-Vis makes the cache again when the network file or the events file
  is newer than the cache. It also does this after an update that changes the
  cache format.

To make the cache again, click **Home > Rebuild Cache**. If you cancel, or
if the rebuild fails, TAREEK-Vis continues to use the old cache.

## Open a different run

When you open a different run:

- The link that you examined opens again in the new run. TAREEK-Vis finds it
  by its ID. Use this to compare one road in two runs.
- The scenario selection, the heatmap, the counts and the selected person or
  vehicle are cleared.
- Playback stops at the start of the new run.
