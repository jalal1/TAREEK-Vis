# Air traffic

TAREEK-Vis shows aircraft (air taxis and drones) from the tareek-air
simulator. A run can have only aircraft, or ground traffic and aircraft
together. The **Air** tab shows only when the run has aircraft.

## Read the air view

Each aircraft has these parts:

| Part | Shows |
|------|-------|
| Aircraft symbol | The aircraft. It points in the direction of flight. |
| Ground mark | A small dot at the position of the aircraft over the ground. |
| Stem | A thin line from the ground mark up to the symbol. A longer stem is a higher aircraft. |
| Trail | The path of the last 30 seconds. It fades out. |
| Color | The height of the aircraft. Violet is low and yellow is high. The key at the bottom right gives the range. |

The symbol moves up the screen by its height. Thus:

- In a take-off, the symbol rises from the vertiport. The ground mark stays at
  the vertiport.
- In a climb, the stem becomes longer as the aircraft flies away.
- In a cruise, the stem has a constant length.
- In a descent and a landing, the symbol comes down onto the ground mark.

A vertiport shows as a helipad mark (a circle with an "H").

The color shows the height when **Color** in **View > Vehicle Style** is
**By Speed**. With
**By Mode (Icons)**, an aircraft shows as a helicopter icon.

## Change the air view

Use the **Air** tab:

| Group | Button | Use |
|-------|--------|-----|
| Layers | Air Links | Show or hide the air links. They are off at the start. |
| Layers | Vertiports | Show or hide the vertiport marks. |
| Layers | Trails | Show or hide the trails. |
| Height | Altitude Lift | How far a symbol moves up for its height: No Lift, Low Lift, Medium Lift (the default) or High Lift. With Medium Lift, 500 m is 20 pixels. |
| Height | Altitude Band | Show only the aircraft in one band of 300 m, or All Heights. |
| Events | Losses | Show or hide the losses of separation. |
| Events | Holds | Show or hide the holds. |

Air links show the flight paths. Their color shows the phase of flight:

| Color | Phase |
|-------|-------|
| White | Vertical (take-off or landing) |
| Green | Climb |
| Blue | Cruise |
| Orange | Descent |
| Yellow | Hover |

## See losses and holds

tareek-air writes events for losses of separation and for holds. TAREEK-Vis
shows them while they last:

| Mark | Shows |
|------|-------|
| Red line between two aircraft, and a red ring around each | A loss of separation: the two aircraft are closer than the separation minimum. Two aircraft at one place, for example on one pad, show as red rings only. |
| Amber ring at a vertiport | A ground hold: an aircraft waits on the ground before it departs. |
| Red-orange ring at a vertiport | An airborne hold: an aircraft waits in the air before it lands. |

A ring is larger when more aircraft hold at the same vertiport. The status
bar shows the number of aircraft, losses and holds now.

## Examine an aircraft

Click the symbol of an aircraft. The **Info** page shows the **Flight**
section:

- **Height**: the height above the datum of the region, in metres.
- **Vertical rate**: in metres per second. A positive rate is a climb.
- **Phase**: Take-off, Climb, Cruise, Descent, Landing or Hover.

To hide all aircraft, clear **Aircraft** in **View > Other Modes**.
