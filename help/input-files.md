# Input files

TAREEK-Vis reads standard MATSim output files. Each file can be plain XML
(`.xml`) or compressed (`.xml.gz`).

## Network (required)

```xml
<network>
  <attributes>
    <attribute name="coordinateReferenceSystem" class="java.lang.String">EPSG:26915</attribute>
  </attributes>
  <nodes>
    <node id="1" x="123.45" y="678.90" />
  </nodes>
  <links>
    <link id="1" from="1" to="2" length="100" freespeed="13.89"
          capacity="1800" permlanes="1" />
  </links>
</network>
```

The `coordinateReferenceSystem` attribute is optional. The background map
needs it.

## Events (required)

```xml
<events>
  <event time="0.0" type="vehicle enters traffic" vehicle="v1" link="1" />
  <event time="10.0" type="entered link" vehicle="v1" link="2" />
  <event time="20.0" type="left link" vehicle="v1" link="1" />
</events>
```

TAREEK-Vis also reads the activity, departure, arrival and transit events.
It uses them for the trips of each person, the passengers and the heatmap.

## Transit schedule (optional)

The standard MATSim `transitSchedule.xml`. TAREEK-Vis uses the stops, the
lines, the routes and the departures.

## Counts (optional)

```xml
<counts>
  <count loc_id="48211" cs_id="station_17">
    <volume h="1" val="120" />
    <volume h="2" val="85" />
  </count>
</counts>
```

`loc_id` is the ID of a link in the network. `h` is the hour (1 to 24).
