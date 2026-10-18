# Compare with traffic counts

Traffic counts are hourly vehicle numbers measured on real roads. TAREEK-Vis
compares them with the simulated volumes on the same links.

## Load counts

1. Open a run.
2. Click **Home > Load Counts** (Ctrl+Shift+C).
3. Select a MATSim counts file (`.xml` or `.xml.gz`).
4. Type the `countsScaleFactor` from the `counts` module of your MATSim
   `config.xml`. Then click **OK**.

TAREEK-Vis multiplies the simulated volumes by this factor. A simulation of a
10% sample usually has a factor of 10.

The count stations show in yellow on the map. To hide them, click
**View > Link Counts**.

## Compare one station

Click a count station on the map. A window shows a chart with two bars for
each hour:

- **Simulated**: the volume from the simulation, multiplied by the scale
  factor.
- **Ground Truth**: the volume from the counts file.

A count station is on top of its link. To see the link details of a counted
link, turn off **View > Link Counts** and click the link. The Info page then
shows the ID of the count station.

## Requirements

The `loc_id` of each count must be the ID of a link in the network. If no
`loc_id` matches, TAREEK-Vis shows "No valid count locations found". See
[Input files](input-files.md).
