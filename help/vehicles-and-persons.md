# Vehicles and persons

## Show or hide vehicles

Use the **View > Vehicles** group:

| Button | Shows |
|--------|-------|
| All Vehicles | All vehicles. Turn it off to hide all of them. |
| Private | Private vehicles (cars). |
| Bus | Buses. |
| Tram | Trams. |
| Rail | Trains. |
| Other Modes | A list of the other modes in the run, for example taxis or bikes, with the number of vehicles. Clear a mode to hide its vehicles. |

TAREEK-Vis finds the mode of a vehicle in this order:

1. The transit schedule gives the buses, trams and trains.
2. The leg mode of the driver's trip gives the other modes. For example, the
   leg mode `taxi` gives a taxi and `bike` gives a bike.
3. All other vehicles are cars.

## Change how vehicles look

Use the **View > Vehicle Style** group:

| Button | Choices |
|--------|---------|
| Shape | Auto (by mode), Circle, Triangle (Arrow), Rectangle, Diamond. Auto is the default. |
| Size | Small (3 px), Medium (5 px), Large (8 px). Large is the default. |

To change the vehicle colors, click **View > Links > Color by**. In the
**Vehicles** part of the menu, select:

- **By Vehicle Speed** (the default). The color shows the speed of the
  vehicle on its link as a part of the free speed.
- **By Mode (Icons)**. Each vehicle shows as an icon of its mode.

With **By Vehicle Speed**, the speed is the free-flow time of the link
divided by the time that the vehicle takes on the link. The free-flow time
is the time that MATSim gives a free vehicle: the length divided by the free
speed, rounded down to a full second, plus one second. Thus a vehicle with
no delay is green, also on a short link.

| Speed | Color |
|-------|-------|
| 80% to 100% of free speed | Green |
| 40% to 80% | Green to yellow |
| Less than 40% | Yellow to red |

A key at the bottom left of the map shows these colors. When the links are
colored too, the vehicle key is above the link key.

## Examine a vehicle

Click a vehicle. A halo shows on the vehicle and the map follows it. The
**Info** page of the side panel shows:

- The mode of the vehicle, for example Car, Bus or Taxi.
- For a car: the ID of the driver and the trips of the driver
  for the day.
- For a bus, tram or train: the line, the route and the passengers aboard
  now. Click a passenger to see the day of that person.

Each trip shows the activity at the start, the mode, the activity at the
end, the departure time, the duration and the arrival time. The trip in
progress has a highlight. "stuck" means that the person did not arrive.

![Vehicle information in the side panel](images/vehicle-info.png)

To stop following the vehicle, click an empty area of the map. The Info page
keeps the vehicle.

To copy an ID, click the copy button beside it.

## See the route of a person

- Click **Show Person Route**. The route of the full day shows on the map in
  yellow, with a marker at each activity.
- Click a trip to show only that trip. Click the trip again to hide it.
- Click **Hide Person Route** to remove the route.

To find a person by their ID, see
[Find a link, person or vehicle](find.md).
