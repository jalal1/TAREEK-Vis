# Road links and hourly volume

## Examine a link

Click a road on the map. The road shows in yellow. The **Info** page of the
side panel shows:

- The link ID. Click the copy button to copy it.
- The length, free speed, capacity, number of lanes, road type and free-flow
  time. The number of lanes can be a decimal number, for example 1.5.
- The from node and the to node.
- The **Hourly Volume** chart.
- The **Metrics Over the Day** chart. See [Link colors](link-colors.md#a-link-over-the-day).

![Link information and the hourly volume chart](images/link-info.png)

## Read the hourly volume chart

Each bar shows the number of vehicles that entered the link in one hour.
Point to a bar to see its hour and its value.

- The chart covers the whole run. If the run goes past 24:00, the chart has
  bars for 24:00, 25:00 and so on. A trip at 25:30 counts in hour 25.
- The numbers come from the same table as the link colors (see
  [Link colors](link-colors.md)), so the chart and the colors agree.
- Before you load counts, the bars show the simulated vehicles from the
  events file. These numbers are not scaled to the full population.
- After you load counts, TAREEK-Vis multiplies the bars by the
  `countsScaleFactor` that you gave. See
  [Compare with traffic counts](counts.md).
- Just after a run opens, TAREEK-Vis reads or makes the link table in the
  background. The chart shows when this is complete.

## Export the chart

1. Click **Export chart...** below the chart, or right-click the chart.
2. Type a file name that ends in `.png` or `.pdf`.

The title of the chart shows the link ID and the name of the run folder. The
file name that TAREEK-Vis suggests is `<run>_link_<link ID>`.

## Compare a link in two runs

1. Click the link in the first run.
2. Open the second run, for example a scenario run.

The same link opens again in the second run. TAREEK-Vis finds it by its ID.
If the second run does not have the link, the status bar tells you.
