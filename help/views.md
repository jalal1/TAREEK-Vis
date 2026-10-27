# Saved views

A view keeps how the map looks at one moment, so that you can go back to it
later. A view keeps:

- the simulation time
- the map position and the zoom
- the layers and the vehicle style (the toggles of the View, Transit and Air
  tabs)
- the selected links, if there are some
- the vehicle that the map follows, if there is one
- the activity heatmap, if one shows

The views of a run are on the **Views** page of the side panel.

## Save a view

1. Set the map as you want to keep it.
2. Press **Ctrl+B**, or click **Views > Save View**.

The view goes to the end of the list, with the name "View 1", "View 2" and so
on. Each card shows a small picture of the map, the time and what the view
keeps.

## Save views over time

To see how the traffic changes during the day at one place:

1. Set the map as you want it.
2. Click **Views > Save Over Time...**.
3. Type a name, the first time (**From**), the last time (**To**) and the
   step (**Every**). For example, every 1 h from 06:00 to 20:00.
4. Click **Save Views**.

TAREEK-Vis saves one view for each time, with the name and the time, for
example "Peak · 08:00". All the views have the same map position and layers.
To make images of them, see "Make images of the views" below.

## Go to a view

Double-click the view, or select it and press Enter.

- A toggle that is not available now stays as it is. For example, **Show Link
  Counts** stays off until you load counts.
- If the view has a heatmap that is not computed yet, TAREEK-Vis computes it.
  On a large network, it asks first.
- A view without selected links does not change the selection that you have.
- If a link, a vehicle or a heatmap of the view is not in this run, the status
  bar tells you.

## Change the list

Right-click a view for these commands:

| Command | Use |
|---------|-----|
| Go to View | The same as a double-click. |
| Update from Current View | Keep the name, and replace the rest with the map as it is now. |
| Rename | Type a new name (or press F2). |
| Duplicate | Make a copy below the view. |
| Delete | Remove the selected views (or press Delete). |

Drag a view up or down to change the order.

## Where the views are kept

TAREEK-Vis keeps the views of a run in the file `tareek-views.json`, in the
folder of the network file. Every change is saved at once. When you open the
run again, its views come back.

If TAREEK-Vis cannot write to the run folder, it keeps the file in its own
data folder, and the status bar tells you.

## Send views to someone

1. Click **Views > Export Views...** and save the file.
2. Send the file and the name of the run.
3. The other person opens the run and clicks **Views > Import Views...**.
   The views go to the end of their list.

You can also import views of a different run, for example to compare a
baseline and a what-if scenario at the same places and times. Links,
vehicles and heatmaps that are not in the other run are not shown.

## Make images of the views

1. Click **Views > Capture Views...**.
2. Select the views. All views are selected at first.
3. Select a resolution: 1x (screen), 2x (recommended), 4x or 8x. The dialog
   shows the size of the images.
4. Select a folder. The default is the `captures` folder in the run folder.
5. Click **Capture**.

TAREEK-Vis goes to each view and saves it as `<view name>.png`. A `:` in the
name becomes `.`, so "Peak · 08:00" becomes `Peak · 08.00.png`. An image
with the same name is replaced.

- If a view has a heatmap that is not computed yet, TAREEK-Vis waits for it.
- Click **Cancel** to stop. The images saved before that stay.
- After the capture, the map goes back to how it was before.

You can also make the images from the command line. A views file is also a
scene file. This command saves one PNG image of each view in `DIR`:

```
TAREEK-Vis <run folder> --scenes <run folder>/tareek-views.json --out DIR
```
