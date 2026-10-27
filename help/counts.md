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

## Export the charts of all stations

To check the fit of the simulation after a calibration run:

1. Load counts.
2. Click **Analysis > Export Count Charts...**.
3. Select **All stations**, or **Stations on the selected links**. To use the
   second choice, Ctrl+click the roads with the stations first.
4. Select a resolution and a folder. The default is the `count-charts` folder
   in the run folder.
5. Click **Export**.

TAREEK-Vis saves one PNG image of each station, `link <link id> - <station
id>.png`. Each image has the chart, and a title with the link, the station
and the daily totals.

If **Also save summary.csv** is on, TAREEK-Vis also saves a table with one
row for each station:

| Column | Meaning |
|--------|---------|
| `link_id`, `station_id` | The link and the count station. |
| `observed_total` | The count of the whole day, from the counts file. |
| `simulated_total` | The simulated volume of the whole day, multiplied by the scale factor. |
| `ratio` | `simulated_total` divided by `observed_total`. 1 is a perfect match. Empty when the count is 0. |
| `difference` | `simulated_total` minus `observed_total`. |
| `max_hourly_geh` | The largest GEH of the 24 hours. |
| `hours_geh_below_5` | The number of hours with a GEH less than 5. |
| `image` | The file name of the chart. |

The GEH compares a simulated volume M with a count C:
GEH = sqrt(2 (M − C)² / (M + C)). A GEH less than 5 is usually a good fit.
Sort the table by `max_hourly_geh` to find the stations with the worst fit.

## Requirements

The `loc_id` of each count must be the ID of a link in the network. If no
`loc_id` matches, TAREEK-Vis shows "No valid count locations found". See
[Input files](input-files.md).
