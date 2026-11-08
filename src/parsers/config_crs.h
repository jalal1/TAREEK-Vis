#pragma once

#include <string>

namespace simvis {

// Finds the CRS of a run in the MATSim config file next to the network.
// Some network files have no coordinateReferenceSystem attribute, but the
// config of the run that wrote them has the CRS.
//
// Files, in this order: output_config.xml, config.xml (also .gz), then any
// other *config*.xml in the folder. In each file: global.coordinateSystem,
// then network.inputCRS. Only "EPSG:<code>" values count; MATSim's default
// "Atlantis" is not a CRS.
//
// Returns the value (for example "EPSG:26915"), or "" if no file has one.
// sourceFile, if given, gets the path of the file that had the value.
std::string findCrsInRunConfig(const std::string& folder,
                               std::string* sourceFile = nullptr);

} // namespace simvis
